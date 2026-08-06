// Copyright (c) 2026 Ekorau LLC

// internal/space/store.go
package space

import (
	"database/sql"
	"fmt"
	"strings"

	"github.com/davidg238/porta/internal/cval"
	_ "github.com/mattn/go-sqlite3"
)

// The caps are a radio constraint, not a preference: 6LoWPAN fragments above
// ~102 B and a datagram survives only if every fragment does, so a 1 KB batch
// reaches the weakest bench node ~54% of the time (RECORDS §2.1/§2.2).
const (
	MaxValueBytes = 256
	MaxBatchBytes = 512
)

type Entry struct {
	Seq   int64
	Path  string
	Kind  cval.Kind
	Value []byte
}

type Event struct {
	Entry
	TsMs      int64
	Direction string
	Actor     string
	Reason    string
}

type Store struct {
	db    *sql.DB
	nowMs func() int64
}

func Open(path string, nowMs func() int64) (*Store, error) {
	db, err := sql.Open("sqlite3", path+"?_busy_timeout=5000")
	if err != nil {
		return nil, err
	}
	if _, err := db.Exec(schema); err != nil {
		db.Close()
		return nil, err
	}
	return &Store{db: db, nowMs: nowMs}, nil
}

func (s *Store) Close() error { return s.db.Close() }

func (s *Store) CountNodes() (int, error) {
	var n int
	err := s.db.QueryRow(`SELECT COUNT(*) FROM nodes`).Scan(&n)
	return n, err
}

// ensureNode inserts the node row if absent. Called on every path that can
// mention a node id, so a write to an unseen node is not an error — porta
// may stage goal state before a node has ever checked in.
func ensureNode(tx *sql.Tx, nodeID string, nowMs int64) error {
	_, err := tx.Exec(
		`INSERT INTO nodes (id, first_seen_ms) VALUES (?, ?) ON CONFLICT(id) DO NOTHING`,
		nodeID, nowMs)
	return err
}

// upsertCell writes the latest value for a path. direction records which side
// produced it, which is what keeps porta's out_seq and the node's push seq
// from being compared to each other (see schema.go).
func upsertCell(tx *sql.Tx, nodeID, path string, seq int64, kind cval.Kind, value []byte, nowMs int64, direction string) error {
	_, err := tx.Exec(`
		INSERT INTO cells (node_id, path, seq, kind, value_cbor, ts_ms, direction)
		VALUES (?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT(node_id, path) DO UPDATE SET
		  seq = excluded.seq, kind = excluded.kind,
		  value_cbor = excluded.value_cbor, ts_ms = excluded.ts_ms,
		  direction = excluded.direction`,
		nodeID, path, seq, int(kind), value, nowMs, direction)
	return err
}

func appendEvent(tx *sql.Tx, nodeID, path string, seq int64, kind cval.Kind, value []byte, nowMs int64, direction, actor, reason string) error {
	_, err := tx.Exec(`
		INSERT INTO events (node_id, path, seq, kind, value_cbor, ts_ms, direction, actor, reason)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		nodeID, path, seq, int(kind), value, nowMs, direction, actor, reason)
	return err
}

// Write records an outbound (porta→node) write, allocating the node's next
// outbound seq. A cell write also updates the cell tree; a stream write only
// appends. Returns the assigned seq.
func (s *Store) Write(nodeID, path string, kind cval.Kind, value []byte, actor, reason string) (int64, error) {
	if len(value) > MaxValueBytes {
		return 0, fmt.Errorf("value is %d bytes, over the %d byte cap", len(value), MaxValueBytes)
	}
	if err := cval.Validate(value); err != nil {
		return 0, fmt.Errorf("value is not a well-formed algebra value: %w", err)
	}
	if kind != cval.KindCell && kind != cval.KindStream {
		return 0, fmt.Errorf("kind %d is neither cell nor stream", kind)
	}
	now := s.nowMs()
	tx, err := s.db.Begin()
	if err != nil {
		return 0, err
	}
	defer tx.Rollback()
	if err := ensureNode(tx, nodeID, now); err != nil {
		return 0, err
	}
	var seq int64
	if err := tx.QueryRow(
		`UPDATE nodes SET out_seq = out_seq + 1 WHERE id = ? RETURNING out_seq`,
		nodeID).Scan(&seq); err != nil {
		return 0, err
	}
	if kind == cval.KindCell {
		if err := upsertCell(tx, nodeID, path, seq, kind, value, now, "out"); err != nil {
			return 0, err
		}
	}
	if err := appendEvent(tx, nodeID, path, seq, kind, value, now, "out", actor, reason); err != nil {
		return 0, err
	}
	return seq, tx.Commit()
}

// OutboundSince returns entries the node has not yet consumed, oldest first,
// stopping before the batch would exceed maxBytes.
//
// Cells are read from the cell tree, so a SUPERSEDED cell write never rides
// the radio — the node needs the current value, not the history that produced
// it. This is also what bounds a cursor-0 replay by tree size rather than by
// history length. Streams come from the log and are delivered once each.
//
// BOTH halves filter direction='out'. The cell tree holds the node's own
// obs/** rows too, and their seqs come from the node's push counter, so an
// unfiltered read would hand the node its own state back stamped with numbers
// from the wrong sequence space — corrupting the cursor in the same reply.
func (s *Store) OutboundSince(nodeID string, cursor int64, maxBytes int) ([]Entry, int64, error) {
	rows, err := s.db.Query(`
		SELECT seq, path, kind, value_cbor FROM cells
		  WHERE node_id = ? AND direction = 'out' AND seq > ?
		UNION ALL
		SELECT seq, path, kind, value_cbor FROM events
		  WHERE node_id = ? AND direction = 'out' AND kind = ? AND seq > ?
		ORDER BY seq`,
		nodeID, cursor, nodeID, int(cval.KindStream), cursor)
	if err != nil {
		return nil, cursor, err
	}
	defer rows.Close()

	var out []Entry
	hi := cursor
	size := 0
	for rows.Next() {
		var e Entry
		var k int
		if err := rows.Scan(&e.Seq, &e.Path, &k, &e.Value); err != nil {
			return nil, cursor, err
		}
		e.Kind = cval.Kind(k)
		// 8 bytes of slack per entry for the seq/kind heads and the array head.
		cost := len(e.Value) + len(e.Path) + 8
		if size+cost > maxBytes && len(out) > 0 {
			break
		}
		out = append(out, e)
		hi = e.Seq
		size += cost
	}
	return out, hi, rows.Err()
}

// Ingest applies a node push. Entries at or below the stored high-water are
// skipped as duplicates — which is why the exchange needs no retransmit
// layer: a lost push is simply resent, and a resent push is idempotent. The
// skip count is returned rather than silently absorbed, so a caller can make
// the dedupe observable (e.g. a log line) instead of it only being provable
// by the absence of a duplicate — which is equally consistent with the node
// never having re-pushed at all.
func (s *Store) Ingest(nodeID string, bootID int64, entries []Entry) (hw int64, skipped int, err error) {
	now := s.nowMs()
	tx, err := s.db.Begin()
	if err != nil {
		return 0, 0, err
	}
	defer tx.Rollback()
	if err := ensureNode(tx, nodeID, now); err != nil {
		return 0, 0, err
	}
	if err := tx.QueryRow(`SELECT node_seq_hw FROM nodes WHERE id = ?`, nodeID).Scan(&hw); err != nil {
		return 0, 0, err
	}
	for _, e := range entries {
		if e.Seq <= hw {
			skipped++
			continue // duplicate; the node is re-sending an unacked batch
		}
		if len(e.Value) > MaxValueBytes {
			return 0, 0, fmt.Errorf("node %s sent a %d byte value at %s, over the %d byte cap",
				nodeID, len(e.Value), e.Path, MaxValueBytes)
		}
		if err := cval.Validate(e.Value); err != nil {
			return 0, 0, fmt.Errorf("node %s sent a malformed value at %s: %w", nodeID, e.Path, err)
		}
		if e.Kind == cval.KindCell {
			if err := upsertCell(tx, nodeID, e.Path, e.Seq, e.Kind, e.Value, now, "in"); err != nil {
				return 0, 0, err
			}
		}
		if err := appendEvent(tx, nodeID, e.Path, e.Seq, e.Kind, e.Value, now, "in", nodeID, ""); err != nil {
			return 0, 0, err
		}
		hw = e.Seq
	}
	if _, err := tx.Exec(
		`UPDATE nodes SET node_seq_hw = ?, boot_id = ?, last_seen_ms = ? WHERE id = ?`,
		hw, bootID, now, nodeID); err != nil {
		return 0, 0, err
	}
	return hw, skipped, tx.Commit()
}

// NoteCheckIn records what a pull declared: the cursor the node has applied, and
// `hw` — what it believes porta has accepted FROM it. Both fields were already on
// the wire; `hw` was decoded and thrown away until 2026-08-05.
//
// It answers whether the node RESTARTED, and rewinds `node_seq_hw` when it did.
//
// THE DEFECT THIS CLOSES. `node_seq_hw` is a CACHE of the node's own `acked`, and
// the node is the only party that can be authoritative for its own outbound
// sequence: the space counter restarts at 0 on every boot and this row does not.
// A rebooted node pushes seq 1..n against a stored high-water of (say) 7138,
// Ingest discards every row as already-applied — and the ack carries 7138, which
// `applyAck:` adopts. From then on the node believes everything it writes has
// been accepted, porta holds observations hours old, and NEITHER END CAN TELL.
// It stayed invisible because the C6 gate wiped porta's db before every run.
// Found by the bridge-health target gate, which scored 3/3 over it.
//
// WHY THE CURSOR AND NOT `hw` ALONE. A missed ack ALSO leaves the node declaring
// a lower `hw` — and that case must NOT rewind, because porta committed those
// rows and the node is legitimately re-offering them: rewinding deletes the
// `seq <= hw` dedupe, which is the one thing making a re-push idempotent. The
// first version of this did exactly that, and the bench integration test caught
// it inside one leg ("the gateway never logged a duplicate-skip", where it had
// logged 959 before).
//
// The cursor separates them, and not as a heuristic: the cursor is advanced by
// the BATCH and the watermark by the ACK, so a swallowed ack cannot move the
// cursor. A cursor that went BACKWARDS can only mean the node lost the RAM
// holding it — which is the same event that reset its seq counter.
//
// KNOWN GAP, named rather than papered over: a node porta has never written to
// has a cursor of 0 in both places, so its reboot carries no signal here and the
// loss above persists for it. Closing that needs a per-boot identity — the
// `bootId` field is already on the wire and is a source constant (#74). This
// mechanism does not conflict with that one; it makes it unnecessary for every
// node that has ever been given a goal.
func (s *Store) NoteCheckIn(nodeID string, cursor, nodeHW int64) (restartedFrom int64, restarted bool, err error) {
	now := s.nowMs()
	tx, err := s.db.Begin()
	if err != nil {
		return 0, false, err
	}
	defer tx.Rollback()
	if err := ensureNode(tx, nodeID, now); err != nil {
		return 0, false, err
	}
	var storedCursor, storedHW int64
	if err := tx.QueryRow(`SELECT cursor_acked, node_seq_hw FROM nodes WHERE id = ?`, nodeID).
		Scan(&storedCursor, &storedHW); err != nil {
		return 0, false, err
	}
	restarted = cursor < storedCursor && nodeHW < storedHW
	if restarted {
		restartedFrom = storedHW
		if _, err := tx.Exec(`UPDATE nodes SET node_seq_hw = ? WHERE id = ?`, nodeHW, nodeID); err != nil {
			return 0, false, err
		}
	}
	if _, err := tx.Exec(
		`UPDATE nodes SET cursor_acked = ?, last_seen_ms = ? WHERE id = ?`,
		cursor, now, nodeID); err != nil {
		return 0, false, err
	}
	return restartedFrom, restarted, tx.Commit()
}

// RecordExchange logs one check-in. addr is the peer address the datagram
// arrived from; an EMPTY addr never clobbers a known one, which is v1's own
// COALESCE discipline (v1 store.go:267) kept deliberately — a caller that does
// not know the peer (the pure Handle path, a replayed session) must not erase
// what the UDP loop learned.
func (s *Store) RecordExchange(nodeID string, bootID, cursorIn, cursorOut, pushedLo, pushedHi int64, addr string) error {
	now := s.nowMs()
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if err := ensureNode(tx, nodeID, now); err != nil {
		return err
	}
	if _, err := tx.Exec(`
		INSERT INTO exchanges (node_id, ts_ms, boot_id, cursor_in, cursor_out, pushed_lo, pushed_hi)
		VALUES (?, ?, ?, ?, ?, ?, ?)`,
		nodeID, now, bootID, cursorIn, cursorOut, pushedLo, pushedHi); err != nil {
		return err
	}
	if _, err := tx.Exec(
		`UPDATE nodes SET last_seen_ms = ?, last_addr = COALESCE(NULLIF(?, ''), last_addr) WHERE id = ?`,
		now, addr, nodeID); err != nil {
		return err
	}
	return tx.Commit()
}

// NodeAddr returns the peer address of the node's last datagram, or "" if it
// has never been seen.
func (s *Store) NodeAddr(nodeID string) (string, error) {
	var addr sql.NullString
	err := s.db.QueryRow(`SELECT last_addr FROM nodes WHERE id = ?`, nodeID).Scan(&addr)
	if err == sql.ErrNoRows {
		return "", nil
	}
	return addr.String, err
}

// Tree returns the current cell values under prefix, path-ordered — the
// operator's view. goal/X vs obs/X is a diff over two of these, which is why
// self-heal disappears as a separate mechanism. Both directions are included
// deliberately: the diff is the whole point.
func (s *Store) Tree(nodeID, prefix string) ([]Entry, error) {
	rows, err := s.db.Query(`
		SELECT seq, path, kind, value_cbor FROM cells
		WHERE node_id = ? AND path LIKE ? ESCAPE '\' ORDER BY path`,
		nodeID, escapeLike(prefix)+"%")
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Entry
	for rows.Next() {
		var e Entry
		var k int
		if err := rows.Scan(&e.Seq, &e.Path, &k, &e.Value); err != nil {
			return nil, err
		}
		e.Kind = cval.Kind(k)
		out = append(out, e)
	}
	return out, rows.Err()
}

// Events returns the most recent log rows, newest first — porta log.
func (s *Store) Events(nodeID string, limit int) ([]Event, error) {
	rows, err := s.db.Query(`
		SELECT seq, path, kind, value_cbor, ts_ms, direction, actor, reason
		FROM events WHERE node_id = ? ORDER BY id DESC LIMIT ?`,
		nodeID, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Event
	for rows.Next() {
		var e Event
		var k int
		if err := rows.Scan(&e.Seq, &e.Path, &k, &e.Value, &e.TsMs, &e.Direction, &e.Actor, &e.Reason); err != nil {
			return nil, err
		}
		e.Kind = cval.Kind(k)
		out = append(out, e)
	}
	return out, rows.Err()
}

func escapeLike(s string) string {
	r := strings.NewReplacer(`\`, `\\`, `%`, `\%`, `_`, `\_`)
	return r.Replace(s)
}

// DB exposes the handle for tests and for the operator surface's ad-hoc
// queries. Not part of the protocol path.
func (s *Store) DB() *sql.DB { return s.db }
