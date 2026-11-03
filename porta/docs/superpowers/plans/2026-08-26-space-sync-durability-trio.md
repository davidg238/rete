# Space-Sync Durability Trio Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make cold-boot replay-from-0 safe for streams: an acked stream row is never served again, so `goal/do/*` and `dbg/req` rows cannot re-execute across reboots (#119, #262), and #117's cursor half closes as deliberately-RAM.

**Architecture:** Porta-only. One new monotonic column `nodes.cursor_hw` (the max cursor the node has ever acked as applied), written by `NoteCheckIn` under a clamp against `out_seq`, read by `OutboundSince`'s stream half. No wire change, no node change, no new tables. Then the protocol documents are amended so the contract states what the code enforces.

**Tech Stack:** Go 1.26, sqlite via `github.com/mattn/go-sqlite3`. All work under `porta/` plus two files under `protocol/`.

**Spec:** `porta/docs/design/2026-08-26-space-sync-durability-trio-design.md` (approved 2026-08-26). Read it first — tasks cite its decisions as D1–D5.

## Global Constraints

- Working directory for all Go commands: `~/workspace/rete/porta`. `go test ./...` must pass and `gofmt -l .` must print nothing before every commit.
- No new Go files are created by this plan; the copyright-header rule (`// Copyright (c) 2026 Ekorau LLC`) therefore never triggers, but applies if you do create one.
- In `schema.go`: **keep column comments ABOVE the CREATE TABLE statement, never inside the parentheses** — sqlite's DROP COLUMN rewrites the stored DDL and an interior comment makes it unparseable (the file says this twice; it was found by the migration test).
- The `addColumns` swallow pattern is exact: only a `duplicate column name` error is discarded; anything else must fail Open.
- Commit messages end with:

  ```
  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01H4VVGBQHafDXek3BsFchPv
  ```
- Five files unrelated to this plan are already dirty in the working tree (`docs/next-steps-overview.md`, `docs/status/dev-status.html`, three porta gatekeeper files). **Never `git add -A` or `git add .`** — stage only the files each task names.

---

### Task 1: `cursor_hw` column and the `NoteCheckIn` clamp (D1 schema + D2 writer)

**Files:**
- Modify: `porta/internal/space/schema.go` (nodes CREATE TABLE ~line 18–31; `addColumns` ~line 173)
- Modify: `porta/internal/space/store.go` (`NoteCheckIn`, the final UPDATE ~line 547)
- Test: `porta/internal/space/store_test.go`

**Interfaces:**
- Consumes: existing `Store.Write(nodeID, path string, kind cval.Kind, value []byte, actor, reason string) (int64, error)`, `Store.NoteCheckIn(nodeID string, cursor, nodeHW, bootID int64) (restartedFrom int64, restarted bool, err error)`, `Store.DB() *sql.DB`, test helpers `testStore(t)` / `enc(t, v)`.
- Produces: column `nodes.cursor_hw INTEGER NOT NULL DEFAULT 0`, advanced by every `NoteCheckIn` to `MAX(cursor_hw, MIN(cursor, out_seq))`. Task 2 reads it by name in SQL; no Go signature changes.

- [ ] **Step 1: Write the failing tests**

Append to `porta/internal/space/store_test.go`:

```go
// The replay guard's state (design 2026-08-26 D1): the maximum cursor the node
// has ever acked as APPLIED. Monotonic — a stale or reordered pull with an old
// low cursor must not lower it.
func TestCheckInAdvancesCursorHighWaterMonotonically(t *testing.T) {
	st := testStore(t)
	st.Write("n1", "goal/a", cval.KindCell, enc(t, "one"), "david", "")
	st.Write("n1", "goal/b", cval.KindCell, enc(t, "two"), "david", "")
	st.Write("n1", "goal/c", cval.KindCell, enc(t, "three"), "david", "")

	hw := func() int64 {
		t.Helper()
		var v int64
		if err := st.DB().QueryRow(
			`SELECT cursor_hw FROM nodes WHERE id = 'n1'`).Scan(&v); err != nil {
			t.Fatalf("read cursor_hw: %v", err)
		}
		return v
	}

	if _, _, err := st.NoteCheckIn("n1", 2, 0, 7); err != nil {
		t.Fatalf("NoteCheckIn: %v", err)
	}
	if hw() != 2 {
		t.Fatalf("cursor_hw = %d after acking 2, want 2", hw())
	}
	// A stale datagram declaring an OLDER cursor must not rewind the guard.
	if _, _, err := st.NoteCheckIn("n1", 1, 0, 7); err != nil {
		t.Fatalf("NoteCheckIn: %v", err)
	}
	if hw() != 2 {
		t.Fatalf("cursor_hw = %d after a stale cursor 1, want 2", hw())
	}
	if _, _, err := st.NoteCheckIn("n1", 3, 0, 7); err != nil {
		t.Fatalf("NoteCheckIn: %v", err)
	}
	if hw() != 3 {
		t.Fatalf("cursor_hw = %d after acking 3, want 3", hw())
	}
}

// The D2 clamp. cursor > out_seq happens exactly when porta's store was reset
// under a still-running node. Recording the impossible cursor into a monotonic
// column would make the guard suppress the fresh store's rows 1..cursor —
// rows the node has never seen — forever. Clamped, the fresh store simply has
// no memory, which is the truth.
func TestCheckInClampsAnImpossibleCursor(t *testing.T) {
	st := testStore(t)
	st.Write("n1", "goal/a", cval.KindCell, enc(t, "one"), "david", "")
	if _, _, err := st.NoteCheckIn("n1", 500, 0, 7); err != nil {
		t.Fatalf("NoteCheckIn: %v", err)
	}
	var v int64
	if err := st.DB().QueryRow(
		`SELECT cursor_hw FROM nodes WHERE id = 'n1'`).Scan(&v); err != nil {
		t.Fatalf("read cursor_hw: %v", err)
	}
	if v != 1 {
		t.Fatalf("cursor_hw = %d after cursor 500 against out_seq 1, want clamped to 1", v)
	}
}

// A store predating cursor_hw must gain the column on open — the same
// contract TestOpenAddsBootIDToAPreExistingStore pins for boot_id.
func TestOpenAddsCursorHwToAPreExistingStore(t *testing.T) {
	dir := t.TempDir()
	path := dir + "/t.db"
	var clock int64 = 1_000
	now := func() int64 { clock += 10; return clock }

	st, err := Open(path, now)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	if _, err := st.DB().Exec(`ALTER TABLE nodes DROP COLUMN cursor_hw`); err != nil {
		t.Fatalf("drop nodes.cursor_hw: %v", err)
	}
	st.Close()

	st, err = Open(path, now)
	if err != nil {
		t.Fatalf("reopen of a pre-cursor_hw store: %v", err)
	}
	defer st.Close()
	var v int64
	if err := st.DB().QueryRow(
		`SELECT COALESCE(cursor_hw, -1) FROM nodes LIMIT 1`).Scan(&v); err != nil && err != sql.ErrNoRows {
		t.Fatalf("cursor_hw not readable after reopen: %v", err)
	}
	// An empty nodes table is fine — the column existing is what is under test.
	if _, err := st.DB().Exec(
		`INSERT INTO nodes (id, cursor_hw) VALUES ('probe', 0)`); err != nil {
		t.Fatalf("cursor_hw column absent after reopen: %v", err)
	}
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd ~/workspace/rete/porta && go test ./internal/space/ -run 'CursorH|ClampsAnImpossible' -v`
Expected: FAIL — `no such column: cursor_hw` on all three.

- [ ] **Step 3: Implement**

In `porta/internal/space/schema.go`, add the column to the nodes DDL after `node_seq_hw` (comment ABOVE would sit outside the parens — this table's convention is short trailing context via the surrounding comments, so add the doc comment above the CREATE TABLE only if you add prose; the column line itself is enough):

```go
CREATE TABLE IF NOT EXISTS nodes (
  id            TEXT PRIMARY KEY,
  name          TEXT,
  first_seen_ms INTEGER,
  last_seen_ms  INTEGER,
  boot_id       INTEGER,
  cursor_acked  INTEGER NOT NULL DEFAULT 0,
  node_seq_hw   INTEGER NOT NULL DEFAULT 0,
  cursor_hw     INTEGER NOT NULL DEFAULT 0,
  out_seq       INTEGER NOT NULL DEFAULT 0,
```

(Leave the existing `-- Successor to v1's nodes.source_addr...` comment and `last_addr` line untouched below.)

And extend `addColumns`:

```go
var addColumns = []string{
	`ALTER TABLE cells  ADD COLUMN boot_id INTEGER NOT NULL DEFAULT 0`,
	`ALTER TABLE events ADD COLUMN boot_id INTEGER NOT NULL DEFAULT 0`,
	// The replay guard's state (design 2026-08-26 D1): the max cursor this
	// node has ever acked as applied. Distinct from cursor_acked, which is
	// the LATEST declared cursor and rewinds to 0 on every reboot — this one
	// never rewinds, which is the whole guard.
	`ALTER TABLE nodes  ADD COLUMN cursor_hw INTEGER NOT NULL DEFAULT 0`,
}
```

In `porta/internal/space/store.go`, `NoteCheckIn`, replace the final UPDATE:

```go
	// boot_id is recorded on the PULL and not only on the push, so the detection
	// above has a previous value to compare against even for a node that never
	// pushes anything.
	//
	// cursor_hw is the replay guard (design 2026-08-26 D1/D2): the max cursor
	// ever acked as applied, monotonic. The MIN against out_seq is load-bearing,
	// not defensive — cursor > out_seq happens exactly when this store was reset
	// under a still-running node, and recording the impossible cursor into a
	// monotonic column would suppress the fresh store's stream rows forever.
	if _, err := tx.Exec(
		`UPDATE nodes SET cursor_acked = ?, boot_id = ?, last_seen_ms = ?,
		        cursor_hw = MAX(cursor_hw, MIN(?, out_seq))
		  WHERE id = ?`,
		cursor, bootID, now, cursor, nodeID); err != nil {
		return 0, false, err
	}
```

(sqlite's two-argument `MAX()`/`MIN()` are the scalar forms, not aggregates — this is a single-row UPDATE and they behave as expected.)

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cd ~/workspace/rete/porta && go test ./internal/space/ -v`
Expected: PASS, including every pre-existing test (the migration ordering tests especially).

- [ ] **Step 5: gofmt and commit**

```bash
cd ~/workspace/rete/porta && gofmt -l . && cd ~/workspace/rete
git add porta/internal/space/schema.go porta/internal/space/store.go porta/internal/space/store_test.go
git commit -m "feat(#117,#119): nodes.cursor_hw — the acked high-water the replay guard reads, clamped against out_seq

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01H4VVGBQHafDXek3BsFchPv"
```

(`gofmt -l .` printing nothing is the pass condition.)

---

### Task 2: the stream guard in `OutboundSince` (D1 reader)

**Files:**
- Modify: `porta/internal/space/store.go` (`OutboundSince` query, ~line 342)
- Test: `porta/internal/space/store_test.go`

**Interfaces:**
- Consumes: `nodes.cursor_hw` from Task 1; `Store.OutboundSince(nodeID string, cursor int64, maxBytes int) ([]Entry, int64, error)` — signature unchanged.
- Produces: the guarantee Task 3 tests end-to-end: stream rows with `seq <= cursor_hw` are never in the result; cells are unaffected.

- [ ] **Step 1: Write the failing tests**

Append to `porta/internal/space/store_test.go`:

```go
// THE REPLAY GUARD (design 2026-08-26 D1). A cursor-0 pull after a reboot
// re-serves cells (latest value, idempotent) but never a stream row the node
// has already acked as applied — replaying goal/do/reboot reboots the node
// again (#119), and #262's dbg/req rows re-ran a 60 s profile window on every
// boot, forever, until this rule existed.
func TestAckedStreamRowsAreNeverReServed(t *testing.T) {
	st := testStore(t)
	st.Write("n1", "goal/name", cval.KindCell, enc(t, "bell"), "david", "")     // seq 1
	st.Write("n1", "dbg/req", cval.KindStream, enc(t, "profile"), "david", "")  // seq 2
	st.Write("n1", "dbg/req", cval.KindStream, enc(t, "profile2"), "david", "") // seq 3
	if _, _, err := st.NoteCheckIn("n1", 3, 0, 7); err != nil {                 // node applied all three
		t.Fatalf("NoteCheckIn: %v", err)
	}

	got, hi, err := st.OutboundSince("n1", 0, MaxBatchBytes) // reboot: cursor 0
	if err != nil {
		t.Fatalf("OutboundSince: %v", err)
	}
	if len(got) != 1 || got[0].Path != "goal/name" {
		t.Fatalf("cursor-0 replay after full ack = %+v, want the cell alone", got)
	}
	if hi != 1 {
		t.Fatalf("hi = %d, want 1 (the one delivered row)", hi)
	}
}

// At-least-once WITHIN a boot is untouched: a lost BATCH never advanced the
// cursor, so it never advanced cursor_hw, and the re-pull re-serves. The
// exchange's no-retransmit posture rests on this.
func TestUnackedStreamRowsSurviveALostBatch(t *testing.T) {
	st := testStore(t)
	st.Write("n1", "dbg/req", cval.KindStream, enc(t, "profile"), "david", "") // seq 1
	if _, _, err := st.NoteCheckIn("n1", 0, 0, 7); err != nil {                // pull sent, batch LOST
		t.Fatalf("NoteCheckIn: %v", err)
	}
	got, _, err := st.OutboundSince("n1", 0, MaxBatchBytes)
	if err != nil {
		t.Fatalf("OutboundSince: %v", err)
	}
	if len(got) != 1 || got[0].Path != "dbg/req" {
		t.Fatalf("unacked stream row after a lost batch = %+v, want it re-served", got)
	}
}

// Delivered-while-down still delivers: a row written during an outage has
// seq > cursor_hw and reaches the NEXT boot exactly once.
func TestOutageWrittenStreamRowDeliversAcrossAReboot(t *testing.T) {
	st := testStore(t)
	st.Write("n1", "dbg/req", cval.KindStream, enc(t, "old"), "david", "") // seq 1
	if _, _, err := st.NoteCheckIn("n1", 1, 0, 7); err != nil {            // applied
		t.Fatalf("NoteCheckIn: %v", err)
	}
	st.Write("n1", "dbg/req", cval.KindStream, enc(t, "new"), "david", "") // seq 2, node down

	got, _, err := st.OutboundSince("n1", 0, MaxBatchBytes) // next boot, cursor 0
	if err != nil {
		t.Fatalf("OutboundSince: %v", err)
	}
	if len(got) != 1 || got[0].Seq != 2 {
		t.Fatalf("post-outage cursor-0 replay = %+v, want only the unapplied seq-2 row", got)
	}
	v, _ := cval.Decode(got[0].Value)
	if v != "new" {
		t.Fatalf("delivered value = %v, want the outage-written one", v)
	}
}

// The D2 clamp seen from the reader's side: after a store reset under a
// running node (an impossible cursor recorded, clamped), the fresh store's
// NEW rows must still serve. Without the clamp this test wedges — cursor_hw
// would be 500 and the guard would eat rows 1..500 of a store that has no
// history at all.
func TestFreshStoreRowsServeAfterAnImpossibleCursor(t *testing.T) {
	st := testStore(t)
	if _, _, err := st.NoteCheckIn("n1", 500, 0, 7); err != nil { // running node, reset store
		t.Fatalf("NoteCheckIn: %v", err)
	}
	st.Write("n1", "goal/name", cval.KindCell, enc(t, "bell"), "david", "")   // seq 1
	st.Write("n1", "dbg/req", cval.KindStream, enc(t, "profile"), "david", "") // seq 2

	got, _, err := st.OutboundSince("n1", 0, MaxBatchBytes) // node's next reboot
	if err != nil {
		t.Fatalf("OutboundSince: %v", err)
	}
	if len(got) != 2 {
		t.Fatalf("fresh store serves %+v, want both rows — the guard ate the store's future", got)
	}
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd ~/workspace/rete/porta && go test ./internal/space/ -run 'ReServed|LostBatch|OutageWritten|ImpossibleCursor' -v`
Expected: `TestAckedStreamRowsAreNeverReServed` and `TestOutageWrittenStreamRowDeliversAcrossAReboot` FAIL (streams re-served); the other two PASS already — they pin the properties the change must not break. Confirm exactly this split before proceeding.

- [ ] **Step 3: Implement**

In `porta/internal/space/store.go`, `OutboundSince`, replace the query (only the stream half changes; note the added `nodeID` parameter in the args list):

```go
	// The stream half carries the replay guard (design 2026-08-26 D1): a row
	// the node has ever acked as applied — seq <= cursor_hw — is never served
	// again, to any boot, on any path. Cells are deliberately unguarded: the
	// tree read already serves latest-value-per-path, idempotent by
	// construction. COALESCE covers a node with no row yet: a bare `seq >
	// NULL` is NULL, which would silently filter EVERY stream row for a node
	// porta has never seen check in.
	rows, err := s.db.Query(`
		SELECT seq, path, kind, value_cbor FROM cells
		  WHERE node_id = ? AND direction = 'out' AND seq > ?
		UNION ALL
		SELECT seq, path, kind, value_cbor FROM events
		  WHERE node_id = ? AND direction = 'out' AND kind = ? AND seq > ?
		    AND seq > COALESCE((SELECT cursor_hw FROM nodes WHERE id = ?), 0)
		ORDER BY seq`,
		nodeID, cursor, nodeID, int(cval.KindStream), cursor, nodeID)
```

- [ ] **Step 4: Run the full package to verify everything passes**

Run: `cd ~/workspace/rete/porta && go test ./internal/space/ -v`
Expected: PASS — all four new tests and every pre-existing one (watch `TestOutboundKeepsEveryStreamEntry` and `TestOutboundRespectsCursor` in particular: they never call `NoteCheckIn`, so `cursor_hw` is 0 and their behaviour must be unchanged).

- [ ] **Step 5: gofmt and commit**

```bash
cd ~/workspace/rete/porta && gofmt -l . && cd ~/workspace/rete
git add porta/internal/space/store.go porta/internal/space/store_test.go
git commit -m "feat(#119,#262): the replay guard — an acked stream row is never re-served, uniformly, cells untouched

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01H4VVGBQHafDXek3BsFchPv"
```

---

### Task 3: the #262 field shape, end-to-end at the wire

**Files:**
- Test: `porta/internal/wire/wire_test.go`

**Interfaces:**
- Consumes: `testServer(t) (*Server, *space.Store)`, `mustEnc(t, v)`, `EncodePull(id string, cursor, bootID, hw int64) ([]byte, error)` — note the order: bootID BEFORE hw — `DecodeBatch(b []byte) (int64, []space.Entry, error)`, `srv.Handle(req []byte) []byte`, `space.Entry{Seq, Path, Kind, Value}`.
- Produces: nothing new — this task is pure verification; no production code may change in it.

- [ ] **Step 1: Write the test**

Append to `porta/internal/wire/wire_test.go`:

```go
// #262 REPRODUCED AND KILLED, at the wire. The field shape: porta wrote
// dbg/req profile requests (streams) and a goal cell; the node applied them
// all; every later cold boot pulled from cursor 0 and re-ran the profile
// window — 60 s of tax and a 228-method dump per boot, forever. With the
// replay guard the cursor-0 pull of a NEW boot carries the cell alone.
func TestColdBootDoesNotReExecuteAckedStreams(t *testing.T) {
	srv, st := testServer(t)
	st.Write("c6", "goal/name", cval.KindCell, mustEnc(t, "bell"), "david", "")      // seq 1
	st.Write("c6", "dbg/req", cval.KindStream, mustEnc(t, "profile-1"), "david", "") // seq 2
	st.Write("c6", "dbg/req", cval.KindStream, mustEnc(t, "profile-2"), "david", "") // seq 3

	// Boot 41: first pull delivers all three.
	req, _ := EncodePull("c6", 0, 41, 0)
	hi, entries, err := DecodeBatch(srv.Handle(req))
	if err != nil {
		t.Fatalf("boot 41 pull: %v", err)
	}
	if hi != 3 || len(entries) != 3 {
		t.Fatalf("boot 41: hi=%d entries=%d, want 3 and 3", hi, len(entries))
	}
	// The node applies the batch and its next pull declares cursor 3 — that
	// declaration is the application receipt the guard is built on.
	req, _ = EncodePull("c6", 3, 41, 0)
	if _, entries, err = DecodeBatch(srv.Handle(req)); err != nil || len(entries) != 0 {
		t.Fatalf("boot 41 idle pull: err=%v entries=%d, want empty", err, len(entries))
	}

	// Boot 42: cold boot, RAM cursor gone, cursor 0 — the #262 moment.
	req, _ = EncodePull("c6", 0, 42, 0)
	hi, entries, err = DecodeBatch(srv.Handle(req))
	if err != nil {
		t.Fatalf("boot 42 pull: %v", err)
	}
	if len(entries) != 1 || entries[0].Path != "goal/name" {
		t.Fatalf("boot 42 cursor-0 replay = %+v, want the goal cell alone — a dbg/req here is #262 back from the dead", entries)
	}
	if hi != 1 {
		t.Fatalf("boot 42: hi=%d, want 1", hi)
	}
}
```

- [ ] **Step 2: Run it**

Run: `cd ~/workspace/rete/porta && go test ./internal/wire/ -run ColdBootDoesNotReExecute -v`
Expected: PASS (Tasks 1–2 built the mechanism; this pins it at the protocol surface). If it FAILS, stop — do not patch the test; the mechanism is wrong, go back to Task 2.

- [ ] **Step 3: Run both packages, then commit**

Run: `cd ~/workspace/rete/porta && go test ./...`
Expected: `ok` for every package.

```bash
cd ~/workspace/rete
git add porta/internal/wire/wire_test.go
git commit -m "test(#262): the field shape at the wire — a new boot's cursor-0 pull carries cells only

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01H4VVGBQHafDXek3BsFchPv"
```

---

### Task 4: amend the documents of record (design §5)

**Files:**
- Modify: `protocol/PROTOCOL-NSL.md` (three passages: the §1 bullet ~line 59, the `seq`/cursors passage ~line 241, the §5.1 convergence line ~line 542)
- Modify: `protocol/PROTOCOL-NSL-RECORDS.md` (§4.4, after the "Delivery class is a property of the channel" paragraph ~line 535)
- Modify: `porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md` (§8 first bullet ~line 545)

**Interfaces:**
- Consumes: the approved design record's D1–D4 wording.
- Produces: contract text Task 5's close-out comments cite. §§1–4 of PROTOCOL-NSL.md are the conformance target — these edits ARE the rung's contract change.

- [ ] **Step 1: PROTOCOL-NSL.md — the §1 bullet**

Find (~line 59):

```
- **Full-state resync by construction.** A reflashed or amnesiac node resyncs from
  cursor 0 with no special rules — cold boot replays the whole `goal/**` tree. v1's
  boot-echo drift-healing dissolves.
```

Replace with:

```
- **Full-state resync by construction.** A reflashed or amnesiac node resyncs from
  cursor 0 with no special rules — cold boot replays the whole `goal/**` CELL tree.
  Streams are exempt by the replay guard (§ "seq, cursors, and changedSince:"):
  an entry the node has already acked as applied is never served again, so a
  resync cannot re-execute old `goal/do/*` actions. v1's boot-echo drift-healing
  dissolves.
```

- [ ] **Step 2: PROTOCOL-NSL.md — the `seq`/cursors passage**

Find (~line 241):

```
space: a latest-value slot per cell, a monotonic per-path `seq`, and a
changed-since-cursor query. A node persists its cursor (NVS) only *after* applying a
batch, so a crash mid-apply re-pulls rather than skips. Cold boot = cursor 0 = full
replay.
```

Replace with:

```
space: a latest-value slot per cell, a monotonic per-path `seq`, and a
changed-since-cursor query. The node's cursor is RAM, deliberately (design
2026-08-26 D3): persistence would buy only a bounded boot-time re-pull and
would cost a stranding mode after a gateway store reset. Cold boot = cursor 0,
and what replay-from-0 MEANS is per kind (design 2026-08-26 D1):

- **cells** — the latest value per path, read from the cell tree, so the replay
  is bounded by tree size and idempotent by construction;
- **streams** — only entries above the node's acked high-water. The gateway
  keeps, per node, the maximum cursor the node has ever declared applied
  (`cursor_hw`, monotonic, clamped to the gateway's own out-seq), and never
  serves a stream row at or below it — to any boot, on any path, uniformly.
  An applied `goal/do/*` action therefore cannot re-execute across reboots;
  an UNAPPLIED entry is served until acked, so at-least-once within a boot
  and delivery of rows written during an outage are both unchanged.
```

- [ ] **Step 3: PROTOCOL-NSL.md — the §5.1 convergence line**

Find (~line 542):

```
seq yet (visible), and a node that lost state resyncs from cursor 0 (automatic).
```

Replace with:

```
seq yet (visible), and a node that lost state resyncs from cursor 0 (automatic —
cells; applied stream actions are never re-served, per §3's replay guard).
```

- [ ] **Step 4: PROTOCOL-NSL-RECORDS.md — define must-deliver (D4)**

Find (~line 535):

```
**Delivery class is a property of the channel, not a paragraph of prose.** `sys/panic`
is must-deliver; `tel/print` is drop-first. That is the whole rule.
```

Replace with:

```
**Delivery class is a property of the channel, not a paragraph of prose.** `sys/panic`
is must-deliver; `tel/print` is drop-first. That is the whole rule.

**Must-deliver MEANS re-offered until acked from a durable source** (design
2026-08-26 D4). A `whenever:` capture cannot provide it — a captured-but-unacked
entry dies with the bridge job, and the bounded pending queue may evict it under
long disconnection. For `sys/panic` the durable source is the rete#215
RebootNote: C state that survives both a bridge crash and the reboot itself,
cleared only by `panicNoteAck:` once porta's ACK covers the published entry, and
(re-)published each check-in while unacked. A duplicate panic row in the
append-only log is acceptable; a lost one is not. No journal, no cell rerouting.
```

- [ ] **Step 5: the 2026-08-01 design §8 pointer**

Find the first hazard bullet's last line (~line 550):

```
  not previously written down. **To be filed as an issue, not built in this slice.**
```

Replace with:

```
  not previously written down. **To be filed as an issue, not built in this slice.**
  (Filed as #119; ANSWERED 2026-08-26 — `2026-08-26-space-sync-durability-trio-design.md`
  D1/D2 built the guard as an acked high-water, superseding the bootId wording here.)
```

- [ ] **Step 6: Commit**

```bash
cd ~/workspace/rete
git add protocol/PROTOCOL-NSL.md protocol/PROTOCOL-NSL-RECORDS.md porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md
git commit -m "docs(protocol): replay-from-0 is per kind; must-deliver means re-offered-until-acked from a durable source (#119 #114)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01H4VVGBQHafDXek3BsFchPv"
```

---

### Task 5: gates, then issue close-outs

**Files:** none modified — verification and `gh` only.

**Interfaces:**
- Consumes: everything above, plus the approved design record for citation paths.
- Produces: closed #117/#119/#262, decision comment on #114.

- [ ] **Step 1: Run the gates**

```bash
cd ~/workspace/rete/porta && gofmt -l . && go test ./...
cd ~/workspace/rete/nodus/oevm && bash tests/porta_sync_host.sh
```

Expected: `gofmt` prints nothing; every package `ok`; the sync script ends `PORTA SYNC OK`. **Do not run the nsl suite concurrently with the sync script** (shared `/tmp` paths). If any gate is red, STOP and report — do not close issues over a red gate.

- [ ] **Step 2: Close #119**

```bash
gh issue close 119 --comment "Built 2026-08-26. The guard is an ACKED HIGH-WATER, not the bootId rule this issue proposed — porta keeps per node the max cursor ever declared applied (nodes.cursor_hw, monotonic, clamped to out_seq) and never serves a stream row at or below it, uniformly, on any path. Cells were already latest-value-per-path. Supersession rationale, invariants and traced edges: porta/docs/design/2026-08-26-space-sync-durability-trio-design.md (D1/D2). Contract amended in PROTOCOL-NSL.md §1/§3/§5.1. Wire-level pin: TestColdBootDoesNotReExecuteAckedStreams. This releases #266's hard gate."
```

- [ ] **Step 3: Close #117**

```bash
gh issue close 117 --comment "Closed 2026-08-26, in two unequal halves. The bootId half landed back in rete#162/#215: platform_backend_boot_id() persists the counter under the settings key, the bridge latches it at construction, porta's restart detection is built on it — this issue's 'source constant (1)' claim has been stale since then. The cursor half is resolved DELIBERATELY RAM: with the #119 replay guard, cursor-0 is safe, and persistence would buy only a bounded boot-time cell re-pull while costing a stranding mode after a porta store reset (the node's cursor is forward-only; no rewind exists on the wire). The earned follow-on (NVS key + an explicit reset signal — NOT authoritative-hi, which reopens a stale-batch re-execution hazard) and what would earn it are recorded in porta/docs/design/2026-08-26-space-sync-durability-trio-design.md D3."
```

- [ ] **Step 4: Close #262**

```bash
gh issue close 262 --comment "Fixed 2026-08-26 by #119's replay guard, taken uniform rather than classed: dbg/req rows stay permanent in the append-only log but are SERVED once per application, ever — a new boot's cursor-0 pull carries cells only (TestColdBootDoesNotReExecuteAckedStreams reproduces this issue's field shape and pins the fix). The manifest-class candidate this issue sketched was not needed: re-serving an APPLIED stream entry is a duplicate on every path, so no class value was minted. Design: porta/docs/design/2026-08-26-space-sync-durability-trio-design.md D1. On-board confirmation via the ReplayProbeC6 recipe piggybacks on the #232 session."
```

- [ ] **Step 5: Comment on #114 (stays open)**

```bash
gh issue comment 114 --body "DECIDED 2026-08-26 (implementation rides #232, the next board session): must-deliver means RE-OFFERED UNTIL ACKED FROM A DURABLE SOURCE. None of this issue's three candidates as written: no C-side journal, no cell rerouting, and supervisor re-subscribe indeed fixes nothing. For sys/panic the durable source already exists — the rete#215 RebootNote survives both a bridge crash and the reboot itself and is cleared only by panicNoteAck:. The pattern: while the note is unacked, (re-)publish the Panic record onto sys/panic at each check-in; ack the note only once porta's ACK covers that entry. Re-offering per check-in (not once at boot) is what survives the bounded pending queue evicting the entry under long disconnection. Duplicates in the append-only log are acceptable; loss is not. RECORDS §4.4 now defines the class this way. Design: porta/docs/design/2026-08-26-space-sync-durability-trio-design.md D4."
```

- [ ] **Step 6: Report**

No commit here. Report the gate outputs verbatim and the four issue URLs. The `docs/next-steps-overview.md` update is session-close work (it is already dirty with unrelated changes) — leave it alone.

---

## Self-review notes (already applied)

- Spec coverage: D1 → Tasks 1–3; D2 → Task 1 (writer) + Task 2 (`TestFreshStoreRowsServeAfterAnImpossibleCursor`); D3/D4 → Task 4 text + Task 5 comments; D5 → nothing built, by design; §5 fates → Task 5; §7 gates → Task 5 Step 1. §6 (backup gateways) defers everything — no task, correct.
- The design's §7 test list maps: monotonic (T1), clamp (T1+T2), cursor-0 cells-not-streams (T2), lost batch (T2), outage row (T2), migration (T1), wire #262 shape (T3).
- Type consistency: `NoteCheckIn`/`OutboundSince` signatures unchanged everywhere; `EncodePull(id, cursor, bootID, hw)` order stated and used correctly in T3.
