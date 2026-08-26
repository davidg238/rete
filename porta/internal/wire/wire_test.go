// Copyright (c) 2026 Ekorau LLC

// internal/wire/wire_test.go
package wire

import (
	"fmt"
	"strings"
	"testing"

	"github.com/davidg238/porta/internal/cval"
	"github.com/davidg238/porta/internal/space"
)

func testServer(t *testing.T) (*Server, *space.Store) {
	t.Helper()
	var clock int64 = 1_000
	st, err := space.Open(t.TempDir()+"/t.db", func() int64 { clock += 10; return clock })
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	t.Cleanup(func() { st.Close() })
	return NewServer(st, func(string, ...any) {}), st
}

func mustEnc(t *testing.T, v any) []byte {
	t.Helper()
	b, err := cval.Encode(v)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func TestPullDeliversGoalWrite(t *testing.T) {
	srv, st := testServer(t)
	st.Write("n1", "goal/name", cval.KindCell, mustEnc(t, "alpha"), "david", "")

	req, _ := EncodePull("n1", 0, 7, 0)
	hi, entries, err := DecodeBatch(srv.Handle(req))
	if err != nil {
		t.Fatalf("DecodeBatch: %v", err)
	}
	if hi != 1 || len(entries) != 1 || entries[0].Path != "goal/name" {
		t.Fatalf("hi=%d entries=%+v", hi, entries)
	}
	v, _ := cval.Decode(entries[0].Value)
	if v != "alpha" {
		t.Fatalf("value = %v, want alpha", v)
	}
}

// A pull at the high-water is the idle heartbeat: empty, and not an error.
func TestPullAtHighWaterIsEmpty(t *testing.T) {
	srv, st := testServer(t)
	st.Write("n1", "goal/name", cval.KindCell, mustEnc(t, "alpha"), "david", "")
	req, _ := EncodePull("n1", 1, 7, 0)
	hi, entries, err := DecodeBatch(srv.Handle(req))
	if err != nil {
		t.Fatalf("DecodeBatch: %v", err)
	}
	if len(entries) != 0 || hi != 1 {
		t.Fatalf("hi=%d entries=%d, want 1 and 0", hi, len(entries))
	}
}

func TestPushIngestsAndAcks(t *testing.T) {
	srv, st := testServer(t)
	req, _ := EncodePush("n1", 7, []space.Entry{
		{Seq: 1, Path: "obs/health", Kind: cval.KindCell, Value: mustEnc(t, int64(42))},
		{Seq: 2, Path: "tel/log", Kind: cval.KindStream, Value: mustEnc(t, "hello")},
	})
	hw, err := DecodeAck(srv.Handle(req))
	if err != nil {
		t.Fatalf("DecodeAck: %v", err)
	}
	if hw != 2 {
		t.Fatalf("hw = %d, want 2", hw)
	}
	tree, _ := st.Tree("n1", "obs/")
	if len(tree) != 1 || tree[0].Path != "obs/health" {
		t.Fatalf("obs tree = %+v", tree)
	}
}

// A REBOOTED NODE IS NOT A DUPLICATE. The whole defect in one test: the node's
// space counter restarts at 0 on boot and porta's node_seq_hw does not, so
// without the pull-declared high-water every cell the rebooted node writes is
// discarded as already-applied — and the ack hands the node the stale watermark,
// so neither end can tell. Found on hardware by the bridge-health target gate
// on 2026-08-05, having been hidden for as long as the C6 gate wiped the db.
func TestRebootedNodeIsNotMistakenForADuplicate(t *testing.T) {
	srv, st := testServer(t)

	// A long first session: the node applies a goal write (cursor 1) and its own
	// counter reaches seq 40.
	st.Write("n1", "goal/name", cval.KindCell, mustEnc(t, "alpha"), "david", "")
	pull, _ := EncodePull("n1", 1, 1, 0)
	srv.Handle(pull)
	req, _ := EncodePush("n1", 1, []space.Entry{
		{Seq: 40, Path: "obs/health", Kind: cval.KindCell, Value: mustEnc(t, int64(1))},
	})
	if hw, _ := DecodeAck(srv.Handle(req)); hw != 40 {
		t.Fatalf("first session hw = %d, want 40", hw)
	}

	// It reboots, and says so: the persisted boot counter steps 1 -> 2. That is
	// what says "restart" rather than "the last ack was lost" — the cursor and
	// watermark also go backwards, but so do they on a missed ack, which is why
	// the boot counter and not that pair is the evidence (see Store.NoteCheckIn).
	pull, _ = EncodePull("n1", 0, 2, 0)
	if _, _, err := DecodeBatch(srv.Handle(pull)); err != nil {
		t.Fatalf("pull after reboot: %v", err)
	}
	req, _ = EncodePush("n1", 2, []space.Entry{
		{Seq: 1, Path: "obs/health", Kind: cval.KindCell, Value: mustEnc(t, int64(2))},
	})
	hw, err := DecodeAck(srv.Handle(req))
	if err != nil {
		t.Fatalf("DecodeAck: %v", err)
	}
	// The ack must describe THIS boot. An ack of 40 is the silent failure: the
	// node adopts it and never offers anything again.
	if hw != 1 {
		t.Fatalf("hw after reboot = %d, want 1 — the node's push was discarded as already-applied", hw)
	}
	tree, _ := st.Tree("n1", "obs/")
	if len(tree) != 1 {
		t.Fatalf("obs tree = %+v", tree)
	}
	v, _ := cval.Decode(tree[0].Value)
	if v != int64(2) {
		t.Fatalf("obs/health = %v, want 2 — porta is still holding the previous boot's value", v)
	}
}

// After the rewind the two boots RE-USE seq values, which is the case the store
// has to tolerate for the fix to be worth anything: the second boot's rows carry
// seqs the first boot already spent. `cells` is keyed by (node, path) so the
// newest write wins, and `events` is append-only with its own id — but that is
// worth a test rather than a reading of the schema.
func TestRebootReusesSeqValues(t *testing.T) {
	srv, st := testServer(t)
	st.Write("n1", "goal/name", cval.KindCell, mustEnc(t, "alpha"), "david", "")
	pull, _ := EncodePull("n1", 1, 1, 0) // first boot, goal applied
	srv.Handle(pull)
	first, _ := EncodePush("n1", 1, []space.Entry{
		{Seq: 1, Path: "tel/log", Kind: cval.KindStream, Value: mustEnc(t, "boot1-a")},
		{Seq: 2, Path: "obs/health", Kind: cval.KindCell, Value: mustEnc(t, int64(11))},
	})
	srv.Handle(first)

	pull, _ = EncodePull("n1", 0, 2, 0) // rebooted: boot counter 1 -> 2
	srv.Handle(pull)

	second, _ := EncodePush("n1", 2, []space.Entry{
		{Seq: 1, Path: "tel/log", Kind: cval.KindStream, Value: mustEnc(t, "boot2-a")},
		{Seq: 2, Path: "obs/health", Kind: cval.KindCell, Value: mustEnc(t, int64(22))},
	})
	if hw, err := DecodeAck(srv.Handle(second)); err != nil || hw != 2 {
		t.Fatalf("second boot ack = %d, %v; want 2, nil", hw, err)
	}
	evs, _ := st.Events("n1", 100, 0)
	ins := 0
	for _, e := range evs {
		if e.Direction == "in" {
			ins++
		}
	}
	if ins != 4 {
		t.Fatalf("got %d inbound events, want 4 — a re-used seq was dropped", ins)
	}
	tree, _ := st.Tree("n1", "obs/")
	v, _ := cval.Decode(tree[0].Value)
	if v != int64(22) {
		t.Fatalf("obs/health = %v, want 22 — the second boot's value did not win", v)
	}
}

// A MISSED ACK IS NOT A REBOOT, and telling them apart is the whole difficulty:
// both make the node declare a high-water below the one porta holds. This is the
// case that must NOT rewind — porta committed those rows and swallowed the ack,
// so the node re-pushes rows that are genuinely already applied, and the
// `seq <= hw` dedupe is the only thing standing between that and a duplicate.
//
// Written after the first version of SyncHighWater rewound on `hw` alone and
// deleted the dedupe: the bench integration test caught it within one leg —
// "the gateway never logged a duplicate-skip" where it had logged 959 before.
//
// The signal that separates them is `bootId`, the node's persisted per-boot
// counter (RECORDS §3): a swallowed ack does not reboot the node, so the
// counter does not move. Note that this test declares the SAME bootId either
// side of the lost ack — that identity is the whole assertion, and it is why
// this case stays correct under a mechanism built to catch the opposite one.
func TestMissedAckIsNotMistakenForAReboot(t *testing.T) {
	srv, st := testServer(t)
	st.Write("n1", "goal/name", cval.KindCell, mustEnc(t, "alpha"), "david", "")

	// The node pulls, applies goal/name, and pushes. porta commits at hw 5.
	pull, _ := EncodePull("n1", 0, 1, 0)
	srv.Handle(pull)
	push, _ := EncodePush("n1", 1, []space.Entry{
		{Seq: 5, Path: "tel/log", Kind: cval.KindStream, Value: mustEnc(t, "one")},
	})
	srv.Handle(push)

	// The ack is lost. The node's watermark stays at 4 — but its cursor does
	// not move backwards, because it applied the batch.
	pull, _ = EncodePull("n1", 1, 1, 4)
	srv.Handle(pull)
	srv.Handle(push) // the same rows, re-offered

	evs, _ := st.Events("n1", 100, 0)
	ins := 0
	for _, e := range evs {
		if e.Direction == "in" {
			ins++
		}
	}
	if ins != 1 {
		t.Fatalf("got %d inbound events, want 1 — a re-push after a swallowed ack was applied twice", ins)
	}
}

// #162, THE WITNESS, and the reason the cursor heuristic was replaced rather
// than extended. c6-bell had never been given a `goal/`, so its cursor was 0 in
// both places and `cursor < storedCursor` was `0 < 0` — false. Its reboot carried
// no signal, porta held a watermark of 3981 against a node whose counter had
// restarted at 1, and for ten hours every push was discarded as already-applied
// while both ends logged success. A night of telemetry went into the gap.
//
// The distinguishing feature of this test is what it does NOT do: no goal is ever
// written. Under the old mechanism this test fails.
func TestNodePortaHasNeverWrittenToStillDetectsAReboot(t *testing.T) {
	srv, st := testServer(t)

	// A long first boot. No goal is ever written to this node, so its cursor
	// stays 0 for the whole of its life — exactly c6-bell's situation.
	pull, _ := EncodePull("n1", 0, 1, 0)
	srv.Handle(pull)
	push, _ := EncodePush("n1", 1, []space.Entry{
		{Seq: 3981, Path: "obs/build", Kind: cval.KindCell, Value: mustEnc(t, "old")},
	})
	if hw, _ := DecodeAck(srv.Handle(push)); hw != 3981 {
		t.Fatalf("first boot hw = %d, want 3981", hw)
	}

	// Repower. The cursor is 0 and was 0 — no signal there, and never will be.
	// The boot counter is the only thing that moved.
	pull, _ = EncodePull("n1", 0, 2, 0)
	srv.Handle(pull)
	push, _ = EncodePush("n1", 2, []space.Entry{
		{Seq: 1, Path: "obs/build", Kind: cval.KindCell, Value: mustEnc(t, "new")},
	})
	hw, err := DecodeAck(srv.Handle(push))
	if err != nil {
		t.Fatalf("DecodeAck: %v", err)
	}
	if hw != 1 {
		t.Fatalf("hw after reboot = %d, want 1 — the ack handed back the previous boot's watermark, which is #162 exactly", hw)
	}
	tree, _ := st.Tree("n1", "obs/")
	v, _ := cval.Decode(tree[0].Value)
	if v != "new" {
		t.Fatalf("obs/build = %v, want \"new\" — porta is still holding the previous boot's value", v)
	}
}

// THE HOLE THE FIX ITSELF COULD OPEN. A check-in is a pull then a push, so the
// pull normally detects the reboot. But datagrams are not guaranteed: lose the
// first pull of a new boot and the PUSH becomes porta's first contact with it.
// If only NoteCheckIn carried the boot test, Ingest would quietly adopt the new
// boot_id while still holding the old watermark, discard every row, and leave the
// next pull comparing two equal boot ids forever — #162 reproduced through the
// fix for it. Hence the same test in both places.
func TestLostPullStillRewindsOnThePush(t *testing.T) {
	srv, st := testServer(t)
	pull, _ := EncodePull("n1", 0, 1, 0)
	srv.Handle(pull)
	push, _ := EncodePush("n1", 1, []space.Entry{
		{Seq: 500, Path: "obs/health", Kind: cval.KindCell, Value: mustEnc(t, int64(1))},
	})
	srv.Handle(push)

	// Reboot, and the new boot's pull never arrives. Its push is first contact.
	push, _ = EncodePush("n1", 2, []space.Entry{
		{Seq: 1, Path: "obs/health", Kind: cval.KindCell, Value: mustEnc(t, int64(2))},
	})
	hw, err := DecodeAck(srv.Handle(push))
	if err != nil {
		t.Fatalf("DecodeAck: %v", err)
	}
	if hw != 1 {
		t.Fatalf("hw = %d, want 1 — the push was discarded against the previous boot's watermark", hw)
	}
	tree, _ := st.Tree("n1", "obs/")
	v, _ := cval.Decode(tree[0].Value)
	if v != int64(2) {
		t.Fatalf("obs/health = %v, want 2", v)
	}
}

// bootId 0 means the node has NO persistent boot identity (no NVS backend, or a
// failed write). porta must then NOT guess: with the same value on every boot
// there is no evidence either way, and inventing some would rewind a watermark on
// a node whose ack was merely lost — the one thing the dedupe cannot survive.
// The node is unprotected, which the server says out loud on every check-in; it
// must not also be actively corrupted.
func TestNoPersistentBootIdentityDoesNotGuess(t *testing.T) {
	srv, _ := testServer(t)
	pull, _ := EncodePull("n1", 0, 0, 0)
	srv.Handle(pull)
	push, _ := EncodePush("n1", 0, []space.Entry{
		{Seq: 7, Path: "tel/log", Kind: cval.KindStream, Value: mustEnc(t, "one")},
	})
	srv.Handle(push)

	// A missed ack on a node with no boot identity. The watermark must hold.
	pull, _ = EncodePull("n1", 0, 0, 0)
	srv.Handle(pull)
	if hw, _ := DecodeAck(srv.Handle(push)); hw != 7 {
		t.Fatalf("hw = %d, want 7 — a bootId of 0 was treated as evidence of a reboot", hw)
	}
}

// The rewind must not re-open a window the node will push into twice. A stale or
// duplicated pull frame from earlier in the SAME boot carries the cursor of that
// moment, which is not below what porta holds, so nothing moves.
func TestStalePullDoesNotDuplicate(t *testing.T) {
	srv, st := testServer(t)
	st.Write("n1", "goal/name", cval.KindCell, mustEnc(t, "alpha"), "david", "")
	pull, _ := EncodePull("n1", 1, 1, 0)
	srv.Handle(pull)
	push, _ := EncodePush("n1", 1, []space.Entry{
		{Seq: 5, Path: "tel/log", Kind: cval.KindStream, Value: mustEnc(t, "one")},
	})
	srv.Handle(push)

	srv.Handle(pull) // the duplicate, arriving late

	next, _ := EncodePush("n1", 1, []space.Entry{
		{Seq: 6, Path: "tel/log", Kind: cval.KindStream, Value: mustEnc(t, "two")},
	})
	srv.Handle(next)

	evs, _ := st.Events("n1", 100, 0)
	ins := 0
	for _, e := range evs {
		if e.Direction == "in" {
			ins++
		}
	}
	if ins != 2 {
		t.Fatalf("got %d inbound events, want 2 — the rewind duplicated a row", ins)
	}
}

// The property that removes the need for a retransmit layer.
func TestReplayedPushIsIdempotent(t *testing.T) {
	srv, st := testServer(t)
	req, _ := EncodePush("n1", 7, []space.Entry{
		{Seq: 1, Path: "tel/log", Kind: cval.KindStream, Value: mustEnc(t, "one")},
	})
	srv.Handle(req)
	srv.Handle(req)
	evs, _ := st.Events("n1", 100, 0)
	if len(evs) != 1 {
		t.Fatalf("got %d events after a replayed push, want 1", len(evs))
	}
}

// The dedupe above was previously provable only by the ABSENCE of a
// duplicate event — which reads identically to a node that never re-pushed
// at all. This test drives the positive evidence: a replayed push logs its
// skip count, and a fresh push logs nothing.
func TestReplayedPushLogsSkipCount(t *testing.T) {
	var lines []string
	st, err := space.Open(t.TempDir()+"/t.db", func() int64 { return 1000 })
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	t.Cleanup(func() { st.Close() })
	srv := NewServer(st, func(format string, args ...any) {
		lines = append(lines, fmt.Sprintf(format, args...))
	})

	req, _ := EncodePush("n1", 7, []space.Entry{
		{Seq: 1, Path: "tel/log", Kind: cval.KindStream, Value: mustEnc(t, "one")},
	})

	srv.Handle(req) // fresh push: nothing to skip
	for _, l := range lines {
		if strings.Contains(l, "skipped") {
			t.Fatalf("a fresh push logged a skip: %q", l)
		}
	}

	lines = nil
	srv.Handle(req) // replay: seq 1 is already applied
	found := false
	for _, l := range lines {
		if strings.Contains(l, "n1") && strings.Contains(l, "skipped 1") {
			found = true
		}
	}
	if !found {
		t.Fatalf("a replayed push did not log its skip count; got %v", lines)
	}
}

// Every exchange is logged — the heartbeat and the replay index (D4.3).
func TestExchangeIsLogged(t *testing.T) {
	srv, st := testServer(t)
	req, _ := EncodePull("n1", 0, 7, 0)
	srv.Handle(req)
	var n int
	if err := st.DB().QueryRow(`SELECT COUNT(*) FROM exchanges WHERE node_id='n1'`).Scan(&n); err != nil {
		t.Fatalf("count: %v", err)
	}
	if n != 1 {
		t.Fatalf("exchanges = %d, want 1", n)
	}
}

// A hostile or mis-versioned peer must get an error record, never a panic
// and never a partial apply.
func TestGarbageRequestAnswersErr(t *testing.T) {
	srv, _ := testServer(t)
	for _, junk := range [][]byte{nil, {0xff}, {0x01}, mustEnc(t, "not a record")} {
		resp := srv.Handle(junk)
		v, err := cval.Decode(resp)
		if err != nil {
			t.Fatalf("response to junk is not decodable: %v", err)
		}
		r, ok := v.(cval.Record)
		if !ok || r.Tag != "err" {
			t.Fatalf("response to junk = %v, want an #err record", cval.Format(v))
		}
	}
}

// THE DATAGRAM THAT ACTUALLY GOES OUT must fit the cap, not just the payloads
// inside it. OutboundSince budgets entry values and paths; the #batch record
// wrapper, the per-entry list heads and the seq/kind heads ride on top of that
// budget. 512 B is a 6LoWPAN constraint (RECORDS §2.1/§2.2) — a datagram
// survives only if every fragment does, so an overrun is lost packets on the
// mesh, not a tidiness question.
//
// The worst case is NEAR-CAP VALUES ON SHORT PATHS, and it is worth naming why.
// The store charges len(value)+len(path)+8 per entry while an entry actually
// encodes to len(value)+len(path)+4 — so each entry carries 4 bytes of margin,
// and the #batch wrapper costs 29. Below ~7 entries the margin cannot cover the
// wrapper, and two 239 B values on 6-char paths fill the store's budget to 506
// and then encode to 527. Long paths hide this; short ones expose it.
func TestEncodedBatchFitsTheRadioCap(t *testing.T) {
	srv, st := testServer(t)
	big := mustEnc(t, string(make([]byte, 237))) // 239 B encoded, under the 256 B value cap
	for _, p := range []string{"goal/a", "goal/b", "goal/c", "goal/d"} {
		if _, err := st.Write("n1", p, cval.KindCell, big, "david", ""); err != nil {
			t.Fatalf("Write %s: %v", p, err)
		}
	}
	req, _ := EncodePull("n1", 0, 7, 0)
	resp := srv.Handle(req)
	if len(resp) > space.MaxBatchBytes {
		hi, entries, _ := DecodeBatch(resp)
		t.Fatalf("encoded batch is %d bytes, over the %d byte cap (hi=%d, %d entries)",
			len(resp), space.MaxBatchBytes, hi, len(entries))
	}
}

// hi must name what was actually delivered. If the batch is trimmed to fit,
// advancing hi past the last entry that rode would make the node skip writes
// it never saw — silent, permanent divergence.
func TestBatchHiMatchesTheLastDeliveredEntry(t *testing.T) {
	srv, st := testServer(t)
	big := mustEnc(t, string(make([]byte, 200)))
	for _, p := range []string{"goal/a", "goal/b", "goal/c", "goal/d"} {
		st.Write("n1", p, cval.KindCell, big, "david", "")
	}
	req, _ := EncodePull("n1", 0, 7, 0)
	hi, entries, err := DecodeBatch(srv.Handle(req))
	if err != nil {
		t.Fatalf("DecodeBatch: %v", err)
	}
	if len(entries) == 0 {
		t.Fatal("no entries delivered")
	}
	if hi != entries[len(entries)-1].Seq {
		t.Fatalf("hi = %d but the last delivered entry is seq %d — the node would skip the gap",
			hi, entries[len(entries)-1].Seq)
	}
}

// Trimming must not stall. Each pull delivers at least one entry and advances
// hi, so a node that keeps checking in converges — otherwise a value slightly
// too large for a shared batch would be retried forever and the node would
// never move past it.
func TestTrimmedBatchesStillConverge(t *testing.T) {
	srv, st := testServer(t)
	big := mustEnc(t, string(make([]byte, 237)))
	paths := []string{"goal/a", "goal/b", "goal/c", "goal/d", "goal/e"}
	for _, p := range paths {
		st.Write("n1", p, cval.KindCell, big, "david", "")
	}
	seen := map[string]bool{}
	cursor := int64(0)
	for round := 0; round < 10 && len(seen) < len(paths); round++ {
		req, _ := EncodePull("n1", cursor, 7, 0)
		resp := srv.Handle(req)
		if len(resp) > space.MaxBatchBytes {
			t.Fatalf("round %d: batch is %d bytes, over the cap", round, len(resp))
		}
		hi, entries, err := DecodeBatch(resp)
		if err != nil {
			t.Fatalf("round %d: DecodeBatch: %v", round, err)
		}
		if len(entries) == 0 {
			t.Fatalf("round %d: empty batch at cursor %d with %d/%d paths seen — stalled",
				round, cursor, len(seen), len(paths))
		}
		for _, e := range entries {
			seen[e.Path] = true
		}
		if hi <= cursor {
			t.Fatalf("round %d: hi %d did not advance past cursor %d", round, hi, cursor)
		}
		cursor = hi
	}
	if len(seen) != len(paths) {
		t.Fatalf("converged on %d/%d paths: %v", len(seen), len(paths), seen)
	}
}

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
