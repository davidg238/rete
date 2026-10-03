// Copyright (c) 2026 Ekorau LLC

// internal/portacli/loopback_test.go
package portacli

import (
	"context"
	"net"
	"testing"
	"time"

	"github.com/davidg238/porta/internal/cval"
	"github.com/davidg238/porta/internal/space"
	"github.com/davidg238/porta/internal/wire"
)

// A synthetic node: exactly the exchange the nsl bridge job will run, over a
// real socket, so the framing is proven before any hardware is involved.
type fakeNode struct {
	t      *testing.T
	conn   net.Conn
	id     string
	bootID int64
	cursor int64
	seq    int64
}

func (n *fakeNode) roundTrip(req []byte) []byte {
	n.t.Helper()
	if _, err := n.conn.Write(req); err != nil {
		n.t.Fatalf("write: %v", err)
	}
	n.conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 1500)
	m, err := n.conn.Read(buf)
	if err != nil {
		n.t.Fatalf("read: %v", err)
	}
	return buf[:m]
}

func (n *fakeNode) pull() []space.Entry {
	n.t.Helper()
	req, err := wire.EncodePull(n.id, n.cursor, n.bootID, 0, 0)
	if err != nil {
		n.t.Fatal(err)
	}
	hi, entries, err := wire.DecodeBatch(n.roundTrip(req))
	if err != nil {
		n.t.Fatalf("DecodeBatch: %v", err)
	}
	n.cursor = hi // persisted only after apply, exactly as the bridge job will
	return entries
}

func (n *fakeNode) push(path string, kind cval.Kind, v any) int64 {
	n.t.Helper()
	raw, err := cval.Encode(v)
	if err != nil {
		n.t.Fatal(err)
	}
	n.seq++
	req, err := wire.EncodePush(n.id, n.bootID, []space.Entry{{Seq: n.seq, Path: path, Kind: kind, Value: raw}})
	if err != nil {
		n.t.Fatal(err)
	}
	hw, err := wire.DecodeAck(n.roundTrip(req))
	if err != nil {
		n.t.Fatalf("DecodeAck: %v", err)
	}
	return hw
}

func TestLoopbackGate(t *testing.T) {
	var clock int64 = 1_000
	st, err := space.Open(t.TempDir()+"/t.db", func() int64 { clock += 10; return clock })
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	defer st.Close()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	srv := wire.NewServer(st, func(string, ...any) {})
	// Bind the real listener on port 0 and read the address back from it, so
	// the socket is never closed and re-opened — a listen/close/re-listen
	// dance can lose the port to another process between the two calls.
	// (rete#158: this comment used to describe an intent the API could not
	// honor — the code below closed pc and re-listened anyway. Serve takes
	// the live socket now.)
	pc, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	addr := pc.LocalAddr().String()
	go srv.Serve(ctx, pc)

	conn, err := net.Dial("udp", addr)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn.Close()
	node := &fakeNode{t: t, conn: conn, id: "c6-01", bootID: 1}

	// The listener races the first datagram: UDP has no accept, so a write
	// that lands before ListenAndServe binds is silently dropped and the read
	// blocks to its deadline. Poll until the server answers rather than
	// sleeping a guessed interval.
	waitForListener(t, node)

	// 1. A cell written at porta reaches the node.
	if _, err := st.Write("c6-01", "goal/name", cval.KindCell, mustEncode(t, "shipsbells"), "david", "bring-up"); err != nil {
		t.Fatalf("Write: %v", err)
	}
	got := node.pull()
	if len(got) != 1 || got[0].Path != "goal/name" {
		t.Fatalf("pull got %+v", got)
	}
	if v, _ := cval.Decode(got[0].Value); v != "shipsbells" {
		t.Fatalf("goal/name = %v", v)
	}

	// 2. An idle pull is the heartbeat: empty, not an error.
	if got := node.pull(); len(got) != 0 {
		t.Fatalf("second pull returned %d entries, want 0", len(got))
	}

	// 3. A cell pushed by the node reaches porta's tree.
	node.push("obs/health", cval.KindCell, cval.Record{Tag: "Health", Fields: cval.Map{
		{K: cval.Symbol("up"), V: int64(12345)},
		{K: cval.Symbol("bid"), V: int64(1)},
	}})
	tree, _ := st.Tree("c6-01", "obs/")
	if len(tree) != 1 || tree[0].Path != "obs/health" {
		t.Fatalf("obs tree = %+v", tree)
	}

	// 3b. The node's own obs cell must not come back on the next pull. The
	// cell tree holds both directions, so this is the one place the loopback
	// can catch an echo that unit tests on a single seq space cannot.
	if got := node.pull(); len(got) != 0 {
		t.Fatalf("pull after the node's own push returned %+v — porta echoed it back", got)
	}

	// 4. Stream entries arrive exactly once across a duplicated datagram —
	//    the property that lets the wire carry no retransmit layer.
	node.seq++
	raw, _ := cval.Encode("boot complete")
	req, _ := wire.EncodePush(node.id, node.bootID, []space.Entry{
		{Seq: node.seq, Path: "tel/log", Kind: cval.KindStream, Value: raw},
	})
	node.roundTrip(req)
	node.roundTrip(req) // the same datagram again, as a lossy radio would
	evs, _ := st.Events("c6-01", 100, 0)
	n := 0
	for _, e := range evs {
		if e.Path == "tel/log" {
			n++
		}
	}
	if n != 1 {
		t.Fatalf("tel/log appears %d times after a duplicated push, want 1", n)
	}

	// 5. Every exchange is logged: 3 pulls + 3 pushes, plus whatever probes
	//    the listener handshake needed.
	var xn int
	if err := st.DB().QueryRow(`SELECT COUNT(*) FROM exchanges WHERE node_id='c6-01'`).Scan(&xn); err != nil {
		t.Fatal(err)
	}
	if xn < 6 {
		t.Fatalf("exchanges = %d, want at least 6 (3 pulls + 3 pushes)", xn)
	}

	// 6. The peer address is recorded — v1's nodes.source_addr, which the
	//    capability audit caught with no successor (design §4.2). This is the
	//    only test that exercises the real socket path that supplies it.
	peer, err := st.NodeAddr("c6-01")
	if err != nil {
		t.Fatalf("NodeAddr: %v", err)
	}
	if peer == "" {
		t.Fatal("no peer address recorded; the UDP loop is not stamping it")
	}
	if _, _, err := net.SplitHostPort(peer); err != nil {
		t.Fatalf("peer address %q is not host:port: %v", peer, err)
	}
}

// waitForListener sends pulls until one is answered. UDP has no connection
// setup, so there is no event that says "the server is bound" — only a reply.
func waitForListener(t *testing.T, n *fakeNode) {
	t.Helper()
	req, err := wire.EncodePull(n.id, 0, n.bootID, 0, 0)
	if err != nil {
		t.Fatal(err)
	}
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if _, err := n.conn.Write(req); err != nil {
			t.Fatalf("write: %v", err)
		}
		n.conn.SetReadDeadline(time.Now().Add(100 * time.Millisecond))
		buf := make([]byte, 1500)
		if _, err := n.conn.Read(buf); err == nil {
			return
		}
	}
	t.Fatal("the wire listener never answered a pull")
}

func mustEncode(t *testing.T, v any) []byte {
	t.Helper()
	b, err := cval.Encode(v)
	if err != nil {
		t.Fatal(err)
	}
	return b
}
