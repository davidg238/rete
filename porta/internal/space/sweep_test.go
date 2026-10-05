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
	// goLive's route, spelled out so the #live transition's own fields can be
	// read: one sample (#unknown, the mark is taken), then on-cadence ticks.
	declare(t, st, "n1", 2_000, 16)
	n := &fakeNode{id: "n1", boot: 41}
	n.tick(t, st, &clock, 0)
	sweepOnce(t, st)
	n.ticks(t, st, &clock, 3, 2_000)
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
	goLive(t, st, &clock, "n1", 2_000)

	clock += 6_500 // three missed check-ins
	changes, err := st.Sweep()
	if err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if len(changes) != 0 || livenessOf(t, st, "n1") != "live" {
		t.Fatalf("changes = %+v, verdict %q, want #live held — 6.5 s is inside a declared 32 s budget", changes, livenessOf(t, st, "n1"))
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
	goLive(t, st, &clock, "n1", 2_000)
	for i := 0; i < 4; i++ {
		clock += 1_000
		if _, err := st.Sweep(); err != nil {
			t.Fatalf("Sweep: %v", err)
		}
	}
	// Two: goLive's #unknown (one sample) and its #live. The four sweeps add none.
	if n := livenessRows(t, st, "n1"); n != 2 {
		t.Fatalf("gw/liveness rows = %d, want 2", n)
	}
}

// The return to life is a transition like any other, and it is the one an
// operator waits for: a node that came back must stop reading #silent without
// anyone clearing anything.
func TestSweepRecordsTheReturnToLife(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	listening(t, st)
	goLive(t, st, &clock, "n1", 2_000)
	clock += 40_000
	if _, err := st.Sweep(); err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if got := livenessOf(t, st, "n1"); got != "silent" {
		t.Fatalf("verdict = %q, want silent before the recovery", got)
	}
	// #silent forgot the mark, so the return is judged afresh (rete#272 D3.3):
	// one sample (#unknown), then a rate.
	re := &fakeNode{id: "n1", boot: 42}
	re.tick(t, st, &clock, 2_000)
	if got := sweepOnce(t, st); len(got) != 1 || got[0].Verdict != "unknown" {
		t.Fatalf("first sweep back = %+v, want one #unknown (one sample)", got)
	}
	re.ticks(t, st, &clock, 3, 2_000)
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
	listening(t, st)
	goLive(t, st, &clock, "n1", 2_000)
	clock += 20_000 // inside the 32 s budget, no new sample: the verdict holds
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
	//
	// Declared BEFORE the trigger, or the declaration for mmm would be the
	// insert that fails and the test would be injecting the wrong fault.
	for _, id := range []string{"aaa", "mmm", "zzz"} {
		declare(t, st, id, 2_000, 16)
		(&fakeNode{id: id, boot: 41}).ticks(t, st, &clock, 5, 0)
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
	stalled, err := cval.Encode(cval.Symbol("stalled"))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(live, livenessLive) || !bytes.Equal(silent, livenessSilent) || !bytes.Equal(stalled, livenessStalled) {
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
	declare(t, st, "n1", 2_000, 8) // 16 s < 30 s
	(&fakeNode{id: "n1", boot: 41}).tick(t, st, &clock, 0)
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
	declare(t, st, "n1", 10_000, 4) // 40 s budget
	(&fakeNode{id: "n1", boot: 41}).tick(t, st, &clock, 0)
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

// fakeNode is a node that ticks the way PortaBridgeNode does: every tick is one
// check-in — a pull carrying the tick count (#wak, rete#272 D3.3), then the
// pull and push exchange rows.
type fakeNode struct {
	id   string
	boot int64
	wak  int64
}

func (n *fakeNode) tick(t *testing.T, st *Store, clock *int64, gap int64) {
	t.Helper()
	*clock += gap
	n.wak++
	if _, _, err := st.NoteCheckIn(n.id, 0, 0, n.boot, n.wak); err != nil {
		t.Fatalf("NoteCheckIn: %v", err)
	}
	checkIn(t, st, clock, n.id, 0) // the pull and push exchange rows
}

func (n *fakeNode) ticks(t *testing.T, st *Store, clock *int64, count int, gap int64) {
	t.Helper()
	for i := 0; i < count; i++ {
		n.tick(t, st, clock, gap)
	}
}

func sweepOnce(t *testing.T, st *Store) []LivenessChange {
	t.Helper()
	changes, err := st.Sweep()
	if err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	return changes
}

// goLive declares telCad x 16 and drives a fresh node to #live the only way the
// rate rule allows: one sample and a sweep (#unknown, the mark is taken), then
// on-cadence ticks and a second sweep. Returns the node for further ticking.
func goLive(t *testing.T, st *Store, clock *int64, id string, telCad int64) *fakeNode {
	t.Helper()
	declare(t, st, id, telCad, 16)
	n := &fakeNode{id: id, boot: 41}
	n.tick(t, st, clock, 0)
	if got := sweepOnce(t, st); len(got) != 1 || got[0].Verdict != "unknown" {
		t.Fatalf("first sweep = %+v, want one #unknown (one sample)", got)
	}
	n.ticks(t, st, clock, 3, telCad)
	if got := sweepOnce(t, st); len(got) != 1 || got[0].Verdict != "live" {
		t.Fatalf("second sweep = %+v, want one #live", got)
	}
	return n
}

// D3: arriving, but behind the cadence it declared — #271's measured case,
// 4 s delivered against 2 s declared. This is the reachability proof the design
// demands BEFORE any test that shows the verdict does not fire.
func TestSweepCallsABehindNodeStalled(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	listening(t, st)
	n := goLive(t, st, &clock, "n1", 2_000)
	n.ticks(t, st, &clock, 5, 4_000)
	got := sweepOnce(t, st)
	if len(got) != 1 || got[0].Verdict != "stalled" {
		t.Fatalf("changes = %+v, want one #stalled", got)
	}
	if r := lastReason(t, st, "n1"); !strings.Contains(r, "5 ticks in 20000 ms") || !strings.Contains(r, "2000") {
		t.Fatalf("reason = %q, want the ticks, the elapsed time and the declared period", r)
	}
}

// Review Focus 4: jitter is not a stall. Short by less than one whole period is
// live; short by exactly one period is stalled — the model's `Behind`, >=.
func TestSweepDoesNotStallANodeOnCadence(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	listening(t, st)
	n := goLive(t, st, &clock, "n1", 2_000)
	for i := 0; i < 10; i++ {
		n.tick(t, st, &clock, 1_990+int64(i%2)*20) // 1990, 2010, ...
	}
	n.tick(t, st, &clock, 3_999) // 1999 ms short: still inside one period
	if got := sweepOnce(t, st); len(got) != 0 {
		t.Fatalf("on cadence: changes = %+v, want none", got)
	}
	n.tick(t, st, &clock, 4_000) // exactly one whole period short
	if got := sweepOnce(t, st); len(got) != 1 || got[0].Verdict != "stalled" {
		t.Fatalf("one period short: changes = %+v, want one #stalled", got)
	}
}

// The model's second refutation: one sample says nothing about a rate.
func TestSweepWithOneSampleIsUnknown(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	listening(t, st)
	declare(t, st, "n1", 2_000, 16)
	n := &fakeNode{id: "n1", boot: 41}
	n.tick(t, st, &clock, 0)
	if got := sweepOnce(t, st); len(got) != 1 || got[0].Verdict != "unknown" {
		t.Fatalf("changes = %+v, want one #unknown", got)
	}
	if r := lastReason(t, st, "n1"); !strings.Contains(r, "one sample") {
		t.Fatalf("reason = %q", r)
	}
}

// The model's first refutation: a sweep with no new sample since the mark has
// no rate evidence. Re-marking there let a stalled node flip back to #live on
// every empty window, unbounded (TLC, 2026-10-02).
func TestSweepEmptyWindowKeepsTheVerdictAndTheMark(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	listening(t, st)
	n := goLive(t, st, &clock, "n1", 2_000)
	n.ticks(t, st, &clock, 5, 4_000)
	if got := sweepOnce(t, st); len(got) != 1 || got[0].Verdict != "stalled" {
		t.Fatalf("setup: %+v, want #stalled", got)
	}
	before, _, _ := st.MarkOf("n1")
	clock += 1_000 // inside telCad of the last sample: guard 4 does not fire
	if got := sweepOnce(t, st); len(got) != 0 {
		t.Fatalf("empty window: changes = %+v, want none — the verdict must hold", got)
	}
	if after, _, _ := st.MarkOf("n1"); after != before {
		t.Fatalf("empty window moved the mark: %+v -> %+v", before, after)
	}
}

// Review Focus 2 / H3: a reboot inside the window. wak restarts under a new boot
// id and comes back BELOW the mark — stalled, with no boot_id join.
func TestSweepCatchesARebootAsStalled(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	listening(t, st)
	n := goLive(t, st, &clock, "n1", 2_000)
	n.ticks(t, st, &clock, 3, 2_000)
	re := &fakeNode{id: "n1", boot: 42} // a new boot: wak from zero
	re.ticks(t, st, &clock, 2, 2_000)
	got := sweepOnce(t, st)
	if len(got) != 1 || got[0].Verdict != "stalled" {
		t.Fatalf("changes = %+v, want one #stalled", got)
	}
	if r := lastReason(t, st, "n1"); !strings.Contains(r, "backwards") {
		t.Fatalf("reason = %q, want the backwards step named", r)
	}
}

// Review Focus 3: an adopted cadence change re-marks without judging, so ticks
// counted at the old period are never measured against the new one.
func TestSweepReMarksWhenTheCadenceMoves(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	listening(t, st)
	n := goLive(t, st, &clock, "n1", 2_000)
	rec, _ := cval.Encode(cval.Record{Tag: "Liveness", Fields: cval.Map{
		{K: cval.Symbol("misses"), V: int64(32)},
		{K: cval.Symbol("telCad"), V: int64(1_000)},
	}})
	if _, _, err := st.Ingest("n1", n.boot, []Entry{{Seq: 2, Path: PathDeclared, Kind: cval.KindCell, Value: rec}}); err != nil {
		t.Fatal(err)
	}
	n.ticks(t, st, &clock, 3, 2_000) // still at the old period this window
	if got := sweepOnce(t, st); len(got) != 0 {
		t.Fatalf("cadence moved: changes = %+v, want none (re-mark, keep)", got)
	}
	if m, _, _ := st.MarkOf("n1"); m.CadMs != 1_000 {
		t.Fatalf("mark cad = %d, want 1000", m.CadMs)
	}
	n.ticks(t, st, &clock, 4, 1_000)
	if got := sweepOnce(t, st); len(got) != 0 {
		t.Fatalf("on the new cadence: changes = %+v, want none (still #live)", got)
	}
}

// Review Focus 1: an image from before the count. It checks in (both legs stamp
// last_seen) and declares a cadence, and porta will not call that live.
func TestSweepNodeWithoutACountIsUnknown(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	listening(t, st)
	checkIn(t, st, &clock, "n1", 0)
	declare(t, st, "n1", 2_000, 16)
	got := sweepOnce(t, st)
	if len(got) != 1 || got[0].Verdict != "unknown" {
		t.Fatalf("changes = %+v, want one #unknown", got)
	}
	if r := lastReason(t, st, "n1"); !strings.Contains(r, "no tick count") {
		t.Fatalf("reason = %q", r)
	}
}

// The guard D3.2 needed is gone because the state it guarded cannot be written:
// silence is the age of the SAMPLE. Pushes that keep landing do not hold #live.
func TestSweepPushesWithoutPullsGoSilent(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	listening(t, st)
	goLive(t, st, &clock, "n1", 2_000)
	for i := 0; i < 20; i++ {
		checkIn(t, st, &clock, "n1", 2_000) // exchange rows only: no counted pull
	}
	got := sweepOnce(t, st)
	if len(got) != 1 || got[0].Verdict != "silent" {
		t.Fatalf("changes = %+v, want one #silent — 40 s since the last counted pull, budget 32 s", got)
	}
}

// Review Focus 3: porta is down 20 s; the 2 s node spins both reply budgets and
// ticks at 4 s (#271). porta's own absence is not evidence about the node.
func TestSweepDoesNotBlameANodeForPortasOutage(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	listening(t, st)
	n := goLive(t, st, &clock, "n1", 2_000)
	clock += 20_000 // porta stops; nothing is recorded for 20 s (inside the 32 s budget)
	n.wak += 5      // the node ticked at 4 s, unheard
	if err := st.StartRun(); err != nil {
		t.Fatal(err)
	}
	n.ticks(t, st, &clock, 3, 2_000)
	if got := sweepOnce(t, st); len(got) != 0 {
		t.Fatalf("after porta's outage: changes = %+v, want none (re-marked, not judged)", got)
	}
	n.ticks(t, st, &clock, 5, 2_000)
	if got := sweepOnce(t, st); len(got) != 0 {
		t.Fatalf("next window, on cadence: changes = %+v, want none", got)
	}
	if v := livenessOf(t, st, "n1"); v != "live" {
		t.Fatalf("verdict = %q, want live", v)
	}
}

// Review Focus 4: unplugged, replugged. The silence ends the conversation the
// mark belonged to, so the return is one sample, then live — never #stalled.
func TestSweepReturnAfterSilenceIsJudgedAfresh(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	listening(t, st)
	goLive(t, st, &clock, "n1", 2_000)
	clock += 120_000
	if got := sweepOnce(t, st); len(got) != 1 || got[0].Verdict != "silent" {
		t.Fatalf("unplugged: %+v, want #silent", got)
	}
	if _, ok, _ := st.MarkOf("n1"); ok {
		t.Fatal("a silent verdict kept its mark")
	}
	re := &fakeNode{id: "n1", boot: 42} // a new boot: the count starts again
	re.ticks(t, st, &clock, 4, 2_000)
	if got := sweepOnce(t, st); len(got) != 1 || got[0].Verdict != "unknown" {
		t.Fatalf("replugged, first sweep: %+v, want #unknown (one sample)", got)
	}
	re.ticks(t, st, &clock, 4, 2_000)
	if got := sweepOnce(t, st); len(got) != 1 || got[0].Verdict != "live" {
		t.Fatalf("replugged, second sweep: %+v, want #live", got)
	}
}

// Review Focus 5: the live gateway's rows — a stored #live from the arrival
// rule, and no mark. One #unknown, then judged.
func TestSweepStoredLiveWithNoMarkIsUnknownOnce(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	listening(t, st)
	declare(t, st, "n1", 2_000, 16)
	if err := st.WriteGateway("n1", PathLiveness, livenessLive, "live: written by the pre-#272 binary"); err != nil {
		t.Fatal(err)
	}
	n := &fakeNode{id: "n1", boot: 41}
	n.ticks(t, st, &clock, 3, 2_000)
	if got := sweepOnce(t, st); len(got) != 1 || got[0].Verdict != "unknown" {
		t.Fatalf("first sweep = %+v, want one #unknown", got)
	}
	n.ticks(t, st, &clock, 3, 2_000)
	if got := sweepOnce(t, st); len(got) != 1 || got[0].Verdict != "live" {
		t.Fatalf("second sweep = %+v, want one #live", got)
	}
}

// What #220's test stops short of: the blocking work ends and the node resumes.
// It was behind its declared cadence for the window, and D3 says so. Pinned so
// nobody meets it as a surprise; whether it is the right call is the fleet's
// question (design D3.3, "the one period of slack").
func TestSweepSaysStalledWhenBlockingWorkEnds(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	listening(t, st)
	n := goLive(t, st, &clock, "n1", 2_000)
	n.tick(t, st, &clock, 6_500) // one tick, 6.5 s late
	n.ticks(t, st, &clock, 10, 2_000)
	got := sweepOnce(t, st)
	if len(got) != 1 || got[0].Verdict != "stalled" {
		t.Fatalf("changes = %+v, want one #stalled (11 ticks in 26500 ms)", got)
	}
}

// The mark lives in the store, like the verdict: a porta restart must not cost
// a node its window.
func TestMarkSurvivesAReopen(t *testing.T) {
	var clock int64 = 10_000
	path := t.TempDir() + "/mark.db"
	st, err := Open(path, func() int64 { return clock })
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	listening(t, st)
	goLive(t, st, &clock, "n1", 2_000)
	before, ok, err := st.MarkOf("n1")
	if err != nil || !ok {
		t.Fatalf("MarkOf: ok=%v err=%v", ok, err)
	}
	st.Close()

	st2, err := Open(path, func() int64 { return clock })
	if err != nil {
		t.Fatalf("reopen: %v", err)
	}
	defer st2.Close()
	if after, ok, err := st2.MarkOf("n1"); err != nil || !ok || after != before {
		t.Fatalf("after reopen: %+v ok=%v err=%v, want %+v", after, ok, err, before)
	}
}

// Review (I1): a node with no count that has ALSO stopped talking must not be
// told it "checks in". The verdict stays #unknown (D3.3: no arrival-only
// fallback); the reason carries the age of its last exchange instead.
func TestSweepUncountedNodeReasonCarriesItsAge(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	listening(t, st)
	checkIn(t, st, &clock, "n1", 0)
	declare(t, st, "n1", 2_000, 16)
	clock += 60_000
	if got := sweepOnce(t, st); len(got) != 1 || got[0].Verdict != "unknown" {
		t.Fatalf("changes = %+v, want one #unknown", got)
	}
	r := lastReason(t, st, "n1")
	if strings.Contains(r, "checks in") || !strings.Contains(r, "60000 ms since its last exchange") {
		t.Fatalf("reason = %q, want the exchange age and no claim that it checks in", r)
	}
}
