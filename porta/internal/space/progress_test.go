// Copyright (c) 2026 Ekorau LLC

// internal/space/progress_test.go
package space

import (
	"database/sql"
	"testing"
)

// sampleOf reads the sample the way Sweep does. ok is false while the node has
// never sent a counted pull.
func sampleOf(t *testing.T, st *Store, node string) (Sample, bool) {
	t.Helper()
	var w, at sql.NullInt64
	if err := st.DB().QueryRow(`SELECT wak, wak_ms FROM nodes WHERE id = ?`, node).Scan(&w, &at); err != nil {
		t.Fatalf("read sample: %v", err)
	}
	return Sample{Wak: w.Int64, AtMs: at.Int64}, w.Valid && at.Valid
}

// The sample is the pull's own count and the moment porta heard it.
func TestNoteCheckInStoresTheCountAndWhenItArrived(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	if _, _, err := st.NoteCheckIn("n1", 0, 0, 41, 7); err != nil {
		t.Fatal(err)
	}
	clock = 12_000
	if _, _, err := st.NoteCheckIn("n1", 0, 0, 41, 8); err != nil {
		t.Fatal(err)
	}
	if got, ok := sampleOf(t, st, "n1"); !ok || got != (Sample{Wak: 8, AtMs: 12_000}) {
		t.Fatalf("sample = %+v ok=%v, want wak 8 at 12000", got, ok)
	}
}

// A push stamps last_seen and must NOT touch the sample: the count rides the
// pull alone (D3.3).
func TestRecordExchangeLeavesTheSampleAlone(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	if err := st.RecordExchange("n1", 41, 0, 0, 0, 0, ""); err != nil {
		t.Fatal(err)
	}
	if _, ok := sampleOf(t, st, "n1"); ok {
		t.Fatal("a bare exchange produced a sample")
	}
}

func TestMarkIsAbsentUntilSetAndGoneWhenForgotten(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	if _, _, err := st.NoteCheckIn("n1", 0, 0, 41, 3); err != nil {
		t.Fatal(err)
	}
	if _, ok, err := st.MarkOf("n1"); err != nil || ok {
		t.Fatalf("fresh node: ok=%v err=%v, want no mark", ok, err)
	}
	want := Mark{Sample: Sample{Wak: 3, AtMs: 10_000}, CadMs: 2_000}
	if err := st.SetMark("n1", want); err != nil {
		t.Fatal(err)
	}
	if got, ok, err := st.MarkOf("n1"); err != nil || !ok || got != want {
		t.Fatalf("MarkOf = %+v ok=%v err=%v, want %+v", got, ok, err, want)
	}
	if err := st.ForgetMark("n1"); err != nil {
		t.Fatal(err)
	}
	if _, ok, _ := st.MarkOf("n1"); ok {
		t.Fatal("ForgetMark left a mark")
	}
}
