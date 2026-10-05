// Copyright (c) 2026 Ekorau LLC

package gate

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/davidg238/porta/internal/cval"
	"github.com/davidg238/porta/internal/space"
)

// metricsRecord builds a well-formed Metrics in wire order, so the fixture goes
// through porta's own write door (and therefore the #243 manifest gate) rather
// than around it. A fixture the real store would refuse proves nothing about the
// real store.
func metricsRecord(t *testing.T, d *Descriptors, vals map[string]any) []byte {
	t.Helper()
	rec := cval.Record{Tag: "Metrics"}
	for _, key := range d.Columns("metrics") {
		v, ok := vals[key]
		if !ok {
			v = int64(0)
		}
		rec.Fields = append(rec.Fields, cval.Pair{K: cval.Symbol(key), V: v})
	}
	b, err := cval.Encode(rec)
	if err != nil {
		t.Fatalf("encode Metrics: %v", err)
	}
	return b
}

// storeWithMetrics is the fixture the decoded-table tests read: three Metrics
// samples from one node, across two boots, plus the goal/obs pair
// storeWithContent already provides.
func storeWithMetrics(t *testing.T, d *Descriptors) (*space.Store, string) {
	t.Helper()
	st, path := storeWithContent(t)
	samples := []struct {
		boot int64
		seq  int64
		lfb  int64
		gcc  int64
	}{
		{41, 2, 4096, 1},
		{41, 3, 9000, 2},
		{42, 1, 512, 1},
	}
	for _, s := range samples {
		val := metricsRecord(t, d, map[string]any{"lfb": s.lfb, "gcc": s.gcc, "hsz": int64(131072)})
		if _, _, err := st.Ingest("n1", s.boot, []space.Entry{
			{Seq: s.seq, Path: "sys/metrics", Kind: cval.KindCell, Value: val},
		}); err != nil {
			t.Fatalf("Ingest metrics: %v", err)
		}
	}
	return st, path
}

func gateOverMetrics(t *testing.T) (*Gate, *Descriptors) {
	t.Helper()
	d := descriptors(t)
	st, _ := storeWithMetrics(t, d)
	g, err := Open(snapshotOf(t, st), d)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	t.Cleanup(func() { g.Close() })
	return g, d
}

func query(t *testing.T, g *Gate, q Query) *Result {
	t.Helper()
	res, err := g.Query(context.Background(), q)
	if err != nil {
		t.Fatalf("Query(%q): %v", q.SQL, err)
	}
	return res
}

// TestPipelineAnswersFromAMaterializedTable is the whole loop end to end: schema
// built, plan taken, one table materialized, statement re-prepared and run.
func TestPipelineAnswersFromAMaterializedTable(t *testing.T) {
	g, _ := gateOverMetrics(t)
	res := query(t, g, Query{SQL: `SELECT node_id, path FROM cells ORDER BY path`})
	if len(res.Rows) == 0 {
		t.Fatal("no rows; the scratch was never materialized")
	}
	if res.Binding != BindingSnap {
		t.Errorf("binding: got %q", res.Binding)
	}
	if res.SnapshotMs == 0 {
		t.Error("snapshot_ms is 0 on a snapshot binding; §3.1's answer must date itself")
	}
}

// TestOnlyReferencedTablesAreMaterialized is the point of using the authorizer
// as the planner: a query over `cells` must not pay for `events`. Sources is the
// evidence, and it is also §7's measurement.
func TestOnlyReferencedTablesAreMaterialized(t *testing.T) {
	g, _ := gateOverMetrics(t)
	res := query(t, g, Query{SQL: `SELECT count(*) FROM cells`})

	got := map[string]bool{}
	for _, s := range res.Sources {
		got[s.Table] = true
	}
	if !got["cells"] {
		t.Fatalf("cells was not materialized: %+v", res.Sources)
	}
	for _, unwanted := range []string{"events", "exchanges", "nodes", "sys_metrics", "descriptors"} {
		if got[unwanted] {
			t.Errorf("%s was materialized for a query that never mentions it: %+v", unwanted, res.Sources)
		}
	}
}

// TestDecodedTableIsDecodedThroughCval: the decoded table's columns are the
// descriptor table's, in wire order, and the values are what the node encoded.
func TestDecodedTableIsDecodedThroughCval(t *testing.T) {
	g, d := gateOverMetrics(t)
	res := query(t, g, Query{SQL: `SELECT node_id, boot_id, seq, lfb, gcc, hsz FROM sys_metrics ORDER BY seq, boot_id`})

	if len(res.Rows) != 3 {
		t.Fatalf("want 3 decoded rows, got %d", len(res.Rows))
	}
	// (boot 42, seq 1), (41, 2), (41, 3)
	wantLfb := []int64{512, 4096, 9000}
	for i, want := range wantLfb {
		if got, ok := res.Rows[i][3].(int64); !ok || got != want {
			t.Errorf("row %d lfb: got %v, want %d", i, res.Rows[i][3], want)
		}
	}
	if res.Rows[0][0] != "n1" {
		t.Errorf("node_id: got %v", res.Rows[0][0])
	}

	// And the schema really is the descriptor table's, in order.
	schema, err := g.schema()
	if err != nil {
		t.Fatal(err)
	}
	td := schema["sys_metrics"]
	var cols []string
	for _, c := range td.cols[len(identityColumns):] {
		cols = append(cols, c.name)
	}
	if !equalStrings(cols, d.Columns("metrics")) {
		t.Errorf("sys_metrics columns:\n got %v\nwant %v", cols, d.Columns("metrics"))
	}
}

// TestEveryAnswerCarriesItsReadingRules is design §3.1. `select … lfb` must come
// back with the reason the naive read of lfb is wrong, not leave it to be
// remembered.
func TestEveryAnswerCarriesItsReadingRules(t *testing.T) {
	g, _ := gateOverMetrics(t)
	res := query(t, g, Query{SQL: `SELECT lfb FROM sys_metrics`})

	var note *Note
	for i := range res.Notes {
		if res.Notes[i].Column == "lfb" {
			note = &res.Notes[i]
		}
	}
	if note == nil {
		t.Fatalf("no note for lfb; notes: %+v", res.Notes)
	}
	if note.Validity == "" {
		t.Error("lfb's note carries no validity — the whole point of §3.1")
	}
	if !strings.Contains(note.Validity, "max(") {
		t.Errorf("lfb's validity does not name the correct aggregate: %q", note.Validity)
	}
	if len(note.Segment) == 0 {
		t.Error("lfb's note carries no segment")
	}
	if note.Record != "Metrics" || note.Table != "sys_metrics" {
		t.Errorf("note is not addressed: %+v", note)
	}
}

// TestIdentityColumnsCarryNoNote: a note per row-address would bury the notes
// that matter.
func TestIdentityColumnsCarryNoNote(t *testing.T) {
	g, _ := gateOverMetrics(t)
	res := query(t, g, Query{SQL: `SELECT node_id, boot_id, seq, src_path FROM sys_metrics`})
	if len(res.Notes) != 0 {
		t.Errorf("identity-only query produced notes: %+v", res.Notes)
	}
}

// TestNonReadIsRefusedByActionNowThatTheTableExists. Task 3 proved `DELETE FROM
// events` fails because there IS no events on Q — the topology talking. Now the
// scratch really does hold the table, so this is the authorizer alone, and it
// must name what it refused.
func TestNonReadIsRefusedByActionNowThatTheTableExists(t *testing.T) {
	g, _ := gateOverMetrics(t)
	_, err := g.Query(context.Background(), Query{SQL: `DELETE FROM cells`})
	if err == nil {
		t.Fatal("DELETE FROM cells was allowed")
	}
	var denied *deniedError
	if !errors.As(err, &denied) {
		t.Fatalf("want a deniedError, got %T: %v", err, err)
	}
	if !contains(denied.Actions(), "DELETE") {
		t.Errorf("refusal does not name DELETE: %v", denied.Actions())
	}
}

// TestOutOfBandIntegerIsRefusedNotClamped is addendum A4's guard. The algebra's
// band is ±(2^64−1) and SQLite's is int64; a value between them must be a loud
// error naming the row, because a clamped number is a wrong answer no reader can
// detect.
//
// The fixture is written past the store's write door on purpose: the guard is
// about what a MATERIALIZER meets, and a row can predate any write-door rule.
func TestOutOfBandIntegerIsRefusedNotClamped(t *testing.T) {
	d := descriptors(t)
	st, _ := storeWithMetrics(t, d)
	snap := snapshotOf(t, st)

	huge := cval.Uint(uint64(math.MaxInt64) + 1)
	val := metricsRecord(t, d, map[string]any{"hsz": huge})
	insertRawCell(t, snap, "n9", "sys/metrics", 7, val)

	g, err := Open(snap, d)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	defer g.Close()

	_, err = g.Query(context.Background(), Query{SQL: `SELECT hsz FROM sys_metrics`})
	if err == nil {
		t.Fatal("an out-of-band integer was accepted; it must be refused, never clamped")
	}
	for _, want := range []string{"hsz", "n9", "9223372036854775808"} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("refusal does not name %q: %v", want, err)
		}
	}
	if strings.Contains(err.Error(), "clamp") && !strings.Contains(err.Error(), "not clamped") &&
		!strings.Contains(err.Error(), "rather than clamped") {
		t.Errorf("refusal reads as a clamp: %v", err)
	}
}

// TestTagMismatchIsFatal: a value at a path must be the shape the catalogue says
// rides there. Decoding it as something else would answer confidently and
// wrongly.
func TestTagMismatchIsFatal(t *testing.T) {
	d := descriptors(t)
	st, _ := storeWithMetrics(t, d)
	snap := snapshotOf(t, st)

	wrong, err := cval.Encode(cval.Record{Tag: "CHeap", Fields: cval.Map{
		{K: cval.Symbol("ob"), V: int64(1)},
	}})
	if err != nil {
		t.Fatal(err)
	}
	insertRawCell(t, snap, "n9", "sys/metrics", 7, wrong)

	g, err := Open(snap, d)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	defer g.Close()

	_, err = g.Query(context.Background(), Query{SQL: `SELECT lfb FROM sys_metrics`})
	if err == nil {
		t.Fatal("a CHeap record at sys/metrics was decoded as Metrics")
	}
	if !strings.Contains(err.Error(), "CHeap") || !strings.Contains(err.Error(), "Metrics") {
		t.Errorf("the error names neither shape: %v", err)
	}
}

// TestRowCapTruncatesLoudly: a silent prefix is a wrong answer.
func TestRowCapTruncatesLoudly(t *testing.T) {
	g, _ := gateOverMetrics(t)

	full := query(t, g, Query{SQL: `SELECT seq FROM sys_metrics`})
	if len(full.Rows) != 3 || full.Truncated {
		t.Fatalf("fixture: want 3 untruncated rows, got %d truncated=%v", len(full.Rows), full.Truncated)
	}
	capped := query(t, g, Query{SQL: `SELECT seq FROM sys_metrics`, MaxRows: 2})
	if len(capped.Rows) != 2 {
		t.Errorf("want 2 rows under the cap, got %d", len(capped.Rows))
	}
	if !capped.Truncated {
		t.Error("truncation was silent")
	}

	// Exactly at the cap is NOT truncated — the distinction the read-one-past
	// exists to make.
	exact := query(t, g, Query{SQL: `SELECT seq FROM sys_metrics`, MaxRows: 3})
	if len(exact.Rows) != 3 || exact.Truncated {
		t.Errorf("exactly-at-cap: got %d rows truncated=%v, want 3 false", len(exact.Rows), exact.Truncated)
	}
}

// TestDeadlineInterruptsARunawayQuery is §2.3's first guard, and it is the one
// read-only never provided: an unbounded recursive CTE is a legal read.
func TestDeadlineInterruptsARunawayQuery(t *testing.T) {
	g, _ := gateOverMetrics(t)
	started := time.Now()
	_, err := g.Query(context.Background(), Query{
		SQL:      `WITH RECURSIVE spin(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM spin) SELECT count(*) FROM spin`,
		Deadline: 250 * time.Millisecond,
	})
	elapsed := time.Since(started)
	if err == nil {
		t.Fatal("an unbounded recursive query ran to completion")
	}
	// The refusal is not the deadline. Until 2026-10-04 the authorizer denied
	// SQLITE_RECURSIVE outright, so this test passed without ever reaching the
	// interrupt it exists to prove.
	var denied *deniedError
	if errors.As(err, &denied) {
		t.Fatalf("the query was refused, not interrupted — the deadline was never exercised: %v", err)
	}
	if elapsed > 10*time.Second {
		t.Errorf("the deadline did not interrupt: %v", elapsed)
	}
}

// TestScratchCapRefusesARunawayMaterialization is §2.3's byte cap, and it must
// fire during materialization rather than after the disk is full.
func TestScratchCapRefusesARunawayMaterialization(t *testing.T) {
	g, _ := gateOverMetrics(t)
	_, err := g.Query(context.Background(), Query{
		SQL:             `SELECT count(*) FROM cells`,
		MaxScratchBytes: 1,
	})
	if err == nil {
		t.Fatal("a 1-byte scratch cap was not enforced")
	}
	if !strings.Contains(err.Error(), "scratch cap") {
		t.Errorf("the error does not name the cap: %v", err)
	}
}

// TestDescriptorsAreQueryable is D2 landing rather than being asserted: the
// dictionary answers to the same read as the data.
func TestDescriptorsAreQueryable(t *testing.T) {
	g, _ := gateOverMetrics(t)
	res := query(t, g, Query{SQL: `SELECT record, unit, validity FROM descriptors WHERE key = 'lfb'`})
	if len(res.Rows) != 1 {
		t.Fatalf("want one lfb row, got %d", len(res.Rows))
	}
	if res.Rows[0][0] != "Metrics" || res.Rows[0][1] != "bytes" {
		t.Errorf("lfb row: %v", res.Rows[0])
	}
	if v, _ := res.Rows[0][2].(string); v == "" {
		t.Error("lfb's validity is empty in the queryable table")
	}
}

// TestSpecifiedRecordSaysWhyItIsEmpty is addendum B2 made structural. Eight of
// twenty shapes have no producer; an agent querying one gets silence, and
// silence at a path is otherwise indistinguishable from a dead node.
func TestSpecifiedRecordSaysWhyItIsEmpty(t *testing.T) {
	g, _ := gateOverMetrics(t)
	res := query(t, g, Query{SQL: `SELECT dropped FROM sys_overrun`})
	if len(res.Rows) != 0 {
		t.Fatalf("sys_overrun has rows in this fixture: %v", res.Rows)
	}
	var src *Source
	for i := range res.Sources {
		if res.Sources[i].Table == "sys_overrun" {
			src = &res.Sources[i]
		}
	}
	if src == nil {
		t.Fatalf("no source for sys_overrun: %+v", res.Sources)
	}
	if src.Status != "specified" {
		t.Errorf("sys_overrun's emptiness is unexplained: status %q", src.Status)
	}
}

// TestStructuralColumnsComeFromTheSource is the drift guard: the scratch's copy
// of porta's schema is READ from porta's schema, so it cannot disagree with it.
func TestStructuralColumnsComeFromTheSource(t *testing.T) {
	d := descriptors(t)
	st, storePath := storeWithMetrics(t, d)
	g, err := Open(snapshotOf(t, st), d)
	if err != nil {
		t.Fatal(err)
	}
	defer g.Close()

	got, err := g.structuralColumns("cells")
	if err != nil {
		t.Fatal(err)
	}
	var names []string
	for _, c := range got {
		names = append(names, c.name)
	}

	raw, err := sql.Open("sqlite3", "file:"+storePath+"?mode=ro")
	if err != nil {
		t.Fatal(err)
	}
	defer raw.Close()
	rows, err := raw.Query(`SELECT name FROM pragma_table_info('cells')`)
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	var want []string
	for rows.Next() {
		var n string
		if err := rows.Scan(&n); err != nil {
			t.Fatal(err)
		}
		want = append(want, n)
	}
	if !equalStrings(names, want) {
		t.Errorf("scratch cells columns %v disagree with the store's %v", names, want)
	}
}

// TestIdentityCollisionIsClosed. `Overrun.path` is a real wire field meaning
// something other than the row's stored path, which is why the identity column
// is `src_path`. This closes the CLASS: a future descriptor colliding with any
// identity column fails at schema build, naming it.
func TestIdentityCollisionIsClosed(t *testing.T) {
	d := descriptors(t)
	if err := checkIdentityCollisions(d); err != nil {
		t.Fatalf("the shipped descriptor table collides with an identity column: %v", err)
	}

	// The negative control: a gate whose red has never been witnessed is not a
	// gate. `node_id` is added as a wire field, and the build must refuse.
	drifted := *d
	drifted.Records = map[string]*Record{}
	for k, v := range d.Records {
		drifted.Records[k] = v
	}
	clone := *d.Records["metrics"]
	clone.Fields = append(append([]*Field{}, clone.Fields...), &Field{
		Key: "node_id", Name: "node_id", Unit: "text", Kind: "identity", Meaning: "collides",
	})
	drifted.Records["metrics"] = &clone

	err := checkIdentityCollisions(&drifted)
	if err == nil {
		t.Fatal("a descriptor field named node_id was accepted")
	}
	if !strings.Contains(err.Error(), "metrics.node_id") {
		t.Errorf("the refusal does not name the collision: %v", err)
	}
}

// TestLiveDecodesFromCellsAndSaysSo: `snap` offers events, so a decoded table is
// the full history; `live` does not, so it is the latest value per path and
// nothing more. That changes what the rows MEAN, so it rides out with them.
func TestLiveDecodesFromCellsAndSaysSo(t *testing.T) {
	d := descriptors(t)
	st, storePath := storeWithMetrics(t, d)
	_ = st

	g, err := OpenLive(storePath, d)
	if err != nil {
		t.Fatalf("OpenLive: %v", err)
	}
	defer g.Close()

	res := query(t, g, Query{SQL: `SELECT seq, lfb FROM sys_metrics`})
	if len(res.Rows) != 1 {
		t.Fatalf("live is latest-value-per-path: want 1 row, got %d", len(res.Rows))
	}
	var src *Source
	for i := range res.Sources {
		if res.Sources[i].Table == "sys_metrics" {
			src = &res.Sources[i]
		}
	}
	if src == nil || src.From != "cells" {
		t.Fatalf("live did not report its provenance as cells: %+v", res.Sources)
	}
	if res.SnapshotMs != 0 {
		t.Errorf("live must date itself as now (0), got %d", res.SnapshotMs)
	}

	// And `events` is not there to be scanned — §2.4's structural boundedness.
	if _, err := g.Query(context.Background(), Query{SQL: `SELECT count(*) FROM events`}); err == nil {
		t.Error("events is reachable on the live binding")
	}
}

// TestSnapDecodesFullHistoryFromEvents is the other half of the pair, so the
// distinction above is a measured difference rather than one binding's quirk.
func TestSnapDecodesFullHistoryFromEvents(t *testing.T) {
	g, _ := gateOverMetrics(t)
	res := query(t, g, Query{SQL: `SELECT seq FROM sys_metrics`})
	if len(res.Rows) != 3 {
		t.Fatalf("snap is full history: want 3 rows, got %d", len(res.Rows))
	}
	for _, s := range res.Sources {
		if s.Table == "sys_metrics" && s.From != "events" {
			t.Errorf("snap's decoded provenance: got %q, want events", s.From)
		}
	}
}

// TestTablesNamesEveryLogicalTable backs `gate tables`.
func TestTablesNamesEveryLogicalTable(t *testing.T) {
	g, d := gateOverMetrics(t)
	names, err := g.Tables()
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"cells", "events", "exchanges", "gw_runs", "nodes", "descriptors", "sys_metrics"} {
		if !contains(names, want) {
			t.Errorf("Tables() omits %q: %v", want, names)
		}
	}
	// One table per record-bearing path, and the wildcard's instance is a row's
	// key rather than part of the table's identity.
	if !contains(names, "diag_spans") {
		t.Errorf("Tables() omits the wildcard-derived diag_spans: %v", names)
	}
	_ = d
}

// insertRawCell writes a row past porta's write door, so a materializer guard
// can be shown acting on the row it exists for. A snapshot is opened read-write
// here deliberately — it is a test fixture, not a live store.
func insertRawCell(t *testing.T, snapPath, nodeID, path string, seq int64, value []byte) {
	t.Helper()
	db, err := sql.Open("sqlite3", "file:"+snapPath)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	if _, err := db.Exec(
		`INSERT INTO cells (node_id, path, seq, kind, value_cbor, ts_ms, direction, boot_id)
		 VALUES (?, ?, ?, ?, ?, ?, 'in', 1)`,
		nodeID, path, seq, int(cval.KindCell), value, 1_700_000_100_000); err != nil {
		t.Fatalf("raw insert: %v", err)
	}
	if _, err := db.Exec(
		`INSERT INTO events (node_id, path, seq, kind, value_cbor, ts_ms, direction, boot_id)
		 VALUES (?, ?, ?, ?, ?, ?, 'in', 1)`,
		nodeID, path, seq, int(cval.KindCell), value, 1_700_000_100_000); err != nil {
		t.Fatalf("raw insert: %v", err)
	}
	if err := db.Close(); err != nil {
		t.Fatal(err)
	}
	// Open() refuses a path with a -wal/-shm sibling, and a fixture that leaves
	// one behind would fail for the wrong reason.
	for _, sib := range []string{snapPath + "-wal", snapPath + "-shm"} {
		if _, err := os.Stat(sib); err == nil {
			t.Fatalf("fixture left %s behind", filepath.Base(sib))
		}
	}
}

var _ = fmt.Sprintf
