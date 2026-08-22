// Copyright (c) 2026 Ekorau LLC

// internal/cval/cval_test.go
package cval

import (
	"bytes"
	"encoding/hex"
	"testing"
)

// The node's own golden, from nsl-tests/14_cbor/02_golden_hex.nsl:
//
//	Map with: #n -> 42 with: #s -> 'hi'
//
// Symbols are tag 39; map keys sort bytewise over ENCODED key bytes.
func TestEncodeNodeGolden(t *testing.T) {
	m := Map{{Symbol("s"), "hi"}, {Symbol("n"), int64(42)}} // deliberately unsorted on input
	got, err := Encode(m)
	if err != nil {
		t.Fatalf("Encode: %v", err)
	}
	want, _ := hex.DecodeString("a2d827616e182ad8276173626869")
	if !bytes.Equal(got, want) {
		t.Fatalf("golden mismatch\n got %x\nwant %x", got, want)
	}
}

// A Record is tag 27 -> array(2) -> [Symbol, Map]. The NAME IS A SYMBOL,
// not a text string — this is where a standard tag-27 implementation diverges.
func TestEncodeRecordNameIsSymbol(t *testing.T) {
	r := Record{Tag: "ack", Fields: Map{{Symbol("hw"), int64(5)}}}
	got, err := Encode(r)
	if err != nil {
		t.Fatalf("Encode: %v", err)
	}
	want, _ := hex.DecodeString("d81b82d8276361636ba1d827626877" + "05")
	if !bytes.Equal(got, want) {
		t.Fatalf("record mismatch\n got %x\nwant %x", got, want)
	}
}

func TestRoundTrip(t *testing.T) {
	orig := Record{Tag: "reading", Fields: Map{
		{Symbol("id"), int64(4294967296)},
		{Symbol("name"), "sensor"},
		{Symbol("ok"), true},
		{Symbol("xs"), List{int64(1), int64(2), int64(3)}},
		{Symbol("raw"), Bytes{0xde, 0xad}},
		{Symbol("none"), nil},
	}}
	enc, err := Encode(orig)
	if err != nil {
		t.Fatalf("Encode: %v", err)
	}
	back, err := Decode(enc)
	if err != nil {
		t.Fatalf("Decode: %v", err)
	}
	re, err := Encode(back)
	if err != nil {
		t.Fatalf("re-Encode: %v", err)
	}
	if !bytes.Equal(enc, re) {
		t.Fatalf("not byte-stable\n first %x\nsecond %x", enc, re)
	}
}

func TestDecodeIsClosed(t *testing.T) {
	cases := map[string]struct {
		hex  string
		want error
	}{
		"unknown tag 42":  {"d82a01", ErrNotAValue},
		"indefinite text": {"7f6161ff", ErrNotAValue},
		"trailing bytes":  {"0101", ErrTrailing},
		"bad utf-8":       {"62c328", ErrUTF8},
	}
	for name, tc := range cases {
		t.Run(name, func(t *testing.T) {
			b, _ := hex.DecodeString(tc.hex)
			if _, err := Decode(b); err != tc.want {
				t.Fatalf("Decode(%s) = %v, want %v", tc.hex, err, tc.want)
			}
		})
	}
}

func TestDepthBound(t *testing.T) {
	v := any(int64(1))
	for i := 0; i < 40; i++ {
		v = List{v}
	}
	if _, err := Encode(v); err != ErrDepth {
		t.Fatalf("Encode deep = %v, want ErrDepth", err)
	}
}

// The L7 band is ±(2^64−1), which does not fit int64. It must survive exactly.
func TestIntegerBandEdges(t *testing.T) {
	for _, v := range []any{Uint(1<<64 - 1), NegUint(1<<64 - 1), int64(-1), int64(0)} {
		enc, err := Encode(v)
		if err != nil {
			t.Fatalf("Encode(%v): %v", v, err)
		}
		back, err := Decode(enc)
		if err != nil {
			t.Fatalf("Decode(%x): %v", enc, err)
		}
		if re, _ := Encode(back); !bytes.Equal(enc, re) {
			t.Fatalf("band value %v not stable: %x vs %x", v, enc, re)
		}
	}
}

// The hex nodus/tuvm/tests/porta_sync_host.sh writes to goal/liveness. A gate
// that pastes a constant nothing checks is a gate that keeps passing after the
// record's shape changes underneath it, so the constant is pinned HERE, where a
// shape change breaks a test instead of quietly writing a record the node will
// log as bad.
func TestGoalLivenessGoldenForTheHostGate(t *testing.T) {
	b, err := Encode(Record{Tag: "Liveness", Fields: Map{
		{K: Symbol("cad"), V: int64(2_000)},
		{K: Symbol("bud"), V: int64(45_000)},
	}})
	if err != nil {
		t.Fatalf("Encode: %v", err)
	}
	const want = "d81b82d827684c6976656e657373a2d8276362756419afc8d827636361641907d0"
	if got := hex.EncodeToString(b); got != want {
		t.Fatalf("goal/liveness golden = %s, want %s\n(if the record's shape changed on purpose, update BOTH this constant and the hex in nodus/tuvm/tests/porta_sync_host.sh)", got, want)
	}
}
