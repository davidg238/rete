// Copyright (c) 2026 Ekorau LLC

// internal/space/sweep_test.go
package space

import (
	"bytes"
	"sort"
	"strings"
	"testing"

	"github.com/davidg238/porta/internal/cval"
)

// livenessOf reads the stored verdict back as a string, so a test failure names
// the verdict rather than a byte slice.
func livenessOf(t *testing.T, st *Store, node string) string {
	t.Helper()
	tree, err := st.Tree(node, PathLiveness)
	if err != nil {
		t.Fatalf("Tree: %v", err)
	}
	if len(tree) == 0 {
		return ""
	}
	v, err := cval.Decode(tree[0].Value)
	if err != nil {
		t.Fatalf("decode verdict: %v", err)
	}
	sym, ok := v.(cval.Symbol)
	if !ok {
		t.Fatalf("verdict is %T, want a Symbol", v)
	}
	return string(sym)
}

func livenessRows(t *testing.T, st *Store, node string) int {
	t.Helper()
	var n int
	if err := st.DB().QueryRow(
		`SELECT COUNT(*) FROM events WHERE node_id = ? AND path = ?`, node, PathLiveness).Scan(&n); err != nil {
		t.Fatalf("count liveness events: %v", err)
	}
	return n
}

// A node checking in every 2 s and then stopping is silent once 3 x cadence has
// passed — and not one tick before.
func TestSweepDeclaresANodeSilentAfterThreeCadences(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	st.RecordExchange("n1", 41, 0, 0, 0, 0, "")
	for i := 0; i < 4; i++ {
		checkIn(t, st, &clock, "n1", 2_000)
	}

	// Still inside the window: the first sweep says live.
	clock += 2_000
	changes, err := st.Sweep()
	if err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if len(changes) != 1 || changes[0].Silent {
		t.Fatalf("changes = %+v, want one #live transition", changes)
	}
	if got := livenessOf(t, st, "n1"); got != "live" {
		t.Fatalf("verdict = %q, want live", got)
	}

	// Past 3 x 2000 ms with nothing arriving.
	clock += 5_000
	changes, err = st.Sweep()
	if err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if len(changes) != 1 || !changes[0].Silent {
		t.Fatalf("changes = %+v, want one #silent transition", changes)
	}
	if changes[0].CadenceMs != 2_000 {
		t.Fatalf("change carries cadence %d, want 2000", changes[0].CadenceMs)
	}
	if got := livenessOf(t, st, "n1"); got != "silent" {
		t.Fatalf("verdict = %q, want silent", got)
	}
}

// Writing only on transition is what makes `porta log` a list of the times the
// fleet went quiet rather than a row every 30 s. A sweep that re-wrote the same
// verdict would bury the transition it exists to record.
func TestSweepWritesOnlyOnTransition(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	st.RecordExchange("n1", 41, 0, 0, 0, 0, "")
	for i := 0; i < 4; i++ {
		checkIn(t, st, &clock, "n1", 2_000)
	}
	clock += 20_000
	if _, err := st.Sweep(); err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	after := livenessRows(t, st, "n1")
	for i := 0; i < 5; i++ {
		clock += 30_000
		changes, err := st.Sweep()
		if err != nil {
			t.Fatalf("Sweep: %v", err)
		}
		if len(changes) != 0 {
			t.Fatalf("sweep %d reported %+v, want no change — the node is still silent", i, changes)
		}
	}
	if got := livenessRows(t, st, "n1"); got != after {
		t.Fatalf("liveness event rows = %d, want %d — a steady state wrote rows", got, after)
	}
}

func TestSweepRecordsTheReturnToLife(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	st.RecordExchange("n1", 41, 0, 0, 0, 0, "")
	for i := 0; i < 4; i++ {
		checkIn(t, st, &clock, "n1", 2_000)
	}
	clock += 20_000
	st.Sweep()
	if got := livenessOf(t, st, "n1"); got != "silent" {
		t.Fatalf("verdict = %q, want silent before the recovery", got)
	}
	checkIn(t, st, &clock, "n1", 2_000)
	changes, err := st.Sweep()
	if err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if len(changes) != 1 || changes[0].Silent {
		t.Fatalf("changes = %+v, want one #live transition", changes)
	}
	if got := livenessOf(t, st, "n1"); got != "live" {
		t.Fatalf("verdict = %q, want live", got)
	}
}

// A node porta has barely met gets no verdict at all — gw/liveness stays
// UNWRITTEN rather than being guessed at.
func TestSweepGivesNoVerdictToANodeItCannotJudge(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	st.RecordExchange("n1", 41, 0, 0, 0, 0, "")
	checkIn(t, st, &clock, "n1", 2_000)
	clock += 600_000
	changes, err := st.Sweep()
	if err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if len(changes) != 0 {
		t.Fatalf("changes = %+v, want none for a node with two exchanges", changes)
	}
	if got := livenessOf(t, st, "n1"); got != "" {
		t.Fatalf("verdict = %q, want it unwritten", got)
	}
}

// A node porta has staged goal state for but never heard from has no exchanges
// at all. It must not be reported as silent — porta was never listening to it.
func TestSweepIgnoresANodeItHasNeverHeardFrom(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	b, _ := cval.Encode("alpha")
	if _, err := st.Write("ghost", "goal/name", cval.KindCell, b, "david", ""); err != nil {
		t.Fatalf("Write: %v", err)
	}
	clock += 600_000
	changes, err := st.Sweep()
	if err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if len(changes) != 0 {
		t.Fatalf("changes = %+v, want none for a node that never checked in", changes)
	}
}

// The verdict lives in the store, not in the sweep's memory: porta restarts,
// and a restart that re-announced every node's current state would fill the log
// with transitions that did not happen.
func TestSweepVerdictSurvivesARestart(t *testing.T) {
	var clock int64 = 10_000
	path := t.TempDir() + "/restart.db"
	st, err := Open(path, func() int64 { return clock })
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	st.RecordExchange("n1", 41, 0, 0, 0, 0, "")
	for i := 0; i < 4; i++ {
		clock += 2_000
		st.RecordExchange("n1", 41, 0, 0, 0, 0, "")
	}
	clock += 20_000
	st.Sweep()
	rows := livenessRows(t, st, "n1")
	st.Close()

	st2, err := Open(path, func() int64 { return clock })
	if err != nil {
		t.Fatalf("reopen: %v", err)
	}
	defer st2.Close()
	changes, err := st2.Sweep()
	if err != nil {
		t.Fatalf("Sweep after restart: %v", err)
	}
	if len(changes) != 0 {
		t.Fatalf("changes = %+v after a restart, want none — the verdict is unchanged", changes)
	}
	if got := livenessRows(t, st2, "n1"); got != rows {
		t.Fatalf("liveness rows = %d, want %d — the restart re-announced the verdict", got, rows)
	}
}

// The inferred cadence is written back to the column that has been waiting for
// it since the schema was written, so an operator can read what porta thinks a
// node's rate IS without recomputing it.
func TestSweepRecordsTheInferredCadenceOnTheNodeRow(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	st.RecordExchange("n1", 41, 0, 0, 0, 0, "")
	for i := 0; i < 4; i++ {
		checkIn(t, st, &clock, "n1", 2_000)
	}
	clock += 1_000
	st.Sweep()
	var cadence int64
	if err := st.DB().QueryRow(`SELECT COALESCE(cadence_ms,0) FROM nodes WHERE id='n1'`).Scan(&cadence); err != nil {
		t.Fatalf("read cadence_ms: %v", err)
	}
	if cadence != 2_000 {
		t.Fatalf("nodes.cadence_ms = %d, want 2000", cadence)
	}
}

// One node's failure must never cost another node its verdict. A trigger is
// the honest injection here: it makes WriteGateway fail for exactly one node,
// in the store, the way a real constraint violation would — no fake, no seam.
func TestSweepIsNotStoppedByOneNodesFailure(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	// ids chosen so the failing one sorts in the MIDDLE of ORDER BY n.id — a
	// failing LAST node would pass even under the old code.
	for _, id := range []string{"aaa", "mmm", "zzz"} {
		st.RecordExchange(id, 41, 0, 0, 0, 0, "")
	}
	for i := 0; i < 4; i++ {
		clock += 2_000
		for _, id := range []string{"aaa", "mmm", "zzz"} {
			st.RecordExchange(id, 41, 0, 0, 0, 0, "")
		}
	}

	if _, err := st.DB().Exec(`CREATE TRIGGER fail_mmm BEFORE INSERT ON cells
		WHEN NEW.node_id = 'mmm' BEGIN SELECT RAISE(ABORT, 'injected'); END`); err != nil {
		t.Fatalf("install trigger: %v", err)
	}

	clock += 7_000 // past 3 x 2000 ms cadence
	changes, err := st.Sweep()
	if err == nil || !strings.Contains(err.Error(), "mmm") {
		t.Fatalf("err = %v, want an error naming node mmm", err)
	}
	if len(changes) != 2 {
		t.Fatalf("changes = %+v, want exactly the two healthy nodes", changes)
	}
	var got []string
	for _, c := range changes {
		got = append(got, c.NodeID)
	}
	sort.Strings(got)
	if got[0] != "aaa" || got[1] != "zzz" {
		t.Fatalf("changed nodes = %v, want [aaa zzz]", got)
	}
	if v := livenessOf(t, st, "aaa"); v != "silent" {
		t.Fatalf("aaa verdict = %q, want silent", v)
	}
	if v := livenessOf(t, st, "zzz"); v != "silent" {
		t.Fatalf("zzz verdict = %q, want silent", v)
	}
	if v := livenessOf(t, st, "mmm"); v != "" {
		t.Fatalf("mmm verdict = %q, want unwritten", v)
	}
}

// The transition test compares ENCODED BYTES. Values in the store are raw
// deterministic CBOR and are decoded only for display (D4.1); a decode here
// would break that rule for no gain, so this pins the two encodings.
func TestVerdictEncodingsAreTheAlgebrasSymbols(t *testing.T) {
	live, err := cval.Encode(cval.Symbol("live"))
	if err != nil {
		t.Fatal(err)
	}
	silent, err := cval.Encode(cval.Symbol("silent"))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(live, livenessLive) || !bytes.Equal(silent, livenessSilent) {
		t.Fatalf("pre-encoded verdicts drifted from cval.Encode")
	}
}
