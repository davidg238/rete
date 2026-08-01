// Copyright (c) 2026 Ekorau LLC

// internal/space/store_test.go
package space

import (
	"testing"

	"github.com/davidg238/porta/internal/cval"
)

func testStore(t *testing.T) *Store {
	t.Helper()
	var clock int64 = 1_000
	st, err := Open(t.TempDir()+"/t.db", func() int64 { clock += 10; return clock })
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	t.Cleanup(func() { st.Close() })
	return st
}

func enc(t *testing.T, v any) []byte {
	t.Helper()
	b, err := cval.Encode(v)
	if err != nil {
		t.Fatalf("Encode: %v", err)
	}
	return b
}

// The outbound seq is per-node and monotonic (design D5).
func TestWriteAssignsMonotonicSeqPerNode(t *testing.T) {
	st := testStore(t)
	s1, _ := st.Write("n1", "goal/name", cval.KindCell, enc(t, "alpha"), "david", "")
	s2, _ := st.Write("n1", "goal/mode", cval.KindCell, enc(t, "fast"), "david", "")
	s3, _ := st.Write("n2", "goal/name", cval.KindCell, enc(t, "beta"), "david", "")
	if s1 != 1 || s2 != 2 {
		t.Fatalf("n1 seqs = %d,%d want 1,2", s1, s2)
	}
	if s3 != 1 {
		t.Fatalf("n2 seq = %d want 1 (per-node counter)", s3)
	}
}

// A superseded cell write is never delivered: the pull carries the CURRENT
// value only. This is what bounds a cursor-0 replay by tree size rather than
// by history length.
func TestOutboundCollapsesSupersededCellWrites(t *testing.T) {
	st := testStore(t)
	st.Write("n1", "goal/name", cval.KindCell, enc(t, "first"), "david", "")
	st.Write("n1", "goal/name", cval.KindCell, enc(t, "second"), "david", "")
	got, hi, err := st.OutboundSince("n1", 0, MaxBatchBytes)
	if err != nil {
		t.Fatalf("OutboundSince: %v", err)
	}
	if len(got) != 1 {
		t.Fatalf("got %d entries, want 1 (superseded write must not ride)", len(got))
	}
	v, _ := cval.Decode(got[0].Value)
	if v != "second" {
		t.Fatalf("got %v, want \"second\"", v)
	}
	if hi != 2 {
		t.Fatalf("hi = %d, want 2", hi)
	}
}

// Stream entries are delivered once each, in order — never collapsed.
func TestOutboundKeepsEveryStreamEntry(t *testing.T) {
	st := testStore(t)
	st.Write("n1", "goal/do/reboot", cval.KindStream, enc(t, cval.Record{Tag: "Reboot"}), "david", "")
	st.Write("n1", "goal/do/reboot", cval.KindStream, enc(t, cval.Record{Tag: "Reboot"}), "david", "")
	got, _, _ := st.OutboundSince("n1", 0, MaxBatchBytes)
	if len(got) != 2 {
		t.Fatalf("got %d stream entries, want 2", len(got))
	}
}

func TestOutboundRespectsCursor(t *testing.T) {
	st := testStore(t)
	st.Write("n1", "goal/a", cval.KindCell, enc(t, int64(1)), "david", "")
	st.Write("n1", "goal/b", cval.KindCell, enc(t, int64(2)), "david", "")
	got, _, _ := st.OutboundSince("n1", 1, MaxBatchBytes)
	if len(got) != 1 || got[0].Path != "goal/b" {
		t.Fatalf("got %+v, want only goal/b", got)
	}
}

// The batch cap is a radio constraint, not a preference (RECORDS §2.2).
func TestOutboundStopsAtBatchCap(t *testing.T) {
	st := testStore(t)
	big := enc(t, string(make([]byte, 200)))
	for _, p := range []string{"goal/a", "goal/b", "goal/c", "goal/d"} {
		st.Write("n1", p, cval.KindCell, big, "david", "")
	}
	got, hi, _ := st.OutboundSince("n1", 0, MaxBatchBytes)
	if len(got) >= 4 {
		t.Fatalf("got %d entries, want a partial batch under the cap", len(got))
	}
	if hi != got[len(got)-1].Seq {
		t.Fatalf("hi = %d, want the last delivered seq %d", hi, got[len(got)-1].Seq)
	}
}

// THE ECHO GUARD. cells holds BOTH directions — it has to, because the
// operator's goal/X vs obs/X diff is one query over one table. But that means
// an unfiltered "SELECT FROM cells WHERE seq > cursor" hands the node back its
// own obs/** cells, stamped with seqs from the node's push counter rather than
// porta's out_seq — so the reply corrupts the very cursor it answers.
func TestOutboundNeverEchoesIngestedCells(t *testing.T) {
	st := testStore(t)
	st.Write("n1", "goal/name", cval.KindCell, enc(t, "alpha"), "david", "")
	if _, err := st.Ingest("n1", 7, []Entry{
		{Seq: 1, Path: "obs/name", Kind: cval.KindCell, Value: enc(t, "alpha")},
		{Seq: 2, Path: "obs/health", Kind: cval.KindCell, Value: enc(t, int64(99))},
	}); err != nil {
		t.Fatalf("Ingest: %v", err)
	}
	got, _, err := st.OutboundSince("n1", 0, MaxBatchBytes)
	if err != nil {
		t.Fatalf("OutboundSince: %v", err)
	}
	for _, e := range got {
		if e.Path != "goal/name" {
			t.Fatalf("outbound batch carries %q, which the node itself pushed", e.Path)
		}
	}
	if len(got) != 1 {
		t.Fatalf("got %d entries, want exactly the 1 porta wrote", len(got))
	}
	// Both directions must still be visible to the operator's diff.
	if tree, _ := st.Tree("n1", ""); len(tree) != 3 {
		t.Fatalf("tree has %d rows, want 3 (goal/name + both obs cells)", len(tree))
	}
}

// A push replayed after loss must not double-apply (dedup on high-water).
func TestIngestDedupesOnHighWater(t *testing.T) {
	st := testStore(t)
	batch := []Entry{
		{Seq: 1, Path: "tel/log", Kind: cval.KindStream, Value: enc(t, "one")},
		{Seq: 2, Path: "tel/log", Kind: cval.KindStream, Value: enc(t, "two")},
	}
	hw, err := st.Ingest("n1", 7, batch)
	if err != nil || hw != 2 {
		t.Fatalf("Ingest = %d, %v; want 2, nil", hw, err)
	}
	if hw, _ = st.Ingest("n1", 7, batch); hw != 2 {
		t.Fatalf("replayed Ingest = %d, want 2", hw)
	}
	evs, _ := st.Events("n1", 100)
	if len(evs) != 2 {
		t.Fatalf("got %d events after replay, want 2", len(evs))
	}
}

// Provenance is the thing v1's issued_by carried and §5.1 forgot (design D3).
func TestEventsCarryProvenance(t *testing.T) {
	st := testStore(t)
	st.Write("n1", "goal/name", cval.KindCell, enc(t, "alpha"), "agent-7", "rollout")
	evs, _ := st.Events("n1", 10)
	if len(evs) != 1 {
		t.Fatalf("got %d events, want 1", len(evs))
	}
	if evs[0].Actor != "agent-7" || evs[0].Reason != "rollout" || evs[0].Direction != "out" {
		t.Fatalf("provenance lost: %+v", evs[0])
	}
}

// Superseded intent survives in the log even though the cell shows only the
// latest value — the property a drained command queue could not offer.
func TestSupersededIntentSurvivesInTheLog(t *testing.T) {
	st := testStore(t)
	st.Write("n1", "goal/mode", cval.KindCell, enc(t, "X"), "david", "")
	st.Write("n1", "goal/mode", cval.KindCell, enc(t, "Y"), "david", "")
	tree, _ := st.Tree("n1", "goal/")
	if len(tree) != 1 {
		t.Fatalf("tree has %d rows, want 1", len(tree))
	}
	evs, _ := st.Events("n1", 10)
	if len(evs) != 2 {
		t.Fatalf("log has %d rows, want 2 (X must still be visible)", len(evs))
	}
}

func TestOversizeValueRejected(t *testing.T) {
	st := testStore(t)
	_, err := st.Write("n1", "goal/x", cval.KindCell, make([]byte, MaxValueBytes+1), "david", "")
	if err == nil {
		t.Fatal("oversize value accepted; the 256 B cap is a radio constraint")
	}
}

func TestExchangeRowRecorded(t *testing.T) {
	st := testStore(t)
	if err := st.RecordExchange("n1", 7, 0, 3, 1, 2, "fdde:ad00::1"); err != nil {
		t.Fatalf("RecordExchange: %v", err)
	}
	if n, _ := st.CountNodes(); n != 1 {
		t.Fatalf("CountNodes = %d, want 1", n)
	}
}

// The peer address is v1's nodes.source_addr, which the capability audit
// (design §4.2) caught with no successor. serverstat.TransportOf() reads it to
// produce the wifi-vs-Thread breakdown, so losing it loses a fleet view, not a
// display field.
func TestExchangeRecordsPeerAddress(t *testing.T) {
	st := testStore(t)
	st.RecordExchange("n1", 7, 0, 3, 1, 2, "fdde:ad00::1")
	got, err := st.NodeAddr("n1")
	if err != nil {
		t.Fatalf("NodeAddr: %v", err)
	}
	if got != "fdde:ad00::1" {
		t.Fatalf("NodeAddr = %q, want fdde:ad00::1", got)
	}
}

// An exchange recorded without a peer address must not erase a known one.
func TestEmptyPeerAddressDoesNotClobber(t *testing.T) {
	st := testStore(t)
	st.RecordExchange("n1", 7, 0, 3, 1, 2, "fdde:ad00::1")
	st.RecordExchange("n1", 7, 3, 4, 0, 0, "")
	if got, _ := st.NodeAddr("n1"); got != "fdde:ad00::1" {
		t.Fatalf("NodeAddr = %q after an addr-less exchange, want the address kept", got)
	}
}
