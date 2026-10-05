-- porta's store, written by porta's own writer: porta/internal/space/fixtures_test.go.
-- Do not edit; regenerate with -update.
CREATE TABLE nodes (
  id            TEXT PRIMARY KEY,
  name          TEXT,
  first_seen_ms INTEGER,
  last_seen_ms  INTEGER,
  boot_id       INTEGER,
  cursor_acked  INTEGER NOT NULL DEFAULT 0,
  node_seq_hw   INTEGER NOT NULL DEFAULT 0,
  cursor_hw     INTEGER NOT NULL DEFAULT 0,
  served_hw     INTEGER NOT NULL DEFAULT 0,
  out_seq       INTEGER NOT NULL DEFAULT 0,
  -- Successor to v1's nodes.source_addr, which the capability audit (design
  -- §4.2) caught with no home. It is not merely a display field: serverstat
  -- .TransportOf() reads it to produce the wifi-vs-Thread fleet breakdown.
  last_addr     TEXT
, wak    INTEGER, wak_ms INTEGER, mark_wak INTEGER, mark_ms  INTEGER, mark_cad INTEGER);
CREATE TABLE cells (
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
CREATE TABLE events (
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
CREATE TABLE exchanges (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  node_id    TEXT NOT NULL,
  ts_ms      INTEGER NOT NULL,
  boot_id    INTEGER NOT NULL,
  cursor_in  INTEGER NOT NULL,
  cursor_out INTEGER NOT NULL,
  pushed_lo  INTEGER NOT NULL,
  pushed_hi  INTEGER NOT NULL
);
CREATE TABLE gw_runs (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  started_ms INTEGER NOT NULL,
  alive_ms   INTEGER NOT NULL
);
CREATE TABLE meta (
  key   TEXT PRIMARY KEY,
  value BLOB NOT NULL
);
INSERT INTO "nodes" VALUES('n1',NULL,1700000001000,1700000002000,41,0,1,0,0,1,NULL,NULL,NULL,NULL,NULL,NULL);
INSERT INTO "cells" VALUES('n1','goal/name',1,2,X'6A62617468726F6F6D2D31',1700000001000,'out',0);
INSERT INTO "cells" VALUES('n1','obs/name',1,2,X'6A62617468726F6F6D2D31',1700000002000,'in',41);
INSERT INTO "events" VALUES(1,'n1','goal/name',1,2,X'6A62617468726F6F6D2D31',1700000001000,'out','op','commission',0);
INSERT INTO "events" VALUES(2,'n1','obs/name',1,2,X'6A62617468726F6F6D2D31',1700000002000,'in','n1','',41);
DELETE FROM sqlite_sequence;
INSERT INTO "sqlite_sequence" VALUES('events',2);
CREATE INDEX idx_cells_seq ON cells(node_id, direction, seq);
CREATE INDEX idx_events_node ON events(node_id, id);
CREATE INDEX idx_events_out ON events(node_id, direction, seq);
CREATE INDEX idx_exchanges_node ON exchanges(node_id, ts_ms);
CREATE INDEX idx_gw_runs_started ON gw_runs(started_ms);
