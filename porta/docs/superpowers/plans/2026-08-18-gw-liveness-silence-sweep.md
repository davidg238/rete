# `gw/**` and the silence sweep — implementation plan (#217)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** porta forms and records a liveness verdict about every node it hears from, at
`gw/liveness`, so that a node which stops talking becomes a visible event rather than an
absence — the precondition for #215's loop breaker being allowed to halt a node at all.

**Architecture:** One new path prefix, `gw/**`, gateway-authored and never on the wire. It
is carried by a third value of the existing `direction` column, which excludes it from
`OutboundSince` **by construction** rather than by a filter someone must remember. One new
goroutine in `porta serve` ticks every 30 s and calls one new store method, `Sweep`, which
infers each node's cadence from that node's own exchange history and writes `gw/liveness`
**only when the verdict changes**. No new table, no new endpoint, no new verb (D2).

**Tech Stack:** Go 1.26, sqlite via `github.com/mattn/go-sqlite3`, cobra. Everything in
`porta/`; nothing on the node side and nothing on the wire.

**Spec:** `docs/design/2026-08-10-node-memory-and-liveness-design.md` §6 (the whole of
sub-project D) and §7 step 3 (the falsification). Read §5.2 too — it is the reason this
exists: *"This is only safe because of sub-project D. A halted node reports nothing;
without `gw/liveness` the breaker would trade a noisy zombie for an invisible corpse."*
Secondary: `docs/design/2026-08-16-vm-liveness-observer-design.md` §5 (the constraint this
discharges) and `porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md` (D2/D4.1
/D4.5, the store schema).

## Global Constraints

- **Go 1.26.** Copyright header `// Copyright (c) 2026 Ekorau LLC` on **every new Go file**.
- **`gofmt -l .` must print nothing** before every commit (run from `porta/`).
- **Gate before every commit:** `cd porta && gofmt -l . && go test ./...`
- **Gate before the final commit of the rung:** `cd nodus/tuvm && bash tests/porta_sync_host.sh`
  must print `PORTA SYNC OK`. The store changed, and that gate is the only host thing that
  runs a real node against a real `porta serve`.
- **All values in the store are raw deterministic CBOR** (D4.1); decode only for display.
  The sweep compares **encoded bytes** and never decodes.
- **All timestamps are milliseconds** (D4.2). All paths are **node-relative** (D4.5).
- **There is no backward compatibility to maintain on the wire** — but the **live soak store
  on gw85224-01 must not be rebuilt from scratch**: it holds the history #190's evidence
  rests on, and dropping it forces every node through a full re-push. Migrations are
  in-place and idempotent, exactly as `addColumns` already is.
- **Nothing in this plan may touch the node side or the wire.** A `gw/` row that reaches a
  node is the defect this design's storage choice exists to make impossible; Task 2 tests
  for it directly.
- **Verbs live at the CLI only.** `porta get <node> gw/` and `porta log <node>` are the whole
  operator surface for this; no new subcommand.

## Constants this plan pins

Recorded here because 2026-08-10 §8's table is where they are promoted at close-out.

| Constant | Value | Standing |
|---|---|---|
| offline window `k` | `3 × cadence` | **Inherited** — v1's constant, kept (rewrite design line 275) |
| cadence inference window | last **10** gaps | **Chosen** — robust to one missed check-in, short enough to track a cadence change |
| minimum exchanges for a verdict | **3** | **Chosen** — fewer than three exchanges is fewer than two gaps; there is no cadence to infer and a guess would be a verdict porta cannot support |
| sweep tick | **30 s** | **Chosen** — a floor on detection latency, below the shortest cadence in use |
| `gw` row `seq` | **0** | **Pinned by this plan.** `seq` is only meaningful inside a sequence space, and `gw` has none: porta's `out_seq` is the node's cursor space and the node's push counter is the other. Burning an `out_seq` on a row the node can never receive would put a permanent gap in a cursor for a row that is not coming. `events.id` already orders the log |
| `gw` row `boot_id` | the node's **current** boot | **Pinned by this plan.** The verdict is about a node *on a boot* — "porta declared c6-bell silent while it was on boot 44" — which is rete#210's argument applied to a gateway-authored row, and the value is already in hand |

---

### Task 1: `direction` accepts a third value, `'gw'`

The `CHECK (direction IN ('in','out'))` constraint is on both `cells` and `events`, and
sqlite cannot alter a CHECK constraint in place — the table must be rebuilt. The rebuild
derives the replacement DDL from the table's **own** stored DDL, so it cannot drift from
the live shape and it works on a store whose column order differs (one whose `boot_id` was
appended by `addColumns` has a different column order from a fresh store, and `SELECT *`
between two tables built from the same DDL text is order-correct by construction).

**Files:**
- Modify: `porta/internal/space/schema.go` (the two CHECK clauses + a `migrations` section)
- Modify: `porta/internal/space/store.go:53-70` (`Open` runs the migration after `addColumns`)
- Test: `porta/internal/space/store_test.go`

**Interfaces:**
- Consumes: nothing.
- Produces: `space.DirectionGateway = "gw"` (exported string const); a store whose `cells`
  and `events` tables accept `direction = 'gw'`. Task 2 writes through it.

- [x] **Step 1: Write the failing test**

Append to `porta/internal/space/store_test.go`:

```go
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
```

Add `"database/sql"` and `"strings"` to the test file's imports, and keep the blank
`_ "github.com/mattn/go-sqlite3"` driver import available (it is already imported by the
package under test, so the test file needs only `database/sql`).

- [x] **Step 2: Run the tests to verify they fail**

Run: `cd porta && go test ./internal/space/ -run 'Migrat|NoOpOnASecond' -v`
Expected: FAIL — the first test fails on `gw cell after migration: CHECK constraint failed`.

- [x] **Step 3: Add the third direction to the schema**

In `porta/internal/space/schema.go`, change **both** CHECK clauses (in `cells` and in
`events`) from:

```
  direction  TEXT NOT NULL CHECK (direction IN ('in','out')),
```

to:

```
  direction  TEXT NOT NULL CHECK (direction IN ('in','out','gw')),
```

Extend the `cells` block comment (the one above the CREATE TABLE, never inside the
parentheses — sqlite rewrites this DDL text on DROP COLUMN and an interior comment makes it
unparseable) with:

```
-- 'gw' is a verdict porta formed ABOUT the node — gateway-authored, never on
-- the wire (design 2026-08-10 §6.1). It is a third direction rather than a
-- fifth table because the unit of extension is a path (D2), and it is carried
-- by THIS column because OutboundSince already filters direction = 'out': the
-- exclusion from the radio is then structural, not a filter someone must
-- remember to write. porta writes ONLY goal/** and dbg/req to a node, and a
-- 'gw' row is neither node-authored nor deliverable to it, so it fits none of
-- the four wire namespaces.
```

- [x] **Step 4: Add the migration**

Append to `porta/internal/space/schema.go`:

```go
// DirectionGateway is the third value of the direction column: a row porta
// wrote ABOUT a node rather than TO it. The two wire directions are 'in'
// (the node's own authority) and 'out' (porta's intent).
const DirectionGateway = "gw"

const (
	oldDirectionCheck = `CHECK (direction IN ('in','out'))`
	newDirectionCheck = `CHECK (direction IN ('in','out','gw'))`
)

// migrateDirectionAcceptsGw rebuilds cells and events when they still carry the
// two-value CHECK. sqlite cannot alter a CHECK in place, so the table must be
// copied — and the replacement DDL is derived from the table's OWN stored DDL
// rather than from the schema const above. That is the whole trick: a store
// that gained boot_id by ALTER has its columns in a different ORDER from a
// fresh store, and a hand-written column list would silently mis-copy it.
// Both tables are built from identical text, so `INSERT INTO tmp SELECT *` is
// order-correct by construction; the events copy carries the AUTOINCREMENT id
// verbatim, because `porta log` orders by it.
//
// Detection is the DDL itself and not a version row: a store that already
// carries 'gw' is skipped, which is what makes a second Open a no-op.
//
// SPIKED 2026-08-18 before this plan was handed over, on both shapes: a fresh
// store (DDL still reads `CREATE TABLE IF NOT EXISTS cells (`) and a store whose
// boot_id was appended by ALTER (sqlite has rewritten the DDL, dropping the
// IF NOT EXISTS and putting boot_id on the direction line). Both migrate, keep
// their rows, and accept 'gw' afterwards. Two findings from that run are baked
// in above: the DROP TABLE IF EXISTS, and the fact that after the rename the
// stored DDL reads `CREATE TABLE "cells" (` — QUOTED — which is harmless only
// because detection keys on the CHECK text and not on the table name.
func migrateDirectionAcceptsGw(db *sql.DB) error {
	for _, tbl := range []string{"cells", "events"} {
		var ddl string
		if err := db.QueryRow(
			`SELECT sql FROM sqlite_master WHERE type='table' AND name=?`, tbl).Scan(&ddl); err != nil {
			return fmt.Errorf("read %s DDL: %w", tbl, err)
		}
		if !strings.Contains(ddl, oldDirectionCheck) {
			continue // already migrated, or a shape this code did not write
		}
		tmp := tbl + "_migrating"
		newDDL := strings.Replace(ddl, oldDirectionCheck, newDirectionCheck, 1)
		newDDL = strings.Replace(newDDL, tbl+" (", tmp+" (", 1)
		tx, err := db.Begin()
		if err != nil {
			return err
		}
		for _, stmt := range []string{
			// A crash between the copy and the rename leaves the tmp table
			// behind, and its DDL carries IF NOT EXISTS — so the next attempt
			// would silently ADOPT the stale table and its rows. Drop it first.
			fmt.Sprintf(`DROP TABLE IF EXISTS %s`, tmp),
			newDDL,
			fmt.Sprintf(`INSERT INTO %s SELECT * FROM %s`, tmp, tbl),
			fmt.Sprintf(`DROP TABLE %s`, tbl),
			fmt.Sprintf(`ALTER TABLE %s RENAME TO %s`, tmp, tbl),
		} {
			if _, err := tx.Exec(stmt); err != nil {
				tx.Rollback()
				return fmt.Errorf("migrate %s: %s: %w", tbl, stmt, err)
			}
		}
		if err := tx.Commit(); err != nil {
			return fmt.Errorf("migrate %s: commit: %w", tbl, err)
		}
	}
	return nil
}
```

Add `"database/sql"`, `"fmt"` and `"strings"` to `schema.go`'s imports (it has none today,
so add an import block below the package comment).

- [x] **Step 5: Run it from `Open`**

In `porta/internal/space/store.go`, inside `Open`, **after** the `addColumns` loop and
**before** the `return`:

```go
	// AFTER addColumns, never before: a store predating boot_id gains the column
	// first, so the rebuild below copies it like any other.
	if err := migrateDirectionAcceptsGw(db); err != nil {
		db.Close()
		return nil, err
	}
	// The rebuild drops the old table and its indexes with it. schema is
	// idempotent (every statement is IF NOT EXISTS), so re-running it restores
	// them and does nothing else.
	if _, err := db.Exec(schema); err != nil {
		db.Close()
		return nil, err
	}
```

- [x] **Step 6: Run the tests to verify they pass**

Run: `cd porta && go test ./internal/space/ -v`
Expected: PASS, including the pre-existing `TestOpenAddsBootIDToAPreExistingStore`.

- [x] **Step 7: Full gate and commit**

```bash
cd porta && gofmt -l . && go test ./...
git add porta/internal/space/schema.go porta/internal/space/store.go porta/internal/space/store_test.go
git commit -m "feat(#217): direction gains a third value, and a live store gains it in place

gw/** is gateway-authored and never on the wire. Carrying it on the existing
direction column is what excludes it from OutboundSince by construction rather
than by a filter someone must remember to write.

sqlite cannot alter a CHECK constraint, so cells and events are rebuilt — from
each table's OWN stored DDL, because a store that gained boot_id by ALTER has a
different column order from a fresh one and a hand-written column list would
mis-copy it silently. The live soak store on gw85224-01 is exactly such a store
and holds the history #190's evidence rests on.

Refs #217."
```

---

### Task 2: `WriteGateway` — a verdict porta formed about a node

**Files:**
- Modify: `porta/internal/space/store.go` (new method beside `Write`)
- Test: `porta/internal/space/store_test.go`

**Interfaces:**
- Consumes: `DirectionGateway` from Task 1.
- Produces:
  - `const PathLiveness = "gw/liveness"`
  - `func (s *Store) WriteGateway(nodeID, path string, value []byte, reason string) error`
    — writes the cell **and** one event row, `direction = "gw"`, `actor = "porta"`,
    `seq = 0`, `boot_id` = the node's current boot. Task 4 calls it.

- [x] **Step 1: Write the failing tests**

Append to `porta/internal/space/store_test.go`:

```go
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
```

- [x] **Step 2: Run the tests to verify they fail**

Run: `cd porta && go test ./internal/space/ -run Gateway -v`
Expected: FAIL — `st.WriteGateway undefined`.

- [x] **Step 3: Implement `WriteGateway`**

In `porta/internal/space/store.go`, immediately after `Write`:

```go
// PathLiveness is the one gw/** path today: porta's verdict on whether a node
// is still talking. #live or #silent, as a Symbol.
const PathLiveness = "gw/liveness"

// WriteGateway records a verdict porta formed ABOUT a node — the third
// direction, gateway-authored and never on the wire.
//
// It is deliberately NOT a variant of Write with a flag. Write allocates the
// node's next outbound seq, because everything it writes is destined for the
// node's cursor; a gw row is destined for nothing, so it takes seq 0 and leaves
// that counter alone. `seq` is only meaningful inside a sequence space and gw
// has none — porta's out_seq is one space, the node's push counter the other.
// The log's own `id` orders these rows.
//
// It writes a CELL and only a cell: a verdict is the current state of porta's
// opinion, and there is no gw stream. When one is needed, it is added then.
func (s *Store) WriteGateway(nodeID, path string, value []byte, reason string) error {
	if len(value) > MaxValueBytes {
		return fmt.Errorf("value is %d bytes, over the %d byte cap", len(value), MaxValueBytes)
	}
	if err := cval.Validate(value); err != nil {
		return fmt.Errorf("value is not a well-formed algebra value: %w", err)
	}
	now := s.nowMs()
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if err := ensureNode(tx, nodeID, now); err != nil {
		return err
	}
	// The boot the verdict is ABOUT. 0 means porta has never heard from the node
	// or the node has no persistent identity — both read as unknown, and neither
	// is invented into a number.
	var boot int64
	if err := tx.QueryRow(`SELECT COALESCE(boot_id, 0) FROM nodes WHERE id = ?`, nodeID).Scan(&boot); err != nil {
		return err
	}
	if err := upsertCell(tx, nodeID, path, 0, cval.KindCell, value, now, DirectionGateway, boot); err != nil {
		return err
	}
	if err := appendEvent(tx, nodeID, path, 0, cval.KindCell, value, now, DirectionGateway, "porta", reason, boot); err != nil {
		return err
	}
	return tx.Commit()
}
```

- [x] **Step 4: Run the tests to verify they pass**

Run: `cd porta && go test ./internal/space/ -v`
Expected: PASS.

- [x] **Step 5: Commit**

```bash
cd porta && gofmt -l . && go test ./...
git add porta/internal/space/store.go porta/internal/space/store_test.go
git commit -m "feat(#217): porta can record what it thinks about a node, where the node cannot read it

WriteGateway is not Write with a flag: Write allocates the node's next outbound
seq because everything it writes is bound for the node's cursor, and a gw row is
bound for nothing. seq 0, boot stamped with the boot the verdict is about.

The test that matters is the negative one — OutboundSince never returns it.

Refs #217."
```

---

### Task 3: `CadenceMs` — the deadline comes from the node, never from a constant

**Files:**
- Modify: `porta/internal/space/store.go`
- Test: `porta/internal/space/store_test.go`

**Interfaces:**
- Consumes: the `exchanges` table (already written by `RecordExchange` on every check-in).
- Produces:
  - `const CadenceWindow = 10`, `const MinExchanges = 3`, `const SilenceFactor = 3`
  - `func (s *Store) CadenceMs(nodeID string) (cadence int64, ok bool, err error)` — Task 4
    calls it.

- [x] **Step 1: Write the failing tests**

Append to `porta/internal/space/store_test.go`:

```go
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

// checkIn advances the clock by gap ms and records one exchange, the way a
// node's check-in does.
func checkIn(t *testing.T, st *Store, clock *int64, node string, gap int64) {
	t.Helper()
	*clock += gap
	if err := st.RecordExchange(node, 41, 0, 0, 0, 0, ""); err != nil {
		t.Fatalf("RecordExchange: %v", err)
	}
}

func TestCadenceIsTheMedianOfTheNodesOwnGaps(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	st.RecordExchange("n1", 41, 0, 0, 0, 0, "") // the first arrival: no gap yet
	for i := 0; i < 5; i++ {
		checkIn(t, st, &clock, "n1", 2_000)
	}
	cadence, ok, err := st.CadenceMs("n1")
	if err != nil || !ok {
		t.Fatalf("CadenceMs = %d, %v, %v; want a verdict", cadence, ok, err)
	}
	if cadence != 2_000 {
		t.Fatalf("cadence = %d ms, want 2000", cadence)
	}
}

// The reason the window is a MEDIAN and not a mean: one missed check-in doubles
// a single gap, and a mean would move the deadline for the next ten minutes.
func TestOneMissedCheckInDoesNotMoveTheCadence(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	st.RecordExchange("n1", 41, 0, 0, 0, 0, "")
	for i := 0; i < 4; i++ {
		checkIn(t, st, &clock, "n1", 2_000)
	}
	checkIn(t, st, &clock, "n1", 60_000) // one long gap: a missed check-in
	for i := 0; i < 4; i++ {
		checkIn(t, st, &clock, "n1", 2_000)
	}
	cadence, ok, _ := st.CadenceMs("n1")
	if !ok || cadence != 2_000 {
		t.Fatalf("cadence = %d, ok = %v; want 2000 — the outlier moved the median", cadence, ok)
	}
}

// Fewer than three exchanges is fewer than two gaps. There is nothing to infer,
// and a guess would be a verdict porta cannot support.
func TestNoCadenceForANodeWithTooFewExchanges(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	st.RecordExchange("n1", 41, 0, 0, 0, 0, "")
	checkIn(t, st, &clock, "n1", 2_000)
	if _, ok, err := st.CadenceMs("n1"); ok || err != nil {
		t.Fatalf("CadenceMs ok = %v err = %v after 2 exchanges; want no verdict", ok, err)
	}
	checkIn(t, st, &clock, "n1", 2_000)
	if _, ok, err := st.CadenceMs("n1"); !ok || err != nil {
		t.Fatalf("CadenceMs ok = %v err = %v after 3 exchanges; want a verdict", ok, err)
	}
}

// A node whose cadence CHANGED must be judged against what it does now, not
// against its whole history — which is what bounds the window at 10 gaps.
func TestCadenceTracksAChangedRate(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	st.RecordExchange("n1", 41, 0, 0, 0, 0, "")
	for i := 0; i < 12; i++ {
		checkIn(t, st, &clock, "n1", 30_000)
	}
	for i := 0; i < 12; i++ {
		checkIn(t, st, &clock, "n1", 2_000)
	}
	cadence, ok, _ := st.CadenceMs("n1")
	if !ok || cadence != 2_000 {
		t.Fatalf("cadence = %d, want 2000 — the window still holds the old rate", cadence)
	}
}
```

- [x] **Step 2: Run the tests to verify they fail**

Run: `cd porta && go test ./internal/space/ -run Cadence -v`
Expected: FAIL — `st.CadenceMs undefined`.

- [x] **Step 3: Implement `CadenceMs`**

In `porta/internal/space/store.go`, after `RecordExchange`. Add `"sort"` to the imports.

```go
// The sweep's numbers. Each is chosen rather than derived, and each is recorded
// with its standing in 2026-08-10 §8 — a deferral written down is falsifiable,
// one left in the code is not.
const (
	// CadenceWindow is how many inter-arrival gaps the median is taken over:
	// robust to one missed check-in, short enough to track a cadence change.
	CadenceWindow = 10
	// MinExchanges is the fewest arrivals that yield a verdict. Two arrivals is
	// one gap, which is a sample and not a cadence.
	MinExchanges = 3
	// SilenceFactor is v1's k, kept: silent when nothing has arrived for
	// k x the node's own cadence.
	SilenceFactor = 3
)

// CadenceMs infers how often a node checks in FROM THAT NODE'S OWN history —
// the median of its last CadenceWindow inter-arrival gaps.
//
// It is inferred and never configured, which is the same rule the node-side
// detector settles on (2026-08-16 §3): the deadline comes from what the node
// declared or did, never from a constant the observer picked. A ten-minute node
// and a two-second node are each judged against themselves.
//
// ok is false when the node has fewer than MinExchanges exchanges. That is not
// an error — it is a node porta has no cadence for yet, and the honest answer is
// no verdict rather than a guess.
func (s *Store) CadenceMs(nodeID string) (int64, bool, error) {
	rows, err := s.db.Query(
		`SELECT ts_ms FROM exchanges WHERE node_id = ? ORDER BY ts_ms DESC LIMIT ?`,
		nodeID, CadenceWindow+1)
	if err != nil {
		return 0, false, err
	}
	defer rows.Close()
	var ts []int64
	for rows.Next() {
		var t int64
		if err := rows.Scan(&t); err != nil {
			return 0, false, err
		}
		ts = append(ts, t)
	}
	if err := rows.Err(); err != nil {
		return 0, false, err
	}
	if len(ts) < MinExchanges {
		return 0, false, nil
	}
	gaps := make([]int64, 0, len(ts)-1)
	for i := 0; i+1 < len(ts); i++ {
		gaps = append(gaps, ts[i]-ts[i+1]) // ts is newest-first, so this is positive
	}
	return medianMs(gaps), true, nil
}

// medianMs sorts in place; the caller owns a scratch slice.
func medianMs(v []int64) int64 {
	sort.Slice(v, func(i, j int) bool { return v[i] < v[j] })
	n := len(v)
	if n%2 == 1 {
		return v[n/2]
	}
	return (v[n/2-1] + v[n/2]) / 2
}
```

- [x] **Step 4: Run the tests to verify they pass**

Run: `cd porta && go test ./internal/space/ -v`
Expected: PASS.

- [x] **Step 5: Commit**

```bash
cd porta && gofmt -l . && go test ./...
git add porta/internal/space/store.go porta/internal/space/store_test.go
git commit -m "feat(#217): a node's deadline is inferred from its own arrivals, never configured

The median of the last ten gaps, so one missed check-in does not move the
deadline. Fewer than three arrivals gets no cadence at all — a guess would be a
verdict porta cannot support.

Refs #217."
```

---

### Task 4: `Sweep` — the verdict, written only when it changes

**Files:**
- Create: `porta/internal/space/sweep.go`
- Create: `porta/internal/space/sweep_test.go`

**Interfaces:**
- Consumes: `CadenceMs`, `WriteGateway`, `PathLiveness`, `SilenceFactor` (Tasks 2–3).
- Produces:
  - `type LivenessChange struct { NodeID string; Silent bool; CadenceMs int64; SinceMs int64 }`
  - `func (s *Store) Sweep() ([]LivenessChange, error)` — Task 6's goroutine calls it.
  - `const SweepTick = 30 * time.Second`

- [x] **Step 1: Write the failing tests**

Create `porta/internal/space/sweep_test.go`:

```go
// Copyright (c) 2026 Ekorau LLC

// internal/space/sweep_test.go
package space

import (
	"bytes"
	"strings"
	"testing"

	"github.com/davidg238/porta/internal/cval"
)

// livenessOf reads the stored verdict back as a string, so a test failure names
// the verdict rather than a byte slice.
func livenessOf(t *testing.T, st *Store, node string) string {
	t.Helper()
	tree, err := st.Tree(node, PathLiveness)
	if err != nil {
		t.Fatalf("Tree: %v", err)
	}
	if len(tree) == 0 {
		return ""
	}
	v, err := cval.Decode(tree[0].Value)
	if err != nil {
		t.Fatalf("decode verdict: %v", err)
	}
	sym, ok := v.(cval.Symbol)
	if !ok {
		t.Fatalf("verdict is %T, want a Symbol", v)
	}
	return string(sym)
}

func livenessRows(t *testing.T, st *Store, node string) int {
	t.Helper()
	var n int
	if err := st.DB().QueryRow(
		`SELECT COUNT(*) FROM events WHERE node_id = ? AND path = ?`, node, PathLiveness).Scan(&n); err != nil {
		t.Fatalf("count liveness events: %v", err)
	}
	return n
}

// A node checking in every 2 s and then stopping is silent once 3 x cadence has
// passed — and not one tick before.
func TestSweepDeclaresANodeSilentAfterThreeCadences(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	st.RecordExchange("n1", 41, 0, 0, 0, 0, "")
	for i := 0; i < 4; i++ {
		checkIn(t, st, &clock, "n1", 2_000)
	}

	// Still inside the window: the first sweep says live.
	clock += 2_000
	changes, err := st.Sweep()
	if err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if len(changes) != 1 || changes[0].Silent {
		t.Fatalf("changes = %+v, want one #live transition", changes)
	}
	if got := livenessOf(t, st, "n1"); got != "live" {
		t.Fatalf("verdict = %q, want live", got)
	}

	// Past 3 x 2000 ms with nothing arriving.
	clock += 5_000
	changes, err = st.Sweep()
	if err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if len(changes) != 1 || !changes[0].Silent {
		t.Fatalf("changes = %+v, want one #silent transition", changes)
	}
	if changes[0].CadenceMs != 2_000 {
		t.Fatalf("change carries cadence %d, want 2000", changes[0].CadenceMs)
	}
	if got := livenessOf(t, st, "n1"); got != "silent" {
		t.Fatalf("verdict = %q, want silent", got)
	}
}

// Writing only on transition is what makes `porta log` a list of the times the
// fleet went quiet rather than a row every 30 s. A sweep that re-wrote the same
// verdict would bury the transition it exists to record.
func TestSweepWritesOnlyOnTransition(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	st.RecordExchange("n1", 41, 0, 0, 0, 0, "")
	for i := 0; i < 4; i++ {
		checkIn(t, st, &clock, "n1", 2_000)
	}
	clock += 20_000
	if _, err := st.Sweep(); err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	after := livenessRows(t, st, "n1")
	for i := 0; i < 5; i++ {
		clock += 30_000
		changes, err := st.Sweep()
		if err != nil {
			t.Fatalf("Sweep: %v", err)
		}
		if len(changes) != 0 {
			t.Fatalf("sweep %d reported %+v, want no change — the node is still silent", i, changes)
		}
	}
	if got := livenessRows(t, st, "n1"); got != after {
		t.Fatalf("liveness event rows = %d, want %d — a steady state wrote rows", got, after)
	}
}

func TestSweepRecordsTheReturnToLife(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	st.RecordExchange("n1", 41, 0, 0, 0, 0, "")
	for i := 0; i < 4; i++ {
		checkIn(t, st, &clock, "n1", 2_000)
	}
	clock += 20_000
	st.Sweep()
	if got := livenessOf(t, st, "n1"); got != "silent" {
		t.Fatalf("verdict = %q, want silent before the recovery", got)
	}
	checkIn(t, st, &clock, "n1", 2_000)
	changes, err := st.Sweep()
	if err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if len(changes) != 1 || changes[0].Silent {
		t.Fatalf("changes = %+v, want one #live transition", changes)
	}
	if got := livenessOf(t, st, "n1"); got != "live" {
		t.Fatalf("verdict = %q, want live", got)
	}
}

// A node porta has barely met gets no verdict at all — gw/liveness stays
// UNWRITTEN rather than being guessed at.
func TestSweepGivesNoVerdictToANodeItCannotJudge(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	st.RecordExchange("n1", 41, 0, 0, 0, 0, "")
	checkIn(t, st, &clock, "n1", 2_000)
	clock += 600_000
	changes, err := st.Sweep()
	if err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if len(changes) != 0 {
		t.Fatalf("changes = %+v, want none for a node with two exchanges", changes)
	}
	if got := livenessOf(t, st, "n1"); got != "" {
		t.Fatalf("verdict = %q, want it unwritten", got)
	}
}

// A node porta has staged goal state for but never heard from has no exchanges
// at all. It must not be reported as silent — porta was never listening to it.
func TestSweepIgnoresANodeItHasNeverHeardFrom(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	b, _ := cval.Encode("alpha")
	if _, err := st.Write("ghost", "goal/name", cval.KindCell, b, "david", ""); err != nil {
		t.Fatalf("Write: %v", err)
	}
	clock += 600_000
	changes, err := st.Sweep()
	if err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if len(changes) != 0 {
		t.Fatalf("changes = %+v, want none for a node that never checked in", changes)
	}
}

// The verdict lives in the store, not in the sweep's memory: porta restarts,
// and a restart that re-announced every node's current state would fill the log
// with transitions that did not happen.
func TestSweepVerdictSurvivesARestart(t *testing.T) {
	var clock int64 = 10_000
	path := t.TempDir() + "/restart.db"
	st, err := Open(path, func() int64 { return clock })
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	st.RecordExchange("n1", 41, 0, 0, 0, 0, "")
	for i := 0; i < 4; i++ {
		clock += 2_000
		st.RecordExchange("n1", 41, 0, 0, 0, 0, "")
	}
	clock += 20_000
	st.Sweep()
	rows := livenessRows(t, st, "n1")
	st.Close()

	st2, err := Open(path, func() int64 { return clock })
	if err != nil {
		t.Fatalf("reopen: %v", err)
	}
	defer st2.Close()
	changes, err := st2.Sweep()
	if err != nil {
		t.Fatalf("Sweep after restart: %v", err)
	}
	if len(changes) != 0 {
		t.Fatalf("changes = %+v after a restart, want none — the verdict is unchanged", changes)
	}
	if got := livenessRows(t, st2, "n1"); got != rows {
		t.Fatalf("liveness rows = %d, want %d — the restart re-announced the verdict", got, rows)
	}
}

// The inferred cadence is written back to the column that has been waiting for
// it since the schema was written, so an operator can read what porta thinks a
// node's rate IS without recomputing it.
func TestSweepRecordsTheInferredCadenceOnTheNodeRow(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	st.RecordExchange("n1", 41, 0, 0, 0, 0, "")
	for i := 0; i < 4; i++ {
		checkIn(t, st, &clock, "n1", 2_000)
	}
	clock += 1_000
	st.Sweep()
	var cadence int64
	if err := st.DB().QueryRow(`SELECT COALESCE(cadence_ms,0) FROM nodes WHERE id='n1'`).Scan(&cadence); err != nil {
		t.Fatalf("read cadence_ms: %v", err)
	}
	if cadence != 2_000 {
		t.Fatalf("nodes.cadence_ms = %d, want 2000", cadence)
	}
}

// One node's failure must never cost another node its verdict. A trigger is the
// honest injection here: it makes WriteGateway fail for exactly one node, in the
// store, the way a real constraint violation would — no fake and no seam. The
// failing node sorts in the MIDDLE of `ORDER BY n.id`, because a failing LAST
// node would pass even against a sweep that aborts.
func TestSweepIsNotStoppedByOneNodesFailure(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	for _, id := range []string{"aaa", "mmm", "zzz"} {
		st.RecordExchange(id, 41, 0, 0, 0, 0, "")
	}
	for i := 0; i < 4; i++ {
		clock += 2_000
		for _, id := range []string{"aaa", "mmm", "zzz"} {
			st.RecordExchange(id, 41, 0, 0, 0, 0, "")
		}
	}
	if _, err := st.DB().Exec(`CREATE TRIGGER fail_mmm BEFORE INSERT ON cells
		WHEN NEW.node_id = 'mmm' BEGIN SELECT RAISE(ABORT, 'injected'); END`); err != nil {
		t.Fatalf("install trigger: %v", err)
	}
	clock += 20_000
	changes, err := st.Sweep()
	if err == nil || !strings.Contains(err.Error(), "mmm") {
		t.Fatalf("err = %v, want one naming mmm", err)
	}
	if len(changes) != 2 {
		t.Fatalf("changes = %+v, want the two healthy nodes", changes)
	}
	for _, id := range []string{"aaa", "zzz"} {
		if got := livenessOf(t, st, id); got != "silent" {
			t.Errorf("%s verdict = %q, want silent — one node's failure blinded the sweep", id, got)
		}
	}
	if got := livenessOf(t, st, "mmm"); got != "" {
		t.Errorf("mmm verdict = %q, want it unwritten", got)
	}
}

// The transition test compares ENCODED BYTES. Values in the store are raw
// deterministic CBOR and are decoded only for display (D4.1); a decode here
// would break that rule for no gain, so this pins the two encodings.
func TestVerdictEncodingsAreTheAlgebrasSymbols(t *testing.T) {
	live, err := cval.Encode(cval.Symbol("live"))
	if err != nil {
		t.Fatal(err)
	}
	silent, err := cval.Encode(cval.Symbol("silent"))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(live, livenessLive) || !bytes.Equal(silent, livenessSilent) {
		t.Fatalf("pre-encoded verdicts drifted from cval.Encode")
	}
}
```

- [x] **Step 2: Run the tests to verify they fail**

Run: `cd porta && go test ./internal/space/ -run Sweep -v`
Expected: FAIL — `st.Sweep undefined`.

- [x] **Step 3: Implement the sweep**

Create `porta/internal/space/sweep.go`:

```go
// Copyright (c) 2026 Ekorau LLC

// internal/space/sweep.go
package space

import (
	"bytes"
	"errors"
	"fmt"
	"time"

	"github.com/davidg238/porta/internal/cval"
)

// SweepTick is how often the verdict is re-formed. It is a floor on detection
// latency and sits below the shortest cadence in use.
const SweepTick = 30 * time.Second

// The two verdicts, pre-encoded once. The transition test compares these bytes
// against what is stored: values in the store are raw deterministic CBOR and
// are decoded only for display (D4.1), so decoding here to compare two symbols
// would break that rule for nothing.
var (
	livenessLive   = mustEncode(cval.Symbol("live"))
	livenessSilent = mustEncode(cval.Symbol("silent"))
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
	NodeID    string
	Silent    bool
	CadenceMs int64
	SinceMs   int64 // how long since the node was last heard from, at the verdict
}

// Sweep forms one liveness verdict per node and writes gw/liveness ONLY where
// the verdict differs from the one already stored.
//
// Why a ticker and not a lazy read-time view (design §6.3): the failure this
// must catch is porta hearing NOTHING, and a system that evaluates only when
// read is silent exactly when everything is. The ticker is not a convenience.
//
// Why transition-only: it makes `porta log` a list of the times the fleet went
// quiet. A row every 30 s would bury the one moment that matters.
//
// A node with no cadence yet (fewer than MinExchanges arrivals) is SKIPPED, not
// defaulted — gw/liveness stays unwritten rather than carrying a guess.
//
// ONE NODE'S FAILURE NEVER COSTS ANOTHER NODE ITS VERDICT. Errors inside the
// loop are collected and the sweep continues, so the changes returned are the
// ones actually committed and the error names every node that failed. An
// aborting sweep would park every node ordered after the failing one, tick
// after tick — a gap in observation that looks exactly like a healthy fleet,
// which is the failure this whole mechanism exists to make visible.
func (s *Store) Sweep() ([]LivenessChange, error) {
	now := s.nowMs()
	rows, err := s.db.Query(`
		SELECT n.id, COALESCE(n.last_seen_ms, 0), COALESCE(c.value_cbor, x'')
		FROM nodes n
		LEFT JOIN cells c ON c.node_id = n.id AND c.path = ?
		ORDER BY n.id`, PathLiveness)
	if err != nil {
		return nil, err
	}
	type row struct {
		id       string
		lastSeen int64
		stored   []byte
	}
	var nodes []row
	for rows.Next() {
		var r row
		if err := rows.Scan(&r.id, &r.lastSeen, &r.stored); err != nil {
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
		cadence, ok, err := s.CadenceMs(n.id)
		if err != nil {
			errs = append(errs, fmt.Errorf("node %s: cadence: %w", n.id, err))
			continue
		}
		if !ok {
			continue
		}
		if err := s.recordCadence(n.id, cadence); err != nil {
			errs = append(errs, fmt.Errorf("node %s: record cadence: %w", n.id, err))
			continue
		}
		since := now - n.lastSeen
		silent := since > SilenceFactor*cadence
		want := livenessLive
		verdict := "live"
		if silent {
			want, verdict = livenessSilent, "silent"
		}
		if bytes.Equal(n.stored, want) {
			continue
		}
		reason := fmt.Sprintf("%s: %d ms since last exchange, cadence %d ms", verdict, since, cadence)
		if err := s.WriteGateway(n.id, PathLiveness, want, reason); err != nil {
			errs = append(errs, fmt.Errorf("node %s: write verdict: %w", n.id, err))
			continue
		}
		changes = append(changes, LivenessChange{
			NodeID: n.id, Silent: silent, CadenceMs: cadence, SinceMs: since,
		})
	}
	// The query and its rows.Err() above still return immediately: those are not
	// per-node, and nothing can proceed without them.
	return changes, errors.Join(errs...)
}

// recordCadence writes the inferred rate back to the column that has been in the
// schema, unused, since the rewrite. It is written on every sweep and not only
// on a transition: it is a measurement, not a verdict.
func (s *Store) recordCadence(nodeID string, cadence int64) error {
	// An UPDATE that matches nothing is not an error here: the node row came
	// from the sweep's own query one statement ago.
	_, err := s.db.Exec(`UPDATE nodes SET cadence_ms = ? WHERE id = ?`, cadence, nodeID)
	return err
}
```

- [x] **Step 4: Run the tests to verify they pass**

Run: `cd porta && go test ./internal/space/ -v`
Expected: PASS, all sweep tests included.

- [x] **Step 5: Commit**

```bash
cd porta && gofmt -l . && go test ./...
git add porta/internal/space/sweep.go porta/internal/space/sweep_test.go
git commit -m "feat(#217): a node that stops talking becomes an event instead of an absence

The sweep forms one verdict per node against that node's own inferred cadence
and writes gw/liveness only when it changes, so porta log is a list of the times
the fleet went quiet rather than a row every 30 s. The verdict lives in the
store, so a gateway restart re-announces nothing.

A node with fewer than three exchanges is skipped: gw/liveness stays unwritten
rather than carrying a guess.

Refs #217."
```

---

### Task 5: the operator surface tells a gateway verdict from a node's own words

A `gw` row rendered by `porta log` today would print `→`, which says porta sent it to the
node. That is a lie about authority in the one place an operator reads authority.

**Files:**
- Modify: `porta/internal/portacli/log.go:36-40` (the direction arrow)
- Test: `porta/internal/portacli/get_test.go`

**Interfaces:**
- Consumes: `space.DirectionGateway`, `space.PathLiveness` (Tasks 1–2).
- Produces: no new symbols. `porta get` needs **no** code change — `bootLabel` already falls
  through to `boot=N` for a gw row, which is correct (the verdict is about that boot), and
  `isStale` already refuses to mark anything but an inbound row. Both are pinned by tests
  here so a later edit cannot quietly widen them.

- [x] **Step 1: Write the failing tests**

Append to `porta/internal/portacli/get_test.go`:

```go
// A gateway verdict crosses no boundary. Printing it with the outbound arrow
// would tell the operator porta sent this to the node, in the one column that
// carries authority.
func TestLogRendersAGatewayVerdictAsCrossingNoBoundary(t *testing.T) {
	path := getTestDB(t)
	st, err := space.Open(path, func() int64 { return 1000 })
	if err != nil {
		t.Fatal(err)
	}
	st.Ingest("c6spare", 44, []space.Entry{
		{Seq: 1, Path: "obs/health", Kind: cval.KindCell, Value: mustEnc(t, int64(1))},
	})
	if err := st.WriteGateway("c6spare", space.PathLiveness,
		mustEnc(t, cval.Symbol("silent")), "silent: 7100 ms since last exchange, cadence 2000 ms"); err != nil {
		t.Fatal(err)
	}
	st.Close()

	out, _ := runSplit(t, "log", "c6spare")
	var line string
	for _, l := range strings.Split(out, "\n") {
		if strings.Contains(l, space.PathLiveness) {
			line = l
		}
	}
	if line == "" {
		t.Fatalf("no gw/liveness line in:\n%s", out)
	}
	if strings.Contains(line, "→") || strings.Contains(line, "←") {
		t.Errorf("gw line carries a wire arrow, which claims a boundary was crossed: %q", line)
	}
	if !strings.Contains(line, "porta") {
		t.Errorf("gw line does not name porta as the actor: %q", line)
	}
	if !strings.Contains(line, "boot=44") {
		t.Errorf("gw line does not say which boot the verdict was about: %q", line)
	}
	if !strings.Contains(line, "#silent") {
		t.Errorf("gw line does not carry the verdict: %q", line)
	}
}

// A gw row is porta's opinion, not the node's observation: no node reboot makes
// it stale, and `porta get` must not mark it or count it into the mixed-read
// banner.
func TestGetNeverCallsAGatewayVerdictStale(t *testing.T) {
	path := getTestDB(t)
	st, err := space.Open(path, func() int64 { return 1000 })
	if err != nil {
		t.Fatal(err)
	}
	st.Ingest("c6spare", 44, []space.Entry{
		{Seq: 1, Path: "obs/health", Kind: cval.KindCell, Value: mustEnc(t, int64(1))},
	})
	st.WriteGateway("c6spare", space.PathLiveness, mustEnc(t, cval.Symbol("silent")), "test")
	// The node reboots and republishes nothing yet: boot 45 is current, and the
	// gw row still carries 44.
	st.Ingest("c6spare", 45, []space.Entry{
		{Seq: 1, Path: "obs/build", Kind: cval.KindCell, Value: mustEnc(t, "gdeadbeef")},
	})
	st.Close()

	out, _ := runSplit(t, "get", "c6spare")
	for _, l := range strings.Split(out, "\n") {
		if strings.Contains(l, space.PathLiveness) && strings.Contains(l, "STALE") {
			t.Errorf("gw row marked STALE: %q", l)
		}
	}
}
```

Add the `"strings"` import to `get_test.go` if it is not already present (it is).

- [x] **Step 2: Run the tests to verify they fail**

Run: `cd porta && go test ./internal/portacli/ -run 'GatewayVerdict|NeverCallsAGateway' -v`
Expected: FAIL on the arrow assertion (`gw line carries a wire arrow`). The `get` test
should already pass — a passing test here is the pin, not a gap.

- [x] **Step 3: Teach the log the third direction**

In `porta/internal/portacli/log.go`, replace:

```go
				dir := "→"
				if e.Direction == "in" {
					dir = "←"
				}
```

with:

```go
				// The arrow is an authority claim, so a gateway verdict gets
				// neither: gw/** is written ABOUT the node and crosses no
				// boundary (design 2026-08-10 §6.1).
				dir := "→"
				switch e.Direction {
				case "in":
					dir = "←"
				case space.DirectionGateway:
					dir = "◆"
				}
```

- [x] **Step 4: Run the tests to verify they pass**

Run: `cd porta && go test ./internal/portacli/ -v`
Expected: PASS.

- [x] **Step 5: Commit**

```bash
cd porta && gofmt -l . && go test ./...
git add porta/internal/portacli/log.go porta/internal/portacli/get_test.go
git commit -m "feat(#217): the log's arrow is an authority claim, so a gateway verdict gets neither

A gw row printed with the outbound arrow tells the operator porta sent it to the
node. get needed no change — bootLabel already reports the boot the verdict was
about and isStale already refuses to mark anything but an inbound row — and both
are now pinned by tests rather than by luck.

Refs #217."
```

---

### Task 6: the fourth goroutine in `porta serve`

**Files:**
- Modify: `porta/internal/portacli/serve.go` (the goroutine + `awaitServeExit`'s fourth channel)
- Test: `porta/internal/portacli/serve_test.go`

**Interfaces:**
- Consumes: `space.SweepTick`, `(*space.Store).Sweep`, `space.LivenessChange` (Task 4).
- Produces:
  - `func sweepLiveness(ctx context.Context, st *space.Store, tick time.Duration, logf func(string, ...any)) error`
  - `awaitServeExit(ctx, udpErr, blocksErr, sweepErr, httpErr)` — one more channel, in the
    same class as udp/blocks (returns only on ctx cancel).

- [x] **Step 1: Write the failing tests**

Append to `porta/internal/portacli/serve_test.go`:

```go
// The sweep is a listener in the same class as udp and blocks: it returns only
// on ctx cancel, and the drain is what keeps an in-flight Sweep from racing the
// store's deferred Close.
func TestAwaitServeExitDrainsTheSweep(t *testing.T) {
	udpErr := make(chan error, 1)
	blocksErr := make(chan error, 1)
	sweepErr := make(chan error, 1)
	httpErr := make(chan error, 1)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	done := make(chan error, 1)
	go func() { done <- awaitServeExit(ctx, udpErr, blocksErr, sweepErr, httpErr) }()

	udpErr <- nil
	blocksErr <- nil
	httpErr <- nil
	select {
	case <-done:
		t.Fatal("returned before draining sweepErr")
	case <-time.After(50 * time.Millisecond):
	}
	sweepErr <- nil
	select {
	case err := <-done:
		if err != nil {
			t.Errorf("got %v, want nil", err)
		}
	case <-time.After(time.Second):
		t.Fatal("did not return after the sweep drained")
	}
}

// A sweep that reports a transition logs it, and one that reports nothing says
// nothing — a line every 30 s per node would bury the transitions.
func TestSweepLivenessLogsOnlyTransitions(t *testing.T) {
	dir := t.TempDir()
	var clock int64 = 10_000
	st, err := space.Open(dir+"/sweep.db", func() int64 { return clock })
	if err != nil {
		t.Fatal(err)
	}
	defer st.Close()
	st.RecordExchange("n1", 41, 0, 0, 0, 0, "")
	for i := 0; i < 4; i++ {
		clock += 2_000
		st.RecordExchange("n1", 41, 0, 0, 0, 0, "")
	}
	clock += 20_000

	var mu sync.Mutex
	var lines []string
	logf := func(format string, args ...any) {
		mu.Lock()
		defer mu.Unlock()
		lines = append(lines, fmt.Sprintf(format, args...))
	}

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- sweepLiveness(ctx, st, time.Millisecond, logf) }()

	deadline := time.After(2 * time.Second)
	for {
		mu.Lock()
		n := len(lines)
		mu.Unlock()
		if n > 0 {
			break
		}
		select {
		case <-deadline:
			cancel()
			t.Fatal("no transition logged within 2s")
		case <-time.After(5 * time.Millisecond):
		}
	}
	// Many ticks have now run; only the ONE transition may have been logged.
	time.Sleep(50 * time.Millisecond)
	cancel()
	if err := <-done; err != nil {
		t.Fatalf("sweepLiveness returned %v, want nil on ctx cancel", err)
	}
	mu.Lock()
	defer mu.Unlock()
	if len(lines) != 1 {
		t.Fatalf("logged %d lines across many ticks, want 1:\n%v", len(lines), lines)
	}
	if !strings.Contains(lines[0], "n1") || !strings.Contains(lines[0], "silent") {
		t.Fatalf("line = %q, want it to name the node and the verdict", lines[0])
	}
}
```

Add `"fmt"`, `"strings"`, `"sync"` and `"github.com/davidg238/porta/internal/space"` to
`serve_test.go`'s imports.

- [x] **Step 2: Run the tests to verify they fail**

Run: `cd porta && go test ./internal/portacli/ -run 'DrainsTheSweep|SweepLiveness' -v`
Expected: FAIL — `sweepLiveness undefined`, and `awaitServeExit` takes 4 arguments not 5.

- [x] **Step 3: Add the goroutine and widen `awaitServeExit`**

In `porta/internal/portacli/serve.go`, add after `newServeCmd`:

```go
// sweepLiveness runs the silence sweep until ctx is cancelled — the fourth
// listener goroutine, alongside UDP, blocks and HTTP.
//
// A sweep failure LOGS AND CONTINUES rather than taking the gateway down: the
// space listener is the node-facing contract and this is an observer of it. An
// observer that can stop the thing it observes is worse than no observer.
func sweepLiveness(ctx context.Context, st *space.Store, tick time.Duration, logf func(string, ...any)) error {
	t := time.NewTicker(tick)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return nil
		case <-t.C:
			changes, err := st.Sweep()
			if err != nil {
				// Log it, but do NOT discard `changes`. Sweep isolates per
				// node, so an error for one node arrives ALONGSIDE verdicts
				// that were really committed for others; dropping them here
				// would lose transitions the store already holds.
				logf("porta: liveness sweep: %v", err)
			}
			for _, c := range changes {
				verdict := "live"
				if c.Silent {
					verdict = "silent"
				}
				logf("porta: %s is %s — %d ms since its last exchange, cadence %d ms",
					c.NodeID, verdict, c.SinceMs, c.CadenceMs)
			}
		}
	}
}
```

Add `"time"` to the file's imports.

Inside `RunE`, after the blocks listener is started and before the HTTP block:

```go
			// The fourth goroutine: porta's own verdict on which nodes are still
			// talking. It is a ticker and not a read-time view because the
			// failure it must catch is porta hearing nothing, and a view that
			// evaluates only when read is silent exactly when everything is
			// (design 2026-08-10 §6.3).
			sweepErr := make(chan error, 1)
			go func() { sweepErr <- sweepLiveness(ctx, st, space.SweepTick, log.Printf) }()
			log.Printf("porta: sweeping for silent nodes every %s", space.SweepTick)
```

and change the return to:

```go
			return awaitServeExit(ctx, udpErr, blocksErr, sweepErr, httpErr)
```

Then widen `awaitServeExit`:

```go
// awaitServeExit blocks until a listener goroutine exits or ctx is cancelled.
// It is transport-agnostic — four error channels; only httpErr may be nil (udp,
// blocks and the liveness sweep are unconditional).
//
//   - A listener error (non-nil udpErr, blocksErr, sweepErr or httpErr)
//     propagates verbatim.
//   - A clean HTTP exit (httpErr delivers nil) while the others still run falls
//     through to wait on whichever of them reports next.
//   - On ctx cancel (SIGINT/SIGTERM) it DRAINS every channel first, so the
//     listeners and the HTTP server's 5s graceful Shutdown complete before RunE
//     returns and its deferred Close calls fire (#7). Draining the sweep is not
//     cosmetic: it holds the store, and Close under an in-flight Sweep is a race.
//     A nil channel's select arm blocks forever and the nil guard skips draining it.
// The ctx-Done check runs in its OWN non-blocking select before the main one.
// Putting all five cases in a single select would race: once ctx is cancelled,
// a listener that has already buffered a value makes its case just as "ready"
// as ctx.Done(), and Go picks uniformly among ready cases — so a listener's
// case can win and return early, skipping the drain of the others entirely.
// That is not just a test artifact: it is the same store-Close race this
// drain exists to prevent, so ctx cancellation must take priority outright.
func awaitServeExit(ctx context.Context, udpErr, blocksErr, sweepErr, httpErr <-chan error) error {
	drain := func() error {
		if udpErr != nil {
			<-udpErr
		}
		if blocksErr != nil {
			<-blocksErr
		}
		if sweepErr != nil {
			<-sweepErr
		}
		if httpErr != nil {
			<-httpErr
		}
		return nil
	}
	select {
	case <-ctx.Done():
		return drain()
	default:
	}
	select {
	case err := <-udpErr:
		return err
	case err := <-blocksErr:
		return err
	case err := <-sweepErr:
		return err
	case err := <-httpErr:
		if err == nil {
			select {
			case err := <-udpErr:
				return err
			case err := <-blocksErr:
				return err
			case err := <-sweepErr:
				return err
			}
		}
		return err
	case <-ctx.Done():
		return drain()
	}
}
```

- [x] **Step 4: Update the three existing `awaitServeExit` call sites in the tests**

In `porta/internal/portacli/serve_test.go`:

- `TestAwaitServeExitDrainsOnCtxCancel` — add `sweepErr := make(chan error, 1)`, pass it as
  the third argument, and send `sweepErr <- nil` alongside the other three.
- `TestAwaitServeExitNilHTTPErr` — same, passing `sweepErr` third and sending nil to it.
- `TestAwaitServeExitPropagatesListenerErrors` — every call becomes
  `awaitServeExit(ctx, udpErr, blocksErr, make(chan error, 1), nil)` (or `httpErr` where the
  case uses one), and add a fourth case:

```go
	// The sweep fails → its error propagates like any other listener's.
	udpErr = make(chan error, 1)
	blocksErr = make(chan error, 1)
	sweepErr := make(chan error, 1)
	sweepErr <- errSentinel
	if got := awaitServeExit(ctx, udpErr, blocksErr, sweepErr, nil); got != errSentinel {
		t.Errorf("sweep error: got %v, want errSentinel", got)
	}
```

- [x] **Step 5: Run the tests to verify they pass**

Run: `cd porta && go test ./internal/portacli/ -v`
Expected: PASS.

- [x] **Step 6: Run the whole gate, including the node interop gate**

```bash
cd porta && gofmt -l . && go test ./...
cd ../nodus/tuvm && bash tests/porta_sync_host.sh
```
Expected: `ok` for every porta package and `PORTA SYNC OK`. **Never run this concurrently
with an nsl suite** — they share `/tmp` paths.

- [x] **Step 7: Commit**

```bash
git add porta/internal/portacli/serve.go porta/internal/portacli/serve_test.go
git commit -m "feat(#217): porta serve watches for silence

A fourth goroutine beside UDP, blocks and HTTP. A sweep failure logs and
continues — an observer that can take down the thing it observes is worse than
no observer — and awaitServeExit drains it on cancel, because it holds the store
and Close under an in-flight Sweep is a race.

Host gate green including tests/porta_sync_host.sh (PORTA SYNC OK).

Refs #217."
```

---

### Task 7: the falsification — §7 step 3, on the bench

The design's prediction, verbatim: *"Halting a node by hand flips `gw/liveness` to
`#silent` within 3 × cadence + 30 s, and one event row records the transition."*

Everything above is host-green, which on this program is not evidence about a running
gateway: **all of porta's operational state is on gw85224-01, not the workstation** — ssh
first, before any local probe. Restarting porta costs every node an outage, so the restart
is deliberate and scheduled, not incidental.

**Files:**
- No source changes. Record the readings in the issue and in the close-out commit.

**Interfaces:**
- Consumes: everything above, deployed.
- Produces: the verified prediction, and the actual numbers (cadence, detection latency) for
  the close-out.

- [x] **Step 1: Find the live gateway before touching anything**

```bash
ssh gw85224-01 'ps aux | grep -c "[p]orta"; ls -la ~/porta-v2; df -h ~; ls -la ~/porta-v2/*.db*'
```
Record: the running binary's path and flags, the db path, the db size, and free disk. **The
migration copies both `cells` and `events` inside one transaction**, so the WAL needs
headroom on the order of the events table. If free space is less than 3× the db size, stop
and report rather than migrating.

- [x] **Step 2: Back the store up, with the gateway still running**

```bash
ssh gw85224-01 'cd ~/porta-v2 && sqlite3 porta.db ".backup porta.db.pre-gw-$(date +%Y%m%dT%H%M%S)" && ls -la porta.db*'
```
(`.backup` is WAL-safe on a live db; a plain `cp` is not.) Adjust the db filename to what
step 1 reported.

- [x] **Step 3: Build for the gateway and copy it up**

```bash
cd ~/workspace/rete/porta
ssh gw85224-01 'uname -m'            # confirm the arch before choosing GOARCH
GOOS=linux GOARCH=<arch from above> go build -o /tmp/porta ./cmd/porta
scp /tmp/porta gw85224-01:~/porta-v2/porta.new
```

- [x] **Step 4: Restart the gateway and confirm the migration ran**

Announce the outage first — every node loses its exchange until porta is back.

```bash
ssh gw85224-01 'cd ~/porta-v2 && pkill -f "porta serve"; sleep 2; mv porta.new porta && chmod +x porta'
# restart with the SAME flags step 1 recorded, under the same detach it already used
ssh gw85224-01 'cd ~/porta-v2 && sqlite3 porta.db "SELECT sql FROM sqlite_master WHERE name IN (\"cells\",\"events\") AND type=\"table\";" | grep -c gw'
```
Expected: `2` — both tables carry the third direction. If it reads 0, the migration did not
run and nothing below means anything.

- [x] **Step 5: Watch one live node acquire a verdict**

```bash
ssh gw85224-01 'cd ~/porta-v2 && sqlite3 porta.db "SELECT id, cadence_ms, last_seen_ms FROM nodes;"'
./porta get c6-bell gw/          # via the gateway's own CLI on gw85224-01
```
Expected within one sweep tick (30 s): `nodes.cadence_ms` is populated for every node with
three or more exchanges, and `gw/liveness` reads `#live`. Record the inferred cadence for
each node and check it against what that node actually does (the bell publishes every 30 s;
c6spare's bridge is faster). **A cadence that disagrees with the node's known period is the
finding**, not a rounding detail.

- [x] **Step 6: Halt a node by hand and time the transition**

Use **c6spare**, the attached disposable board — `TARGET_DEFAULT_BOARD`, resolved through
the collector env files and never by a ttyUSB number. **Do not touch c6-bell**: it is the
long-duration witness and the only unattended wallwart node, and flashing it rings the bell.
Halting here means removing power, not reflashing.

Record `T0` (the moment power is removed), then poll:

```bash
ssh gw85224-01 'cd ~/porta-v2 && while true; do date -u +%H:%M:%S; ./porta get c6spare gw/; sleep 5; done'
```

Expected: `#silent` within **3 × cadence + 30 s** of the last exchange. Record the observed
latency and compare it to the bound. A transition that arrives LATE is a finding about the
sweep; one that arrives EARLY means the cadence was inferred too short, which is the same
finding wearing the other costume.

- [x] **Step 7: Confirm exactly one row records it, and only one**

```bash
ssh gw85224-01 'cd ~/porta-v2 && ./porta log c6spare --limit 20'
```
Expected: exactly one `gw/liveness` line carrying `#silent`, with `porta` as the actor and
no wire arrow. Then leave it silent for at least five more sweep ticks (2.5 min) and re-run:
still exactly one row. **A second row is the transition-only rule failing**, and it is the
failure that would make `porta log` unreadable in a week.

- [x] **Step 8: Power the node back and confirm the return**

Expected: one further row, `#live`, within one sweep tick of its first exchange. Record the
latency.

- [x] **Step 9: Record the readings on the issue**

Post to #217: the inferred cadence per node, the observed silent-detection latency against
the `3 × cadence + 30 s` bound, the row counts from steps 7 and 8, and the db size before
and after the migration. **State plainly whether the prediction held.** If any part did not
run, say which and why — a partial bench run reported as a pass is the false-green class
this bench already catalogues.

---

### Task 8: promotion — the rung is not closed until the knowledge leaves this plan

Every item here is required by the promotion rule in `CLAUDE.md`: the dated design stays
behind as the historical "why", and the mainline documents carry what is now true.

**Files:**
- Modify: `protocol/PROTOCOL-NSL.md` §3
- Modify: `docs/design/2026-08-10-node-memory-and-liveness-design.md` §6, §7, §8
- Modify: `porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md` (store schema)
- Modify: `porta/docs/ARCHITECTURE.md`
- Modify: `review/tests/CLAIMS.md` (append one row)
- Modify: `review/program-sequence.md` (the rung)
- Modify: `docs/next-steps-overview.md` (the three-item ladder entry for #217)

- [x] **Step 1: `protocol/PROTOCOL-NSL.md` §3 gains the fifth row**

The picture at line ~165 gains a row that crosses no boundary, and the caption below it
gains the authority line. Add inside the porta box, after `node/<id>/dbg/resp`:

```
  │  node/<id>/gw/**         │   (never)   │  — not present on the node —      │
```

and extend the caption:

```
       porta writes ONLY goal/** and dbg/req;    the bridge job holds the
       the node writes ONLY its own node/<id>/** radio capability + these grants
       gw/** is porta's own verdict ABOUT a node: gateway-authored, never on the
       wire, and excluded from the exchange by the store's direction column
       rather than by a filter (design 2026-08-10 §6.1)
```

Keep the ASCII box aligned — check the rendered width after editing.

- [x] **Step 2: 2026-08-10 §6 records what was BUILT, and §8 gains the rows**

In `docs/design/2026-08-10-node-memory-and-liveness-design.md`:

- Head §6 with `**BUILT 2026-08-XX (#217).**` and the commit range.
- Record the two decisions this plan pinned that §6 left open, in §6.1: **`gw` rows take
  `seq` 0** (gw is not a sequence space) and **carry the node's current `boot_id`** (the
  verdict is about a node on a boot).
- Add to §8's constants table:

| Constant | Value | Standing |
|---|---|---|
| minimum exchanges for a verdict | 3 | **Chosen.** Two arrivals is one gap, which is a sample and not a cadence. Earns revision if a real fleet has a node whose first verdict matters sooner than its third check-in |

- In §7's staging table, mark step 3 **DONE** with the observed detection latency from
  Task 7, and note that step 4's gate — the HALT arm — is now unblocked.

- [x] **Step 3: the rewrite design's store schema gains the third direction**

In `porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md`, wherever the `cells`
and `events` schemas are given, update `direction` to `'in' | 'out' | 'gw'` and add one
sentence: a `gw` row is porta's verdict about the node, excluded from `OutboundSince` by
construction. Do not add a new table to the four-table story — **there is still no fifth
table**, which is the point.

- [x] **Step 4: `porta/docs/ARCHITECTURE.md` gains the fourth goroutine**

Add the sweep to whatever list of `serve`'s listeners exists (the file is stale in parts —
fix only what this rung touches; do not sweep it).

- [x] **Step 5: `review/tests/CLAIMS.md` gains one row**

Append **one** row in the existing format — do not reorder, retitle or tidy anything else in
`review/`. The criterion is roughly: *"A node that stops talking becomes an event porta
recorded, not an absence an operator has to notice"*, proven by the named tests from Tasks
1–6 plus the Task 7 bench reading, and enforced by `porta/internal/space/sweep.go` +
`schema.go`'s third direction. Include the design reference.

- [x] **Step 6: `review/program-sequence.md` gains the rung**

Follow the file's existing entry format exactly. One entry for #217, naming what it
unblocks: 2026-08-10 §7 step 4, the HALT arm of #215's loop breaker.

- [x] **Step 7: `docs/next-steps-overview.md`**

Update the `#217` bullet from "queued with #215" to DONE with the observed numbers, and
strike the "either it lands alongside or the HALT arm does not ship" constraint from #215's
bullet — it is discharged. Keep the ordering paragraph: the order was the argument.

- [x] **Step 8: Final gate and close-out commit**

```bash
cd ~/workspace/rete/porta && gofmt -l . && go test ./...
cd ~/workspace/rete/nodus/tuvm && bash tests/porta_sync_host.sh
```

```bash
git add protocol/PROTOCOL-NSL.md docs/design/2026-08-10-node-memory-and-liveness-design.md \
        porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md porta/docs/ARCHITECTURE.md \
        review/tests/CLAIMS.md review/program-sequence.md docs/next-steps-overview.md
git commit -m "docs(#217): the silence sweep is built, and the HALT arm is unblocked

<the observed cadence per node, the detection latency against the 3 x cadence +
30 s bound, and the row counts — the numbers, not the adjective>

2026-08-10 §6 is BUILT; §7 step 3 is done, which is what step 4 was gated on.
The two things §6 left open are pinned in it now: a gw row takes seq 0 because
gw is not a sequence space, and carries the node's current boot because the
verdict is about a node on a boot.

Closes #217. Refs #215."
```

- [x] **Step 9: Close #217 with the verification detail**

Post the close-out comment on #217 — the readings, the deviations from what this plan
proposed, and anything the bench run refuted. Then say on **#215** that its inherited
constraint is discharged and the HALT arm may ship.

---

## Self-review

**Spec coverage.** §6.1 the namespace → Tasks 1–2 (third direction, structural exclusion,
tested by `TestGatewayVerdictNeverRidesTheWire`). §6.2 the sweep → Tasks 3–4 and 6 (30 s
ticker, median of last 10 gaps, cadence written back to `nodes.cadence_ms`, `3 × cadence`,
transition-only writes, no verdict under three exchanges). §6.3 not a lazy view → Task 6's
ticker, with the reasoning in the code comment. §7 step 3's falsification → Task 7. §8's
constants → Task 8 step 2. The one thing §6 does **not** say and this plan decides: `seq`
and `boot_id` for a gw row — pinned in the constants table above, tested in Task 2, promoted
in Task 8.

**Out of scope, deliberately.** Nothing on the node side; nothing on the wire; no HTTP or
`/api/status` surface for liveness (`porta get <node> gw/` answers it, and a new endpoint
would be a verb the store does not have); no alerting. The `httpsrv` fleet view is a
candidate consumer once there is a verdict to show, and is not this rung.

**Interfaces.** `DirectionGateway` (Task 1) → used by Tasks 2, 4, 5. `PathLiveness`,
`WriteGateway` (Task 2) → used by Task 4 and both display tests. `CadenceMs`,
`SilenceFactor`, `MinExchanges`, `CadenceWindow` (Task 3) → used by Task 4. `Sweep`,
`LivenessChange`, `SweepTick` (Task 4) → used by Task 6. Verdict strings are `live` /
`silent` everywhere, encoded as `cval.Symbol`, and `TestVerdictEncodingsAreTheAlgebrasSymbols`
pins the pre-encoded bytes against `cval.Encode` so the two cannot drift.

**What was already run.** The migration mechanics in Task 1 are not plan-embedded
guesses: they were spiked against both store shapes on 2026-08-18 and the two defects that
run found (an adopted leftover table, and the DDL coming back quoted) are fixed in the code
above. Everything else in this plan is unrun code and should be treated as such.

**The one risk worth naming before Task 7.** The migration rebuilds two tables of a live
store inside one transaction. It is tested against a hand-built legacy store, but the live
store on gw85224-01 is bigger than any test fixture and it is the record #190's evidence
rests on. Step 2's `.backup` is not optional, and step 1's free-space check gates the whole
thing.
