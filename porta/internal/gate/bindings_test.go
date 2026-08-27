// Copyright (c) 2026 Ekorau LLC

package gate

import (
	"database/sql"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/davidg238/porta/internal/cval"
	"github.com/davidg238/porta/internal/space"
)

// storeWithContent builds a small real store through porta's own writer, so the
// bindings are exercised against the schema they will actually meet.
func storeWithContent(t *testing.T) (*space.Store, string) {
	t.Helper()
	dir := t.TempDir()
	path := filepath.Join(dir, "store.db")
	var clock int64 = 1_700_000_000_000
	st, err := space.Open(path, func() int64 { clock += 1_000; return clock })
	if err != nil {
		t.Fatalf("space.Open: %v", err)
	}
	t.Cleanup(func() { st.Close() })
	val, err := cval.Encode("bathroom-1")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := st.Write("n1", "goal/name", cval.KindCell, val, "op", "commission"); err != nil {
		t.Fatalf("Write: %v", err)
	}
	if _, _, err := st.Ingest("n1", 41, []space.Entry{
		{Seq: 1, Path: "obs/name", Kind: cval.KindCell, Value: val},
	}); err != nil {
		t.Fatalf("Ingest: %v", err)
	}
	return st, path
}

func snapshotOf(t *testing.T, st *space.Store) string {
	t.Helper()
	snap := filepath.Join(t.TempDir(), "snap.db")
	if err := st.SnapshotInto(snap); err != nil {
		t.Fatalf("SnapshotInto: %v", err)
	}
	return snap
}

func descriptors(t *testing.T) *Descriptors {
	t.Helper()
	d, err := LoadDescriptors(repoFile(t, "protocol", "descriptors", "records.yaml"))
	if err != nil {
		t.Fatalf("descriptors: %v", err)
	}
	return d
}

// TestOpenSnapshotUnderARealBoundary: the snapshot binding reads through a
// genuine OS-level boundary — chmod a-w on the file AND its directory — which a
// live WAL store cannot do, because a WAL reader must write -shm. This is design
// §2.1's measured table, exercised.
func TestOpenSnapshotUnderARealBoundary(t *testing.T) {
	st, _ := storeWithContent(t)
	snap := snapshotOf(t, st)
	dir := filepath.Dir(snap)
	if err := os.Chmod(snap, 0o444); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(dir, 0o555); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { os.Chmod(dir, 0o755) })

	g, err := Open(snap, descriptors(t))
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	defer g.Close()

	if g.Binding() != BindingSnap {
		t.Errorf("binding = %q", g.Binding())
	}
	if g.SnapshotMs() == 0 {
		t.Error("snapshot binding has no snapshot_ms; an answer cannot say how old it is")
	}
	var n int
	if err := g.r.QueryRow("SELECT count(*) FROM cells").Scan(&n); err != nil {
		t.Fatalf("read through the boundary: %v", err)
	}
	if n != 2 {
		t.Errorf("cells = %d, want 2", n)
	}
	// Nothing was written beside it — the whole point of immutable=1.
	for _, sib := range []string{snap + "-wal", snap + "-shm"} {
		if _, err := os.Stat(sib); err == nil {
			t.Errorf("%s exists: the immutable read wrote to the directory", filepath.Base(sib))
		}
	}
}

// TestOpenRefusesALiveStore is the guard that matters most here, because the
// failure it prevents is SILENT: immutable=1 on a live store asserts something
// false and yields stale pages rather than an error. A -wal sibling is the
// evidence, and it must be refused rather than trusted.
func TestOpenRefusesALiveStore(t *testing.T) {
	_, live := storeWithContent(t)
	if _, err := os.Stat(live + "-wal"); err != nil {
		t.Skipf("no -wal beside the live store, so this guard cannot be exercised: %v", err)
	}
	_, err := Open(live, descriptors(t))
	if err == nil {
		t.Fatal("Open accepted a LIVE store as a snapshot")
	}
	if !strings.Contains(err.Error(), "LIVE store") {
		t.Errorf("refused for the wrong reason: %v", err)
	}
}

// TestOpenLiveIsBoundedAndReadOnly: mode=ro is a promise SQLite keeps, so the
// write must fail — and the binding must also be structurally bounded, since the
// promise is not the guard.
func TestOpenLiveIsBoundedAndReadOnly(t *testing.T) {
	_, live := storeWithContent(t)
	g, err := OpenLive(live, descriptors(t))
	if err != nil {
		t.Fatalf("OpenLive: %v", err)
	}
	defer g.Close()

	if g.SnapshotMs() != 0 {
		t.Error("live binding claims a snapshot time; live means now")
	}
	if _, err := g.r.Exec("DELETE FROM cells"); err == nil {
		t.Error("mode=ro accepted a write")
	}
	tables := g.BaseTables()
	if contains(tables, "events") {
		t.Errorf("live binding exposes events (1.7 M rows on the real store): %v", tables)
	}
	if !contains(tables, "cells") || !contains(tables, "nodes") || !contains(tables, "exchanges") {
		t.Errorf("live binding is missing a bounded table: %v", tables)
	}
	if g.ExchangeCapMs() != exchangeWindowMs {
		t.Errorf("exchanges cap = %d, want %d", g.ExchangeCapMs(), exchangeWindowMs)
	}
	// snap is the unbounded one, and it is where a long history query belongs.
	sg, err := Open(snapshotOf(t, mustStore(t, live)), descriptors(t))
	if err != nil {
		t.Fatalf("Open snapshot: %v", err)
	}
	defer sg.Close()
	if !contains(sg.BaseTables(), "events") {
		t.Error("snap binding does not offer events")
	}
	if sg.ExchangeCapMs() != 0 {
		t.Error("snap binding caps exchanges; all history is the point of it")
	}
}

func mustStore(t *testing.T, path string) *space.Store {
	t.Helper()
	var clock int64 = 1_700_000_000_000
	st, err := space.Open(path, func() int64 { clock += 1_000; return clock })
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { st.Close() })
	return st
}

// TestScratchTopology: what a caller can reach from Q. `DELETE FROM events` is
// not denied — there is no events there to delete, which is the guard §2.2 calls
// unreachable rather than blocked.
func TestScratchTopology(t *testing.T) {
	st, _ := storeWithContent(t)
	snap := snapshotOf(t, st)
	g, err := Open(snap, descriptors(t))
	if err != nil {
		t.Fatal(err)
	}
	defer g.Close()

	s, err := g.newScratch()
	if err != nil {
		t.Fatalf("newScratch: %v", err)
	}
	defer s.Close()

	// W builds; Q reads.
	if _, err := s.w.Exec(`CREATE TABLE sys_metrics (node_id TEXT, lfb INTEGER, gcc INTEGER)`); err != nil {
		t.Fatalf("W create: %v", err)
	}
	if _, err := s.w.Exec(`INSERT INTO sys_metrics VALUES ('n1', 8, 3)`); err != nil {
		t.Fatalf("W insert: %v", err)
	}
	var lfb int
	if err := s.q.QueryRow(`SELECT lfb FROM sys_metrics WHERE node_id = 'n1'`).Scan(&lfb); err != nil {
		t.Fatalf("Q select: %v", err)
	}
	if lfb != 8 {
		t.Errorf("lfb = %d", lfb)
	}

	// The authorizer is also the planner: it reported exactly what was read.
	reads := s.auth.Reads()
	cols := reads["sys_metrics"]
	if !contains(cols, "lfb") || !contains(cols, "node_id") {
		t.Errorf("authorizer did not record the read set: %v", reads)
	}
	if contains(cols, "gcc") {
		t.Errorf("authorizer recorded a column the query never touched: %v", cols)
	}

	// Every store table is UNREACHABLE, not forbidden — the error is "no such
	// table", which is the topology talking and not the authorizer.
	for _, stmt := range []string{
		`DELETE FROM events`,
		`SELECT count(*) FROM events`,
		`SELECT count(*) FROM cells`,
	} {
		_, err := s.q.Exec(stmt)
		if err == nil {
			t.Errorf("%q succeeded from Q", stmt)
			continue
		}
		if !strings.Contains(err.Error(), "no such table") {
			t.Errorf("%q failed for the wrong reason (want no such table): %v", stmt, err)
		}
	}
}

// TestAuthorizerDeniesEverythingThatIsNotARead. ATTACH is the one that would
// otherwise reach back out to the filesystem, which is why the authorizer is not
// optional even given the topology — so it is tested against the real snapshot
// path, not a placeholder.
func TestAuthorizerDeniesEverythingThatIsNotARead(t *testing.T) {
	st, _ := storeWithContent(t)
	snap := snapshotOf(t, st)
	g, err := Open(snap, descriptors(t))
	if err != nil {
		t.Fatal(err)
	}
	defer g.Close()
	s, err := g.newScratch()
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	if _, err := s.w.Exec(`CREATE TABLE t (a INTEGER)`); err != nil {
		t.Fatal(err)
	}

	cases := map[string]string{
		"attach the snapshot itself": `ATTACH DATABASE '` + snap + `' AS stolen`,
		"attach anything":            `ATTACH DATABASE ':memory:' AS m`,
		"detach":                     `DETACH DATABASE main`,
		"pragma":                     `PRAGMA journal_mode = delete`,
		"insert":                     `INSERT INTO t VALUES (1)`,
		"update":                     `UPDATE t SET a = 2`,
		"delete":                     `DELETE FROM t`,
		"drop":                       `DROP TABLE t`,
		"create":                     `CREATE TABLE u (b INTEGER)`,
		"create view":                `CREATE VIEW v AS SELECT * FROM t`,
		"alter":                      `ALTER TABLE t ADD COLUMN c INTEGER`,
	}
	for name, stmt := range cases {
		t.Run(name, func(t *testing.T) {
			s.auth.reset()
			if _, err := s.q.Exec(stmt); err == nil {
				t.Fatalf("Q accepted: %s", stmt)
			}
			if d := s.auth.Denied(); len(d) == 0 {
				t.Errorf("statement failed but the authorizer refused nothing — check WHY it failed")
			}
		})
	}

	// A read still works after all that, so the denials are not a wedged conn.
	s.auth.reset()
	var n int
	if err := s.q.QueryRow(`SELECT count(*) FROM t`).Scan(&n); err != nil {
		t.Fatalf("read after denials: %v", err)
	}
	if d := s.auth.Denied(); len(d) != 0 {
		t.Errorf("a plain SELECT was refused: %v", d)
	}
}

// TestQueryOnlyHoldsWithoutTheAuthorizer proves the second layer ALONE. With
// both guards on one connection, a passing test cannot say which one worked —
// and a guard that has never been shown to act by itself is a guard nobody knows
// they have.
func TestQueryOnlyHoldsWithoutTheAuthorizer(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "scratch.db")
	rw, err := sql.Open("sqlite3", "file:"+path)
	if err != nil {
		t.Fatal(err)
	}
	defer rw.Close()
	if _, err := rw.Exec(`CREATE TABLE t (a INTEGER)`); err != nil {
		t.Fatal(err)
	}

	qo, err := sql.Open("sqlite3", "file:"+path+"?_query_only=1")
	if err != nil {
		t.Fatal(err)
	}
	defer qo.Close()
	if _, err := qo.Exec(`INSERT INTO t VALUES (1)`); err == nil {
		t.Fatal("_query_only accepted a write with no authorizer present")
	}
	var n int
	if err := qo.QueryRow(`SELECT count(*) FROM t`).Scan(&n); err != nil {
		t.Fatalf("_query_only refused a read: %v", err)
	}

	// And the mirror, which is why the authorizer is NOT optional given the
	// topology: _query_only does not stop ATTACH, because attaching is not a
	// write. Without the authorizer, a caller reaches back out to the
	// filesystem from a connection that cannot write a byte.
	other := filepath.Join(dir, "other.db")
	rw2, err := sql.Open("sqlite3", "file:"+other)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := rw2.Exec(`CREATE TABLE secret (a INTEGER)`); err != nil {
		t.Fatal(err)
	}
	rw2.Close()
	if _, err := qo.Exec(`ATTACH DATABASE '` + other + `' AS reached`); err != nil {
		t.Fatalf("expected _query_only to ALLOW attach (which is why the authorizer exists); got: %v", err)
	}
	if err := qo.QueryRow(`SELECT count(*) FROM reached.secret`).Scan(&n); err != nil {
		t.Fatalf("attached but could not read, so the hazard is unproven: %v", err)
	}
}

// TestScratchDoesNotOutliveTheQuery: the file going away is what makes a store
// unreachable rather than merely forbidden, so it is asserted rather than
// assumed.
func TestScratchDoesNotOutliveTheQuery(t *testing.T) {
	st, _ := storeWithContent(t)
	g, err := Open(snapshotOf(t, st), descriptors(t))
	if err != nil {
		t.Fatal(err)
	}
	defer g.Close()

	s, err := g.newScratch()
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.w.Exec(`CREATE TABLE t (a INTEGER)`); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(s.path); err != nil {
		t.Fatalf("scratch file missing while the query is live: %v", err)
	}
	dir := s.dir
	if err := s.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
	if _, err := os.Stat(dir); !os.IsNotExist(err) {
		t.Errorf("scratch directory survived the query: %v", err)
	}
}

// TestTwoScratchesDoNotShareRecording: the authorizer is per query, so two
// concurrent callers cannot read each other's plan. Registering a driver name
// globally would have broken exactly this.
func TestTwoScratchesDoNotShareRecording(t *testing.T) {
	st, _ := storeWithContent(t)
	g, err := Open(snapshotOf(t, st), descriptors(t))
	if err != nil {
		t.Fatal(err)
	}
	defer g.Close()

	a, err := g.newScratch()
	if err != nil {
		t.Fatal(err)
	}
	defer a.Close()
	b, err := g.newScratch()
	if err != nil {
		t.Fatal(err)
	}
	defer b.Close()

	if a.path == b.path {
		t.Fatal("two queries share one scratch file")
	}
	if _, err := a.w.Exec(`CREATE TABLE only_in_a (x INTEGER)`); err != nil {
		t.Fatal(err)
	}
	if _, err := a.q.Exec(`SELECT x FROM only_in_a`); err != nil {
		t.Fatalf("A read its own table: %v", err)
	}
	if _, ok := b.auth.Reads()["only_in_a"]; ok {
		t.Error("B's authorizer recorded A's read")
	}
	if _, err := b.q.Exec(`SELECT x FROM only_in_a`); err == nil {
		t.Error("B reached A's table")
	}
}
