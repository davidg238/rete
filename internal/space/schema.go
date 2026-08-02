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
CREATE TABLE IF NOT EXISTS cells (
  node_id    TEXT NOT NULL,
  path       TEXT NOT NULL,
  seq        INTEGER NOT NULL,
  kind       INTEGER NOT NULL,
  value_cbor BLOB NOT NULL,
  ts_ms      INTEGER NOT NULL,
  direction  TEXT NOT NULL CHECK (direction IN ('in','out')),
  PRIMARY KEY (node_id, path)
);
CREATE INDEX IF NOT EXISTS idx_cells_seq ON cells(node_id, direction, seq);

-- Append-only, both directions, with provenance (design D2/D3). There is no
-- tombstone column: stopped state is written explicitly as a value, and
-- genuine removal is an action on a do/ stream.
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
  reason     TEXT NOT NULL DEFAULT ''
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
