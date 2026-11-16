// Copyright (c) 2026 Ekorau LLC

package coapsrv

import (
	"bytes"
	"context"
	"net"
	"sync"
	"testing"
	"time"

	"github.com/davidg238/porta/internal/wire"
	"github.com/davidg238/rete/protocol/cval"
	"github.com/plgd-dev/go-coap/v3/message"
	"github.com/plgd-dev/go-coap/v3/message/codes"
	"github.com/plgd-dev/go-coap/v3/net/blockwise"
)

// Design §8.5: overlapping check-ins for one node, half over the UDP loop and
// half over CoAP, all released at once. None may come back refused: a
// collision in the store's read-then-write (NoteCheckIn) would reach a node as
// an #err. Run under -race.
func TestOverlappingCheckInsDoNotCollide(t *testing.T) {
	rg := harness(t)
	rg.st.Write("n1", "goal/name", cval.KindCell, mustEnc(t, "alpha"), "david", "")

	udpPC, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	udpDone := make(chan error, 1)
	go func() { udpDone <- rg.ex.Serve(ctx, udpPC) }()
	t.Cleanup(func() { cancel(); <-udpDone })
	udpAddr := udpPC.LocalAddr().String()

	req, err := wire.EncodePull("n1", 0, 1, 0, 0)
	if err != nil {
		t.Fatal(err)
	}

	const each = 40
	start := make(chan struct{})
	replies := make(chan []byte, 2*each)
	failures := make(chan string, 2*each)
	var wg sync.WaitGroup
	for i := 0; i < each; i++ {
		wg.Add(2)
		go func() { // plain UDP
			defer wg.Done()
			conn, err := net.Dial("udp", udpAddr)
			if err != nil {
				failures <- "udp dial: " + err.Error()
				return
			}
			defer conn.Close()
			<-start
			if _, err := conn.Write(req); err != nil {
				failures <- "udp write: " + err.Error()
				return
			}
			conn.SetReadDeadline(time.Now().Add(2 * time.Second))
			buf := make([]byte, 1500)
			n, err := conn.Read(buf)
			if err != nil {
				failures <- "udp read: " + err.Error()
				return
			}
			replies <- buf[:n]
		}()
		c := dial(t, "127.0.0.1", rg.port, false, blockwise.SZX256)
		go func() { // CoAP
			defer wg.Done()
			<-start
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			resp, err := c.Post(ctx, "/space", message.AppCBOR, bytes.NewReader(req))
			if err != nil {
				failures <- "coap post: " + err.Error()
				return
			}
			if resp.Code() != codes.Changed {
				failures <- "coap code: " + resp.Code().String()
				return
			}
			b, err := resp.ReadBody()
			if err != nil {
				failures <- "coap body: " + err.Error()
				return
			}
			replies <- b
		}()
	}
	close(start)
	wg.Wait()
	close(replies)
	close(failures)
	for f := range failures {
		t.Errorf("%s", f)
	}
	got := 0
	for b := range replies {
		got++
		v, err := cval.Decode(b)
		if err != nil {
			t.Errorf("reply not CBOR: %v", err)
			continue
		}
		if r, ok := v.(cval.Record); ok && r.Tag == "err" {
			t.Errorf("check-in refused: %v", r.Fields)
		}
	}
	if got != 2*each {
		t.Errorf("replies = %d, want %d", got, 2*each)
	}
}
