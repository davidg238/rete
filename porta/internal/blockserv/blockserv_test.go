// Copyright (c) 2026 Ekorau LLC
package blockserv

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"testing"

	"github.com/davidg238/porta/internal/blob"
)

func srv(t *testing.T, body []byte) (*Server, [32]byte) {
	st, err := blob.Open(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	h, _, err := st.Put(bytes.NewReader(body))
	if err != nil {
		t.Fatal(err)
	}
	return New(st), h
}

func req(hash [32]byte, blk uint32) []byte {
	b := make([]byte, 37)
	b[0] = 0x01
	copy(b[1:33], hash[:])
	binary.BigEndian.PutUint32(b[33:], blk)
	return b
}

func TestMiddleAndLastBlock(t *testing.T) {
	body := make([]byte, 600) // 3 blocks: 256, 256, 88
	for i := range body {
		body[i] = byte(i)
	}
	s, h := srv(t, body)
	r1 := s.Handle(req(h, 1))
	if r1[0] != 0x02 || binary.BigEndian.Uint32(r1[1:5]) != 1 || r1[5] != 1 {
		t.Fatalf("block 1 header wrong: % x", r1[:6])
	}
	if !bytes.Equal(r1[6:], body[256:512]) {
		t.Fatal("block 1 payload wrong")
	}
	r2 := s.Handle(req(h, 2))
	if r2[5] != 0 || len(r2[6:]) != 88 {
		t.Fatalf("last block: more=%d len=%d", r2[5], len(r2[6:]))
	}
}

func TestUnknownHashAndMalformed(t *testing.T) {
	s, _ := srv(t, []byte("x"))
	if r := s.Handle(req(sha256.Sum256([]byte("other")), 0)); r[0] != 0x7F {
		t.Fatal("unknown hash must answer 0x7F")
	}
	if r := s.Handle([]byte{0x01, 0x02}); r[0] != 0x7F {
		t.Fatal("short frame must answer 0x7F")
	}
	s2, h := srv(t, []byte("x"))
	if r := s2.Handle(req(h, 9)); r[0] != 0x7F {
		t.Fatal("block past end must answer 0x7F")
	}
}
