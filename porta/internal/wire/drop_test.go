// Copyright (c) 2026 Ekorau LLC

// internal/wire/drop_test.go
package wire

import (
	"context"
	"net"
	"testing"
	"time"

	"github.com/davidg238/porta/internal/cval"
	"github.com/davidg238/porta/internal/space"
)

// dropHarness runs a real ListenAndServe on loopback and answers push/pull
// helpers. A real socket rather than a direct Handle call, because the
// swallow happens in the UDP loop and not in Handle — testing Handle would
// test the wrong layer AND would never exercise swallowAck at all.
func dropHarness(t *testing.T, dropEvery int) (*Server, *space.Store, func(seq int64, text string) bool, func(cursor int64) (int64, []space.Entry, bool)) {
	t.Helper()
	srv, st := testServer(t)
	srv.SetDropAckEvery(dropEvery)

	// Bind on port 0 and read the address back, then close: the listen/close/
	// re-listen window is small and the alternative (a guessed port) is worse.
	pc, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	addr := pc.LocalAddr().String()
	pc.Close()

	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	go srv.ListenAndServe(ctx, addr)

	conn, err := net.Dial("udp", addr)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	t.Cleanup(func() { conn.Close() })

	push := func(seq int64, text string) bool {
		t.Helper()
		req, err := EncodePush("n1", 1, []space.Entry{
			{Seq: seq, Path: "tel/log", Kind: cval.KindStream, Value: mustEnc(t, text)},
		})
		if err != nil {
			t.Fatalf("EncodePush: %v", err)
		}
		if _, err := conn.Write(req); err != nil {
			t.Fatalf("write: %v", err)
		}
		buf := make([]byte, 1500)
		conn.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
		n, err := conn.Read(buf)
		if err != nil {
			return false // no reply within the budget: swallowed (or lost)
		}
		if _, err := DecodeAck(buf[:n]); err != nil {
			t.Fatalf("reply to seq %d was not an #ack: %v", seq, err)
		}
		return true
	}

	pull := func(cursor int64) (int64, []space.Entry, bool) {
		t.Helper()
		req, err := EncodePull("n1", cursor, 1, 0)
		if err != nil {
			t.Fatalf("EncodePull: %v", err)
		}
		if _, err := conn.Write(req); err != nil {
			t.Fatalf("write: %v", err)
		}
		buf := make([]byte, 1500)
		conn.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
		n, err := conn.Read(buf)
		if err != nil {
			return 0, nil, false // no reply within the budget: lost (never swallowed)
		}
		hi, entries, err := DecodeBatch(buf[:n])
		if err != nil {
			t.Fatalf("reply to pull at cursor %d was not a #batch: %v", cursor, err)
		}
		return hi, entries, true
	}
	return srv, st, push, pull
}

// waitForListener sends seq 1 until it is answered. UDP has no accept, so a
// datagram that lands before the bind completes is silently lost. Re-sending
// the same seq is safe — an already-ingested seq is deduped, which is the very
// property this file exists to protect.
//
// The budget is denominated in TIME, not in attempts (rete#183). It used to be
// 50 immediate retries with no delay, which is a budget that shrinks exactly
// when the thing it waits for gets slower: under full-package load the whole
// budget could be spent before ListenAndServe's bind completed, and `go test
// ./...` — a gate — went red 3 runs in 10 with no code change involved. A poll
// budget equal to its own cost reads as a real assertion and is not one.
func waitForListener(t *testing.T, push func(int64, string) bool) {
	t.Helper()
	waitForBind(t, func() bool { return push(1, "one") }, "the listener never answered")
}

// waitForListenerViaPull is waitForListener's counterpart for a harness where
// push can never signal readiness — e.g. dropEvery=1, where every ack is
// swallowed and push always reports false. A #batch is never swallowed, so
// retrying a pull is the "the socket is up" probe instead.
func waitForListenerViaPull(t *testing.T, pull func(int64) (int64, []space.Entry, bool)) {
	t.Helper()
	waitForBind(t, func() bool { _, _, ok := pull(0); return ok }, "the listener never answered a pull")
}

// waitForBind is the one deadline loop both helpers above use — the probe differs,
// the budget must not. Five seconds is far longer than a bind ever takes and is
// there to be never reached: the point is that the budget cannot be exhausted by
// the scheduler being busy, so a failure here means the listener is genuinely dead
// rather than merely slow. The 10 ms sleep is what stops the loop from spending its
// own budget spinning against the goroutine it is waiting for.
func waitForBind(t *testing.T, probe func() bool, msg string) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if probe() {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal(msg)
}

// The fault injector the C6 hardware gate scores leg 4 with. What matters is not
// that an ack went missing but WHEN: the push must already be committed, so the
// node re-pushes rows porta has applied and only the `seq <= hw` dedupe stands
// between that and a duplicate. A drop placed before the commit would merely
// reproduce a lost push, which any retry handles and which proves nothing.
func TestDropAckEverySwallowsTheNthAckAfterTheCommit(t *testing.T) {
	_, st, push, _ := dropHarness(t, 3)
	waitForListener(t, push) // ack #1

	if !push(2, "two") {
		t.Fatal("the second ack was dropped; only every third should be")
	}
	if push(3, "three") {
		t.Fatal("the third ack should have been swallowed")
	}

	evs, err := st.Events("n1", 100)
	if err != nil {
		t.Fatalf("Events: %v", err)
	}
	if len(evs) != 3 {
		t.Fatalf("got %d events, want 3 — a swallowed ack must not skip the commit", len(evs))
	}
}

// The push half only. A node that never hears a #batch stalls its cursor, which
// is a different failure from the one being injected — so with every ack
// swallowed, pulls must still answer.
//
// This must go over the live socket, not a direct srv.HandleFrom call:
// HandleFrom never routes through swallowAck (that only runs inside
// ListenAndServe's reply loop), so asserting against HandleFrom would still
// pass even if swallowAck's #ack-vs-everything-else guard were deleted and it
// swallowed every outbound datagram. Only a real round-trip through the
// listener exercises the discriminator this test exists to prove.
func TestDropAckEveryAnswersPullsWhileSwallowingAcks(t *testing.T) {
	_, st, push, pull := dropHarness(t, 1)
	if _, err := st.Write("n1", "goal/name", cval.KindCell, mustEnc(t, "alpha"), "david", ""); err != nil {
		t.Fatalf("Write: %v", err)
	}
	// push can't signal readiness here — dropEvery=1 swallows every ack, so
	// push always reports false. Wait via pull instead: a #batch is never
	// swallowed.
	waitForListenerViaPull(t, pull)

	hi, entries, ok := pull(0)
	if !ok {
		t.Fatal("a #batch must never be swallowed, but the pull got no reply")
	}
	if hi < 1 {
		t.Fatalf("hi = %d, want at least 1", hi)
	}
	found := false
	for _, e := range entries {
		if e.Path == "goal/name" {
			found = true
		}
	}
	if !found {
		t.Fatalf("batch entries = %+v, missing goal/name", entries)
	}

	if push(1, "one") {
		t.Fatal("with dropEvery=1 no ack should ever be delivered")
	}
}

// The zero value must be a true off-switch. Every other test in this file
// drives a Server whose SetDropAckEvery was called with a nonzero N; none
// exercises the default a production gateway actually runs with. dropEvery=0
// is the constructor's zero value too, so this also covers a Server on which
// SetDropAckEvery was never called at all.
func TestDropAckEveryZeroNeverSwallows(t *testing.T) {
	_, _, push, _ := dropHarness(t, 0)
	waitForListener(t, push) // ack #1
	if !push(2, "two") {
		t.Fatal("dropAckEvery=0 must never swallow an ack, but the push got no reply")
	}
}
