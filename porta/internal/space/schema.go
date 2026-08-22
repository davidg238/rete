// Copyright (c) 2026 Ekorau LLC

// Package space is porta's per-node space mirror: the cell tree, the
// append-only write log, and the check-in log. It replaces v1's nine
// per-feature tables (see the design record §4.1) with four generic ones,
// because the unit of extension is now a path, not a table.
package space

import (
	"database/sql"
	"fmt"
	"strings"
)

const schema = `
PRAGMA journal_mode=WAL;

CREATE TABLE IF NOT EXISTS nodes (
  id            TEXT PRIMARY KEY,
  name          TEXT,
  first_seen_ms INTEGER,
  last_seen_ms  INTEGER,
  boot_id       INTEGER,
  cursor_acked  INTEGER NOT NULL DEFAULT 0,
  node_seq_hw   INTEGER NOT NULL DEFAULT 0,
  out_seq       INTEGER NOT NULL DEFAULT 0,
  -- Successor to v1's nodes.source_addr, which the capability audit (design
  -- §4.2) caught with no home. It is not merely a display field: serverstat
  -- .TransportOf() reads it to produce the wifi-vs-Thread fleet breakdown.
  last_addr     TEXT
);

-- Latest value per path. Paths are NODE-RELATIVE (design D4.5): the
-- node/<id>/** prefix in PROTOCOL-NSL.md §3 is a view, never storage, so a
-- captured session can be re-pointed at a different node.
--
-- This table holds BOTH directions, because the operator's goal/X vs obs/X
-- diff is one query over one table. direction is therefore load-bearing and
-- not decoration: seq is drawn from porta's out_seq for 'out' rows and from
-- the NODE's push counter for 'in' rows, so the two are not comparable and a
-- read that spans them is meaningless. OutboundSince filters on it.
-- boot_id answers "which boot published this", which no other column can: the
-- node rewrites its whole tree after a reboot but the rows arrive over several
-- seconds, so a read taken inside that window mixes two boots and looks exactly
-- like a coherent one (rete#210). The value is not new — it is the "bid" the
-- node already sends on every envelope. For 'out' rows it is 0, because porta's
-- own intent has no node boot; direction separates that from the other 0,
-- which is a node with no persistent identity (posix without TUVM_BOOTID_FILE,
-- or a board without CONFIG_SETTINGS) and reads as UNKNOWN, never as stale.
--
-- 'gw' is a verdict porta formed ABOUT the node — gateway-authored, never on
-- the wire (design 2026-08-10 §6.1). It is a third direction rather than a
-- fifth table because the unit of extension is a path (D2), and it is carried
-- by THIS column because OutboundSince already filters direction = 'out': the
-- exclusion from the radio is then structural, not a filter someone must
-- remember to write. porta writes ONLY goal/** and dbg/req to a node, and a
-- 'gw' row is neither node-authored nor deliverable to it, so it fits none of
-- the four wire namespaces.
CREATE TABLE IF NOT EXISTS cells (
  node_id    TEXT NOT NULL,
  path       TEXT NOT NULL,
  seq        INTEGER NOT NULL,
  kind       INTEGER NOT NULL,
  value_cbor BLOB NOT NULL,
  ts_ms      INTEGER NOT NULL,
  direction  TEXT NOT NULL CHECK (direction IN ('in','out','gw')),
  boot_id    INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (node_id, path)
);
CREATE INDEX IF NOT EXISTS idx_cells_seq ON cells(node_id, direction, seq);

-- Append-only, both directions, with provenance (design D2/D3). There is no
-- tombstone column: stopped state is written explicitly as a value, and
-- genuine removal is an action on a do/ stream.
--
-- boot_id has the same meaning as cells.boot_id, and here it is what lets
-- "porta log --boot N" reconstruct one boot truthfully — which the cell tree
-- cannot do, since cells keeps only the latest value per path and could answer
-- at most "paths whose LAST write fell in boot N" while promising otherwise.
--
-- KEEP COLUMN COMMENTS ABOVE THE STATEMENT, NEVER INSIDE THE PARENTHESES.
-- sqlite implements ALTER TABLE ... DROP COLUMN by editing this DDL text, and an
-- interior comment makes the rewritten statement unparseable ("incomplete
-- input"). Found by the migration test, which is the only thing that exercises
-- that path.
CREATE TABLE IF NOT EXISTS events (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  node_id    TEXT NOT NULL,
  path       TEXT NOT NULL,
  seq        INTEGER NOT NULL,
  kind       INTEGER NOT NULL,
  value_cbor BLOB NOT NULL,
  ts_ms      INTEGER NOT NULL,
  direction  TEXT NOT NULL CHECK (direction IN ('in','out','gw')),
  actor      TEXT NOT NULL DEFAULT '',
  reason     TEXT NOT NULL DEFAULT '',
  boot_id    INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_events_node ON events(node_id, id);
CREATE INDEX IF NOT EXISTS idx_events_out ON events(node_id, direction, seq);

-- One row per REQUEST, not per check-in: a check-in is a pull followed by a
-- push, and handlePull/handlePush (internal/wire/server.go) each call
-- RecordExchange once, so every check-in leaves TWO rows here. What the table
-- is for, now that rete#231 D4 has deleted the cadence inference: it is the
-- REPLAY INDEX (design D4.3) and the LAST_SEEN HEARTBEAT — a node's cursor and
-- boot id at each request, and the arrival that nodes.last_seen_ms is stamped
-- from. Both want every request; neither cares which leg produced it, which is
-- why the leg column is gone (see dropColumns). The only reader that ever
-- needed the distinction was the inference, which had to count pulls alone or
-- infer a period exactly half the node's real one (#217 C1).
CREATE TABLE IF NOT EXISTS exchanges (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  node_id    TEXT NOT NULL,
  ts_ms      INTEGER NOT NULL,
  boot_id    INTEGER NOT NULL,
  cursor_in  INTEGER NOT NULL,
  cursor_out INTEGER NOT NULL,
  pushed_lo  INTEGER NOT NULL,
  pushed_hi  INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_exchanges_node ON exchanges(node_id, ts_ms);
`

// addColumns are run after schema on every Open. CREATE TABLE IF NOT EXISTS
// cannot add a column to a table that already exists, and sqlite has no
// ADD COLUMN IF NOT EXISTS, so each statement is executed and its "duplicate
// column name" error discarded — the second Open is a no-op by construction.
//
// This is not the start of a migration framework, and the standing "no backward
// compatibility" rule is not a licence to delete a running store: it governs the
// wire and the record shapes. The live soak store carries the history rete#190's
// evidence rests on, and rebuilding it would also force every node through a
// full re-push. Rows written before the column exists read 0 / unknown, which is
// true of them, and each path acquires a real boot id the next time it is
// written — the store heals one write at a time and nothing is backfilled.
var addColumns = []string{
	`ALTER TABLE cells  ADD COLUMN boot_id INTEGER NOT NULL DEFAULT 0`,
	`ALTER TABLE events ADD COLUMN boot_id INTEGER NOT NULL DEFAULT 0`,
}

// dropColumns are run after addColumns on every Open, and their errors are
// discarded exactly as addColumns' are: on a fresh store the column is already
// absent and sqlite says so. Both entries are columns whose ONLY reader was the
// cadence inference rete#231 D4 deleted, and a column nobody reads is precisely
// what that decision mocked — nodes.cadence_ms was chosen over a declaration in
// 2026-08-10 D8 because it "was found to be a dead column", and leaving it dead
// a second time would be the same mistake with the reasons reversed.
//
// KEEP THE `nodes` AND `exchanges` COLUMN COMMENTS ABOVE THE STATEMENT, NEVER
// INSIDE THE PARENTHESES: sqlite implements DROP COLUMN by editing the stored
// DDL text, and an interior comment makes the rewritten statement unparseable.
var dropColumns = []string{
	`ALTER TABLE nodes DROP COLUMN cadence_ms`,
	`ALTER TABLE exchanges DROP COLUMN leg`,
}

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
// SPIKED 2026-08-18 before this plan was handed over, on both shapes: sqlite
// strips `IF NOT EXISTS` from the STORED DDL text immediately on any
// `CREATE TABLE IF NOT EXISTS`, so both a fresh store and a store whose
// boot_id was appended by ALTER have the clause already gone. Both migrate, keep
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

// assertDirectionAcceptsGw is the check migrateDirectionAcceptsGw's own
// comment warns is missing: that function SKIPS any DDL shape it does not
// recognise (a defensive read, not a defensive write), so a store whose
// stored DDL drifted from either shape this code knows would fall through
// unmigrated and unmigrated-looking IDENTICAL to a healthy store — a dead
// instrument that presents as a live one, right up until the first gw/ write
// hits the old two-value CHECK and the gateway's own liveness sweep starts
// failing with no clue why. Called once, right after the migration, so the
// failure surfaces at Open — loudly, at startup — rather than three sweeps
// later on a production box.
func assertDirectionAcceptsGw(db *sql.DB) error {
	for _, tbl := range []string{"cells", "events"} {
		var ddl string
		if err := db.QueryRow(
			`SELECT sql FROM sqlite_master WHERE type='table' AND name=?`, tbl).Scan(&ddl); err != nil {
			return fmt.Errorf("read %s DDL: %w", tbl, err)
		}
		if !strings.Contains(ddl, "'gw'") {
			return fmt.Errorf("%s.direction does not accept 'gw' after migration — DDL:\n%s", tbl, ddl)
		}
	}
	return nil
}
