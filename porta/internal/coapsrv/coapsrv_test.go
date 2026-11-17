// Copyright (c) 2026 Ekorau LLC

package coapsrv

import (
	"bytes"
	"context"
	"encoding/hex"
	"errors"
	"fmt"
	"net"
	"os"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/davidg238/porta/internal/blob"
	"github.com/davidg238/porta/internal/blockserv"
	"github.com/davidg238/porta/internal/space"
	"github.com/davidg238/porta/internal/wire"
	"github.com/davidg238/rete/protocol/cval"
	"github.com/plgd-dev/go-coap/v3/message"
	"github.com/plgd-dev/go-coap/v3/message/codes"
	"github.com/plgd-dev/go-coap/v3/message/pool"
	"github.com/plgd-dev/go-coap/v3/net/blockwise"
	"github.com/plgd-dev/go-coap/v3/options"
	"github.com/plgd-dev/go-coap/v3/udp"
	"github.com/plgd-dev/go-coap/v3/udp/client"
)

type rig struct {
	port  int
	st    *space.Store
	blobs *blob.Store
	ex    *wire.Server
}

// harness starts a coapsrv on a dual-stack ephemeral port. The store's clock is
// atomic: a bare int64 clock would itself be a data race once two listeners
// call the store (Task 3's -race run), a check that shares a cause with the
// failure it is meant to find.
func harness(t *testing.T) *rig {
	t.Helper()
	var clock atomic.Int64
	clock.Store(1_000)
	st, err := space.Open(t.TempDir()+"/t.db", func() int64 { return clock.Add(10) })
	if err != nil {
		t.Fatalf("space.Open: %v", err)
	}
	blobs, err := blob.Open(t.TempDir())
	if err != nil {
		t.Fatalf("blob.Open: %v", err)
	}
	ex := wire.NewServer(st, nil)
	pc, err := net.ListenUDP("udp", &net.UDPAddr{})
	if err != nil {
		t.Fatalf("ListenUDP: %v", err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- New(ex, blobs, nil).Serve(ctx, pc) }()
	t.Cleanup(func() {
		cancel()
		select {
		case err := <-done:
			if err != nil {
				t.Errorf("Serve returned %v after cancel, want nil", err)
			}
		case <-time.After(5 * time.Second):
			t.Errorf("Serve did not return within 5 s of cancel")
		}
		st.Close()
	})
	return &rig{port: pc.LocalAddr().(*net.UDPAddr).Port, st: st, blobs: blobs, ex: ex}
}

// dial opens a CoAP client to host:port; bw switches the library's blockwise
// layer on the CLIENT side, at szx.
func dial(t *testing.T, host string, port int, bw bool, szx blockwise.SZX) *client.Conn {
	t.Helper()
	c, err := udp.Dial(net.JoinHostPort(host, fmt.Sprint(port)),
		options.WithBlockwise(bw, szx, 5*time.Second))
	if err != nil {
		t.Fatalf("udp.Dial %s: %v", host, err)
	}
	t.Cleanup(func() { c.Close() })
	return c
}

func ctx5(t *testing.T) context.Context {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	t.Cleanup(cancel)
	return ctx
}

func mustEnc(t *testing.T, v any) []byte {
	t.Helper()
	b, err := cval.Encode(v)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func mustPull(t *testing.T, id string) []byte {
	t.Helper()
	req, err := wire.EncodePull(id, 0, 7, 0, 0)
	if err != nil {
		t.Fatal(err)
	}
	return req
}

// post sends body to path and answers code, content format and body.
func post(t *testing.T, c *client.Conn, path string, body []byte) (codes.Code, *pool.Message, []byte) {
	t.Helper()
	resp, err := c.Post(ctx5(t), path, message.AppCBOR, bytes.NewReader(body))
	if err != nil {
		t.Fatalf("POST %s: %v", path, err)
	}
	return resp.Code(), resp, readBody(t, resp)
}

func readBody(t *testing.T, m *pool.Message) []byte {
	t.Helper()
	if m.Body() == nil {
		return nil
	}
	b, err := m.ReadBody()
	if err != nil {
		t.Fatalf("ReadBody: %v", err)
	}
	return b
}

func recordTag(t *testing.T, b []byte) (string, cval.Record) {
	t.Helper()
	v, err := cval.Decode(b)
	if err != nil {
		t.Fatalf("reply is not CBOR: %v (%x)", err, b)
	}
	r, ok := v.(cval.Record)
	if !ok {
		t.Fatalf("reply is not a record: %#v", v)
	}
	return string(r.Tag), r
}

func exchanges(t *testing.T, st *space.Store, id string) int {
	t.Helper()
	var n int
	if err := st.DB().QueryRow(`SELECT COUNT(*) FROM exchanges WHERE node_id = ?`, id).Scan(&n); err != nil {
		t.Fatal(err)
	}
	return n
}

func TestPullMatchesHandleFrom(t *testing.T) {
	rig := harness(t)
	rig.st.Write("n1", "goal/name", cval.KindCell, mustEnc(t, "alpha"), "david", "")
	req := mustPull(t, "n1")
	code, resp, body := post(t, dial(t, "127.0.0.1", rig.port, false, blockwise.SZX256), "/space", req)
	if code != codes.Changed {
		t.Fatalf("code = %v, want Changed", code)
	}
	if cf, err := resp.ContentFormat(); err != nil || cf != message.AppCBOR {
		t.Fatalf("content format = %v (%v), want AppCBOR", cf, err)
	}
	// A cursor-0 pull of a cell replays identically.
	if got2 := rig.ex.HandleFrom(req, ""); !bytes.Equal(body, got2) {
		t.Fatalf("CoAP body %x != HandleFrom %x", body, got2)
	}
	if tag, _ := recordTag(t, body); tag != "batch" {
		t.Fatalf("tag = %s, want batch", tag)
	}
}

func mustPush(t *testing.T) []byte {
	t.Helper()
	req, err := wire.EncodePush("n1", 1, 0, []space.Entry{
		{Seq: 1, Path: "tel/log", Kind: cval.KindStream, Value: mustEnc(t, "hello")},
	})
	if err != nil {
		t.Fatal(err)
	}
	return req
}

func TestPushAckedOverCoAP(t *testing.T) {
	rig := harness(t)
	code, _, body := post(t, dial(t, "127.0.0.1", rig.port, false, blockwise.SZX256), "/space", mustPush(t))
	if code != codes.Changed {
		t.Fatalf("code = %v", code)
	}
	hw, err := wire.DecodeAck(body)
	if err != nil || hw != 1 {
		t.Fatalf("ack hw=%d err=%v", hw, err)
	}
}

func TestPushTwiceAcksOnce(t *testing.T) {
	rig := harness(t)
	c := dial(t, "127.0.0.1", rig.port, false, blockwise.SZX256)
	for i := 0; i < 2; i++ {
		code, _, body := post(t, c, "/space", mustPush(t))
		if code != codes.Changed {
			t.Fatalf("push %d: code = %v", i, code)
		}
		if hw, err := wire.DecodeAck(body); err != nil || hw != 1 {
			t.Fatalf("push %d: ack hw=%d err=%v", i, hw, err)
		}
	}
	evs, err := rig.st.Events("n1", 100, 0)
	if err != nil {
		t.Fatal(err)
	}
	n := 0
	for _, e := range evs {
		if e.Path == "tel/log" {
			n++
		}
	}
	if n != 1 {
		t.Fatalf("tel/log rows = %d, want 1 (events %+v)", n, evs)
	}
}

func TestRefusalIsErrIn204(t *testing.T) {
	rig := harness(t)
	rig.ex.SetRefuseAll(true)
	code, _, body := post(t, dial(t, "127.0.0.1", rig.port, false, blockwise.SZX256), "/space", mustPull(t, "n1"))
	if code != codes.Changed {
		t.Fatalf("code = %v, want Changed", code)
	}
	if tag, _ := recordTag(t, body); tag != "err" {
		t.Fatalf("tag = %s, want err", tag)
	}
}

func TestGarbageBodyIsErrRecord(t *testing.T) {
	rig := harness(t)
	c := dial(t, "127.0.0.1", rig.port, false, blockwise.SZX256)
	for _, b := range [][]byte{{}, {0xff}} {
		code, _, body := post(t, c, "/space", b)
		if code != codes.Changed {
			t.Fatalf("body %x: code = %v, want Changed", b, code)
		}
		if tag, _ := recordTag(t, body); tag != "err" {
			t.Fatalf("body %x: tag = %s, want err", b, tag)
		}
	}
}

func TestSpaceMethodAndPath(t *testing.T) {
	rig := harness(t)
	c := dial(t, "127.0.0.1", rig.port, false, blockwise.SZX256)
	resp, err := c.Get(ctx5(t), "/space")
	if err != nil {
		t.Fatal(err)
	}
	if resp.Code() != codes.MethodNotAllowed {
		t.Fatalf("GET /space = %v, want MethodNotAllowed", resp.Code())
	}
	resp, err = c.Get(ctx5(t), "/nope")
	if err != nil {
		t.Fatal(err)
	}
	if resp.Code() != codes.NotFound {
		t.Fatalf("GET /nope = %v, want NotFound", resp.Code())
	}
}

func TestSpaceRouteIsExact(t *testing.T) {
	rig := harness(t)
	c := dial(t, "127.0.0.1", rig.port, false, blockwise.SZX256)
	for _, p := range []string{"/spacex", "/space/x"} {
		code, _, _ := post(t, c, p, mustPull(t, "n1"))
		if code != codes.NotFound {
			t.Fatalf("POST %s = %v, want NotFound", p, code)
		}
	}
	if n := exchanges(t, rig.st, "n1"); n != 0 {
		t.Fatalf("exchange rows for n1 = %d, want 0: a near-miss path reached HandleFrom", n)
	}
}

func TestSpaceReplyIsNeverSplit(t *testing.T) {
	rig := harness(t)
	for i := 0; i < 3; i++ {
		rig.st.Write("n1", fmt.Sprintf("goal/c%d", i), cval.KindCell, mustEnc(t, strings.Repeat("x", 120)), "david", "")
	}
	code, resp, body := post(t, dial(t, "127.0.0.1", rig.port, false, blockwise.SZX256), "/space", mustPull(t, "n1"))
	if code != codes.Changed {
		t.Fatalf("code = %v", code)
	}
	if len(body) <= blockserv.BlockSize {
		t.Fatalf("body %d bytes; the test needs more than %d to mean anything", len(body), blockserv.BlockSize)
	}
	if _, err := resp.GetOptionUint32(message.Block2); !errors.Is(err, message.ErrOptionNotFound) {
		t.Fatalf("reply carries Block2 (err=%v); a check-in must be one datagram", err)
	}
	if tag, _ := recordTag(t, body); tag != "batch" {
		t.Fatalf("tag = %s", tag)
	}
}

func TestBothFamilies(t *testing.T) {
	rig := harness(t)
	rig.st.Write("n1", "goal/name", cval.KindCell, mustEnc(t, "alpha"), "david", "")
	for _, host := range []string{"127.0.0.1", "::1"} {
		code, _, body := post(t, dial(t, host, rig.port, false, blockwise.SZX256), "/space", mustPull(t, "n1"))
		if code != codes.Changed {
			t.Fatalf("%s: code = %v", host, code)
		}
		if tag, _ := recordTag(t, body); tag != "batch" {
			t.Fatalf("%s: tag = %s", host, tag)
		}
	}
}

// The interop gate's record is pinned to the encoder, so it cannot drift.
func TestInteropFixtureIsThePull(t *testing.T) {
	raw, err := os.ReadFile("../../tests/fixtures/pull.hex")
	if err != nil {
		t.Fatal(err)
	}
	got, err := hex.DecodeString(strings.TrimSpace(string(raw)))
	if err != nil {
		t.Fatalf("pull.hex: %v", err)
	}
	want, err := wire.EncodePull("0123456789ab", 0, 1, 0, 0)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, want) {
		t.Fatalf("pull.hex = %x, want %x", got, want)
	}
}
