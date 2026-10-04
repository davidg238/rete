// Copyright (c) 2026 Ekorau LLC

package gate

// acceptance_test.go — design §10: the five questions, each answered by a
// committed query (testdata/acceptance/q*.sql) against a committed fixture
// snapshot (testdata/acceptance.db). Task 8 runs the same query files against
// the real store, so the fixture answer and the recorded answer are answers to
// the same text.
//
// §10's prediction is the test expectation: (2) and (5) answerable as they
// stand on `live`; (1) and (3) only through value projection AND the reading
// rules — each test also shows the naive aggregate going wrong on the same
// data; (4) only through build segmentation. If one of these fails, the design
// is wrong, not merely incomplete.
//
// Regenerate the fixture after a deliberate change to acceptanceStore:
//
//	go test ./internal/gate -run TestAcceptanceFixtureIsCurrent -update

import (
	"context"
	"database/sql"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"

	"github.com/davidg238/porta/internal/cval"
	"github.com/davidg238/porta/internal/space"
)

var update = flag.Bool("update", false, "rewrite testdata/acceptance.db from acceptanceStore")

const (
	acceptanceDB = "testdata/acceptance.db"
	hourMs       = int64(3_600_000)
	dayMs        = 24 * hourMs
	// t0 is the leak build's first boot; everything is placed relative to it.
	t0 = int64(1_780_000_000_000)
)

// acceptanceStore builds the §10 fleet through porta's own writer.
//
//	leaky  the rete#190 leak+fix shape, one node across four boots:
//	       boot 1  build v0, 45 days before t0 — outside "the last month"
//	       boot 2  build v1, 10 h, the envelope falls 500 B a cycle: the leak
//	       boot 3  build v2, 10 h, flat: the fix
//	       boot 4  build v2, falling over 2 h of envelope (6 h less warm-up) —
//	               too short to fit, and must be refused
//	       Every cycle has a pre-collection sample (lfb = the failing request,
//	       96 B) and a post-sweep one, and every boot opens on five warm-up
//	       cycles of an unswept arena — the two traps lfb's validity rule names.
//	porch  goal/mode run, obs/mode idle: disagrees with its goal; #stalled
//	quiet  heard, then #silent
//	ghost  commissioned, never heard: #neverSeen, and no obs at all
//	twin   boots 5 and 7 interleaved: two boards under one id (§6), unjudged
func acceptanceStore(t *testing.T, path string) {
	t.Helper()
	d := descriptors(t)
	now := t0
	st, err := space.Open(path, func() int64 { now++; return now })
	if err != nil {
		t.Fatal(err)
	}
	defer st.Close()

	enc := func(v any) []byte {
		t.Helper()
		b, err := cval.Encode(v)
		if err != nil {
			t.Fatal(err)
		}
		return b
	}
	seq := map[string]int64{}
	put := func(node string, boot int64, path string, v []byte) {
		t.Helper()
		seq[node]++
		if _, _, err := st.Ingest(node, boot, []space.Entry{{Seq: seq[node], Path: path, Kind: cval.KindCell, Value: v}}); err != nil {
			t.Fatal(err)
		}
	}
	ex := func(node string, boot int64) {
		t.Helper()
		if err := st.RecordExchange(node, boot, 0, 0, 0, 0, "100.64.0.9:7"); err != nil {
			t.Fatal(err)
		}
	}
	metrics := func(lfb, lb, gcc int64) []byte {
		return metricsRecord(t, d, map[string]any{"lfb": lfb, "lb": lb, "gcc": gcc, "hsz": int64(131072)})
	}
	build := func(name string, man byte) []byte {
		return enc(cval.Record{Tag: "Build", Fields: []cval.Pair{
			{K: cval.Symbol("man"), V: cval.Bytes{man, 0, 0, 0, 0, 0, 0, man}},
			{K: cval.Symbol("chip"), V: cval.Symbol("esp32c6")},
			{K: cval.Symbol("board"), V: "c6bench"},
			{K: cval.Symbol("build"), V: name},
		}})
	}
	// soak runs one boot of `leaky`: cycles GC cycles spread evenly over
	// spanMs, the post-sweep lfb of cycle gcc given by envelope.
	soak := func(boot, start, spanMs, cycles int64, b []byte, envelope func(gcc int64) int64) {
		t.Helper()
		now = start
		ex("leaky", boot)
		put("leaky", boot, "obs/build", b)
		for gcc := int64(1); gcc <= cycles; gcc++ {
			now = start + spanMs*gcc/cycles
			lfb := int64(100_000) // an arena that has never been swept
			if gcc > gcWarmupCycles {
				lfb = envelope(gcc)
			}
			put("leaky", boot, "sys/metrics", metrics(96, 130_000, gcc)) // the failing request
			put("leaky", boot, "sys/metrics", metrics(lfb, 131_072-lfb, gcc))
		}
		ex("leaky", boot)
	}

	soak(1, t0-45*dayMs, 6*hourMs, 24, build("v0", 0xa0), func(int64) int64 { return 30_000 })
	soak(2, t0, 10*hourMs, 40, build("v1", 0xa1), func(gcc int64) int64 { return 60_000 - 500*(gcc-6) })
	soak(3, t0+dayMs, 10*hourMs, 40, build("v2", 0xa2), func(gcc int64) int64 { return 60_000 + 100*(gcc%2) })
	soak(4, t0+2*dayMs, 6*hourMs, 9, build("v2", 0xa2), func(gcc int64) int64 { return 62_000 - 1_000*(gcc-6) })

	now = t0 + 2*dayMs + 3*hourMs
	if _, err := st.Write("leaky", "goal/name", cval.KindCell, enc("kitchen"), "david", "commission"); err != nil {
		t.Fatal(err)
	}
	put("leaky", 4, "obs/name", enc("kitchen"))
	ex("leaky", 4)

	if _, err := st.Write("porch", "goal/mode", cval.KindCell, enc(cval.Symbol("run")), "david", "start the bridge"); err != nil {
		t.Fatal(err)
	}
	ex("porch", 3)
	put("porch", 3, "obs/mode", enc(cval.Symbol("idle")))
	ex("quiet", 9)
	if _, err := st.Write("ghost", "goal/name", cval.KindCell, enc("attic"), "david", "commission"); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 3; i++ {
		ex("twin", 5)
		ex("twin", 7)
	}

	now += hourMs
	for _, v := range []struct{ node, verdict, reason string }{
		{"leaky", "live", "live: 4000 ms since the last check-in, 0.1 missed check-ins of 30000 ms, 3 tolerated; 12 ticks in 30000 ms"},
		{"porch", "stalled", "stalled: 2 ticks in 30000 ms between samples, declared one per 1000 ms — 28.0 periods behind"},
		{"quiet", "silent", "silent: 3600000 ms since the last check-in — 120.0 missed check-ins of 30000 ms, 3 tolerated"},
		{"ghost", "neverSeen", "neverSeen: no exchange has ever arrived from this node"},
	} {
		if err := st.WriteGateway(v.node, space.PathLiveness, enc(cval.Symbol(v.verdict)), v.reason); err != nil {
			t.Fatal(err)
		}
	}
}

// buildAcceptanceSnapshot builds the fleet and snapshots it to dest, as
// `porta snapshot --out` would.
func buildAcceptanceSnapshot(t *testing.T, dest string) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "store.db")
	acceptanceStore(t, path)
	st, err := space.Open(path, func() int64 { return t0 })
	if err != nil {
		t.Fatal(err)
	}
	defer st.Close()
	if err := st.SnapshotInto(dest); err != nil {
		t.Fatal(err)
	}
}

// dumpDB is a database's content as sorted lines, so two snapshots compare by
// what they hold rather than by page layout.
func dumpDB(t *testing.T, path string) []string {
	t.Helper()
	db, err := sql.Open("sqlite3", "file:"+path+"?mode=ro&immutable=1")
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	names, err := db.Query(`SELECT name, sql FROM sqlite_master WHERE sql IS NOT NULL ORDER BY name`)
	if err != nil {
		t.Fatal(err)
	}
	var out, tables []string
	for names.Next() {
		var name, ddl string
		if err := names.Scan(&name, &ddl); err != nil {
			t.Fatal(err)
		}
		out = append(out, "schema "+ddl)
		if strings.HasPrefix(ddl, "CREATE TABLE") {
			tables = append(tables, name)
		}
	}
	names.Close()
	for _, tbl := range tables {
		rows, err := db.Query(`SELECT * FROM ` + quoteIdent(tbl))
		if err != nil {
			t.Fatal(err)
		}
		cols, _ := rows.Columns()
		for rows.Next() {
			vals := make([]any, len(cols))
			ptrs := make([]any, len(cols))
			for i := range vals {
				ptrs[i] = &vals[i]
			}
			if err := rows.Scan(ptrs...); err != nil {
				t.Fatal(err)
			}
			out = append(out, fmt.Sprintf("%s %v", tbl, vals))
		}
		rows.Close()
	}
	sort.Strings(out)
	return out
}

// TestAcceptanceFixtureIsCurrent: the committed snapshot is what
// acceptanceStore builds today. A fixture nobody can regenerate is a fixture
// nobody can trust; one that has drifted from its generator answers questions
// about a fleet nobody can read.
func TestAcceptanceFixtureIsCurrent(t *testing.T) {
	fresh := filepath.Join(t.TempDir(), "acceptance.db")
	buildAcceptanceSnapshot(t, fresh)
	if *update {
		os.Remove(acceptanceDB)
		buildAcceptanceSnapshot(t, acceptanceDB)
	}
	want, got := dumpDB(t, fresh), dumpDB(t, acceptanceDB)
	if strings.Join(want, "\n") != strings.Join(got, "\n") {
		t.Fatalf("%s has drifted from acceptanceStore (%d lines vs %d); regenerate with -update",
			acceptanceDB, len(got), len(want))
	}
}

// acceptanceGate opens the committed fixture on one binding. `live` opens a
// copy: the binding is porta's store, and the committed file is not that.
func acceptanceGate(t *testing.T, b Binding) *Gate {
	t.Helper()
	var g *Gate
	var err error
	if b == BindingSnap {
		g, err = Open(acceptanceDB, descriptors(t))
	} else {
		raw, rerr := os.ReadFile(acceptanceDB)
		if rerr != nil {
			t.Fatal(rerr)
		}
		live := filepath.Join(t.TempDir(), "store.db")
		if werr := os.WriteFile(live, raw, 0o600); werr != nil {
			t.Fatal(werr)
		}
		g, err = OpenLive(live, descriptors(t))
	}
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { g.Close() })
	return g
}

func acceptanceQuery(t *testing.T, g *Gate, file string) *Result {
	t.Helper()
	q, err := os.ReadFile(filepath.Join("testdata", "acceptance", file))
	if err != nil {
		t.Fatal(err)
	}
	res, err := g.Query(context.Background(), Query{SQL: string(q)})
	if err != nil {
		t.Fatalf("%s: %v", file, err)
	}
	return res
}

// note finds the reading rule that rode out for one column of one table.
func note(res *Result, table, column string) *Note {
	for i := range res.Notes {
		if res.Notes[i].Table == table && res.Notes[i].Column == column {
			return &res.Notes[i]
		}
	}
	return nil
}

func asFloat(t *testing.T, v any) float64 {
	t.Helper()
	switch n := v.(type) {
	case int64:
		return float64(n)
	case float64:
		return n
	}
	t.Fatalf("not a number: %#v", v)
	return 0
}

// TestQ1MedianFreeMemory: the gc_envelope gauge, carrying lfb's validity note.
// The fixture's last month is boots 2–4 (boot 1 is 45 days older). Its
// envelope median is 60,000 B; the raw median of the same month's lfb is under
// half that, because half of all samples are the failing request's 96 B.
func TestQ1MedianFreeMemory(t *testing.T) {
	g := acceptanceGate(t, BindingSnap)
	res := acceptanceQuery(t, g, "q1-median-free-memory.sql")
	got := rowsBy(t, res, 1)
	if len(got) != 1 {
		t.Fatalf("want one node with GC history, got %v", res.Rows)
	}
	if r := got["leaky"]; asFloat(t, r["median_lfb"]) != 60_000 || asInt(t, r["cycles"]) != 35+35+4 {
		t.Errorf("leaky = %v, want median 60000 over 74 envelope cycles", r)
	}
	n := note(res, "sys_metrics", "lfb")
	if n == nil || !strings.Contains(n.Validity, "INVALID as a point sample") {
		t.Errorf("lfb's validity note did not ride out with the answer: %+v", res.Notes)
	}

	// The naive aggregate, on the same month: what the note exists to prevent.
	naive := query(t, g, Query{SQL: `
WITH w AS (SELECT lfb FROM sys_metrics WHERE node_id = 'leaky' AND ts_ms >= ?),
     r AS (SELECT lfb, ROW_NUMBER() OVER (ORDER BY lfb) AS i, COUNT(*) OVER () AS n FROM w)
SELECT AVG(lfb) FROM r WHERE i IN ((n + 1) / 2, (n + 2) / 2)`, Args: []any{t0 - 30*dayMs}})
	if m := asFloat(t, naive.Rows[0][0]); m >= 30_000 {
		t.Errorf("the naive median is %v; the fixture no longer holds the trap", m)
	}
}

// TestQ2NodesDisagreeingWithGoal: answerable on `live` as it stands —
// convergence names porch's mode and ghost's never-echoed name, not leaky.
func TestQ2NodesDisagreeingWithGoal(t *testing.T) {
	g := acceptanceGate(t, BindingLive)
	res := acceptanceQuery(t, g, "q2-disagree-with-goal.sql")
	got := rowsBy(t, res, 2)
	if len(got) != 2 {
		t.Fatalf("want porch and ghost, got %v", res.Rows)
	}
	if r := got["porch mode"]; asText(r["goal"]) != "run" || asText(r["obs"]) != "idle" {
		t.Errorf("porch mode = %v", r)
	}
	if r := got["ghost name"]; asText(r["goal"]) != "attic" || r["obs"] != nil {
		t.Errorf("ghost name = %v", r)
	}
}

// TestQ3ArenaGrowth: segmented by boot and build, the leak (v1) falls, the fix
// (v2) holds, and boot 4 — 2 h of envelope — gets no slope at all. Unsegmented, the same
// envelope reads as a node whose free memory ROSE — rete#190's fix hiding its
// own leak — which is why lfb's `segment` names boot and build.
func TestQ3ArenaGrowth(t *testing.T) {
	g := acceptanceGate(t, BindingSnap)
	res := acceptanceQuery(t, g, "q3-arena-growth.sql")
	got := rowsBy(t, res, 2)
	if len(got) != 4 {
		t.Fatalf("want leaky's four boots, got %v", res.Rows)
	}
	if r := got["leaky 2"]; asText(r["build"]) != "v1" || asFloat(t, r["lfb_slope_per_h"]) > -1_500 {
		t.Errorf("leaky 2 (the leak) = %v, want a slope near -2000 B/h", r)
	}
	if r := got["leaky 3"]; asText(r["build"]) != "v2" || abs(asFloat(t, r["lfb_slope_per_h"])) > 50 {
		t.Errorf("leaky 3 (the fix) = %v, want flat", r)
	}
	if r := got["leaky 4"]; r["lfb_slope_per_h"] != nil || r["refusal"] == nil {
		t.Errorf("leaky 4 (2 h of envelope) = %v, want a refusal and no slope", r)
	}
	n := note(res, "sys_metrics", "lfb")
	if n == nil || !strings.Contains(n.Validity, "shorter than ~5 h") {
		t.Errorf("the ~5 h rule did not ride out with the answer: %+v", res.Notes)
	}
	if n == nil || !contains(n.Segment, "boot") || !contains(n.Segment, "obs/build") {
		t.Errorf("lfb's segment does not name boot and build: %+v", n)
	}

	// Unsegmented over the last month: one slope across the leak and the fix.
	naive := query(t, g, Query{SQL: `
WITH c AS (SELECT lfb_max AS y, (last_ms - ?) / 3600000.0 AS x FROM gc_envelope WHERE last_ms >= ?)
SELECT (COUNT(*) * SUM(x * y) - SUM(x) * SUM(y)) / (COUNT(*) * SUM(x * x) - SUM(x) * SUM(x)) FROM c`,
		Args: []any{t0, t0 - 30*dayMs}})
	if s := asFloat(t, naive.Rows[0][0]); s <= 0 {
		t.Errorf("the unsegmented slope is %v; the fixture no longer holds the trap", s)
	}
}

func abs(f float64) float64 {
	if f < 0 {
		return -f
	}
	return f
}

// TestQ4WhatChangedBetweenBuilds: boot × obs_build puts each build's boots,
// manifest pin and envelope floor side by side, in the order the node ran
// them.
func TestQ4WhatChangedBetweenBuilds(t *testing.T) {
	g := acceptanceGate(t, BindingSnap)
	res := acceptanceQuery(t, g, "q4-build-diff.sql")
	if len(res.Rows) != 3 {
		t.Fatalf("want v0, v1, v2, got %v", res.Rows)
	}
	got := rowsBy(t, res, 2)
	v1, v2 := got["leaky v1"], got["leaky v2"]
	if asInt(t, v1["boots"]) != 1 || asInt(t, v2["boots"]) != 2 ||
		asInt(t, v2["first_boot"]) != 3 || asInt(t, v2["last_boot"]) != 4 {
		t.Errorf("boots per build: v1 = %v, v2 = %v", v1, v2)
	}
	if asText(v1["man"]) != "A1000000000000A1" || asText(v2["man"]) != "A2000000000000A2" {
		t.Errorf("manifest pins: v1 %v, v2 %v", v1["man"], v2["man"])
	}
	if asInt(t, v1["envelope_floor"]) != 43_000 || asInt(t, v2["envelope_floor"]) != 59_000 {
		t.Errorf("envelope floors: v1 %v, v2 %v", v1["envelope_floor"], v2["envelope_floor"])
	}
	if asText(res.Rows[0][1]) != "v0" || asText(res.Rows[2][1]) != "v2" {
		t.Errorf("builds are not in the order the node ran them: %v", res.Rows)
	}
}

// TestQ5StaleAndWhy: answerable on `live` as it stands, with porta's verdict
// in place of §4.3's re-derived `why` (addendum D1). The three operator actions
// are three different verdicts; an unjudged node reads NULL.
func TestQ5StaleAndWhy(t *testing.T) {
	g := acceptanceGate(t, BindingLive)
	res := acceptanceQuery(t, g, "q5-stale-and-why.sql")
	got := rowsBy(t, res, 1)
	want := map[string]string{"porch": "stalled", "quiet": "silent", "ghost": "neverSeen", "twin": "<nil>"}
	if len(got) != len(want) {
		t.Fatalf("want %v, got %v", want, res.Rows)
	}
	for id, v := range want {
		r := got[id]
		if asText(r["verdict"]) != v {
			t.Errorf("%s verdict = %v, want %s", id, r["verdict"], v)
		}
		if v != "<nil>" && !strings.HasPrefix(asText(r["reason"]), v+":") {
			t.Errorf("%s reason = %v", id, r["reason"])
		}
	}
	if asText(got["ghost"]["role"]) != "attic" {
		t.Errorf("ghost role = %v", got["ghost"]["role"])
	}
}

// TestAcceptanceDeviceConflictIsTheTwinAlone: §6's instrument on the
// acceptance fleet. leaky's four boots follow one another, so the invariant
// must name twin and nothing else — an instrument that fires on a reboot is
// as dead as one that never fires.
func TestAcceptanceDeviceConflictIsTheTwinAlone(t *testing.T) {
	g := acceptanceGate(t, BindingSnap)
	res := query(t, g, Query{SQL: `SELECT node_id, boot_a, boot_b FROM device_conflict`})
	if len(res.Rows) != 1 || asText(res.Rows[0][0]) != "twin" {
		t.Errorf("device_conflict = %v, want twin 5/7 alone", res.Rows)
	}
}
