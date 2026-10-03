// Copyright (c) 2026 Ekorau LLC

// internal/space/progress_test.go
package space

import (
	"testing"

	"github.com/davidg238/porta/internal/cval"
)

// pushHealth stores what a node's push leg carries at obs/health, through Ingest
// so the kind pin and the replay guard are the real ones. Only `wak` is set:
// it is the one field the verdict reads.
func pushHealth(t *testing.T, st *Store, node string, boot, seq, wak int64) {
	t.Helper()
	rec, err := cval.Encode(cval.Record{Tag: "Health", Fields: cval.Map{
		{K: cval.Symbol("wak"), V: wak},
	}})
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, _, err := st.Ingest(node, boot, []Entry{
		{Seq: seq, Path: PathHealth, Kind: cval.KindCell, Value: rec},
	}); err != nil {
		t.Fatalf("Ingest obs/health: %v", err)
	}
}

func TestLatestSampleReadsWakAndWhenItLanded(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	if _, ok, err := st.LatestSample("n1"); err != nil || ok {
		t.Fatalf("before any obs/health: ok=%v err=%v, want no sample", ok, err)
	}
	pushHealth(t, st, "n1", 41, 1, 7)
	clock = 12_000
	pushHealth(t, st, "n1", 41, 2, 8)
	got, ok, err := st.LatestSample("n1")
	if err != nil || !ok {
		t.Fatalf("LatestSample: ok=%v err=%v", ok, err)
	}
	if got != (Sample{Wak: 8, AtMs: 12_000}) {
		t.Fatalf("sample = %+v, want wak 8 at 12000", got)
	}
}

// A Health record without an integer wak is not a sample. Not an error: one
// garbled record must not cost the fleet its verdicts (DeclaredLiveness' rule).
func TestLatestSampleWithoutWakIsNoSample(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	rec, _ := cval.Encode(cval.Record{Tag: "Health", Fields: cval.Map{
		{K: cval.Symbol("up"), V: int64(5)},
	}})
	if _, _, err := st.Ingest("n1", 41, []Entry{{Seq: 1, Path: PathHealth, Kind: cval.KindCell, Value: rec}}); err != nil {
		t.Fatal(err)
	}
	if _, ok, err := st.LatestSample("n1"); err != nil || ok {
		t.Fatalf("ok=%v err=%v, want no sample", ok, err)
	}
}

// Review Focus 5: a store written by the pre-#272 binary has NULL mark columns,
// and NULL is "no mark" — the first sweep then answers #unknown once.
func TestMarkIsAbsentUntilSet(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	pushHealth(t, st, "n1", 41, 1, 3) // creates the nodes row
	if _, ok, err := st.MarkOf("n1"); err != nil || ok {
		t.Fatalf("fresh node: ok=%v err=%v, want no mark", ok, err)
	}
	want := Mark{Sample: Sample{Wak: 3, AtMs: 10_000}, CadMs: 2_000}
	if err := st.SetMark("n1", want); err != nil {
		t.Fatalf("SetMark: %v", err)
	}
	got, ok, err := st.MarkOf("n1")
	if err != nil || !ok || got != want {
		t.Fatalf("MarkOf = %+v ok=%v err=%v, want %+v", got, ok, err, want)
	}
}
