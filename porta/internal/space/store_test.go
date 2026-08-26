// Copyright (c) 2026 Ekorau LLC

// internal/space/store_test.go
package space

import (
	"database/sql"
	"os"
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
// node never made (posix without OEVM_BOOTID_FILE; a board without
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

// legacyNodesDDL and legacyExchangesDDL are the shapes as they stood before
// rete#231 D4 deleted the cadence inference — nodes.cadence_ms and exchanges.leg
// existed and were written on every sweep and every request. The nodes DDL is
// copied verbatim INCLUDING its interior comment, because that comment is the
// hazard: sqlite implements DROP COLUMN by re-parsing the stored DDL text, and
// the events table already carries a warning about exactly this. A test that
// spelled the old table without the comment would prove the migration works on
// a store nobody has.
const legacyNodesDDL = `
CREATE TABLE IF NOT EXISTS nodes (
  id            TEXT PRIMARY KEY,
  name          TEXT,
  first_seen_ms INTEGER,
  last_seen_ms  INTEGER,
  cadence_ms    INTEGER,
  boot_id       INTEGER,
  cursor_acked  INTEGER NOT NULL DEFAULT 0,
  node_seq_hw   INTEGER NOT NULL DEFAULT 0,
  out_seq       INTEGER NOT NULL DEFAULT 0,
  -- Successor to v1's nodes.source_addr, which the capability audit (design
  -- §4.2) caught with no home. It is not merely a display field: serverstat
  -- .TransportOf() reads it to produce the wifi-vs-Thread fleet breakdown.
  last_addr     TEXT
);`

const legacyExchangesDDL = `
CREATE TABLE IF NOT EXISTS exchanges (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  node_id    TEXT NOT NULL,
  ts_ms      INTEGER NOT NULL,
  boot_id    INTEGER NOT NULL,
  cursor_in  INTEGER NOT NULL,
  cursor_out INTEGER NOT NULL,
  pushed_lo  INTEGER NOT NULL,
  pushed_hi  INTEGER NOT NULL,
  leg        TEXT NOT NULL DEFAULT ''
);`

// The two dead columns are DROPPED from a store that already has them, and the
// rows they sat beside survive. The live soak store on gw85224-01 is such a
// store; it cannot be rebuilt, so this migration is the deliverable.
//
// The second Open must be a no-op — the columns are gone, sqlite says "no such
// column", and Open tolerates exactly that and nothing else.
func TestOpenDropsTheCadenceColumnsFromAPreExistingStore(t *testing.T) {
	path := t.TempDir() + "/cadence.db"
	db, err := sql.Open("sqlite3", path)
	if err != nil {
		t.Fatalf("sql.Open: %v", err)
	}
	for _, stmt := range []string{legacyNodesDDL, legacyExchangesDDL} {
		if _, err := db.Exec(stmt); err != nil {
			t.Fatalf("legacy DDL: %v", err)
		}
	}
	if _, err := db.Exec(`INSERT INTO nodes (id, first_seen_ms, last_seen_ms, cadence_ms, last_addr)
		VALUES ('n1', 1000, 2000, 2000, 'fdde:ad00::1')`); err != nil {
		t.Fatalf("seed node: %v", err)
	}
	if _, err := db.Exec(`INSERT INTO exchanges (node_id, ts_ms, boot_id, cursor_in, cursor_out, pushed_lo, pushed_hi, leg)
		VALUES ('n1', 2000, 41, 0, 0, 0, 0, 'pull')`); err != nil {
		t.Fatalf("seed exchange: %v", err)
	}
	db.Close()

	for _, pass := range []string{"first", "second"} {
		st, err := Open(path, func() int64 { return 3000 })
		if err != nil {
			t.Fatalf("%s Open of a pre-rete#231 store: %v", pass, err)
		}
		for _, q := range []string{
			`SELECT cadence_ms FROM nodes`,
			`SELECT leg FROM exchanges`,
		} {
			if _, err := st.DB().Exec(q); err == nil {
				t.Fatalf("%s open: %q still works, want the column dropped", pass, q)
			}
		}
		// What sat beside the dropped columns is still there.
		var addr string
		var lastSeen int64
		if err := st.DB().QueryRow(`SELECT last_addr, last_seen_ms FROM nodes WHERE id='n1'`).Scan(&addr, &lastSeen); err != nil {
			t.Fatalf("%s open: read node row: %v", pass, err)
		}
		if addr != "fdde:ad00::1" || lastSeen != 2000 {
			t.Fatalf("%s open: node row = (%q, %d), want the seeded values", pass, addr, lastSeen)
		}
		var n int
		if err := st.DB().QueryRow(`SELECT COUNT(*) FROM exchanges WHERE node_id='n1'`).Scan(&n); err != nil {
			t.Fatalf("%s open: count exchanges: %v", pass, err)
		}
		if n != 1 {
			t.Fatalf("%s open: %d exchange rows, want the seeded one", pass, n)
		}
		st.Close()
	}
}

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

// The property the whole storage choice exists for: a verdict porta formed
// about a node is never handed to that node. If this ever fails, a node is
// being told what porta thinks of it — over a namespace §4.1 says only the
// node may author.
func TestGatewayVerdictNeverRidesTheWire(t *testing.T) {
	st := testStore(t)
	st.Ingest("n1", 41, []Entry{{Seq: 1, Path: "obs/health", Kind: cval.KindCell, Value: enc(t, int64(1))}})
	if err := st.WriteGateway("n1", PathLiveness, enc(t, cval.Symbol("silent")), "test"); err != nil {
		t.Fatalf("WriteGateway: %v", err)
	}
	entries, hi, err := st.OutboundSince("n1", 0, MaxBatchBytes)
	if err != nil {
		t.Fatalf("OutboundSince: %v", err)
	}
	for _, e := range entries {
		if e.Path == PathLiveness {
			t.Fatalf("gw/liveness was delivered to the node: %+v (cursor now %d)", e, hi)
		}
	}
	if len(entries) != 0 {
		t.Fatalf("outbound = %+v, want nothing (the node has no goal writes)", entries)
	}

	// The CELLS query is `direction='out' AND seq > cursor`. With cursor=0 above,
	// seq=0 fails the `seq > 0` check — so the gw row is excluded by seq, not by
	// direction. Prove direction is the actual guard by calling with cursor=-1,
	// which makes `seq > -1` TRUE. The gw row still must not ride; direction='gw'
	// is the only remaining explanation for its absence.
	entries, _, err = st.OutboundSince("n1", -1, MaxBatchBytes)
	if err != nil {
		t.Fatalf("OutboundSince with cursor -1: %v", err)
	}
	for _, e := range entries {
		if e.Path == PathLiveness {
			t.Fatalf("gw/liveness was delivered to the node (cursor -1): %+v; direction filter is not the guard", e)
		}
	}
}

// The verdict is about a node ON A BOOT. Stamping the node's current boot costs
// nothing — the value is already in hand — and it is what lets `porta log`
// answer "which boot was porta calling silent" (rete#210's argument, applied to
// a row porta wrote itself).
func TestGatewayVerdictCarriesTheNodesCurrentBoot(t *testing.T) {
	st := testStore(t)
	st.Ingest("n1", 44, []Entry{{Seq: 1, Path: "obs/health", Kind: cval.KindCell, Value: enc(t, int64(1))}})
	if err := st.WriteGateway("n1", PathLiveness, enc(t, cval.Symbol("silent")), "no exchange"); err != nil {
		t.Fatalf("WriteGateway: %v", err)
	}
	tree, _ := st.Tree("n1", PathLiveness)
	if len(tree) != 1 {
		t.Fatalf("tree = %+v, want the gw row", tree)
	}
	if tree[0].Direction != DirectionGateway || tree[0].BootID != 44 || tree[0].Seq != 0 {
		t.Fatalf("gw row = %+v, want direction=gw boot=44 seq=0", tree[0])
	}
	events, _ := st.Events("n1", 10, 0)
	var found *Event
	for i := range events {
		if events[i].Path == PathLiveness {
			found = &events[i]
		}
	}
	if found == nil {
		t.Fatal("no event row for the verdict; a transition that leaves no log line is not a record")
	}
	if found.Direction != DirectionGateway || found.Actor != "porta" || found.Reason != "no exchange" {
		t.Fatalf("event = %+v, want direction=gw actor=porta reason='no exchange'", *found)
	}
}

// porta's own outbound counter is the node's cursor space. Burning a seq on a
// row the node can never receive would put a permanent hole in that cursor.
func TestGatewayWriteDoesNotConsumeTheOutboundSeq(t *testing.T) {
	st := testStore(t)
	st.WriteGateway("n1", PathLiveness, enc(t, cval.Symbol("live")), "test")
	seq, err := st.Write("n1", "goal/name", cval.KindCell, enc(t, "alpha"), "david", "")
	if err != nil {
		t.Fatalf("Write: %v", err)
	}
	if seq != 1 {
		t.Fatalf("first goal write got seq %d, want 1 — the gw row consumed one", seq)
	}
}

// storeAtClock hands the test the pen. *clock IS the store's now, so a silence
// is produced by moving time rather than by sleeping — a sweep test that slept
// would be measuring the test runner.
func storeAtClock(t *testing.T, clock *int64) *Store {
	t.Helper()
	st, err := Open(t.TempDir()+"/t.db", func() int64 { return *clock })
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	t.Cleanup(func() { st.Close() })
	return st
}

// checkIn advances the clock by gap ms and records ONE CHECK-IN, the way a
// real node's does: a pull followed by a push (internal/wire/server.go calls
// RecordExchange once from each handler). Still two rows and not one after
// rete#231 D4 deleted the leg column — the count is what a real check-in
// leaves, and a helper that models a protocol the gateway does not implement
// is how #217 C1 survived here in the first place.
func checkIn(t *testing.T, st *Store, clock *int64, node string, gap int64) {
	t.Helper()
	*clock += gap
	if err := st.RecordExchange(node, 41, 0, 0, 0, 0, ""); err != nil {
		t.Fatalf("RecordExchange pull: %v", err)
	}
	if err := st.RecordExchange(node, 41, 0, 0, 0, 0, ""); err != nil {
		t.Fatalf("RecordExchange push: %v", err)
	}
}

// rete#217 I3: gw/** is gateway-authored (WriteGateway) and must never be
// reachable through Store.Write, which is the path a node write actually
// takes (OutboundSince delivers everything stamped direction='out'). Without
// this rejection, `porta put <node> gw/liveness live` reaches the node — the
// one thing the third `direction` value exists to make structurally
// impossible.
func TestWriteRejectsTheGwPrefix(t *testing.T) {
	st := testStore(t)
	_, err := st.Write("n1", "gw/liveness", cval.KindCell, enc(t, cval.Symbol("live")), "operator", "")
	if err == nil {
		t.Fatal("Write accepted a gw/ path; a node write must never reach gateway-authored state")
	}
	// Nothing must have been written or staged: a rejected write costs the
	// node no outbound seq and leaves no half-applied row behind.
	tree, _ := st.Tree("n1", "")
	if len(tree) != 0 {
		t.Fatalf("tree = %+v after a rejected gw/ write, want nothing written", tree)
	}
}

// The budget porta judges against comes from the NODE, and a node that has not
// declared one yields no budget rather than a guess. It is the same `ok` shape
// the deleted CadenceMs had, for the opposite reason: there is now nothing to
// infer from, and nothing that wants to.
func TestDeclaredBudgetReadsTheNodesOwnRecord(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)

	if _, ok, err := st.DeclaredBudget("n1"); err != nil || ok {
		t.Fatalf("no declaration: ok = %v, err = %v, want false, nil", ok, err)
	}

	rec, err := cval.Encode(cval.Record{Tag: "Liveness", Fields: cval.Map{
		{K: cval.Symbol("cad"), V: int64(2_000)},
		{K: cval.Symbol("bud"), V: int64(16_000)},
	}})
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, _, err := st.Ingest("n1", 1, []Entry{
		{Seq: 1, Path: PathDeclared, Kind: cval.KindCell, Value: rec},
	}); err != nil {
		t.Fatalf("Ingest: %v", err)
	}
	bud, ok, err := st.DeclaredBudget("n1")
	if err != nil || !ok || bud != 16_000 {
		t.Fatalf("DeclaredBudget = (%d, %v, %v), want (16000, true, nil)", bud, ok, err)
	}
}

// A record porta cannot read is NOT an error that stops the sweep: it is one node
// with no budget, and the honest answer is no verdict for that node. A gateway an
// unreadable value can take down is worse than no gateway.
func TestDeclaredBudgetSurvivesAGarbledDeclaration(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	for i, tc := range []struct {
		name string
		v    any
	}{
		{"not a record", cval.Symbol("live")},
		{"wrong tag", cval.Record{Tag: "Health", Fields: cval.Map{{K: cval.Symbol("bud"), V: int64(1)}}}},
		{"no bud field", cval.Record{Tag: "Liveness", Fields: cval.Map{{K: cval.Symbol("cad"), V: int64(2_000)}}}},
		{"bud is not an int", cval.Record{Tag: "Liveness", Fields: cval.Map{{K: cval.Symbol("bud"), V: "soon"}}}},
		{"bud is not positive", cval.Record{Tag: "Liveness", Fields: cval.Map{{K: cval.Symbol("bud"), V: int64(0)}}}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			b, err := cval.Encode(tc.v)
			if err != nil {
				t.Fatalf("encode: %v", err)
			}
			// Ascending seq per case: Ingest skips at-or-below the high-water,
			// so a fixed seq would silently test the FIRST case five times —
			// and every case expects the same answer, so the skip would be
			// invisible. `skipped` is asserted for exactly that reason.
			_, skipped, err := st.Ingest("n1", 1, []Entry{
				{Seq: int64(i + 1), Path: PathDeclared, Kind: cval.KindCell, Value: b},
			})
			if err != nil || skipped != 0 {
				t.Fatalf("Ingest: skipped = %d, err = %v; this case never reached the store", skipped, err)
			}
			if _, ok, err := st.DeclaredBudget("n1"); err != nil || ok {
				t.Fatalf("ok = %v, err = %v, want false, nil", ok, err)
			}
		})
	}
}

// OpenExisting is the operator-verb door (rete#221): sqlite creates on open,
// so a wrong path handed to Open answers from a fresh empty store — output
// byte-identical to "no rows yet". The read door must refuse instead, naming
// the path, and must leave no decoy file behind.
func TestOpenExistingRefusesAMissingStore(t *testing.T) {
	missing := t.TempDir() + "/nope.db"
	st, err := OpenExisting(missing, func() int64 { return 1000 })
	if err == nil {
		st.Close()
		t.Fatal("OpenExisting on a missing path succeeded")
	}
	if !strings.Contains(err.Error(), missing) {
		t.Errorf("error does not name the path: %v", err)
	}
	if _, statErr := os.Stat(missing); !os.IsNotExist(statErr) {
		t.Errorf("a decoy store was created at %s", missing)
	}
}

func TestOpenExistingOpensAStoreServeCreated(t *testing.T) {
	path := t.TempDir() + "/t.db"
	st, err := Open(path, func() int64 { return 1000 })
	if err != nil {
		t.Fatal(err)
	}
	if _, err := st.Write("n1", "goal/name", cval.KindCell, enc(t, "alpha"), "david", ""); err != nil {
		t.Fatal(err)
	}
	st.Close()

	st2, err := OpenExisting(path, func() int64 { return 2000 })
	if err != nil {
		t.Fatalf("OpenExisting on a real store: %v", err)
	}
	defer st2.Close()
	rows, err := st2.Tree("n1", "")
	if err != nil || len(rows) != 1 {
		t.Fatalf("Tree = %v rows, err %v; want the row serve wrote", len(rows), err)
	}
}
