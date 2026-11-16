// Copyright (c) 2026 Ekorau LLC

package coapsrv

import (
	"bytes"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"strings"
	"testing"

	"github.com/davidg238/porta/internal/blockserv"
	"github.com/plgd-dev/go-coap/v3/message"
	"github.com/plgd-dev/go-coap/v3/message/codes"
	"github.com/plgd-dev/go-coap/v3/net/blockwise"
)

func TestMaxSZXIsBlockSize(t *testing.T) {
	if maxSZX.Size() != blockserv.BlockSize {
		t.Fatalf("maxSZX.Size() = %d, blockserv.BlockSize = %d", maxSZX.Size(), blockserv.BlockSize)
	}
}

func enc(t *testing.T, szx blockwise.SZX, num int64, more bool) uint32 {
	t.Helper()
	v, err := blockwise.EncodeBlockOption(szx, num, more)
	if err != nil {
		t.Fatal(err)
	}
	return v
}

func TestWindow(t *testing.T) {
	type want struct {
		off, end int64
		szx      blockwise.SZX
		num      int64
		more     bool
	}
	rows := []struct {
		name  string
		n     int64
		has   bool
		szx   blockwise.SZX
		num   int64
		want  want
		isErr bool
	}{
		{"absent", 600, false, 0, 0, want{0, 256, blockwise.SZX256, 0, true}, false},
		{"last", 600, true, blockwise.SZX256, 2, want{512, 600, blockwise.SZX256, 2, false}, false},
		{"smaller size honoured", 600, true, blockwise.SZX64, 3, want{192, 256, blockwise.SZX64, 3, true}, false},
		{"larger size: same offset, renumbered", 2000, true, blockwise.SZX1024, 1, want{1024, 1280, blockwise.SZX256, 4, true}, false},
		{"exact multiple", 512, true, blockwise.SZX256, 1, want{256, 512, blockwise.SZX256, 1, false}, false},
		{"past end of exact multiple", 512, true, blockwise.SZX256, 2, want{}, true},
		{"empty blob", 0, false, 0, 0, want{0, 0, blockwise.SZX256, 0, false}, false},
		{"empty blob block 1", 0, true, blockwise.SZX256, 1, want{}, true},
		{"BERT", 600, true, blockwise.SZXBERT, 0, want{}, true},
	}
	for _, r := range rows {
		var v uint32
		if r.has {
			v = enc(t, r.szx, r.num, false)
		}
		off, end, reply, err := window(r.n, v, r.has)
		if r.isErr {
			if err == nil {
				t.Errorf("%s: want err, got off=%d end=%d", r.name, off, end)
			}
			continue
		}
		if err != nil {
			t.Errorf("%s: %v", r.name, err)
			continue
		}
		szx, num, more, err := blockwise.DecodeBlockOption(reply)
		if err != nil {
			t.Fatalf("%s: reply %#x does not decode: %v", r.name, reply, err)
		}
		got := want{off, end, szx, num, more}
		if got != r.want {
			t.Errorf("%s: got %+v, want %+v", r.name, got, r.want)
		}
	}
}

func putBlob(t *testing.T, rg *rig, n int) ([]byte, string) {
	t.Helper()
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		t.Fatal(err)
	}
	h, _, err := rg.blobs.Put(bytes.NewReader(b))
	if err != nil {
		t.Fatal(err)
	}
	return b, hex.EncodeToString(h[:])
}

func TestBlobWhole(t *testing.T) {
	rg := harness(t)
	for _, n := range []int{0, 1, 256, 512, 600} {
		_, h := putBlob(t, rg, n)
		for _, szx := range []blockwise.SZX{blockwise.SZX1024, blockwise.SZX16} {
			c := dial(t, "127.0.0.1", rg.port, true, szx)
			resp, err := c.Get(ctx5(t), "/blob/"+h)
			if err != nil {
				t.Fatalf("n=%d szx=%d: GET: %v", n, szx.Size(), err)
			}
			if resp.Code() != codes.Content {
				t.Fatalf("n=%d szx=%d: code %v", n, szx.Size(), resp.Code())
			}
			if cf, err := resp.ContentFormat(); err != nil || cf != message.AppOctets {
				t.Fatalf("n=%d szx=%d: content format %v (%v)", n, szx.Size(), cf, err)
			}
			body := readBody(t, resp)
			sum := sha256.Sum256(body)
			if hex.EncodeToString(sum[:]) != h || len(body) != n {
				t.Fatalf("n=%d szx=%d: got %d bytes, sha256 mismatch=%v", n, szx.Size(), len(body), hex.EncodeToString(sum[:]) != h)
			}
		}
	}
}

// Design §8.2: a fresh socket, no earlier request, asks for block 2.
func TestColdBlock(t *testing.T) {
	rg := harness(t)
	b, h := putBlob(t, rg, 600)
	c := dial(t, "127.0.0.1", rg.port, false, blockwise.SZX256)
	buf := make([]byte, 4)
	n, err := message.EncodeUint32(buf, enc(t, blockwise.SZX256, 2, false))
	if err != nil {
		t.Fatal(err)
	}
	resp, err := c.Get(ctx5(t), "/blob/"+h, message.Option{ID: message.Block2, Value: buf[:n]})
	if err != nil {
		t.Fatal(err)
	}
	if resp.Code() != codes.Content {
		t.Fatalf("code %v", resp.Code())
	}
	if got := readBody(t, resp); !bytes.Equal(got, b[512:600]) {
		t.Fatalf("block 2 = %d bytes, not blob[512:600]", len(got))
	}
	v, err := resp.GetOptionUint32(message.Block2)
	if err != nil {
		t.Fatalf("reply Block2: %v", err)
	}
	_, num, more, err := blockwise.DecodeBlockOption(v)
	if err != nil || num != 2 || more {
		t.Fatalf("reply Block2 num=%d more=%v err=%v, want 2 false", num, more, err)
	}
}

func TestBlobRefusals(t *testing.T) {
	rg := harness(t)
	_, h := putBlob(t, rg, 600)
	c := dial(t, "127.0.0.1", rg.port, false, blockwise.SZX256)
	absent := strings.Repeat("ab", 32)
	get := func(path string, opts ...message.Option) codes.Code {
		t.Helper()
		resp, err := c.Get(ctx5(t), path, opts...)
		if err != nil {
			t.Fatalf("GET %s: %v", path, err)
		}
		return resp.Code()
	}
	for _, r := range []struct {
		path string
		want codes.Code
	}{
		{"/blob/" + absent, codes.NotFound},
		{"/blob/" + strings.ToUpper(h), codes.BadRequest},
		{"/blob/" + h[:63], codes.BadRequest},
		{"/blob/" + h + "/x", codes.NotFound},
	} {
		if got := get(r.path); got != r.want {
			t.Errorf("GET %s = %v, want %v", r.path, got, r.want)
		}
	}
	resp, err := c.Post(ctx5(t), "/blob/"+h, message.AppOctets, bytes.NewReader([]byte{1}))
	if err != nil {
		t.Fatal(err)
	}
	if resp.Code() != codes.MethodNotAllowed {
		t.Errorf("POST /blob = %v, want MethodNotAllowed", resp.Code())
	}
	buf := make([]byte, 4)
	n, _ := message.EncodeUint32(buf, enc(t, blockwise.SZX256, 3, false))
	if got := get("/blob/"+h, message.Option{ID: message.Block2, Value: buf[:n]}); got != codes.BadOption {
		t.Errorf("block past end = %v, want BadOption", got)
	}
}

// Review focus 1 at the handler, not only in window: a library client may
// never open with a large SZX, so this asks SZX1024 block 1 by hand and needs
// the bytes at offset 1024, renumbered as SZX256 block 4.
func TestLargerBlockRenumbered(t *testing.T) {
	rg := harness(t)
	b, h := putBlob(t, rg, 2000)
	c := dial(t, "127.0.0.1", rg.port, false, blockwise.SZX256)
	buf := make([]byte, 4)
	n, err := message.EncodeUint32(buf, enc(t, blockwise.SZX1024, 1, false))
	if err != nil {
		t.Fatal(err)
	}
	resp, err := c.Get(ctx5(t), "/blob/"+h, message.Option{ID: message.Block2, Value: buf[:n]})
	if err != nil {
		t.Fatal(err)
	}
	if resp.Code() != codes.Content {
		t.Fatalf("code %v", resp.Code())
	}
	if got := readBody(t, resp); !bytes.Equal(got, b[1024:1280]) {
		t.Fatalf("got %d bytes, not blob[1024:1280]", len(got))
	}
	v, err := resp.GetOptionUint32(message.Block2)
	if err != nil {
		t.Fatalf("reply Block2: %v", err)
	}
	szx, num, more, err := blockwise.DecodeBlockOption(v)
	if err != nil || szx != blockwise.SZX256 || num != 4 || !more {
		t.Fatalf("reply Block2 szx=%d num=%d more=%v err=%v, want 256/4/true", szx.Size(), num, more, err)
	}
}
