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

// listening opens a gw_runs row, which is what `porta serve` does before its
// first tick. Without one every window is uncovered and every verdict past the
// budget is #unknown — correctly, since a store with no recorded run cannot
// testify that anyone was listening (rete#231 D6).
func listening(t *testing.T, st *Store) {
	t.Helper()
	if err := st.StartRun(); err != nil {
		t.Fatalf("StartRun: %v", err)
	}
}

// declare writes what a node would have pushed at obs/liveness: its telCad and
// the number of consecutive misses it tolerates (rete#273 D2). porta derives
// the budget; nothing here says 16000. It goes through Ingest and not a raw
// INSERT, so the test exercises the same path a real push takes — including
// the kind pin.
func declare(t *testing.T, st *Store, node string, telCadMs, misses int64) {
	t.Helper()
	rec, err := cval.Encode(cval.Record{Tag: "Liveness", Fields: cval.Map{
		{K: cval.Symbol("misses"), V: misses},
		{K: cval.Symbol("telCad"), V: telCadMs},
	}})
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, _, err := st.Ingest(node, 41, []Entry{
		{Seq: 1, Path: PathDeclared, Kind: cval.KindCell, Value: rec},
	}); err != nil {
		t.Fatalf("Ingest: %v", err)
	}
}

// A node is judged against the budget IT declared, and against nothing else. The
// rule is one line with no constants in it: now - last_seen > telCad x misses.
// 16 misses and not 8 because 2 s x 8 = 16 s is below the 30 s sweep tick and
// porta declines to judge it (D2.1, its own test below).
func TestSweepJudgesAgainstTheDeclaredBudget(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	listening(t, st)
	checkIn(t, st, &clock, "n1", 0)
	declare(t, st, "n1", 2_000, 16)
	lastSeen := clock

	// Well inside the budget.
	clock = lastSeen + 10_000
	changes, err := st.Sweep()
	if err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if len(changes) != 1 || changes[0].Verdict != "live" {
		t.Fatalf("changes = %+v, want one #live", changes)
	}
	if changes[0].BudgetMs != 32_000 || changes[0].TelCadMs != 2_000 || changes[0].Misses != 16 {
		t.Fatalf("change = %+v, want budget 32000 = telCad 2000 x misses 16", changes[0])
	}

	// EXACTLY at the budget is still live: the inequality is strict, so the
	// declaration is honoured to its last millisecond.
	clock = lastSeen + 32_000
	if changes, err = st.Sweep(); err != nil {
		t.Fatalf("Sweep at the boundary: %v", err)
	} else if len(changes) != 0 {
		t.Fatalf("at exactly bud: changes = %+v, want no transition", changes)
	}
	if got := livenessOf(t, st, "n1"); got != "live" {
		t.Fatalf("verdict at exactly bud = %q, want live", got)
	}

	clock++
	if changes, err = st.Sweep(); err != nil {
		t.Fatalf("Sweep past the boundary: %v", err)
	} else if len(changes) != 1 || changes[0].Verdict != "silent" {
		t.Fatalf("one ms past bud: changes = %+v, want one #silent", changes)
	}
}

// #220: a node checking in every 2 s that tolerates 16 misses is NOT silent
// after 6 s of routine blocking work. Under the inference this was 3 x 2 s and
// the verdict was wrong; the node's own declaration is what makes it right, and
// the gateway multiplies nothing of its own.
func TestSweepDoesNotCallAFastNodeSilentForOrdinaryBlockingWork(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	listening(t, st)
	for i := 0; i < 5; i++ {
		checkIn(t, st, &clock, "n1", 2_000)
	}
	declare(t, st, "n1", 2_000, 16)

	clock += 6_500 // three missed check-ins
	changes, err := st.Sweep()
	if err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if len(changes) != 1 || changes[0].Verdict != "live" {
		t.Fatalf("changes = %+v, want #live — 6.5 s is inside a declared 32 s budget", changes)
	}
}

// D5: a node porta has NEVER heard from is a commissioning failure, not a
// liveness failure, and the two do not have the same operator action — go plug
// it in versus go find out why it stopped. It is judgeable immediately, which
// is what dissolved #219: under the inference it was invisible forever, because
// a node that never spoke supplies no history to infer from.
//
// The node row is created the way a real one would be before its first check-in:
// by porta staging goal state for it. There is no EnsureNode to call, and adding
// one for a test would be a mechanism the system does not otherwise need.
func TestSweepDistinguishesNeverSeenFromSilent(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	name, err := cval.Encode("alpha")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, err := st.Write("ghost", "goal/name", cval.KindCell, name, "david", ""); err != nil {
		t.Fatalf("Write: %v", err)
	}

	clock += 60_000
	changes, err := st.Sweep()
	if err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if len(changes) != 1 || changes[0].Verdict != "neverSeen" {
		t.Fatalf("changes = %+v, want one #neverSeen", changes)
	}
	if got := livenessOf(t, st, "ghost"); got != "neverSeen" {
		t.Fatalf("verdict = %q, want neverSeen", got)
	}
}

// A node that has spoken but declared nothing gets NO confident verdict. porta
// has no number to judge against and will not invent one — which is the whole
// argument of this design, applied to porta's own convenience.
func TestSweepWithoutADeclarationIsUnknownAndNotSilent(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	checkIn(t, st, &clock, "n1", 0)

	clock += 10 * 60_000
	changes, err := st.Sweep()
	if err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if len(changes) != 1 || changes[0].Verdict != "unknown" {
		t.Fatalf("changes = %+v, want one #unknown", changes)
	}
}

// Transition-only, unchanged from the inference era: a row every 30 s would bury
// the one moment that matters, and `porta log` is meant to read as a list of the
// times the fleet went quiet.
func TestSweepWritesOnlyOnTransition(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	listening(t, st)
	checkIn(t, st, &clock, "n1", 0)
	declare(t, st, "n1", 2_000, 16)
	for i := 0; i < 4; i++ {
		clock += 1_000
		if _, err := st.Sweep(); err != nil {
			t.Fatalf("Sweep: %v", err)
		}
	}
	if n := livenessRows(t, st, "n1"); n != 1 {
		t.Fatalf("gw/liveness rows = %d, want 1", n)
	}
}

// The return to life is a transition like any other, and it is the one an
// operator waits for: a node that came back must stop reading #silent without
// anyone clearing anything.
func TestSweepRecordsTheReturnToLife(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	listening(t, st)
	checkIn(t, st, &clock, "n1", 0)
	declare(t, st, "n1", 2_000, 16)
	clock += 40_000
	if _, err := st.Sweep(); err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if got := livenessOf(t, st, "n1"); got != "silent" {
		t.Fatalf("verdict = %q, want silent before the recovery", got)
	}
	checkIn(t, st, &clock, "n1", 2_000)
	changes, err := st.Sweep()
	if err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if len(changes) != 1 || changes[0].Verdict != "live" {
		t.Fatalf("changes = %+v, want one #live transition", changes)
	}
	if got := livenessOf(t, st, "n1"); got != "live" {
		t.Fatalf("verdict = %q, want live", got)
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
	checkIn(t, st, &clock, "n1", 0)
	for i := 0; i < 4; i++ {
		checkIn(t, st, &clock, "n1", 2_000)
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

// One node's failure must never cost another node its verdict. A trigger is
// the honest injection here: it makes WriteGateway fail for exactly one node,
// in the store, the way a real constraint violation would — no fake, no seam.
func TestSweepIsNotStoppedByOneNodesFailure(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	listening(t, st)
	// ids chosen so the failing one sorts in the MIDDLE of ORDER BY n.id — a
	// failing LAST node would pass even under the old code.
	for _, id := range []string{"aaa", "mmm", "zzz"} {
		st.RecordExchange(id, 41, 0, 0, 0, 0, "")
		st.RecordExchange(id, 41, 0, 0, 0, 0, "")
	}
	for i := 0; i < 4; i++ {
		clock += 2_000
		for _, id := range []string{"aaa", "mmm", "zzz"} {
			st.RecordExchange(id, 41, 0, 0, 0, 0, "")
			st.RecordExchange(id, 41, 0, 0, 0, 0, "")
		}
	}

	// Declared BEFORE the trigger, or the declaration for mmm would be the
	// insert that fails and the test would be injecting the wrong fault.
	for _, id := range []string{"aaa", "mmm", "zzz"} {
		declare(t, st, id, 2_000, 16)
	}

	if _, err := st.DB().Exec(`CREATE TRIGGER fail_mmm BEFORE INSERT ON cells
		WHEN NEW.node_id = 'mmm' BEGIN SELECT RAISE(ABORT, 'injected'); END`); err != nil {
		t.Fatalf("install trigger: %v", err)
	}

	clock += 40_000 // past the declared 32 s budget
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

// lastReason reads the reason porta wrote with the node's most recent verdict.
func lastReason(t *testing.T, st *Store, node string) string {
	t.Helper()
	var r string
	if err := st.DB().QueryRow(
		`SELECT reason FROM events WHERE node_id = ? AND path = ? ORDER BY id DESC LIMIT 1`,
		node, PathLiveness).Scan(&r); err != nil {
		t.Fatalf("last reason: %v", err)
	}
	return r
}

// D2.1: porta refuses to promise what its sweep cannot deliver. A budget below
// SweepTick is #unknown with a reason naming all three numbers, not a quiet 30 s.
func TestSweepRefusesABudgetBelowItsOwnTick(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	listening(t, st)
	checkIn(t, st, &clock, "n1", 0)
	declare(t, st, "n1", 2_000, 8) // 16 s < 30 s
	clock += 1_000
	changes, err := st.Sweep()
	if err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if len(changes) != 1 || changes[0].Verdict != "unknown" {
		t.Fatalf("changes = %+v, want one #unknown", changes)
	}
	if changes[0].TelCadMs != 2_000 || changes[0].Misses != 8 || changes[0].BudgetMs != 16_000 {
		t.Fatalf("change = %+v, want telCad 2000 misses 8 budget 16000", changes[0])
	}
	reason := lastReason(t, st, "n1")
	for _, want := range []string{"2000", "8", "16000", "30000", "sweep tick"} {
		if !strings.Contains(reason, want) {
			t.Fatalf("reason %q does not name %q", reason, want)
		}
	}
}

// D2.2: the miss count rides the REASON, not a verdict.
func TestSweepReasonCountsTheMisses(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	listening(t, st)
	checkIn(t, st, &clock, "n1", 0)
	declare(t, st, "n1", 10_000, 4) // 40 s budget
	clock += 45_000
	if _, err := st.Sweep(); err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if got := livenessOf(t, st, "n1"); got != "silent" {
		t.Fatalf("verdict = %q, want silent", got)
	}
	if reason := lastReason(t, st, "n1"); !strings.Contains(reason, "4.5 missed check-ins of 10000 ms, 4 tolerated") {
		t.Fatalf("reason = %q", reason)
	}
}
