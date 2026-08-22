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
	BootID    int64
}

// Cell is a row of the operator's tree read. Entry is deliberately NOT extended
// to carry BootID: Entry is what rides the radio, and a display field on the
// wire type that the encoder must remember to ignore is a field added beside
// what was there rather than instead of it. Event already follows this shape —
// Entry plus what only the store knows — so Cell is the symmetric one.
type Cell struct {
	Entry
	BootID    int64
	Direction string
	TsMs      int64
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
	for _, stmt := range addColumns {
		// Only "duplicate column name" is expected here — the column is already
		// present, which is the steady state. Anything else is a real fault and
		// must not be swallowed, or a store that failed to gain the column would
		// serve unanchored reads while looking healthy.
		if _, err := db.Exec(stmt); err != nil && !strings.Contains(err.Error(), "duplicate column name") {
			db.Close()
			return nil, fmt.Errorf("%s: %w", stmt, err)
		}
	}
	for _, stmt := range dropColumns {
		// Only "no such column" is expected — on a fresh store the column was
		// never created, which is the steady state. Anything else is a real
		// fault and is NOT swallowed: a live store whose DROP failed would keep
		// a column nothing reads while every fresh store lacks it, and the two
		// would then disagree about their own shape with nothing to say so.
		if _, err := db.Exec(stmt); err != nil && !strings.Contains(err.Error(), "no such column") {
			db.Close()
			return nil, fmt.Errorf("%s: %w", stmt, err)
		}
	}
	// AFTER addColumns, never before: a store predating boot_id gains the column
	// first, so the rebuild below copies it like any other.
	if err := migrateDirectionAcceptsGw(db); err != nil {
		db.Close()
		return nil, err
	}
	// migrateDirectionAcceptsGw silently no-ops on any DDL shape it does not
	// recognise (see its comment) — a store that fell through unrebuilt would
	// otherwise look identical to a healthy one until a gw/ write hit the
	// CHECK constraint. Assert what it promised rather than trust it silently.
	if err := assertDirectionAcceptsGw(db); err != nil {
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
func upsertCell(tx *sql.Tx, nodeID, path string, seq int64, kind cval.Kind, value []byte, nowMs int64, direction string, bootID int64) error {
	_, err := tx.Exec(`
		INSERT INTO cells (node_id, path, seq, kind, value_cbor, ts_ms, direction, boot_id)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT(node_id, path) DO UPDATE SET
		  seq = excluded.seq, kind = excluded.kind,
		  value_cbor = excluded.value_cbor, ts_ms = excluded.ts_ms,
		  direction = excluded.direction, boot_id = excluded.boot_id`,
		nodeID, path, seq, int(kind), value, nowMs, direction, bootID)
	return err
}

func appendEvent(tx *sql.Tx, nodeID, path string, seq int64, kind cval.Kind, value []byte, nowMs int64, direction, actor, reason string, bootID int64) error {
	_, err := tx.Exec(`
		INSERT INTO events (node_id, path, seq, kind, value_cbor, ts_ms, direction, actor, reason, boot_id)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		nodeID, path, seq, int(kind), value, nowMs, direction, actor, reason, bootID)
	return err
}

// Write records an outbound (porta→node) write, allocating the node's next
// outbound seq. A cell write also updates the cell tree; a stream write only
// appends. Returns the assigned seq.
func (s *Store) Write(nodeID, path string, kind cval.Kind, value []byte, actor, reason string) (int64, error) {
	// gw/** is gateway-authored (WriteGateway) and porta may only write
	// goal/** and dbg/req TO a node. Without this, `porta put <node>
	// gw/liveness live` reaches this method, gets stamped direction='out',
	// and OutboundSince — which filters on direction, not on path — hands it
	// straight to the node: the one thing the third `direction` value exists
	// to make structurally impossible (rete#217 I3).
	if strings.HasPrefix(path, "gw/") {
		return 0, fmt.Errorf("path %q is under gw/, which is gateway-authored and may not be written to a node", path)
	}
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
	// boot_id 0 on an outbound row: porta's own intent has no node boot, and
	// `direction` is what tells that apart from a node with no persistent
	// identity. Neither is ever reported as stale.
	if kind == cval.KindCell {
		if err := upsertCell(tx, nodeID, path, seq, kind, value, now, "out", 0); err != nil {
			return 0, err
		}
	}
	if err := appendEvent(tx, nodeID, path, seq, kind, value, now, "out", actor, reason, 0); err != nil {
		return 0, err
	}
	return seq, tx.Commit()
}

// PathLiveness is the one gw/** path today: porta's verdict on whether a node
// is still talking. #live or #silent, as a Symbol.
const PathLiveness = "gw/liveness"

// PathDeclared is where a node states how long its silence may last (rete#231
// D2). It is an obs/** path because it is an observation the node makes about
// ITSELF — battery, duty cycle and radio conditions are its own — and porta
// writing there would be a lie (2026-08-10 D7). The operator's half is
// goal/liveness, which is a REQUEST; the node always wins.
const PathDeclared = "obs/liveness"

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
	var storedBoot int64
	if err := tx.QueryRow(`SELECT node_seq_hw, COALESCE(boot_id, 0) FROM nodes WHERE id = ?`, nodeID).
		Scan(&hw, &storedBoot); err != nil {
		return 0, 0, err
	}
	// THE SAME BOOT TEST AS NoteCheckIn, AND IT MUST BE HERE TOO. A check-in is
	// a pull then a push, so the pull normally detects the reboot first and this
	// is a no-op. But a pull datagram is not guaranteed to arrive: lose one, and
	// the push is porta's FIRST contact with the new boot. Without this, Ingest
	// would adopt the new boot_id below while `hw` still held the old boot's
	// watermark — every row discarded as a duplicate, the ack carrying the stale
	// watermark back — and the next pull would then compare two EQUAL boot ids
	// and never rewind. That is #162 reproduced exactly, through a hole opened by
	// the fix for it.
	//
	// The rewind target is 0, not a declared watermark: a push carries no `hw`
	// field, and a boot whose reboot we are learning about here has by definition
	// had nothing accepted from it yet.
	//
	// THE TEST IS `!=` AND NOT `>`, and the asymmetry is deliberate. `>` would be
	// immune to a stale datagram from the PREVIOUS boot arriving after the new
	// boot's first contact (UDP reorders, and a reboot takes seconds, so this is
	// rare but not impossible) — that misfire costs duplicate events and a
	// temporarily low watermark, both of which the next push corrects. But `>`
	// wedges permanently after an NVS erase, which is an ordinary development act
	// (`west flash --erase`): the counter restarts at 1, never exceeds the stored
	// high-water again for as many boots as the node had before, and restart
	// detection is silently off for all of them. Duplicates are recoverable and a
	// wedge is not, so the failure is steered toward the recoverable one.
	if bootID != storedBoot && bootID != 0 && storedBoot != 0 {
		hw = 0
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
		// bootID is stamped on the row, not merely on the node: it is the only
		// thing that can later say WHICH boot published this value, and a
		// rebooted node's tree arrives over seconds, so a read taken mid-rewrite
		// otherwise blends two boots and cannot be told from a coherent one
		// (rete#210). The value costs nothing to carry — it is already in hand.
		if e.Kind == cval.KindCell {
			if err := upsertCell(tx, nodeID, e.Path, e.Seq, e.Kind, e.Value, now, "in", bootID); err != nil {
				return 0, 0, err
			}
		}
		if err := appendEvent(tx, nodeID, e.Path, e.Seq, e.Kind, e.Value, now, "in", nodeID, "", bootID); err != nil {
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
// WHY NOT `hw` ALONE. A missed ack ALSO leaves the node declaring a lower `hw` —
// and that case must NOT rewind, because porta committed those rows and the node
// is legitimately re-offering them: rewinding deletes the `seq <= hw` dedupe,
// which is the one thing making a re-push idempotent. The first version of this
// did exactly that, and the bench integration test caught it inside one leg
// ("the gateway never logged a duplicate-skip", where it had logged 959 before).
//
// `bootId` SEPARATES THEM, and it is evidence rather than inference: it is a
// counter the node increments once per boot and persists in NVS (RECORDS §3),
// so a CHANGE in it means the node rebooted and nothing else can produce one. A
// swallowed ack leaves it untouched, which is precisely the negative above.
//
// THIS REPLACED A CURSOR HEURISTIC on 2026-08-09, and the replacement is the
// point rather than an addition beside it. That test — `cursor < storedCursor &&
// nodeHW < storedHW` — inferred a restart from the cursor going backwards, which
// is sound but INCOMPLETE: a node porta has never written to has a cursor of 0 in
// both places, so `0 < 0` is false and its reboot carried no signal at all. That
// gap was named in this comment for four days before c6-bell fell into it and
// lost a night of telemetry with both ends reporting success (#162). A node's own
// boot counter needs no goal to have been written, so the heuristic has no
// remaining case and is gone.
//
// bootID 0 means the node has NO persistent boot identity — no settings/NVS
// backend, or a write that failed (see platform_backend.h). Restart detection is
// genuinely unavailable for such a node, and the honest response is to leave the
// watermark alone and let the caller say so out loud, NOT to guess from a value
// that is the same on every boot.
func (s *Store) NoteCheckIn(nodeID string, cursor, nodeHW, bootID int64) (restartedFrom int64, restarted bool, err error) {
	now := s.nowMs()
	tx, err := s.db.Begin()
	if err != nil {
		return 0, false, err
	}
	defer tx.Rollback()
	if err := ensureNode(tx, nodeID, now); err != nil {
		return 0, false, err
	}
	var storedHW, storedBoot int64
	if err := tx.QueryRow(`SELECT node_seq_hw, COALESCE(boot_id, 0) FROM nodes WHERE id = ?`, nodeID).
		Scan(&storedHW, &storedBoot); err != nil {
		return 0, false, err
	}
	// storedBoot == 0 is a node porta has never recorded a boot for, so there is
	// no PREVIOUS boot to have differed from — the first check-in of a node's
	// life must not be reported as a restart. `storedHW > nodeHW` is the guard
	// that keeps this to cases with something to actually correct.
	restarted = bootID != 0 && storedBoot != 0 && bootID != storedBoot && storedHW > nodeHW
	if restarted {
		restartedFrom = storedHW
		if _, err := tx.Exec(`UPDATE nodes SET node_seq_hw = ? WHERE id = ?`, nodeHW, nodeID); err != nil {
			return 0, false, err
		}
	}
	// boot_id is recorded on the PULL and not only on the push, so the detection
	// above has a previous value to compare against even for a node that never
	// pushes anything.
	if _, err := tx.Exec(
		`UPDATE nodes SET cursor_acked = ?, boot_id = ?, last_seen_ms = ? WHERE id = ?`,
		cursor, bootID, now, nodeID); err != nil {
		return 0, false, err
	}
	return restartedFrom, restarted, tx.Commit()
}

// RecordExchange logs one REQUEST — not one check-in; a check-in is a pull
// followed by a push, and the two call sites in internal/wire/server.go each
// call this once, so every check-in leaves two rows. Which leg a row came from
// was recorded until rete#231 D4 and is not any more: its ONLY reader was the
// cadence inference, which needed the pull alone to avoid inferring a period
// half the node's real one (#217 C1). Nothing infers a period now — the node
// declares its budget — and what remains of this table is the replay index and
// the last_seen heartbeat, both of which want every request, not one leg.
// addr is the peer address the datagram arrived from; an EMPTY addr never
// clobbers a known one, which is v1's own COALESCE discipline (v1
// store.go:267) kept deliberately — a caller that does not know the peer
// (the pure Handle path, a replayed session) must not erase what the UDP
// loop learned.
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

// DeclaredBudget answers the silence budget a node has declared for itself, in
// milliseconds.
//
// This is the one place in the store that DECODES a value rather than moving
// bytes (D4.1 says decode only for display; this decodes for judgement, which
// is the narrow exception this path buys). Everything downstream — the verdict
// comparison in Sweep — is still byte-wise against pre-encoded symbols.
//
// ok is false for a node that has never declared, AND for a declaration porta
// cannot read: a wrong tag, a missing or non-positive `bud`, or bytes that are
// not a closed cval encoding at all. None of those is returned as an error,
// because the caller is a sweep over every node and one garbled record must not
// cost the fleet its verdicts. No verdict is the honest answer for that node;
// a default would be porta inventing the number this whole design exists to
// stop inventing.
func (s *Store) DeclaredBudget(nodeID string) (int64, bool, error) {
	var raw []byte
	err := s.db.QueryRow(
		`SELECT value_cbor FROM cells WHERE node_id = ? AND path = ?`,
		nodeID, PathDeclared).Scan(&raw)
	if err == sql.ErrNoRows {
		return 0, false, nil
	}
	if err != nil {
		return 0, false, err
	}
	v, err := cval.Decode(raw)
	if err != nil {
		return 0, false, nil
	}
	rec, ok := v.(cval.Record)
	if !ok || rec.Tag != "Liveness" {
		return 0, false, nil
	}
	for _, p := range rec.Fields {
		if k, ok := p.K.(cval.Symbol); !ok || k != "bud" {
			continue
		}
		bud, ok := p.V.(int64)
		if !ok || bud <= 0 {
			return 0, false, nil
		}
		return bud, true, nil
	}
	return 0, false, nil
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
// Each row carries the boot that published it, so a caller can tell a coherent
// read from one taken while a rebooted node was still rewriting its tree. See
// CurrentBoot for the other half of that comparison.
func (s *Store) Tree(nodeID, prefix string) ([]Cell, error) {
	rows, err := s.db.Query(`
		SELECT seq, path, kind, value_cbor, boot_id, direction, ts_ms FROM cells
		WHERE node_id = ? AND path LIKE ? ESCAPE '\' ORDER BY path`,
		nodeID, escapeLike(prefix)+"%")
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Cell
	for rows.Next() {
		var c Cell
		var k int
		if err := rows.Scan(&c.Seq, &c.Path, &k, &c.Value, &c.BootID, &c.Direction, &c.TsMs); err != nil {
			return nil, err
		}
		c.Kind = cval.Kind(k)
		out = append(out, c)
	}
	return out, rows.Err()
}

// CurrentBoot is the boot the node is on now — the `bid` of its most recent
// envelope. 0 means either that porta has never heard from it or that the node
// has no persistent identity; both are answered as "unknown" rather than being
// distinguished here, because a caller can do nothing different with them.
func (s *Store) CurrentBoot(nodeID string) (int64, error) {
	var boot sql.NullInt64
	err := s.db.QueryRow(`SELECT boot_id FROM nodes WHERE id = ?`, nodeID).Scan(&boot)
	if err == sql.ErrNoRows {
		return 0, nil
	}
	return boot.Int64, err
}

// Events returns the most recent log rows, newest first — porta log.
//
// boot > 0 restricts the read to one boot. That question is answerable HERE and
// not on the cell tree: the log is append-only, so "what did the node publish
// during boot N" is a filter, whereas cells holds only the latest value per path
// and could answer at most "paths whose last write happened to fall in boot N".
func (s *Store) Events(nodeID string, limit int, boot int64) ([]Event, error) {
	rows, err := s.db.Query(`
		SELECT seq, path, kind, value_cbor, ts_ms, direction, actor, reason, boot_id
		FROM events WHERE node_id = ? AND (? = 0 OR boot_id = ?)
		ORDER BY id DESC LIMIT ?`,
		nodeID, boot, boot, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Event
	for rows.Next() {
		var e Event
		var k int
		if err := rows.Scan(&e.Seq, &e.Path, &k, &e.Value, &e.TsMs, &e.Direction, &e.Actor, &e.Reason, &e.BootID); err != nil {
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
