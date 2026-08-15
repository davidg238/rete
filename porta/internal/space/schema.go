// Copyright (c) 2026 Ekorau LLC

// Package space is porta's per-node space mirror: the cell tree, the
// append-only write log, and the check-in log. It replaces v1's nine
// per-feature tables (see the design record §4.1) with four generic ones,
// because the unit of extension is now a path, not a table.
package space

const schema = `
PRAGMA journal_mode=WAL;

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
  direction  TEXT NOT NULL CHECK (direction IN ('in','out')),
  actor      TEXT NOT NULL DEFAULT '',
  reason     TEXT NOT NULL DEFAULT '',
  boot_id    INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_events_node ON events(node_id, id);
CREATE INDEX IF NOT EXISTS idx_events_out ON events(node_id, direction, seq);

-- One row per check-in: the heartbeat AND the replay index (design D4.3).
-- Without it the batch boundary a value arrived on is unrecoverable.
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
