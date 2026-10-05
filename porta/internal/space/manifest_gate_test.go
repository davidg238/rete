// Copyright (c) 2026 Ekorau LLC

// rete#243: the manifest gate at the store's three write doors. The real
// protocol/MANIFEST.cddl is the fixture — these tests are also the proof
// that the shipped manifest admits what the system actually writes.
package space

import (
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/davidg238/rete/protocol/cval"
	"github.com/davidg238/rete/protocol/manifest"
	"github.com/davidg238/rete/protocol/store"
)

func loadRealManifest(t *testing.T) *manifest.Table {
	t.Helper()
	_, this, _, _ := runtime.Caller(0)
	p := filepath.Join(filepath.Dir(this), "..", "..", "..", "protocol", "MANIFEST.cddl")
	tbl, err := manifest.Load(p)
	if err != nil {
		t.Fatal(err)
	}
	return tbl
}

func TestManifestGateOperatorDoor(t *testing.T) {
	s := testStore(t)
	s.SetManifest(loadRealManifest(t))
	v := enc(t, "bell")
	// wrong kind at a manifest path: refused with the named check
	if _, err := s.Write("n1", "goal/name", cval.KindStream, v, "op", "test"); err == nil ||
		!strings.HasPrefix(err.Error(), "manifest:kind") {
		t.Fatalf("want manifest:kind, got %v", err)
	}
	// unknown path: refused
	if _, err := s.Write("n1", "goal/bogus", cval.KindCell, v, "op", "test"); err == nil ||
		!strings.HasPrefix(err.Error(), "manifest:path") {
		t.Fatalf("want manifest:path, got %v", err)
	}
	// clean write passes
	if _, err := s.Write("n1", "goal/name", cval.KindCell, v, "op", "test"); err != nil {
		t.Fatal(err)
	}
}

func TestManifestGateWireDoor(t *testing.T) {
	s := testStore(t)
	s.SetManifest(loadRealManifest(t))
	health := enc(t, cval.Record{Tag: "Health", Fields: cval.Map{{K: cval.Symbol("up"), V: int64(1)}}})
	bad := enc(t, cval.Record{Tag: "Metrics"}) // wrong record at obs/health
	hw, skipped, err := s.Ingest("n1", 7, []Entry{
		{Seq: 1, Path: "obs/health", Kind: cval.KindCell, Value: health},
		{Seq: 2, Path: "obs/health", Kind: cval.KindCell, Value: bad},
		{Seq: 3, Path: "obs/health", Kind: cval.KindCell, Value: health},
	})
	if err != nil {
		t.Fatal(err)
	}
	if hw != 3 || skipped != 0 {
		t.Fatalf("refused entry must still ack: hw=%d skipped=%d", hw, skipped)
	}
	// the refused bytes are KEPT: an events row with actor=gate naming the check
	evs, err := s.Events("n1", 10, 0)
	if err != nil {
		t.Fatal(err)
	}
	var gate int
	for _, e := range evs {
		if e.Actor == "gate" && strings.HasPrefix(e.Reason, "manifest:head") {
			gate++
		}
	}
	if gate != 1 {
		t.Fatalf("want exactly one actor=gate refusal row, got %d in %+v", gate, evs)
	}
	// and the cell tree holds the LAST GOOD value, not the refused one
	cells, err := s.Tree("n1", "obs/health")
	if err != nil {
		t.Fatal(err)
	}
	if len(cells) != 1 || cells[0].Seq != 3 {
		t.Fatalf("cell tree took the refused write: %+v", cells)
	}
}

func TestManifestGateDirection(t *testing.T) {
	s := testStore(t)
	s.SetManifest(loadRealManifest(t))
	v := enc(t, "x")
	// a node writing goal/** is refused-but-kept at the wire door
	if _, _, err := s.Ingest("n1", 1, []Entry{
		{Seq: 1, Path: "goal/name", Kind: cval.KindCell, Value: v}}); err != nil {
		t.Fatal(err)
	}
	evs, err := s.Events("n1", 10, 0)
	if err != nil {
		t.Fatal(err)
	}
	if len(evs) != 1 || evs[0].Actor != "gate" || !strings.HasPrefix(evs[0].Reason, "manifest:dir") {
		t.Fatalf("node→goal must be a gate row: %+v", evs)
	}
}

func TestManifestGateGateway(t *testing.T) {
	s := testStore(t)
	s.SetManifest(loadRealManifest(t))
	live := enc(t, cval.Symbol("live"))
	if err := s.WriteGateway("n1", store.PathLiveness, live, "sweep"); err != nil {
		t.Fatalf("gw/liveness refused: %v", err)
	}
	if err := s.WriteGateway("n1", "gw/bogus", live, "sweep"); err == nil {
		t.Fatal("unknown gw path accepted")
	}
}

func TestStoredManifestArmsEveryOpen(t *testing.T) {
	// The persistence contract: StoreManifest in one Store, and a SECOND
	// process's Open of the same db is gated — the CLI-door case that an
	// in-memory-only arm would miss (#180/#206 both came through `put`).
	dir := t.TempDir()
	path := dir + "/t.db"
	st, err := Open(path, func() int64 { return 1000 })
	if err != nil {
		t.Fatal(err)
	}
	if err := st.StoreManifest(loadRealManifest(t).Raw); err != nil {
		t.Fatal(err)
	}
	st.Close()
	st2, err := Open(path, func() int64 { return 2000 })
	if err != nil {
		t.Fatal(err)
	}
	defer st2.Close()
	v := enc(t, "bell")
	if _, err := st2.Write("n1", "goal/name", cval.KindStream, v, "op", "t"); err == nil ||
		!strings.HasPrefix(err.Error(), "manifest:kind") {
		t.Fatalf("reopened store not gated: %v", err)
	}
}

func TestNoManifestNoGate(t *testing.T) {
	s := testStore(t) // no SetManifest: every pre-manifest caller keeps working
	v := enc(t, "x")
	if _, err := s.Write("n1", "goal/anything", cval.KindCell, v, "op", "t"); err != nil {
		t.Fatal(err)
	}
}

// rete#218, closed by the #243 gate rather than a second namespace check: a
// node pushing gw/liveness must NOT overwrite the gateway's own verdict.
// gw/** is dir=gw in the manifest, the wire door checks every push as dir=up,
// so the forged entry is refused-but-kept (acked, actor=gate) and the sweep's
// cell survives with its direction intact. The residue — an UNGATED store
// still applies the forgery — is the flagless-serve deferral recorded on #258,
// and TestNoManifestNoGate above is honest about that world.
func TestIngestRefusesAForgedGatewayVerdict(t *testing.T) {
	s := testStore(t)
	s.SetManifest(loadRealManifest(t))
	silent := enc(t, cval.Symbol("silent"))
	forged := enc(t, cval.Symbol("live"))
	if err := s.WriteGateway("n1", store.PathLiveness, silent, "silence sweep"); err != nil {
		t.Fatal(err)
	}
	hw, _, err := s.Ingest("n1", 9, []Entry{
		{Seq: 1, Path: store.PathLiveness, Kind: cval.KindCell, Value: forged}})
	if err != nil {
		t.Fatal(err)
	}
	if hw != 1 {
		t.Fatalf("refused forgery must still ack so the node advances: hw=%d", hw)
	}
	cells, err := s.Tree("n1", "gw/")
	if err != nil {
		t.Fatal(err)
	}
	if len(cells) != 1 || string(cells[0].Value) != string(silent) ||
		cells[0].Direction != store.DirectionGateway {
		t.Fatalf("the node overwrote the gateway's verdict: %+v", cells)
	}
	evs, err := s.Events("n1", 10, 0)
	if err != nil {
		t.Fatal(err)
	}
	var gate int
	for _, e := range evs {
		if e.Actor == "gate" && strings.HasPrefix(e.Reason, "manifest:dir") {
			gate++
		}
	}
	if gate != 1 {
		t.Fatalf("want one actor=gate manifest:dir row for the forgery, got %d", gate)
	}
}
