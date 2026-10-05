// Copyright (c) 2026 Ekorau LLC

package manifest

import (
	"bytes"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/davidg238/rete/protocol/cval"
)

func load(t *testing.T) *Table {
	t.Helper()
	_, this, _, _ := runtime.Caller(0)
	p := filepath.Join(filepath.Dir(this), "..", "MANIFEST.cddl")
	tbl, err := Load(p)
	if err != nil {
		t.Fatal(err)
	}
	return tbl
}

func encRecord(t *testing.T, name string, fields cval.Map) []byte {
	t.Helper()
	b, err := cval.Encode(cval.Record{Tag: cval.Symbol(name), Fields: fields})
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func TestHealthAccepted(t *testing.T) {
	tbl := load(t)
	v := encRecord(t, "Health", cval.Map{{K: cval.Symbol("up"), V: int64(5)}})
	if err := tbl.Check("obs/health", cval.KindCell, DirUp, v); err != nil {
		t.Fatalf("well-formed Health refused: %v", err)
	}
}

func TestKindMismatchRefused(t *testing.T) {
	tbl := load(t)
	v := encRecord(t, "Health", nil)
	err := tbl.Check("obs/health", cval.KindStream, DirUp, v)
	if err == nil || !strings.HasPrefix(err.Error(), "manifest:kind") {
		t.Fatalf("want manifest:kind error, got %v", err)
	}
}

func TestWrongRecordAtPathRefused(t *testing.T) {
	tbl := load(t)
	v := encRecord(t, "Metrics", nil)
	err := tbl.Check("obs/health", cval.KindCell, DirUp, v)
	if err == nil || !strings.HasPrefix(err.Error(), "manifest:head") {
		t.Fatalf("want manifest:head error, got %v", err)
	}
}

func TestUnknownPathRefused(t *testing.T) {
	tbl := load(t)
	err := tbl.Check("bogus/path", cval.KindCell, DirUp, []byte{0x01})
	if err == nil || !strings.HasPrefix(err.Error(), "manifest:path") {
		t.Fatalf("want manifest:path error, got %v", err)
	}
}

func TestDirectionRefused(t *testing.T) {
	tbl := load(t)
	v := encRecord(t, "Health", nil)
	err := tbl.Check("obs/health", cval.KindCell, DirDown, v)
	if err == nil || !strings.HasPrefix(err.Error(), "manifest:dir") {
		t.Fatalf("want manifest:dir error, got %v", err)
	}
}

func TestLiteralShadowsWildcard(t *testing.T) {
	tbl := load(t)
	ctl := encRecord(t, "SpanCtl", cval.Map{{K: cval.Symbol("state"), V: cval.Symbol("running")}})
	if err := tbl.Check("diag/spans/ctl", cval.KindCell, DirUp, ctl); err != nil {
		t.Fatalf("ctl literal not shadowing wildcard: %v", err)
	}
	span := encRecord(t, "LatencySpan", nil)
	if err := tbl.Check("diag/spans/pullSend", cval.KindCell, DirUp, span); err != nil {
		t.Fatalf("wildcard row broken: %v", err)
	}
}

func TestWildcardIsOneSegment(t *testing.T) {
	tbl := load(t)
	v, _ := cval.Encode(int64(42))
	if err := tbl.Check("goal/config/app/key", cval.KindCell, DirDown, v); err != nil {
		t.Fatalf("two-wildcard row broken: %v", err)
	}
	if err := tbl.Check("goal/config/app", cval.KindCell, DirDown, v); err == nil {
		t.Fatal("short path matched a longer pattern")
	}
	if err := tbl.Check("goal/config/app/key/extra", cval.KindCell, DirDown, v); err == nil {
		t.Fatal("long path matched a shorter pattern")
	}
}

func TestScalarRow(t *testing.T) {
	tbl := load(t)
	s, _ := cval.Encode("bell") // tstr at goal/name
	if err := tbl.Check("goal/name", cval.KindCell, DirDown, s); err != nil {
		t.Fatalf("tstr at goal/name refused: %v", err)
	}
	b, _ := cval.Encode(cval.Bytes{1, 2})
	if err := tbl.Check("goal/name", cval.KindCell, DirDown, b); err == nil {
		t.Fatal("bytes at goal/name accepted")
	}
}

func TestGwDoor(t *testing.T) {
	tbl := load(t)
	v, _ := cval.Encode(cval.Symbol("live"))
	if err := tbl.Check("gw/liveness", cval.KindCell, DirGw, v); err != nil {
		t.Fatalf("gw/liveness refused: %v", err)
	}
	if err := tbl.Check("gw/liveness", cval.KindCell, DirUp, v); err == nil {
		t.Fatal("a node writing gw/liveness must be refused")
	}
}

func TestNilTableIsOpen(t *testing.T) {
	var tbl *Table
	if err := tbl.Check("anything", cval.KindCell, DirUp, nil); err != nil {
		t.Fatal("nil table must gate nothing")
	}
}

func TestRecordHeadDerivation(t *testing.T) {
	// The expected head is the record's encoding minus the trailing empty
	// map (0xa0): d8 1b (tag 27) 82 (array 2) d8 27 (tag 39) 66 "Health".
	tbl := load(t)
	e := tbl.match("obs/health")
	if e == nil {
		t.Fatal("obs/health missing")
	}
	want := []byte{0xd8, 0x1b, 0x82, 0xd8, 0x27, 0x66, 'H', 'e', 'a', 'l', 't', 'h'}
	if !bytes.Equal(e.Head, want) {
		t.Fatalf("Health head = %x, want %x", e.Head, want)
	}
}

func TestSHA256AndRawFilled(t *testing.T) {
	tbl := load(t)
	if len(tbl.Raw) == 0 || len(tbl.SHA256) != 64 {
		t.Fatalf("Raw/SHA256 not filled: %d bytes, %q", len(tbl.Raw), tbl.SHA256)
	}
}
