// Copyright (c) 2026 Ekorau LLC

// internal/space/sweep.go
package space

import (
	"bytes"
	"database/sql"
	"errors"
	"fmt"
	"time"

	"github.com/davidg238/porta/internal/cval"
)

// SweepTick is how often the verdict is re-formed. It is a floor on detection
// latency: a node whose budget is shorter than one tick is detected at the next
// tick, not at its budget. It is also, as of rete#231, the resolution of the
// gateway's own liveness record (runs.go).
const SweepTick = 30 * time.Second

// The four verdicts, pre-encoded once. The transition test compares these bytes
// against what is stored: values in the store are raw deterministic CBOR and are
// decoded only for display (D4.1), so decoding here to compare two symbols would
// break that rule for nothing.
//
// FOUR AND NOT TWO, because they are four different operator actions (rete#231
// D5/D6). #neverSeen is a commissioning failure — go plug it in. #silent is a
// liveness failure — go find out why it stopped. #unknown is porta declining to
// answer, and it has two causes, told apart by the row's `reason`: the node has
// declared no budget, or the window falls inside an interval porta was not
// listening. The store must not keep asserting the last thing it heard.
var (
	livenessLive      = mustEncode(cval.Symbol("live"))
	livenessSilent    = mustEncode(cval.Symbol("silent"))
	livenessNeverSeen = mustEncode(cval.Symbol("neverSeen"))
	livenessUnknown   = mustEncode(cval.Symbol("unknown"))
)

func mustEncode(v any) []byte {
	b, err := cval.Encode(v)
	if err != nil {
		panic(fmt.Sprintf("space: encoding a constant failed: %v", err))
	}
	return b
}

// LivenessChange is one transition, returned so the caller can log it. A sweep
// that changed nothing returns an empty slice — which is the normal case.
type LivenessChange struct {
	NodeID   string
	Verdict  string // live | silent | neverSeen | unknown
	SinceMs  int64  // how long since the node was last heard from, at the verdict
	BudgetMs int64  // telCad x misses, both from the NODE; 0 when it declared none
	TelCadMs int64  // the cadence the node declared; 0 when it declared none
	Misses   int64  // the consecutive misses it tolerates; 0 when it declared none
}

// Sweep forms one liveness verdict per node and writes gw/liveness ONLY where
// the verdict differs from the one already stored.
//
// THE RULE IS ONE LINE AND THERE ARE NO CONSTANTS IN IT: now - last_seen > bud,
// where bud = telCad x misses, both declared by the NODE at obs/liveness
// (rete#273 D2 — there is no bud on the wire, so an incoherent pair is
// unrepresentable). porta infers nothing (rete#231 D4) and contributes no
// number; the ONE case it declines is a product below its own SweepTick
// (D2.1), which it says out loud as #unknown rather than judging at 30 s
// while claiming 16. What that deleted, and why it is a deletion rather
// than a replacement: cadence used to be the median of the last ten
// inter-arrival gaps, silence was 3 x that, and both the 10 and the 3 were
// numbers porta picked. A node that had never spoken supplied no gaps and was
// therefore never judged at all (#219); a fast node doing routine blocking work
// was judged against 3 x 2 s and called silent (#220). 2026-08-10 D8 chose
// inference because the declaration did not exist. It does now.
//
// Why a ticker and not a lazy read-time view (2026-08-10 §6.3): the failure this
// must catch is porta hearing NOTHING, and a system that evaluates only when
// read is silent exactly when everything is. The ticker is not a convenience.
//
// Why transition-only: it makes `porta log` a list of the times the fleet went
// quiet. A row every 30 s would bury the one moment that matters.
//
// One node's failure never costs another node its verdict: each node is
// evaluated independently, the changes returned are only the ones actually
// committed, and the returned error (via errors.Join) names every node that
// failed. Only a failure in the query that lists the nodes themselves — not
// per-node — aborts the sweep outright, because nothing can proceed without it.
//
// ONE ACCEPTED FALSE-REPORT MODE, and one that is now FIXED:
//
//   - I4 — A GATEWAY OUTAGE READ AS NODE SILENCE. FIXED by rete#231 D6: porta
//     records the intervals it was actually listening (runs.go), and a verdict
//     whose window falls inside a gap reads #unknown. Before that fix,
//     restarting porta after an outage longer than a node's budget wrote
//     #silent for every affected node and then #live as each checked back in —
//     a burst byte-identical in `porta log` to a genuine fleet-wide failure.
//
//   - I5 — A SICK NODE'S INFERRED CADENCE INFLATED, DELAYING DETECTION. GONE
//     with the inference itself: there is no cadence to inflate, and a node in
//     a reboot loop is judged against the budget it declared before it got sick.
//
//   - I7 — A POWERED-OFF NODE IS INDISTINGUISHABLE FROM A WEDGED-AND-HALTED
//     ONE, and nothing on the gateway side can change that (design §4). What
//     the budget buys is knowing WHEN TO START WORRYING, which is what was
//     missing. Do not read #silent as "crashed".
func (s *Store) Sweep() ([]LivenessChange, error) {
	now := s.nowMs()
	// The gateway's own presence, stamped BEFORE any verdict is formed — see
	// MarkAlive: a verdict formed at `now` against a run whose alive_ms is one
	// tick behind would find its own window uncovered and report #unknown about
	// a gateway that was plainly awake. It lives HERE rather than in serve's
	// sweep loop because this method IS the tick: a caller that forgot the call
	// would get a fleet of #unknown with nothing to say why, and there is no
	// second caller for whom that would be right. A store nobody is serving
	// from (every read-only CLI path) has no open run and this is a no-op.
	if err := s.MarkAlive(); err != nil {
		return nil, fmt.Errorf("marking the gateway alive: %w", err)
	}
	rows, err := s.db.Query(`
		SELECT n.id, n.last_seen_ms, COALESCE(c.value_cbor, x'')
		FROM nodes n
		LEFT JOIN cells c ON c.node_id = n.id AND c.path = ?
		ORDER BY n.id`, PathLiveness)
	if err != nil {
		return nil, err
	}
	type row struct {
		id       string
		lastSeen sql.NullInt64
		stored   []byte
	}
	var nodes []row
	for rows.Next() {
		var r row
		if err := rows.Scan(&r.id, &r.lastSeen, &r.stored); err != nil {
			rows.Close()
			return nil, err
		}
		nodes = append(nodes, r)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, err
	}

	var changes []LivenessChange
	var errs []error
	for _, n := range nodes {
		want, verdict, since, d, reason, err := s.verdictFor(n.id, n.lastSeen, now)
		if err != nil {
			errs = append(errs, fmt.Errorf("node %s: %w", n.id, err))
			continue
		}
		if bytes.Equal(n.stored, want) {
			continue
		}
		if err := s.WriteGateway(n.id, PathLiveness, want, reason); err != nil {
			errs = append(errs, fmt.Errorf("node %s: write verdict: %w", n.id, err))
			continue
		}
		changes = append(changes, LivenessChange{
			NodeID: n.id, Verdict: verdict, SinceMs: since,
			BudgetMs: d.BudgetMs(), TelCadMs: d.TelCadMs, Misses: d.Misses,
		})
	}
	return changes, errors.Join(errs...)
}

// verdictFor is the whole decision, in the order the evidence allows.
//
// The ORDER is load-bearing. Speech is positive evidence and outranks
// everything: a node heard from inside its budget is #live even if porta was
// deaf for the ten hours before that. Absence is only ever evidence when porta
// was listening, which is why the deaf test sits between "past the budget" and
// "silent" rather than at the top.
func (s *Store) verdictFor(nodeID string, lastSeen sql.NullInt64, now int64) (want []byte, verdict string, since int64, d Declaration, reason string, err error) {
	if !lastSeen.Valid {
		// D5: never heard from. Judgeable immediately and WITHOUT a budget,
		// because the question "has this node ever spoken" needs no number.
		return livenessNeverSeen, "neverSeen", 0, Declaration{},
			"neverSeen: no exchange has ever arrived from this node", nil
	}
	since = now - lastSeen.Int64
	d, ok, err := s.DeclaredLiveness(nodeID)
	if err != nil {
		return nil, "", 0, Declaration{}, "", fmt.Errorf("declared liveness: %w", err)
	}
	if !ok {
		return livenessUnknown, "unknown", since, Declaration{},
			fmt.Sprintf("unknown: %d ms since the last exchange, and this node has declared no cadence", since), nil
	}
	bud := d.BudgetMs()
	// rete#273 D2.1: porta refuses to promise what its sweep cannot deliver.
	// A budget below one tick would be detected at the tick, not at the
	// budget; saying so is worth more than quietly enforcing 30 s.
	if tick := SweepTick.Milliseconds(); bud < tick {
		return livenessUnknown, "unknown", since, d,
			fmt.Sprintf("unknown: telCad %d ms x misses %d = %d ms is below the sweep tick %d ms; porta cannot judge at that resolution — raise misses or telCad", d.TelCadMs, d.Misses, bud, tick), nil
	}
	// rete#273 D2.2: the miss count is a REASON, not a verdict — the ladder
	// an operator reads, never a fifth state.
	missed := float64(since) / float64(d.TelCadMs)
	if since <= bud {
		return livenessLive, "live", since, d,
			fmt.Sprintf("live: %d ms since the last exchange, %.1f missed check-ins of %d ms, %d tolerated", since, missed, d.TelCadMs, d.Misses), nil
	}
	// D6: absence is only evidence when porta was listening. A window that
	// touches an interval porta was down yields #unknown — never #silent,
	// because the gateway has no evidence in either direction, and never
	// #live, because it has none of that either. Note the position: this is
	// BELOW the `since <= bud` test, because speech is positive evidence and
	// outranks the gateway's own absence.
	deaf, err := s.WasDeaf(now-since, now)
	if err != nil {
		return nil, "", 0, Declaration{}, "", fmt.Errorf("deaf intervals: %w", err)
	}
	if deaf {
		return livenessUnknown, "unknown", since, d,
			fmt.Sprintf("unknown: %d ms since the last exchange, budget %d ms, and porta was not listening for part of it", since, bud), nil
	}
	return livenessSilent, "silent", since, d,
		fmt.Sprintf("silent: %d ms since the last exchange — %.1f missed check-ins of %d ms, %d tolerated", since, missed, d.TelCadMs, d.Misses), nil
}
