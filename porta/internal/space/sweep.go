// Copyright (c) 2026 Ekorau LLC

// internal/space/sweep.go
package space

import (
	"bytes"
	"database/sql"
	"errors"
	"fmt"
	"time"

	"github.com/davidg238/rete/protocol/cval"
	"github.com/davidg238/rete/protocol/store"
)

// SweepTick is how often the verdict is re-formed. It is a floor on detection
// latency: a node whose budget is shorter than one tick is detected at the next
// tick, not at its budget. It is also, as of rete#231, the resolution of the
// gateway's own liveness record (runs.go).
const SweepTick = 30 * time.Second

// The five verdicts, pre-encoded once. The transition test compares these bytes
// against what is stored: values in the store are raw deterministic CBOR and are
// decoded only for display (D4.1), so decoding here to compare two symbols would
// break that rule for nothing.
//
// FIVE AND NOT TWO, because they are five different operator actions (rete#231
// D5/D6, rete#272 D3). #neverSeen is a commissioning failure — go plug it in. #silent is a
// liveness failure — go find out why it stopped. #unknown is porta declining to
// answer, and it has two causes, told apart by the row's `reason`: the node has
// declared no budget, or the window falls inside an interval porta was not
// listening. The store must not keep asserting the last thing it heard.
// #stalled is arriving but behind the cadence it declared — it is talking and
// not getting anywhere; go find what is blocking its tick (rete#272 D3).
var (
	livenessLive      = mustEncode(cval.Symbol("live"))
	livenessSilent    = mustEncode(cval.Symbol("silent"))
	livenessNeverSeen = mustEncode(cval.Symbol("neverSeen"))
	livenessUnknown   = mustEncode(cval.Symbol("unknown"))
	livenessStalled   = mustEncode(cval.Symbol("stalled"))
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
	Verdict  string // live | silent | neverSeen | unknown | stalled
	SinceMs  int64  // the age of the latest sample (before one: of the last exchange)
	BudgetMs int64  // telCad x misses, both from the NODE; 0 when it declared none
	TelCadMs int64  // the cadence the node declared; 0 when it declared none
	Misses   int64  // the consecutive misses it tolerates; 0 when it declared none
}

// Sweep forms one liveness verdict per node and writes gw/liveness ONLY where
// the verdict differs from the one already stored.
//
// THERE ARE TWO RULES AND NO CONSTANTS IN EITHER, and both read ONE sample:
// the count the node sends in every pull (#wak) and the moment porta heard it
// (rete#272 D3.3). Silence is the sample's age:
// now - wak_ms > bud, where bud = telCad x misses, both declared by the NODE at obs/liveness
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
// Inside the budget, the rate judges progress (judgeProgress): a node whose
// pulls arrive but whose count is a whole declared period behind is #stalled,
// because arrival alone called a wedged-but-talking node #live (rete#272 D3,
// model-checked before the Go was written). The count rides the PULL because
// the pull is built fresh every tick; the push frame is re-sent unchanged until
// acked (rete#199), so a count there would be stale exactly when it mattered
// (D3.3). An image whose pulls carry no count reads #unknown — there is no
// arrival-only fallback.
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
		SELECT n.id, n.last_seen_ms, n.wak, n.wak_ms, COALESCE(c.value_cbor, x'')
		FROM nodes n
		LEFT JOIN cells c ON c.node_id = n.id AND c.path = ?
		ORDER BY n.id`, store.PathLiveness)
	if err != nil {
		return nil, err
	}
	type row struct {
		id       string
		lastSeen sql.NullInt64
		wak      sql.NullInt64
		wakMs    sql.NullInt64
		stored   []byte
	}
	var nodes []row
	for rows.Next() {
		var r row
		if err := rows.Scan(&r.id, &r.lastSeen, &r.wak, &r.wakMs, &r.stored); err != nil {
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
		var latest *Sample
		if n.wak.Valid && n.wakMs.Valid {
			latest = &Sample{Wak: n.wak.Int64, AtMs: n.wakMs.Int64}
		}
		j, err := s.verdictFor(n.id, n.lastSeen, latest, n.stored, now)
		if err != nil {
			errs = append(errs, fmt.Errorf("node %s: %w", n.id, err))
			continue
		}
		if !bytes.Equal(n.stored, j.want) {
			if err := s.WriteGateway(n.id, store.PathLiveness, j.want, j.reason); err != nil {
				errs = append(errs, fmt.Errorf("node %s: write verdict: %w", n.id, err))
				continue // the mark judged this verdict; it must not move without it
			}
			changes = append(changes, LivenessChange{
				NodeID: n.id, Verdict: j.verdict, SinceMs: j.since,
				BudgetMs: j.d.BudgetMs(), TelCadMs: j.d.TelCadMs, Misses: j.d.Misses,
			})
		}
		switch {
		case j.forget:
			if err := s.ForgetMark(n.id); err != nil {
				errs = append(errs, fmt.Errorf("node %s: forget mark: %w", n.id, err))
			}
		case j.mark != nil:
			if err := s.SetMark(n.id, *j.mark); err != nil {
				errs = append(errs, fmt.Errorf("node %s: move mark: %w", n.id, err))
			}
		}
	}
	return changes, errors.Join(errs...)
}

// judgement is verdictFor's answer. mark is non-nil when the sweep must move
// the node's mark — written even when the verdict itself is unchanged.
type judgement struct {
	want    []byte
	verdict string
	since   int64
	d       Declaration
	reason  string
	mark    *Mark
	forget  bool // the sweep must drop the node's mark
}

// verdictFor is the whole decision, in the order the evidence allows.
//
// The ORDER is load-bearing. A sample inside its budget is positive evidence
// and outranks porta's own absence: it is judged on its rate (judgeProgress),
// never called #silent or #unknown for the hours porta was deaf before it.
// Absence is only ever evidence when porta was listening, which is why the
// deaf test sits between "past the budget" and "silent" rather than at the top.
// last_seen tells #neverSeen apart and dates the two head #unknowns; past them,
// nothing but the sample is read (rete#272 D3.3).
func (s *Store) verdictFor(nodeID string, lastSeen sql.NullInt64, latest *Sample, stored []byte, now int64) (judgement, error) {
	if !lastSeen.Valid {
		// D5: never heard from. Judgeable immediately and WITHOUT a budget,
		// because the question "has this node ever spoken" needs no number.
		return judgement{want: livenessNeverSeen, verdict: "neverSeen",
			reason: "neverSeen: no exchange has ever arrived from this node"}, nil
	}
	since := now - lastSeen.Int64
	d, ok, err := s.DeclaredLiveness(nodeID)
	if err != nil {
		return judgement{}, fmt.Errorf("declared liveness: %w", err)
	}
	if !ok {
		return judgement{want: livenessUnknown, verdict: "unknown", since: since,
			reason: fmt.Sprintf("unknown: %d ms since the last exchange, and this node has declared no cadence", since)}, nil
	}
	bud := d.BudgetMs()
	// rete#273 D2.1: porta refuses to promise what its sweep cannot deliver.
	// A budget below one tick would be detected at the tick, not at the
	// budget; saying so is worth more than quietly enforcing 30 s.
	if tick := SweepTick.Milliseconds(); bud < tick {
		return judgement{want: livenessUnknown, verdict: "unknown", since: since, d: d,
			reason: fmt.Sprintf("unknown: telCad %d ms x misses %d = %d ms is below the sweep tick %d ms; porta cannot judge at that resolution — raise misses or telCad", d.TelCadMs, d.Misses, bud, tick)}, nil
	}
	// rete#272 D3.3: ONE sample. Everything below reads the count the node sent
	// in its latest pull and the moment porta heard it — and nothing else.
	if latest == nil {
		return judgement{want: livenessUnknown, verdict: "unknown", since: since, d: d,
			reason: fmt.Sprintf("unknown: %d ms since its last exchange, and its pulls carry no tick count — this image predates rete#272; reflash it", since)}, nil
	}
	since = now - latest.AtMs
	// Inside the budget, arrival is settled and the question becomes progress
	// (rete#272 D3) — judgeProgress, below.
	if since <= bud {
		return s.judgeProgress(nodeID, *latest, stored, since, d)
	}
	// rete#273 D2.2: the miss count is a REASON, not a verdict — the ladder
	// an operator reads, never a state of its own.
	missed := float64(since) / float64(d.TelCadMs)
	// D6: absence is only evidence when porta was listening. A window that
	// touches an interval porta was down yields #unknown — never #silent,
	// because the gateway has no evidence in either direction, and never
	// #live, because it has none of that either. Note the position: this is
	// BELOW the `since <= bud` test, because speech is positive evidence and
	// outranks the gateway's own absence.
	deaf, err := s.WasDeaf(now-since, now)
	if err != nil {
		return judgement{}, fmt.Errorf("deaf intervals: %w", err)
	}
	// Past the budget the conversation the mark belonged to is over: forget
	// it, so a node that returns is judged from its first new sample, never
	// across the silence (rete#272 D3.3).
	if deaf {
		return judgement{want: livenessUnknown, verdict: "unknown", since: since, d: d, forget: true,
			reason: fmt.Sprintf("unknown: %d ms since the last check-in, budget %d ms, and porta was not listening for part of it", since, bud)}, nil
	}
	return judgement{want: livenessSilent, verdict: "silent", since: since, d: d, forget: true,
		reason: fmt.Sprintf("silent: %d ms since the last check-in — %.1f missed check-ins of %d ms, %d tolerated", since, missed, d.TelCadMs, d.Misses)}, nil
}

// judgeProgress is rete#272 D3, for a node already inside its budget: is it
// ticking at the rate it declared? The rule is the model-checked one
// (docs/spikes/2026-08-27-liveness-model, `Behind` and `DoSweep`, config
// sound-rate.cfg; with the sample on the pull, fresh-clear.cfg and
// fresh-lossy.cfg): between the mark and the latest sample, short by one whole
// declared period or more is #stalled. The sample is the pull's #wak and the
// moment porta heard it (rete#272 D3.3). The one period is the only slack, and
// it is not a tunable — it is the smallest that absorbs a real node's ms jitter.
//
// THE ORDER IS LOAD-BEARING, and two branches exist because TLC refuted the
// version without them: a sweep with no new sample keeps the verdict AND the
// mark (re-marking let a stalled node flip back to #live, unbounded), and a
// node with no mark is #unknown (one sample says nothing about a rate). A
// window that spans an interval porta was not listening is re-marked, not
// judged (D3.3).
func (s *Store) judgeProgress(nodeID string, latest Sample, stored []byte, since int64, d Declaration) (judgement, error) {
	j := judgement{since: since, d: d}
	keep := func(why string) (judgement, error) {
		if len(stored) == 0 {
			j.want, j.verdict, j.reason = livenessUnknown, "unknown", "unknown: "+why
			return j, nil
		}
		j.want, j.reason = stored, why
		return j, nil
	}
	mark, hasMark, err := s.MarkOf(nodeID)
	if err != nil {
		return judgement{}, fmt.Errorf("mark: %w", err)
	}
	next := &Mark{Sample: latest, CadMs: d.TelCadMs}
	if !hasMark {
		j.mark = next
		j.want, j.verdict = livenessUnknown, "unknown"
		j.reason = "unknown: one sample says nothing about a rate — judged from the next sweep"
		return j, nil
	}
	if mark.CadMs != d.TelCadMs {
		j.mark = next
		return keep(fmt.Sprintf("telCad moved %d -> %d ms: re-marked, not judged", mark.CadMs, d.TelCadMs))
	}
	if latest.AtMs == mark.AtMs {
		return keep("no new sample since the mark")
	}
	// porta's own absence is not evidence about the node: a 2 s node ticks at
	// 4 s while nothing answers it (#271), and a window that spans an interval
	// porta was not listening would blame the node for the gateway's outage.
	deaf, err := s.WasDeaf(mark.AtMs, latest.AtMs)
	if err != nil {
		return judgement{}, fmt.Errorf("deaf intervals: %w", err)
	}
	if deaf {
		j.mark = next
		return keep("porta was not listening for part of the window: re-marked, not judged")
	}
	j.mark = next
	elapsed := latest.AtMs - mark.AtMs
	ticks := latest.Wak - mark.Wak
	if short := elapsed - ticks*d.TelCadMs; short >= d.TelCadMs {
		j.want, j.verdict = livenessStalled, "stalled"
		j.reason = fmt.Sprintf("stalled: %d ticks in %d ms between samples, declared one per %d ms — %.1f periods behind",
			ticks, elapsed, d.TelCadMs, float64(short)/float64(d.TelCadMs))
		if ticks < 0 {
			j.reason += fmt.Sprintf("; wak went backwards (%d -> %d): the node restarted inside the window", mark.Wak, latest.Wak)
		}
		return j, nil
	}
	missed := float64(since) / float64(d.TelCadMs)
	j.want, j.verdict = livenessLive, "live"
	j.reason = fmt.Sprintf("live: %d ms since the last check-in, %.1f missed check-ins of %d ms, %d tolerated; %d ticks in %d ms",
		since, missed, d.TelCadMs, d.Misses, ticks, elapsed)
	return j, nil
}
