// Copyright (c) 2026 Ekorau LLC

package space

// fixtures_test.go — the stores custos's tests read, written by porta's own
// writer and committed as SQL text under protocol/fixtures/store/ (rete#338).
//
// custos reads porta's store and may not import porta's code, so the store a
// custos test opens cannot be built by calling this package. It is built here
// instead, dumped, and committed; custos loads the text. The store's schema is
// a contract between the two peers like any other, and these files are its
// checked-in instances, as protocol/fixtures/image/ is the image's.
//
// Regenerate after a deliberate change to a builder or to the schema:
//
//	go test ./internal/space -run TestStoreFixturesAreCurrent -update

import (
	"database/sql"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/davidg238/rete/protocol/cval"
	"github.com/davidg238/rete/protocol/store"
)

var update = flag.Bool("update", false, "rewrite protocol/fixtures/store/*.sql from their builders")

// storeFixtures names each committed store and the builder that writes it.
var storeFixtures = []struct {
	name  string
	build func(t *testing.T, path string)
}{
	{"content", contentStore},
	{"metrics", metricsStore},
	{"fleet", fleetStore},
	{"porch", porchStore},
	{"retired", retiredStore},
	{"history", historyStore},
	{"acceptance", acceptanceStore},
}

func fixtureDir(t *testing.T) string {
	t.Helper()
	_, here, _, _ := runtime.Caller(0)
	return filepath.Join(filepath.Dir(here), "..", "..", "..", "protocol", "fixtures", "store")
}

// TestStoreFixturesAreCurrent: each committed fixture is what its builder
// writes today. A fixture nobody can regenerate is a fixture nobody can trust;
// one that has drifted from porta's writer tests custos against a store porta
// no longer writes.
func TestStoreFixturesAreCurrent(t *testing.T) {
	dir := fixtureDir(t)
	for _, f := range storeFixtures {
		t.Run(f.name, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "store.db")
			f.build(t, path)
			got := dumpStore(t, path)
			file := filepath.Join(dir, f.name+".sql")
			if *update {
				if err := os.WriteFile(file, []byte(got), 0o644); err != nil {
					t.Fatal(err)
				}
			}
			want, err := os.ReadFile(file)
			if err != nil {
				t.Fatalf("%v (regenerate with -update)", err)
			}
			if string(want) != got {
				t.Fatalf("%s has drifted from its builder; regenerate with -update", file)
			}
		})
	}
}

// dumpStore is a store as the SQL that rebuilds it: the tables, then their
// rows in rowid order, then the indexes. Every literal is SQLite's own quote(),
// so a blob, a real and a text all come back as they went in.
func dumpStore(t *testing.T, path string) string {
	t.Helper()
	db, err := sql.Open("sqlite3", "file:"+path+"?mode=ro")
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()

	type object struct{ typ, name, ddl string }
	var objs []object
	rows, err := db.Query(`SELECT type, name, sql FROM sqlite_master
		WHERE sql IS NOT NULL AND name NOT LIKE 'sqlite_%' ORDER BY rowid`)
	if err != nil {
		t.Fatal(err)
	}
	for rows.Next() {
		var o object
		if err := rows.Scan(&o.typ, &o.name, &o.ddl); err != nil {
			t.Fatal(err)
		}
		objs = append(objs, o)
	}
	rows.Close()

	var b strings.Builder
	b.WriteString("-- porta's store, written by porta's own writer: porta/internal/space/fixtures_test.go.\n")
	b.WriteString("-- Do not edit; regenerate with -update.\n")
	for _, o := range objs {
		if o.typ == "table" {
			fmt.Fprintf(&b, "%s;\n", o.ddl)
		}
	}
	dumpRows := func(table string, cols []string) {
		quoted := make([]string, len(cols))
		for i, c := range cols {
			quoted[i] = "quote(" + quoteIdent(c) + ")"
		}
		rs, err := db.Query(`SELECT ` + strings.Join(quoted, ` || ',' || `) + ` FROM ` + quoteIdent(table) + ` ORDER BY rowid`)
		if err != nil {
			t.Fatal(err)
		}
		defer rs.Close()
		for rs.Next() {
			var vals string
			if err := rs.Scan(&vals); err != nil {
				t.Fatal(err)
			}
			fmt.Fprintf(&b, "INSERT INTO %s VALUES(%s);\n", quoteIdent(table), vals)
		}
	}
	for _, o := range objs {
		if o.typ == "table" {
			dumpRows(o.name, columnsOf(t, db, o.name))
		}
	}
	// AUTOINCREMENT's high-water marks, restated after the inserts that moved them.
	b.WriteString("DELETE FROM sqlite_sequence;\n")
	dumpRows("sqlite_sequence", []string{"name", "seq"})
	for _, o := range objs {
		if o.typ != "table" {
			fmt.Fprintf(&b, "%s;\n", o.ddl)
		}
	}
	return b.String()
}

func columnsOf(t *testing.T, db *sql.DB, table string) []string {
	t.Helper()
	rows, err := db.Query(`SELECT name FROM pragma_table_info(?) ORDER BY cid`, table)
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	var cols []string
	for rows.Next() {
		var c string
		if err := rows.Scan(&c); err != nil {
			t.Fatal(err)
		}
		cols = append(cols, c)
	}
	return cols
}

func quoteIdent(s string) string { return `"` + strings.ReplaceAll(s, `"`, `""`) + `"` }

// fixtureStore opens a store at path on a clock that advances a second a call
// from 1_700_000_000_000, the clock most fixtures share.
func fixtureStore(t *testing.T, path string) (*Store, *int64) {
	t.Helper()
	clock := new(int64)
	*clock = 1_700_000_000_000
	st, err := Open(path, func() int64 { *clock += 1_000; return *clock })
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { st.Close() })
	return st, clock
}

func fixtureEnc(t *testing.T) func(v any) []byte {
	return func(v any) []byte {
		t.Helper()
		b, err := cval.Encode(v)
		if err != nil {
			t.Fatal(err)
		}
		return b
	}
}

// metricsKeys is the Metrics record's fields in the order
// protocol/descriptors/records.yaml declares them.
var metricsKeys = []string{"lb", "gcc", "hhw", "hnl", "hsz", "lfb", "ovr", "pco", "pdr", "pst", "qhw", "hnhw"}

// metricsRecord is a Metrics value with vals filled in and every other field 0.
func metricsRecord(t *testing.T, vals map[string]any) []byte {
	t.Helper()
	rec := cval.Record{Tag: "Metrics"}
	for _, key := range metricsKeys {
		v, ok := vals[key]
		if !ok {
			v = int64(0)
		}
		rec.Fields = append(rec.Fields, cval.Pair{K: cval.Symbol(key), V: v})
	}
	return fixtureEnc(t)(rec)
}

// contentStore: one node, commissioned and echoing its name.
func contentStore(t *testing.T, path string) {
	st, _ := fixtureStore(t, path)
	writeContent(t, st)
}

func writeContent(t *testing.T, st *Store) {
	t.Helper()
	val := fixtureEnc(t)("bathroom-1")
	if _, err := st.Write("n1", "goal/name", cval.KindCell, val, "op", "commission"); err != nil {
		t.Fatal(err)
	}
	if _, _, err := st.Ingest("n1", 41, []Entry{
		{Seq: 1, Path: "obs/name", Kind: cval.KindCell, Value: val},
	}); err != nil {
		t.Fatal(err)
	}
}

// metricsStore: content, plus three Metrics samples from n1 across two boots.
func metricsStore(t *testing.T, path string) {
	st, _ := fixtureStore(t, path)
	writeContent(t, st)
	for _, s := range []struct{ boot, seq, lfb, gcc int64 }{
		{41, 2, 4096, 1},
		{41, 3, 9000, 2},
		{42, 1, 512, 1},
	} {
		val := metricsRecord(t, map[string]any{"lfb": s.lfb, "gcc": s.gcc, "hsz": int64(131072)})
		if _, _, err := st.Ingest("n1", s.boot, []Entry{
			{Seq: s.seq, Path: "sys/metrics", Kind: cval.KindCell, Value: val},
		}); err != nil {
			t.Fatal(err)
		}
	}
}

// fleetStore: n1 is commissioned and converged, then reboots and republishes
// only part of its tree; n2 has a goal it has not echoed and a gw verdict with
// a reason.
func fleetStore(t *testing.T, path string) {
	st, _ := fixtureStore(t, path)
	enc := fixtureEnc(t)
	if _, err := st.Write("n1", "goal/name", cval.KindCell, enc("bathroom-1"), "david", "commission"); err != nil {
		t.Fatal(err)
	}
	if _, _, err := st.Ingest("n1", 41, []Entry{
		{Seq: 1, Path: "obs/name", Kind: cval.KindCell, Value: enc("bathroom-1")},
		{Seq: 2, Path: "obs/mode", Kind: cval.KindCell, Value: enc(cval.Symbol("run"))},
	}); err != nil {
		t.Fatal(err)
	}
	// n1 reboots into boot 42 and has so far republished only obs/name, so
	// obs/mode is the rete#210 mixed-read row.
	if _, _, err := st.Ingest("n1", 42, []Entry{
		{Seq: 1, Path: "obs/name", Kind: cval.KindCell, Value: enc("bathroom-1")},
	}); err != nil {
		t.Fatal(err)
	}
	if _, err := st.Write("n2", "goal/name", cval.KindCell, enc("porch"), "david", "commission"); err != nil {
		t.Fatal(err)
	}
	if _, _, err := st.Ingest("n2", 7, []Entry{
		{Seq: 1, Path: "obs/name", Kind: cval.KindCell, Value: enc("n2")},
	}); err != nil {
		t.Fatal(err)
	}
	if err := st.WriteGateway("n2", store.PathLiveness, enc(cval.Symbol("silent")),
		"silent: 90000 ms since the last check-in"); err != nil {
		t.Fatal(err)
	}
}

// porchStore: one node with a commissioned role and a #live verdict — the
// fleet page's smallest case.
func porchStore(t *testing.T, path string) {
	st, _ := fixtureStore(t, path)
	enc := fixtureEnc(t)
	if _, err := st.Write("n1", "goal/name", cval.KindCell, enc("porch"), "david", "commission"); err != nil {
		t.Fatal(err)
	}
	if _, _, err := st.Ingest("n1", 3, []Entry{
		{Seq: 1, Path: "obs/name", Kind: cval.KindCell, Value: enc("porch")},
	}); err != nil {
		t.Fatal(err)
	}
	if err := st.WriteGateway("n1", store.PathLiveness, enc(cval.Symbol("live")), "live: fine"); err != nil {
		t.Fatal(err)
	}
}

func buildRecord(t *testing.T, board, build string) []byte {
	t.Helper()
	fields := []cval.Pair{{K: cval.Symbol("chip"), V: cval.Symbol("esp32c6")}}
	if board != "" {
		fields = append(fields, cval.Pair{K: cval.Symbol("board"), V: board})
	}
	fields = append(fields, cval.Pair{K: cval.Symbol("build"), V: build})
	return fixtureEnc(t)(cval.Record{Tag: "Build", Fields: fields})
}

// retiredStore: bench1 falls silent and then a factory-id board asserting
// board "bench1" appears — one board succeeding itself. bench2's factory board
// appears while bench2 is still checking in — two boards, which must NOT be
// folded together.
func retiredStore(t *testing.T, path string) {
	st, clock := fixtureStore(t, path)
	ingest := func(node string, boot int64, path string, v []byte) {
		t.Helper()
		if _, _, err := st.Ingest(node, boot, []Entry{{Seq: *clock, Path: path, Kind: cval.KindCell, Value: v}}); err != nil {
			t.Fatal(err)
		}
	}
	name := fixtureEnc(t)

	ingest("bench1", 4, "obs/name", name("bench1"))
	ingest("bench1", 4, "obs/name", name("bench1"))
	ingest("aaaa00000001", 1, "obs/build", buildRecord(t, "bench1", "test"))

	ingest("bench2", 9, "obs/name", name("bench2"))
	ingest("aaaa00000002", 1, "obs/build", buildRecord(t, "bench2", "test"))
	ingest("bench2", 9, "obs/name", name("bench2")) // still alive: overlap
}

// historyStore is the fixture the `snap` views are proven against. Each line of
// the fleet is a trap one of the views exists for:
//
//	slotA  boots 10, 11            ┐ one board: the counter runs on gaplessly
//	slotB  boots 12, 13            ┘ across two ids, and their lives never overlap
//	f00000000001                     the same board on its factory id, asserting
//	                                 board "slotB" after slotB fell silent
//	other  boots 3 (during slotB), 14 (after it)  a DIFFERENT board whose counter
//	                                 happens to continue slotB's — its boot 3
//	                                 overlaps slotB, so it is never folded in
//	dup    boots 5 and 7, interleaved: two boards flashed with one id
//	self   boots 11, 12 after slotA: an ordinary reboot pair. Its boot 12 is
//	       not slotA's successor, because self has a boot 11 of its own
//	rival  boot 12, last of all: a second candidate successor for slotA's
//	       boot 11. slotB's boot 12 is nearer, so rival stays its own board
//
// The last two are the real store's false links, reduced (addendum D5).
//
// slotA boot 10 also carries seven GC cycles of metrics and an obs/build, and
// f00000000001 is commissioned twice (role history).
func historyStore(t *testing.T, path string) {
	st, _ := fixtureStore(t, path)
	var seq int64
	ex := func(node string, boot int64) {
		t.Helper()
		if err := st.RecordExchange(node, boot, 0, 0, 0, 0, "10.0.0.1:1"); err != nil {
			t.Fatal(err)
		}
	}
	put := func(node string, boot int64, path string, v []byte) {
		t.Helper()
		seq++
		if _, _, err := st.Ingest(node, boot, []Entry{{Seq: seq, Path: path, Kind: cval.KindCell, Value: v}}); err != nil {
			t.Fatal(err)
		}
	}

	ex("slotA", 10)
	put("slotA", 10, "obs/build", buildRecord(t, "", "b1"))
	for gcc := int64(1); gcc <= 7; gcc++ {
		// Two samples per cycle; the larger lfb is the readable one.
		put("slotA", 10, "sys/metrics", metricsRecord(t, map[string]any{"gcc": gcc, "lfb": int64(100 * gcc), "lb": int64(50)}))
		put("slotA", 10, "sys/metrics", metricsRecord(t, map[string]any{"gcc": gcc, "lfb": int64(100*gcc + 7), "lb": int64(40)}))
	}
	ex("slotA", 10)
	ex("slotA", 11)
	ex("slotA", 11)
	// Metrics from before the store recorded boot ids: boot 0 is many boots,
	// and gcc restarts at each, so it can never be one cycle's evidence.
	put("legacy", 0, "sys/metrics", metricsRecord(t, map[string]any{"gcc": int64(9), "lfb": int64(1)}))
	ex("self", 11)
	ex("self", 12)

	ex("slotB", 12)
	ex("slotB", 12)
	ex("other", 3) // a different board, alive across slotB's boot 13
	ex("slotB", 13)
	ex("other", 3)
	ex("slotB", 13)

	ex("other", 14)
	ex("other", 14)

	put("f00000000001", 1, "obs/build", buildRecord(t, "slotB", "b2"))
	ex("f00000000001", 1)
	for _, role := range []string{"kitchen", "porch"} {
		if _, err := st.Write("f00000000001", "goal/name", cval.KindCell, fixtureEnc(t)(role), "david", "commission"); err != nil {
			t.Fatal(err)
		}
	}

	for i := 0; i < 3; i++ {
		ex("dup", 5)
		ex("dup", 7)
	}
	ex("rival", 12)
	ex("rival", 12)
}

const (
	hourMs = int64(3_600_000)
	dayMs  = 24 * hourMs
	// acceptanceT0 is the leak build's first boot; everything is placed relative to it.
	acceptanceT0 = int64(1_780_000_000_000)
	// gcWarmupCycles is how many cycles of a boot run on an arena never swept;
	// custos discards them (the metrics descriptor's gcc validity rule).
	gcWarmupCycles = 5
)

// acceptanceStore builds the custos first-cut design's §10 fleet.
//
//	leaky  the rete#190 leak+fix shape, one node across four boots:
//	       boot 1  build v0, 45 days before t0 — outside "the last month"
//	       boot 2  build v1, 10 h, the envelope falls 500 B a cycle: the leak
//	       boot 3  build v2, 10 h, flat: the fix
//	       boot 4  build v2, falling over 2 h of envelope (6 h less warm-up) —
//	               too short to fit, and must be refused
//	       Each GC cycle publishes a failing-request sample (lfb 96 B) and a
//	       post-sweep one, and every boot opens on five warm-up cycles of an
//	       unswept arena — the two traps lfb's validity rule names.
//	porch  goal/mode run, obs/mode idle: disagrees with its goal; #stalled
//	quiet  heard, then #silent
//	ghost  commissioned, never heard: #neverSeen, and no obs at all
//	twin   boots 5 and 7 interleaved: two boards under one id (§6), unjudged
func acceptanceStore(t *testing.T, path string) {
	now := acceptanceT0
	st, err := Open(path, func() int64 { now++; return now })
	if err != nil {
		t.Fatal(err)
	}
	defer st.Close()
	enc := fixtureEnc(t)

	seq := map[string]int64{}
	put := func(node string, boot int64, path string, v []byte) {
		t.Helper()
		seq[node]++
		if _, _, err := st.Ingest(node, boot, []Entry{{Seq: seq[node], Path: path, Kind: cval.KindCell, Value: v}}); err != nil {
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
		return metricsRecord(t, map[string]any{"lfb": lfb, "lb": lb, "gcc": gcc, "hsz": int64(131072)})
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

	t0 := acceptanceT0
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
		if err := st.WriteGateway(v.node, store.PathLiveness, enc(cval.Symbol(v.verdict)), v.reason); err != nil {
			t.Fatal(err)
		}
	}
}
