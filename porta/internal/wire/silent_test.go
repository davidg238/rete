// Copyright (c) 2026 Ekorau LLC

// internal/wire/silent_test.go — rete#269's injector.
package wire

import (
	"context"
	"net"
	"testing"
	"time"

	"github.com/davidg238/porta/internal/cval"
	"github.com/davidg238/porta/internal/space"
)

// silentHarness runs a real Serve on loopback. A real socket and not a Handle
// call, because SILENCE CANNOT BE EXPRESSED THROUGH Handle — its contract is
// that it never returns nil, which is exactly the reason this injector lives in
// the loop and exactly the reason #232's gate could not make this assertion.
func silentHarness(t *testing.T, drop bool) (*Server, *space.Store, func([]byte) ([]byte, bool)) {
	t.Helper()
	srv, st := testServer(t)
	srv.SetDropAll(drop)

	pc, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	addr := pc.LocalAddr().String()

	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	go srv.Serve(ctx, pc)

	conn, err := net.Dial("udp", addr)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	t.Cleanup(func() { conn.Close() })

	// ask answers the reply and whether one arrived at all.
	ask := func(req []byte) ([]byte, bool) {
		t.Helper()
		if _, err := conn.Write(req); err != nil {
			t.Fatalf("write: %v", err)
		}
		buf := make([]byte, 1500)
		conn.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
		n, err := conn.Read(buf)
		if err != nil {
			return nil, false
		}
		out := make([]byte, n)
		copy(out, buf[:n])
		return out, true
	}
	return srv, st, ask
}

func silentFrames(t *testing.T) (pull, push []byte) {
	t.Helper()
	var err error
	if pull, err = EncodePull("n1", 0, 7, 0, 0); err != nil {
		t.Fatalf("EncodePull: %v", err)
	}
	if push, err = EncodePush("n1", 7, []space.Entry{
		{Seq: 1, Path: "tel/log", Kind: cval.KindStream, Value: mustEnc(t, "hello")},
	}); err != nil {
		t.Fatalf("EncodePush: %v", err)
	}
	return pull, push
}

// TestDropAllAnswersNothingAndIngestsNothing is rete#269's injector: the
// condition that killed c6spare on 2026-08-27 — a node whose datagrams stop
// arriving at a gateway that is otherwise healthy. Both legs must go
// unanswered, and neither may reach the store, because porta held no exchange
// row for that node after 10:40:09.
func TestDropAllAnswersNothingAndIngestsNothing(t *testing.T) {
	_, st, ask := silentHarness(t, true)
	pull, push := silentFrames(t)

	for name, req := range map[string][]byte{"pull": pull, "push": push} {
		if resp, got := ask(req); got {
			t.Fatalf("%s was answered (%d bytes) — this injector's whole property is silence", name, len(resp))
		}
	}

	n, err := st.CountNodes()
	if err != nil {
		t.Fatalf("CountNodes: %v", err)
	}
	if n != 0 {
		t.Fatalf("a dropped exchange reached the store: %d node row(s) — the drop must be at the DOOR, "+
			"since the witnessed failure ingested nothing", n)
	}
}

// TestDropAllOffServesNormally is the control, and it is not optional: without
// it, a harness that never delivered a datagram at all would pass the test
// above while proving nothing. A no-reply result is only evidence of the
// injector if a reply arrives with the injector off.
func TestDropAllOffServesNormally(t *testing.T) {
	_, st, ask := silentHarness(t, false)
	pull, push := silentFrames(t)

	resp, got := ask(push)
	if !got {
		t.Fatal("push went unanswered with the knob OFF — the harness never delivered a datagram, " +
			"so the silence test above would have passed for the wrong reason")
	}
	if _, err := DecodeAck(resp); err != nil {
		t.Fatalf("push with the knob off was not acked: %v", err)
	}
	resp, got = ask(pull)
	if !got {
		t.Fatal("pull went unanswered with the knob OFF")
	}
	if _, _, err := DecodeBatch(resp); err != nil {
		t.Fatalf("pull with the knob off was not served a #batch: %v", err)
	}
	if n, err := st.CountNodes(); err != nil || n != 1 {
		t.Fatalf("knob off should have ingested: %d node row(s), err %v", n, err)
	}
}

// TestSilenceIsNotRefusal is the distinction rete#269 turns on, asserted rather
// than left in a comment. Both injectors sit at the door and both ingest
// nothing; they differ ONLY in whether the node gets a reply — and that
// difference is what `misses` counts. A refused node reads `misses 0` to its
// death (which is why #232's gate reads `ref` and not `misses`); a node on a
// silent uplink reads `misses` climbing and `ref` at 0, which is what c6spare
// reported for its last 204 seconds.
func TestSilenceIsNotRefusal(t *testing.T) {
	pull, _ := silentFrames(t)

	// Refusal: a reply, every time.
	refuser, _ := testServer(t)
	refuser.SetRefuseAll(true)
	if resp := refuser.Handle(pull); resp == nil {
		t.Fatal("refusal answered nil — refusal is a reply")
	}

	// Silence: no reply, ever. And it cannot even be asked of Handle, which is
	// the structural half of the same point.
	_, _, ask := silentHarness(t, true)
	if _, got := ask(pull); got {
		t.Fatal("the silent injector answered — then it is a second refuser, not a lost uplink")
	}
}

// TestDropAllAndRefuseAllAreIndependent: a gateway may be silent, refusing,
// both or neither, and the zero value of each must leave a production gateway
// alone. Silence wins when both are set, because a dropped datagram never
// reaches the code that would refuse it — asserted so the precedence is a fact
// rather than a reading of the loop.
func TestDropAllAndRefuseAllAreIndependent(t *testing.T) {
	pull, _ := silentFrames(t)
	srv, _, ask := silentHarness(t, true)
	srv.SetRefuseAll(true)

	if _, got := ask(pull); got {
		t.Fatal("with both injectors on, a reply arrived — the drop must precede the refusal")
	}

	// And with both off, the same socket serves normally.
	srv.SetDropAll(false)
	srv.SetRefuseAll(false)
	resp, got := ask(pull)
	if !got {
		t.Fatal("both knobs off: no reply")
	}
	if _, _, err := DecodeBatch(resp); err != nil {
		t.Fatalf("both knobs off: not a #batch: %v", err)
	}
}
