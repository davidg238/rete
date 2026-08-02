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

// The property that removes the need for a retransmit layer.
func TestReplayedPushIsIdempotent(t *testing.T) {
	srv, st := testServer(t)
	req, _ := EncodePush("n1", 7, []space.Entry{
		{Seq: 1, Path: "tel/log", Kind: cval.KindStream, Value: mustEnc(t, "one")},
	})
	srv.Handle(req)
	srv.Handle(req)
	evs, _ := st.Events("n1", 100)
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
