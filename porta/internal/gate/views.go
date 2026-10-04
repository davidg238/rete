// Copyright (c) 2026 Ekorau LLC

package gate

// views.go — design §4.1 and §4.3: what custos shows of the fleet RIGHT NOW.
//
// Two pieces, both small:
//
//   - `cells` gains four DERIVED columns, computed by the materializer from R:
//     `value` (the stored CBOR decoded through cval — a scalar path such as
//     goal/name or gw/liveness has no decoded table, so without this column it
//     is unanswerable on every binding), `stale` (porta get's isStale, §4.1),
//     and `actor`/`reason` (the provenance of the write that produced the
//     cell's current value — the only place porta says WHY a node is #silent,
//     and the only reader `--reason` has ever had, rete#280).
//   - two VIEWS on the `live` binding, `convergence` and `liveness`, which are
//     plain SQL over the materialized tables. A view is planned like any other
//     statement: the authorizer reports the base columns underneath it, so the
//     materializer fills only those (TestViewsPlanThroughTheirTables).
//
// `liveness` surfaces porta's OWN verdict and its reason. It does not re-derive
// one: §4.3's `staleness` (why = never/refused/wedged via obs_health.ref) was
// written before rete#231/#272 gave porta a verdict with its cause in the row's
// reason, and two judges of one question is the drift this project keeps
// paying for (design addendum D1, 2026-10-04).

import (
	"context"
	"database/sql"
	"fmt"

	"github.com/davidg238/porta/internal/cval"
	"github.com/davidg238/porta/internal/space"
)

// cellDerivedColumns follow the store's own cells columns.
var cellDerivedColumns = []column{
	{"value", ""}, // no affinity: the value decides
	{"stale", "INTEGER"},
	{"actor", "TEXT"},
	{"reason", "TEXT"},
}

// viewDef is one view: its name and its body. Bodies name only logical tables
// and their columns, never query text — they are authored here, like the
// descriptor table, and W executes them.
type viewDef struct {
	name string
	sql  string
}

// convergence is the goal/obs tree diff (ARCHITECTURE §3): one row per goal/X
// porta holds for a node, beside the node's obs/X. Deterministic CBOR makes
// equality of the encoded bytes equality of the values, so judging needs no
// decode — `goal` and `obs` are decoded only to be shown.
var viewConvergence = viewDef{"convergence", `
SELECT g.node_id                                         AS node_id,
       substr(g.path, 6)                                 AS key,
       g.value                                           AS goal,
       o.value                                           AS obs,
       (o.value_cbor IS NOT NULL AND o.value_cbor = g.value_cbor) AS converged,
       g.ts_ms                                           AS goal_ms,
       o.ts_ms                                           AS obs_ms,
       o.stale                                           AS obs_stale
  FROM cells g
  LEFT JOIN cells o
    ON o.node_id = g.node_id AND o.direction = 'in' AND o.path = 'obs/' || substr(g.path, 6)
 WHERE g.direction = 'out' AND g.path GLOB 'goal/*'`}

// liveness is one row per node porta knows, with porta's verdict about it. A
// node porta has not judged yet reads NULL — never a guessed #live. `role` is
// goal/name, optional by design (first-cut §5.2): NULL is an uncommissioned
// device, which is a legal state.
//
// `board` is the bench name the node asserts in obs/build, and `retired_into`
// folds an id a board has left into the board that succeeded it (addendum D4):
// the device whose obs/build names this id as its board AND was first heard
// at or after this id was last heard. That second clause is §5.3's invariant —
// non-overlapping means one board succeeding itself; overlapping means two
// boards, and nothing is folded.
var viewLiveness = viewDef{"liveness", fmt.Sprintf(`
SELECT n.id           AS node_id,
       r.value        AS role,
       v.value        AS verdict,
       v.reason       AS reason,
       v.ts_ms        AS verdict_ms,
       n.last_seen_ms AS last_seen_ms,
       n.boot_id      AS boot_id,
       n.last_addr    AS last_addr,
       b.board        AS board,
       (SELECT s.node_id FROM %[3]s s JOIN nodes sn ON sn.id = s.node_id
         WHERE s.board = n.id AND s.node_id <> n.id AND sn.first_seen_ms >= n.last_seen_ms
         ORDER BY sn.first_seen_ms LIMIT 1) AS retired_into
  FROM nodes n
  LEFT JOIN cells r ON r.node_id = n.id AND r.direction = 'out' AND r.path = 'goal/name'
  LEFT JOIN cells v ON v.node_id = n.id AND v.direction = '%[1]s' AND v.path = '%[2]s'
  LEFT JOIN %[3]s b ON b.node_id = n.id`,
	space.DirectionGateway, space.PathLiveness, TableName("obs/build"))}

// createViews builds the binding's views on W, after its tables.
func createViews(ctx context.Context, s *scratch, views []viewDef) error {
	for _, v := range views {
		if _, err := s.w.ExecContext(ctx, "CREATE VIEW "+quoteIdent(v.name)+" AS "+v.sql); err != nil {
			return fmt.Errorf("gate: creating view %s: %w", v.name, err)
		}
	}
	return nil
}

// cellDeriver computes the derived cells columns for one materialization. It
// reads R directly — including `events` on `live`, where Q is never offered
// that table — through two bounded reads: the nodes table once, and one indexed
// lookup per cell.
type cellDeriver struct {
	td      *tableDef
	boot    map[string]int64 // node -> current boot, porta get's CurrentBoot
	prov    *sql.Stmt
	nodeIx  int
	pathIx  int
	seqIx   int
	valueIx int
	dirIx   int
	bootIx  int
}

func (g *Gate) newCellDeriver(ctx context.Context, td *tableDef, base []column) (*cellDeriver, error) {
	d := &cellDeriver{td: td, boot: map[string]int64{}}
	idx := map[string]*int{
		"node_id": &d.nodeIx, "path": &d.pathIx, "seq": &d.seqIx,
		"value_cbor": &d.valueIx, "direction": &d.dirIx, "boot_id": &d.bootIx,
	}
	found := map[string]bool{}
	for i, c := range base {
		if p, ok := idx[c.name]; ok {
			*p = i
			found[c.name] = true
		}
	}
	for name := range idx {
		if !found[name] {
			return nil, fmt.Errorf("gate: cells in %s has no %s column — the derived columns cannot be computed", g.source, name)
		}
	}

	rows, err := g.r.QueryContext(ctx, `SELECT id, COALESCE(boot_id, 0) FROM nodes`)
	if err != nil {
		return nil, fmt.Errorf("gate: reading node boots: %w", err)
	}
	defer rows.Close()
	for rows.Next() {
		var id string
		var b int64
		if err := rows.Scan(&id, &b); err != nil {
			return nil, err
		}
		d.boot[id] = b
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}

	// The write that produced a cell's current value is the latest event with
	// the same node, direction, seq and path. (node_id, direction, seq) is
	// idx_events_out, so this is an index range, not a scan of the node's log;
	// path and the id order settle a seq a node reused after a reboot.
	d.prov, err = g.r.PrepareContext(ctx, `SELECT actor, reason FROM events
		WHERE node_id = ? AND direction = ? AND seq = ? AND path = ?
		ORDER BY id DESC LIMIT 1`)
	if err != nil {
		return nil, fmt.Errorf("gate: preparing the provenance lookup: %w", err)
	}
	return d, nil
}

func (d *cellDeriver) Close() { d.prov.Close() }

// derive answers the derived columns for one base row, in cellDerivedColumns
// order.
func (d *cellDeriver) derive(ctx context.Context, base []any) ([]any, error) {
	nodeID, _ := base[d.nodeIx].(string)
	path, _ := base[d.pathIx].(string)
	dir, _ := base[d.dirIx].(string)
	seq, _ := base[d.seqIx].(int64)
	boot, _ := base[d.bootIx].(int64)
	blob, _ := base[d.valueIx].([]byte)

	v, err := cval.Decode(blob)
	if err != nil {
		// Fatal, as for a decoded table: an undecodable stored value is a real
		// fault, and a NULL here would read exactly like an absent one.
		return nil, fmt.Errorf("gate: cells: decoding %s seq %d at %s: %w", nodeID, seq, path, err)
	}
	value, err := sqlValue(v, d.td, "value", nodeID, seq)
	if err != nil {
		return nil, err
	}

	stale := space.IsStale(dir, boot, d.boot[nodeID])

	var actor, reason sql.NullString
	err = d.prov.QueryRowContext(ctx, nodeID, dir, seq, path).Scan(&actor, &reason)
	if err != nil && err != sql.ErrNoRows {
		return nil, fmt.Errorf("gate: cells: provenance of %s %s: %w", nodeID, path, err)
	}
	return []any{value, stale, nullable(actor), nullable(reason)}, nil
}

func nullable(s sql.NullString) any {
	if !s.Valid {
		return nil
	}
	return s.String
}
