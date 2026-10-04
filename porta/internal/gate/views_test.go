// Copyright (c) 2026 Ekorau LLC

package gate

import (
	"context"
	"path/filepath"
	"testing"

	"github.com/davidg238/porta/internal/cval"
	"github.com/davidg238/porta/internal/space"
)

// fleetStore is a small fleet built through porta's own writer: n1 is
// commissioned and converged, then reboots and republishes only part of its
// tree; n2 has a goal it has not echoed and a gw verdict with a reason.
func fleetStore(t *testing.T) (*space.Store, string) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "store.db")
	var clock int64 = 1_700_000_000_000
	st, err := space.Open(path, func() int64 { clock += 1_000; return clock })
	if err != nil {
		t.Fatalf("space.Open: %v", err)
	}
	t.Cleanup(func() { st.Close() })

	enc := func(v any) []byte {
		b, err := cval.Encode(v)
		if err != nil {
			t.Fatal(err)
		}
		return b
	}
	if _, err := st.Write("n1", "goal/name", cval.KindCell, enc("bathroom-1"), "david", "commission"); err != nil {
		t.Fatal(err)
	}
	if _, _, err := st.Ingest("n1", 41, []space.Entry{
		{Seq: 1, Path: "obs/name", Kind: cval.KindCell, Value: enc("bathroom-1")},
		{Seq: 2, Path: "obs/mode", Kind: cval.KindCell, Value: enc(cval.Symbol("run"))},
	}); err != nil {
		t.Fatal(err)
	}
	// n1 reboots into boot 42 and has so far republished only obs/name, so
	// obs/mode is the rete#210 mixed-read row.
	if _, _, err := st.Ingest("n1", 42, []space.Entry{
		{Seq: 1, Path: "obs/name", Kind: cval.KindCell, Value: enc("bathroom-1")},
	}); err != nil {
		t.Fatal(err)
	}

	if _, err := st.Write("n2", "goal/name", cval.KindCell, enc("porch"), "david", "commission"); err != nil {
		t.Fatal(err)
	}
	if _, _, err := st.Ingest("n2", 7, []space.Entry{
		{Seq: 1, Path: "obs/name", Kind: cval.KindCell, Value: enc("n2")},
	}); err != nil {
		t.Fatal(err)
	}
	if err := st.WriteGateway("n2", space.PathLiveness, enc(cval.Symbol("silent")),
		"silent: 90000 ms since the last check-in"); err != nil {
		t.Fatal(err)
	}
	return st, path
}

func liveFleet(t *testing.T) *Gate {
	t.Helper()
	_, path := fleetStore(t)
	g, err := OpenLive(path, descriptors(t))
	if err != nil {
		t.Fatalf("OpenLive: %v", err)
	}
	t.Cleanup(func() { g.Close() })
	return g
}

// rowsBy keys a result by the concatenation of its first n columns.
func rowsBy(t *testing.T, res *Result, n int) map[string]map[string]any {
	t.Helper()
	out := map[string]map[string]any{}
	for _, r := range res.Rows {
		key := ""
		for i := 0; i < n; i++ {
			if i > 0 {
				key += " "
			}
			key += asText(r[i])
		}
		m := map[string]any{}
		for i, c := range res.Columns {
			m[c] = r[i]
		}
		out[key] = m
	}
	return out
}

func asText(v any) string {
	switch t := v.(type) {
	case nil:
		return "<nil>"
	case string:
		return t
	case []byte:
		return string(t)
	}
	return cval.Format(v)
}

func asInt(t *testing.T, v any) int64 {
	t.Helper()
	switch n := v.(type) {
	case int64:
		return n
	case bool:
		if n {
			return 1
		}
		return 0
	}
	t.Fatalf("not an integer: %#v", v)
	return 0
}

// TestCellsCarryTheDecodedValue: a scalar path has no decoded table, so the cell
// row itself must carry its value readably, or goal/name and gw/liveness are
// unanswerable on every binding.
func TestCellsCarryTheDecodedValue(t *testing.T) {
	g := liveFleet(t)
	res := query(t, g, Query{SQL: `SELECT node_id, path, value FROM cells`})
	got := rowsBy(t, res, 2)
	for key, want := range map[string]string{
		"n1 goal/name":   "bathroom-1",
		"n1 obs/mode":    "run",
		"n2 gw/liveness": "silent",
	} {
		row, ok := got[key]
		if !ok {
			t.Fatalf("no row %q in %v", key, res.Rows)
		}
		if asText(row["value"]) != want {
			t.Errorf("%s value = %#v, want %q", key, row["value"], want)
		}
	}
}

// TestCellsStaleIsPortaGetsRule: the stale column is porta get's isStale and no
// wider — only an inbound row from a known boot other than the node's current
// one. An 'out' row never is, whatever its boot.
func TestCellsStaleIsPortaGetsRule(t *testing.T) {
	g := liveFleet(t)
	got := rowsBy(t, query(t, g, Query{SQL: `SELECT node_id, path, stale FROM cells`}), 2)
	for key, want := range map[string]int64{
		"n1 obs/mode":  1, // boot 41, node now on 42
		"n1 obs/name":  0, // republished in 42
		"n1 goal/name": 0, // porta's intent
		"n2 obs/name":  0,
	} {
		if v := asInt(t, got[key]["stale"]); v != want {
			t.Errorf("%s stale = %d, want %d", key, v, want)
		}
	}
}

// TestCellsCarryTheirProvenance: the actor and reason recorded with the write
// that produced each cell's current value. gw/liveness's reason is the only
// place porta says WHY a node is #silent or #stalled, and `--reason` is
// otherwise printed by nothing (rete#280).
func TestCellsCarryTheirProvenance(t *testing.T) {
	g := liveFleet(t)
	got := rowsBy(t, query(t, g, Query{SQL: `SELECT node_id, path, actor, reason FROM cells`}), 2)
	if r := got["n1 goal/name"]; asText(r["actor"]) != "david" || asText(r["reason"]) != "commission" {
		t.Errorf("goal/name provenance = %v / %v", r["actor"], r["reason"])
	}
	if r := got["n2 gw/liveness"]; asText(r["actor"]) != "porta" ||
		asText(r["reason"]) != "silent: 90000 ms since the last check-in" {
		t.Errorf("gw/liveness provenance = %v / %v", r["actor"], r["reason"])
	}
}

// TestConvergenceIsTheGoalObsDiff: one row per goal/X, beside obs/X, converged
// only when the two values are equal. Deterministic CBOR makes that byte
// equality, so no decode is needed to judge it — only to show it.
func TestConvergenceIsTheGoalObsDiff(t *testing.T) {
	g := liveFleet(t)
	res := query(t, g, Query{SQL: `SELECT node_id, key, goal, obs, converged FROM convergence`})
	got := rowsBy(t, res, 2)
	if len(got) != 2 {
		t.Fatalf("want one row per goal path (2), got %v", res.Rows)
	}
	if r := got["n1 name"]; asInt(t, r["converged"]) != 1 || asText(r["obs"]) != "bathroom-1" {
		t.Errorf("n1 name = %v", r)
	}
	if r := got["n2 name"]; asInt(t, r["converged"]) != 0 || asText(r["goal"]) != "porch" || asText(r["obs"]) != "n2" {
		t.Errorf("n2 name = %v", r)
	}
}

// TestLivenessIsPortasVerdict: one row per node, carrying porta's own verdict
// and its reason rather than re-deriving one. A node porta has not judged yet
// reads NULL, never a guessed #live.
func TestLivenessIsPortasVerdict(t *testing.T) {
	g := liveFleet(t)
	res := query(t, g, Query{SQL: `SELECT node_id, role, verdict, reason, boot_id, last_seen_ms FROM liveness`})
	got := rowsBy(t, res, 1)
	if len(got) != 2 {
		t.Fatalf("want one row per node, got %v", res.Rows)
	}
	n1, n2 := got["n1"], got["n2"]
	if asText(n1["role"]) != "bathroom-1" || n1["verdict"] != nil || asInt(t, n1["boot_id"]) != 42 {
		t.Errorf("n1 = %v", n1)
	}
	if asText(n2["role"]) != "porch" || asText(n2["verdict"]) != "silent" ||
		asText(n2["reason"]) != "silent: 90000 ms since the last check-in" {
		t.Errorf("n2 = %v", n2)
	}
	if n2["last_seen_ms"] == nil {
		t.Errorf("n2 last_seen_ms is NULL")
	}
}

// TestViewsPlanThroughTheirTables: a view is planned like any statement — the
// authorizer reports the base tables underneath it, and only those are
// materialized.
func TestViewsPlanThroughTheirTables(t *testing.T) {
	g := liveFleet(t)
	res := query(t, g, Query{SQL: `SELECT * FROM convergence`})
	if len(res.Sources) != 1 || res.Sources[0].Table != "cells" {
		t.Errorf("convergence materialized %v, want cells alone", res.Sources)
	}
}

// TestSnapOffersNoFleetView: a verdict or a convergence read from a snapshot is
// yesterday's answer to "right now" — rete#225 reproduced by construction — so
// the snapshot binding does not offer them at all.
func TestSnapOffersNoFleetView(t *testing.T) {
	st, _ := fleetStore(t)
	g, err := Open(snapshotOf(t, st), descriptors(t))
	if err != nil {
		t.Fatal(err)
	}
	defer g.Close()
	names, err := g.Tables()
	if err != nil {
		t.Fatal(err)
	}
	for _, v := range []string{"liveness", "convergence"} {
		if contains(names, v) {
			t.Errorf("snap offers %q", v)
		}
	}
	if _, err := g.Query(context.Background(), Query{SQL: `SELECT * FROM liveness`}); err == nil {
		t.Errorf("snap answered a liveness query")
	}
	// cells still carries its derived columns on snap: they are per-row facts
	// of the snapshot, not claims about now.
	query(t, g, Query{SQL: `SELECT value, stale, actor, reason FROM cells`})
}

// TestQueryBindsArgs: a statement's parameters are bound, never spliced, so a
// named query can take a node id from a URL without that id becoming SQL.
func TestQueryBindsArgs(t *testing.T) {
	g := liveFleet(t)
	res := query(t, g, Query{SQL: `SELECT DISTINCT node_id FROM cells WHERE node_id = ?`, Args: []any{"n2"}})
	if len(res.Rows) != 1 || res.Rows[0][0] != "n2" {
		t.Errorf("rows = %v", res.Rows)
	}
	res = query(t, g, Query{SQL: `SELECT node_id FROM cells WHERE node_id = ?`, Args: []any{"n2' OR '1'='1"}})
	if len(res.Rows) != 0 {
		t.Errorf("a quoted id matched %d rows", len(res.Rows))
	}
}
