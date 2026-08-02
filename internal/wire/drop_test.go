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

// dropHarness runs a real ListenAndServe on loopback and answers a push helper.
// A real socket rather than a direct Handle call, because the swallow happens in
// the UDP loop and not in Handle — testing Handle would test the wrong layer.
func dropHarness(t *testing.T, dropEvery int) (*Server, *space.Store, func(seq int64, text string) bool) {
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
	return srv, st, push
}

// waitForListener sends seq 1 until it is answered. UDP has no accept, so a
// datagram that lands before the bind completes is silently lost. Re-sending
// the same seq is safe — an already-ingested seq is deduped, which is the very
// property this file exists to protect.
func waitForListener(t *testing.T, push func(int64, string) bool) {
	t.Helper()
	for i := 0; i < 50; i++ {
		if push(1, "one") {
			return
		}
	}
	t.Fatal("the listener never answered")
}

// The fault injector the C6 hardware gate scores leg 4 with. What matters is not
// that an ack went missing but WHEN: the push must already be committed, so the
// node re-pushes rows porta has applied and only the `seq <= hw` dedupe stands
// between that and a duplicate. A drop placed before the commit would merely
// reproduce a lost push, which any retry handles and which proves nothing.
func TestDropAckEverySwallowsTheNthAckAfterTheCommit(t *testing.T) {
	_, st, push := dropHarness(t, 3)
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
func TestDropAckEveryAnswersPullsWhileSwallowingAcks(t *testing.T) {
	srv, st, push := dropHarness(t, 1)
	if _, err := st.Write("n1", "goal/name", cval.KindCell, mustEnc(t, "alpha"), "david", ""); err != nil {
		t.Fatalf("Write: %v", err)
	}
	req, err := EncodePull("n1", 0, 1, 0)
	if err != nil {
		t.Fatalf("EncodePull: %v", err)
	}
	// Handle directly for the batch half: the swallow is ack-only, so this
	// asserts the discriminator rather than the socket.
	if _, _, err := DecodeBatch(srv.HandleFrom(req, "")); err != nil {
		t.Fatalf("a #batch must never be swallowed: %v", err)
	}
	if push(1, "one") {
		t.Fatal("with dropEvery=1 no ack should ever be delivered")
	}
}
