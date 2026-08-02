"""porta-lite store: space_cells + space_events, SQLite (stdlib).
Schema mirrors protocol/PROTOCOL-NSL.md:349-352. value stored as JSON text
(the codec's decoded dict) rather than raw CBOR — the gw is the readable end."""
import json
import sqlite3


class Store:
    def __init__(self, path):
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS space_events(
              node_id TEXT, path TEXT, seq INTEGER, value_json TEXT,
              ts REAL, direction TEXT);
            CREATE TABLE IF NOT EXISTS space_cells(
              node_id TEXT, path TEXT, seq INTEGER, value_json TEXT, ts REAL,
              PRIMARY KEY(node_id, path));
        """)
        self.db.commit()

    def write_event(self, node_id, path, seq, value, ts, direction):
        self.db.execute(
            "INSERT INTO space_events VALUES(?,?,?,?,?,?)",
            (node_id, path, seq, json.dumps(value), ts, direction))
        self.db.commit()

    def upsert_cell(self, node_id, path, seq, value, ts):
        self.db.execute(
            "INSERT INTO space_cells VALUES(?,?,?,?,?) "
            "ON CONFLICT(node_id, path) DO UPDATE SET "
            "seq=excluded.seq, value_json=excluded.value_json, ts=excluded.ts "
            "WHERE excluded.seq >= space_cells.seq",
            (node_id, path, seq, json.dumps(value), ts))
        self.db.commit()

    def latest_cell(self, node_id, path):
        row = self.db.execute(
            "SELECT value_json FROM space_cells WHERE node_id=? AND path=?",
            (node_id, path)).fetchone()
        return json.loads(row[0]) if row else None

    def event_count(self, node_id):
        return self.db.execute(
            "SELECT COUNT(*) FROM space_events WHERE node_id=?",
            (node_id,)).fetchone()[0]
