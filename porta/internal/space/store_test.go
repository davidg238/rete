// Copyright (c) 2026 Ekorau LLC

// internal/space/store_test.go
package space

import (
	"database/sql"
	"strings"
	"testing"

	"github.com/davidg238/porta/internal/cval"
	_ "github.com/mattn/go-sqlite3"
)

func testStore(t *testing.T) *Store {
	t.Helper()
	var clock int64 = 1_000
	st, err := Open(t.TempDir()+"/t.db", func() int64 { clock += 10; return clock })
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	t.Cleanup(func() { st.Close() })
	return st
}

func enc(t *testing.T, v any) []byte {
	t.Helper()
	b, err := cval.Encode(v)
	if err != nil {
		t.Fatalf("Encode: %v", err)
	}
	return b
}

// The outbound seq is per-node and monotonic (design D5).
func TestWriteAssignsMonotonicSeqPerNode(t *testing.T) {
	st := testStore(t)
	s1, _ := st.Write("n1", "goal/name", cval.KindCell, enc(t, "alpha"), "david", "")
	s2, _ := st.Write("n1", "goal/mode", cval.KindCell, enc(t, "fast"), "david", "")
	s3, _ := st.Write("n2", "goal/name", cval.KindCell, enc(t, "beta"), "david", "")
	if s1 != 1 || s2 != 2 {
		t.Fatalf("n1 seqs = %d,%d want 1,2", s1, s2)
	}
	if s3 != 1 {
		t.Fatalf("n2 seq = %d want 1 (per-node counter)", s3)
	}
}

// A superseded cell write is never delivered: the pull carries the CURRENT
// value only. This is what bounds a cursor-0 replay by tree size rather than
// by history length.
func TestOutboundCollapsesSupersededCellWrites(t *testing.T) {
	st := testStore(t)
	st.Write("n1", "goal/name", cval.KindCell, enc(t, "first"), "david", "")
	st.Write("n1", "goal/name", cval.KindCell, enc(t, "second"), "david", "")
	got, hi, err := st.OutboundSince("n1", 0, MaxBatchBytes)
	if err != nil {
		t.Fatalf("OutboundSince: %v", err)
	}
	if len(got) != 1 {
		t.Fatalf("got %d entries, want 1 (superseded write must not ride)", len(got))
	}
	v, _ := cval.Decode(got[0].Value)
	if v != "second" {
		t.Fatalf("got %v, want \"second\"", v)
	}
	if hi != 2 {
		t.Fatalf("hi = %d, want 2", hi)
	}
}

// Stream entries are delivered once each, in order — never collapsed.
func TestOutboundKeepsEveryStreamEntry(t *testing.T) {
	st := testStore(t)
	st.Write("n1", "goal/do/reboot", cval.KindStream, enc(t, cval.Record{Tag: "Reboot"}), "david", "")
	st.Write("n1", "goal/do/reboot", cval.KindStream, enc(t, cval.Record{Tag: "Reboot"}), "david", "")
	got, _, _ := st.OutboundSince("n1", 0, MaxBatchBytes)
	if len(got) != 2 {
		t.Fatalf("got %d stream entries, want 2", len(got))
	}
}

func TestOutboundRespectsCursor(t *testing.T) {
	st := testStore(t)
	st.Write("n1", "goal/a", cval.KindCell, enc(t, int64(1)), "david", "")
	st.Write("n1", "goal/b", cval.KindCell, enc(t, int64(2)), "david", "")
	got, _, _ := st.OutboundSince("n1", 1, MaxBatchBytes)
	if len(got) != 1 || got[0].Path != "goal/b" {
		t.Fatalf("got %+v, want only goal/b", got)
	}
}

// The batch cap is a radio constraint, not a preference (RECORDS §2.2).
func TestOutboundStopsAtBatchCap(t *testing.T) {
	st := testStore(t)
	big := enc(t, string(make([]byte, 200)))
	for _, p := range []string{"goal/a", "goal/b", "goal/c", "goal/d"} {
		st.Write("n1", p, cval.KindCell, big, "david", "")
	}
	got, hi, _ := st.OutboundSince("n1", 0, MaxBatchBytes)
	if len(got) >= 4 {
		t.Fatalf("got %d entries, want a partial batch under the cap", len(got))
	}
	if hi != got[len(got)-1].Seq {
		t.Fatalf("hi = %d, want the last delivered seq %d", hi, got[len(got)-1].Seq)
	}
}

// THE ECHO GUARD. cells holds BOTH directions — it has to, because the
// operator's goal/X vs obs/X diff is one query over one table. But that means
// an unfiltered "SELECT FROM cells WHERE seq > cursor" hands the node back its
// own obs/** cells, stamped with seqs from the node's push counter rather than
// porta's out_seq — so the reply corrupts the very cursor it answers.
func TestOutboundNeverEchoesIngestedCells(t *testing.T) {
	st := testStore(t)
	st.Write("n1", "goal/name", cval.KindCell, enc(t, "alpha"), "david", "")
	if _, _, err := st.Ingest("n1", 7, []Entry{
		{Seq: 1, Path: "obs/name", Kind: cval.KindCell, Value: enc(t, "alpha")},
		{Seq: 2, Path: "obs/health", Kind: cval.KindCell, Value: enc(t, int64(99))},
	}); err != nil {
		t.Fatalf("Ingest: %v", err)
	}
	got, _, err := st.OutboundSince("n1", 0, MaxBatchBytes)
	if err != nil {
		t.Fatalf("OutboundSince: %v", err)
	}
	for _, e := range got {
		if e.Path != "goal/name" {
			t.Fatalf("outbound batch carries %q, which the node itself pushed", e.Path)
		}
	}
	if len(got) != 1 {
		t.Fatalf("got %d entries, want exactly the 1 porta wrote", len(got))
	}
	// Both directions must still be visible to the operator's diff.
	if tree, _ := st.Tree("n1", ""); len(tree) != 3 {
		t.Fatalf("tree has %d rows, want 3 (goal/name + both obs cells)", len(tree))
	}
}

// A push replayed after loss must not double-apply (dedup on high-water).
// The skip count is asserted too: it is what makes the dedupe observable
// rather than provable only by the absence of a duplicate.
func TestIngestDedupesOnHighWater(t *testing.T) {
	st := testStore(t)
	batch := []Entry{
		{Seq: 1, Path: "tel/log", Kind: cval.KindStream, Value: enc(t, "one")},
		{Seq: 2, Path: "tel/log", Kind: cval.KindStream, Value: enc(t, "two")},
	}
	hw, skipped, err := st.Ingest("n1", 7, batch)
	if err != nil || hw != 2 || skipped != 0 {
		t.Fatalf("Ingest = %d, %d, %v; want 2, 0, nil", hw, skipped, err)
	}
	if hw, skipped, err = st.Ingest("n1", 7, batch); hw != 2 || skipped != 2 || err != nil {
		t.Fatalf("replayed Ingest = %d, %d, %v; want 2, 2, nil", hw, skipped, err)
	}
	evs, _ := st.Events("n1", 100, 0)
	if len(evs) != 2 {
		t.Fatalf("got %d events after replay, want 2", len(evs))
	}
}

// Provenance is the thing v1's issued_by carried and §5.1 forgot (design D3).
func TestEventsCarryProvenance(t *testing.T) {
	st := testStore(t)
	st.Write("n1", "goal/name", cval.KindCell, enc(t, "alpha"), "agent-7", "rollout")
	evs, _ := st.Events("n1", 10, 0)
	if len(evs) != 1 {
		t.Fatalf("got %d events, want 1", len(evs))
	}
	if evs[0].Actor != "agent-7" || evs[0].Reason != "rollout" || evs[0].Direction != "out" {
		t.Fatalf("provenance lost: %+v", evs[0])
	}
}

// Superseded intent survives in the log even though the cell shows only the
// latest value — the property a drained command queue could not offer.
func TestSupersededIntentSurvivesInTheLog(t *testing.T) {
	st := testStore(t)
	st.Write("n1", "goal/mode", cval.KindCell, enc(t, "X"), "david", "")
	st.Write("n1", "goal/mode", cval.KindCell, enc(t, "Y"), "david", "")
	tree, _ := st.Tree("n1", "goal/")
	if len(tree) != 1 {
		t.Fatalf("tree has %d rows, want 1", len(tree))
	}
	evs, _ := st.Events("n1", 10, 0)
	if len(evs) != 2 {
		t.Fatalf("log has %d rows, want 2 (X must still be visible)", len(evs))
	}
}

func TestOversizeValueRejected(t *testing.T) {
	st := testStore(t)
	_, err := st.Write("n1", "goal/x", cval.KindCell, make([]byte, MaxValueBytes+1), "david", "")
	if err == nil {
		t.Fatal("oversize value accepted; the 256 B cap is a radio constraint")
	}
}

func TestExchangeRowRecorded(t *testing.T) {
	st := testStore(t)
	if err := st.RecordExchange("n1", 7, 0, 3, 1, 2, "fdde:ad00::1"); err != nil {
		t.Fatalf("RecordExchange: %v", err)
	}
	if n, _ := st.CountNodes(); n != 1 {
		t.Fatalf("CountNodes = %d, want 1", n)
	}
}

// The peer address is v1's nodes.source_addr, which the capability audit
// (design §4.2) caught with no successor. serverstat.TransportOf() reads it to
// produce the wifi-vs-Thread breakdown, so losing it loses a fleet view, not a
// display field.
func TestExchangeRecordsPeerAddress(t *testing.T) {
	st := testStore(t)
	st.RecordExchange("n1", 7, 0, 3, 1, 2, "fdde:ad00::1")
	got, err := st.NodeAddr("n1")
	if err != nil {
		t.Fatalf("NodeAddr: %v", err)
	}
	if got != "fdde:ad00::1" {
		t.Fatalf("NodeAddr = %q, want fdde:ad00::1", got)
	}
}

// An exchange recorded without a peer address must not erase a known one.
func TestEmptyPeerAddressDoesNotClobber(t *testing.T) {
	st := testStore(t)
	st.RecordExchange("n1", 7, 0, 3, 1, 2, "fdde:ad00::1")
	st.RecordExchange("n1", 7, 3, 4, 0, 0, "")
	if got, _ := st.NodeAddr("n1"); got != "fdde:ad00::1" {
		t.Fatalf("NodeAddr = %q after an addr-less exchange, want the address kept", got)
	}
}

// THE 2026-08-14 READING, REPRODUCED IN THE STORE. A rebooted node rewrites its
// whole tree, but the rows arrive over several seconds — measured at 11 s for
// sixteen diag/spans rows, which begins AFTER the target harness's console
// verdict has returned. A read taken inside that window is a mixture of two
// boots, and before boot_id nothing in a row could say which was which: it read
// as a coherent answer and nearly inverted a hardware finding (rete#210/#161).
func TestTreeReportsWhichBootPublishedEachCell(t *testing.T) {
	st := testStore(t)
	if _, _, err := st.Ingest("n1", 40, []Entry{
		{Seq: 1, Path: "diag/spans/pushTrim", Kind: cval.KindCell, Value: enc(t, int64(51923))},
		{Seq: 2, Path: "diag/spans/checkIn", Kind: cval.KindCell, Value: enc(t, int64(243307))},
	}); err != nil {
		t.Fatalf("boot 40 Ingest: %v", err)
	}
	// The node reboots and has republished only part of its tree so far.
	if _, _, err := st.Ingest("n1", 41, []Entry{
		{Seq: 1, Path: "diag/spans/checkIn", Kind: cval.KindCell, Value: enc(t, int64(235037))},
	}); err != nil {
		t.Fatalf("boot 41 Ingest: %v", err)
	}
	boot, err := st.CurrentBoot("n1")
	if err != nil || boot != 41 {
		t.Fatalf("CurrentBoot = %d, %v; want 41, nil", boot, err)
	}
	tree, err := st.Tree("n1", "diag/")
	if err != nil {
		t.Fatalf("Tree: %v", err)
	}
	got := map[string]int64{}
	for _, c := range tree {
		got[c.Path] = c.BootID
	}
	if got["diag/spans/checkIn"] != 41 {
		t.Fatalf("checkIn boot = %d, want 41 (republished after the reboot)", got["diag/spans/checkIn"])
	}
	if got["diag/spans/pushTrim"] != 40 {
		t.Fatalf("pushTrim boot = %d, want 40 — the previous boot's value, which is exactly the row that read as fresh", got["diag/spans/pushTrim"])
	}
}

// The log can reconstruct one boot; the cell tree cannot, which is why --boot
// lives here and not on get.
func TestEventsFilterByBoot(t *testing.T) {
	st := testStore(t)
	st.Ingest("n1", 40, []Entry{{Seq: 1, Path: "tel/log", Kind: cval.KindStream, Value: enc(t, "old")}})
	st.Ingest("n1", 41, []Entry{{Seq: 1, Path: "tel/log", Kind: cval.KindStream, Value: enc(t, "new")}})
	all, _ := st.Events("n1", 100, 0)
	if len(all) != 2 {
		t.Fatalf("unfiltered log has %d rows, want 2", len(all))
	}
	one, _ := st.Events("n1", 100, 41)
	if len(one) != 1 || one[0].BootID != 41 {
		t.Fatalf("--boot 41 returned %+v, want exactly the boot-41 row", one)
	}
}

// porta's own writes carry no node boot, and must never be reported as stale:
// a node reboot does not invalidate the operator's intent.
func TestOutboundRowsCarryNoBoot(t *testing.T) {
	st := testStore(t)
	st.Write("n1", "goal/name", cval.KindCell, enc(t, "alpha"), "david", "")
	st.Ingest("n1", 41, []Entry{{Seq: 1, Path: "obs/name", Kind: cval.KindCell, Value: enc(t, "alpha")}})
	tree, _ := st.Tree("n1", "")
	for _, c := range tree {
		switch c.Path {
		case "goal/name":
			if c.Direction != "out" || c.BootID != 0 {
				t.Fatalf("goal/name = dir %q boot %d; want out, 0", c.Direction, c.BootID)
			}
		case "obs/name":
			if c.Direction != "in" || c.BootID != 41 {
				t.Fatalf("obs/name = dir %q boot %d; want in, 41", c.Direction, c.BootID)
			}
		}
	}
}

// A node with no persistent identity pushes bid 0. That must read as UNKNOWN
// and never as a boot number, or the guard would fabricate a distinction the
// node never made (posix without TUVM_BOOTID_FILE; a board without
// CONFIG_SETTINGS returns 0 from platform_backend_boot_id).
func TestBootlessNodeStoresZeroRatherThanInventingOne(t *testing.T) {
	st := testStore(t)
	st.Ingest("n1", 0, []Entry{{Seq: 1, Path: "obs/health", Kind: cval.KindCell, Value: enc(t, int64(1))}})
	tree, _ := st.Tree("n1", "")
	if len(tree) != 1 || tree[0].BootID != 0 {
		t.Fatalf("tree = %+v, want one row with boot 0", tree)
	}
	boot, _ := st.CurrentBoot("n1")
	if boot != 0 {
		t.Fatalf("CurrentBoot = %d, want 0", boot)
	}
}

// A store created before boot_id existed must gain the column on open, and a
// second open must be a no-op. Without this the live soak store would have to
// be rebuilt, costing the history rete#190's evidence rests on and forcing every
// node through a full re-push.
func TestOpenAddsBootIDToAPreExistingStore(t *testing.T) {
	path := t.TempDir() + "/old.db"
	st, err := Open(path, func() int64 { return 1000 })
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	// Recreate the pre-boot_id shape by dropping the columns back off.
	for _, tbl := range []string{"cells", "events"} {
		if _, err := st.DB().Exec("ALTER TABLE " + tbl + " DROP COLUMN boot_id"); err != nil {
			t.Fatalf("drop %s.boot_id: %v", tbl, err)
		}
	}
	st.Close()

	st2, err := Open(path, func() int64 { return 2000 })
	if err != nil {
		t.Fatalf("reopen of a pre-boot_id store: %v", err)
	}
	defer st2.Close()
	if _, _, err := st2.Ingest("n1", 41, []Entry{
		{Seq: 1, Path: "obs/health", Kind: cval.KindCell, Value: enc(t, int64(7))},
	}); err != nil {
		t.Fatalf("Ingest after migration: %v", err)
	}
	tree, _ := st2.Tree("n1", "")
	if len(tree) != 1 || tree[0].BootID != 41 {
		t.Fatalf("tree = %+v, want the migrated column carrying boot 41", tree)
	}
}

// legacyDDL is the pre-gw shape, copied verbatim from the schema as it stood
// before this rung. It is spelled out here rather than derived, because a test
// that builds the old store from the NEW schema text proves nothing about the
// store actually running on gw85224-01.
const legacyCellsDDL = `
CREATE TABLE IF NOT EXISTS cells (
  node_id    TEXT NOT NULL,
  path       TEXT NOT NULL,
  seq        INTEGER NOT NULL,
  kind       INTEGER NOT NULL,
  value_cbor BLOB NOT NULL,
  ts_ms      INTEGER NOT NULL,
  direction  TEXT NOT NULL CHECK (direction IN ('in','out')),
  boot_id    INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (node_id, path)
);`

const legacyEventsDDL = `
CREATE TABLE IF NOT EXISTS events (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  node_id    TEXT NOT NULL,
  path       TEXT NOT NULL,
  seq        INTEGER NOT NULL,
  kind       INTEGER NOT NULL,
  value_cbor BLOB NOT NULL,
  ts_ms      INTEGER NOT NULL,
  direction  TEXT NOT NULL CHECK (direction IN ('in','out')),
  actor      TEXT NOT NULL DEFAULT '',
  reason     TEXT NOT NULL DEFAULT '',
  boot_id    INTEGER NOT NULL DEFAULT 0
);`

// A store created before gw/** existed must GAIN the third direction on open,
// keeping every row it already held. The live soak store on gw85224-01 is such
// a store, and it carries the history rete#190's evidence rests on — rebuilding
// it is not an option, so the migration is the deliverable, not a convenience.
func TestOpenMigratesTheDirectionCheckOnAPreExistingStore(t *testing.T) {
	path := t.TempDir() + "/legacy.db"
	db, err := sql.Open("sqlite3", path)
	if err != nil {
		t.Fatalf("sql.Open: %v", err)
	}
	for _, stmt := range []string{legacyCellsDDL, legacyEventsDDL} {
		if _, err := db.Exec(stmt); err != nil {
			t.Fatalf("legacy DDL: %v", err)
		}
	}
	// One row of each, so the migration has something to lose.
	if _, err := db.Exec(`INSERT INTO cells VALUES ('n1','obs/health',7,2,x'01',1000,'in',41)`); err != nil {
		t.Fatalf("seed cell: %v", err)
	}
	if _, err := db.Exec(`INSERT INTO events (node_id,path,seq,kind,value_cbor,ts_ms,direction,actor,reason,boot_id)
		VALUES ('n1','obs/health',7,2,x'01',1000,'in','n1','',41)`); err != nil {
		t.Fatalf("seed event: %v", err)
	}
	db.Close()

	st, err := Open(path, func() int64 { return 2000 })
	if err != nil {
		t.Fatalf("reopen of a pre-gw store: %v", err)
	}
	defer st.Close()

	// The rows survived.
	var n int
	if err := st.DB().QueryRow(`SELECT COUNT(*) FROM cells WHERE node_id='n1' AND boot_id=41`).Scan(&n); err != nil || n != 1 {
		t.Fatalf("cells after migration: n=%d err=%v, want the seeded row intact", n, err)
	}
	if err := st.DB().QueryRow(`SELECT COUNT(*) FROM events WHERE node_id='n1' AND boot_id=41`).Scan(&n); err != nil || n != 1 {
		t.Fatalf("events after migration: n=%d err=%v, want the seeded row intact", n, err)
	}
	// The index the rebuild dropped with the old table is back.
	if err := st.DB().QueryRow(
		`SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='idx_cells_seq'`).Scan(&n); err != nil || n != 1 {
		t.Fatalf("idx_cells_seq after migration: n=%d err=%v, want 1", n, err)
	}
	// And the third direction is now legal in both tables.
	if _, err := st.DB().Exec(
		`INSERT INTO cells VALUES ('n1','gw/liveness',0,2,x'02',2000,'gw',41)`); err != nil {
		t.Fatalf("gw cell after migration: %v", err)
	}
	if _, err := st.DB().Exec(`INSERT INTO events (node_id,path,seq,kind,value_cbor,ts_ms,direction,actor,reason,boot_id)
		VALUES ('n1','gw/liveness',0,2,x'02',2000,'gw','porta','',41)`); err != nil {
		t.Fatalf("gw event after migration: %v", err)
	}

	// The live gateway restarts, so a second open must be a true no-op: rows
	// survive, indexes are present, and the migration does not run again.
	st.Close()

	st2, err := Open(path, func() int64 { return 3000 })
	if err != nil {
		t.Fatalf("second open of migrated store: %v", err)
	}
	defer st2.Close()

	// The seeded rows from the legacy schema survived the migration and the close/reopen.
	if err := st2.DB().QueryRow(`SELECT COUNT(*) FROM cells WHERE node_id='n1' AND boot_id=41 AND path='obs/health'`).Scan(&n); err != nil || n != 1 {
		t.Fatalf("seeded cell after reopen: n=%d err=%v, want the original row intact", n, err)
	}
	if err := st2.DB().QueryRow(`SELECT COUNT(*) FROM events WHERE node_id='n1' AND boot_id=41 AND path='obs/health'`).Scan(&n); err != nil || n != 1 {
		t.Fatalf("seeded event after reopen: n=%d err=%v, want the original row intact", n, err)
	}

	// The rows added after the first migration survived the close/reopen.
	if err := st2.DB().QueryRow(`SELECT COUNT(*) FROM cells WHERE node_id='n1' AND direction='gw' AND path='gw/liveness'`).Scan(&n); err != nil || n != 1 {
		t.Fatalf("gw cell after reopen: n=%d err=%v, want the post-migration row intact", n, err)
	}
	if err := st2.DB().QueryRow(`SELECT COUNT(*) FROM events WHERE node_id='n1' AND direction='gw' AND path='gw/liveness'`).Scan(&n); err != nil || n != 1 {
		t.Fatalf("gw event after reopen: n=%d err=%v, want the post-migration row intact", n, err)
	}

	// The second open did not rebuild: the DDL still has exactly one 'gw' mention
	// (in the CHECK constraint), not multiple rebuilds' worth.
	var ddl string
	if err := st2.DB().QueryRow(
		`SELECT sql FROM sqlite_master WHERE type='table' AND name='cells'`).Scan(&ddl); err != nil {
		t.Fatalf("read cells DDL after reopen: %v", err)
	}
	if strings.Count(ddl, "'gw'") != 1 {
		t.Fatalf("cells DDL after reopen carries %d 'gw' mentions, want exactly 1 (no rebuild):\n%s",
			strings.Count(ddl, "'gw'"), ddl)
	}

	// And the third direction still works.
	if _, err := st2.DB().Exec(
		`INSERT INTO cells VALUES ('n1','gw/verdict',0,2,x'03',3000,'gw',41)`); err != nil {
		t.Fatalf("another gw cell after reopen: %v", err)
	}
}

// The second open must be a no-op: the migration is detected by the table's own
// DDL, not by a version row, so re-running it would rebuild the table on every
// start of the gateway.
func TestDirectionMigrationIsANoOpOnASecondOpen(t *testing.T) {
	path := t.TempDir() + "/twice.db"
	st, err := Open(path, func() int64 { return 1000 })
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	st.Ingest("n1", 41, []Entry{{Seq: 1, Path: "obs/health", Kind: cval.KindCell, Value: enc(t, int64(1))}})
	st.Close()

	st2, err := Open(path, func() int64 { return 2000 })
	if err != nil {
		t.Fatalf("second Open: %v", err)
	}
	defer st2.Close()
	var ddl string
	if err := st2.DB().QueryRow(
		`SELECT sql FROM sqlite_master WHERE type='table' AND name='cells'`).Scan(&ddl); err != nil {
		t.Fatalf("read cells DDL: %v", err)
	}
	if strings.Count(ddl, "'gw'") != 1 {
		t.Fatalf("cells DDL carries %d 'gw' mentions, want exactly 1:\n%s", strings.Count(ddl, "'gw'"), ddl)
	}
	tree, _ := st2.Tree("n1", "")
	if len(tree) != 1 {
		t.Fatalf("tree = %+v, want the row from before the second open", tree)
	}
}
