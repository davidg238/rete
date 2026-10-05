# #272 — `#stalled`: porta judges progress, not only arrival — Implementation Plan

> **STATUS 2026-10-03:** Tasks 1–3 executed (`4513b81`, `4b6820b`, `614f501`). The whole-branch review then found
> the premise in Global Constraints line 1 false — `wak` does NOT ride every check-in (`pushFrame` re-sends a cached
> frame, rete#199). **Task 4 is superseded by `2026-10-03-272-count-on-the-pull.md`** (design D3.3), which also
> reworks the store half and the rule this plan built. Do not execute anything below.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** porta's liveness sweep says `#stalled` about a node whose datagrams still arrive but whose own tick counter (`obs/health.wak`) is behind the cadence it declared, and stops calling such a node `#live`.

**Architecture:** At each sweep porta compares two samples of `wak`: the **mark** (the sample it judged from at the previous sweep, stored on `nodes`) and the **latest** (the `obs/health` cell and its `ts_ms`). If `elapsed − Δwak × telCad ≥ telCad` (short by one whole declared period or more), the verdict is `#stalled`. The rate test sits *below* the existing budget, deafness and silence tests. It adds one verdict symbol and no wire field. The rule was model-checked before any Go was written.

**Tech Stack:** Go (`porta/internal/space`), sqlite, the hand-rolled `cval` CBOR codec, `protocol/MANIFEST.cddl` (zcbor 0.9.1-validated).

**Spec:** `docs/design/2026-09-14-liveness-two-cadences-design.md` §D3 (the decision), and `docs/spikes/2026-08-27-liveness-model/README.md` §"What the rate rule found (2026-10-02)" (the model-checked rule: `Behind` / `DoSweep` in `Liveness.tla`, config `sound-rate.cfg`). Paths are relative to the repo root `~/workspace/rete`.

## Global Constraints

- No wire change and no new field: `wak` already rides `obs/health` on every check-in (`PortaBridgeNode.nsl` `publishHealth`, `#wak -> ticks`). The node increments `ticks` once per base tick, and the base tick runs at `cadMs` = the declared `telCad` (`kernel.nsl` `startTicking:on:`).
- **No backward compatibility.** The new store columns go in through the existing `addColumns` list (`schema.go`); nothing is versioned or dual-read.
- **No invented tunables.** The only slack is ONE declared period (`d.TelCadMs`), as in the model's `Behind`. Do not add a constant.
- Every verdict stays pre-encoded once and compared as **bytes** (D4.1): add `livenessStalled` beside the other four in `sweep.go`.
- `MANIFEST.cddl` editing rules (memory `manifest-cddl-editing-rules`): run `protocol/tools/manifest_lint.py`, and edit `protocol/descriptors/records.yaml` in the **same commit** as the manifest.
- Gates (root `CLAUDE.md`): `cd porta && go test ./...`; `cd nodus/nsl && python3 tools/run_tests_oevm.py` (~20 min, background, `-u` to a file; never under `timeout`; read the aux line); `cd nodus/oevm && bash tests/porta_sync_host.sh` (never beside an nsl suite).
- `cd porta && gofmt -l .` must print nothing before every commit; new Go files carry `// Copyright (c) 2026 Ekorau LLC` (`porta/CLAUDE.md`).
- Issue references are `refs #272`. Never use a closing keyword.

## Review Focus

1. **Arrivals while `obs/health` has stopped landing** (the 2026-08 batch-cap overflow put exactly this in the record): the verdict must not stay `#live` indefinitely. It reads `#unknown`, naming how long the counter has been absent. Pinned in Task 2, `TestSweepArrivingWithoutProgressIsUnknown`.
2. **A reboot inside the window** (`wak` restarts from 0 under a new boot id): this must read `#stalled` with the backwards step named, and it must not need a `boot_id` join (#272 forbids one). Pinned in Task 2, `TestSweepCatchesARebootAsStalled`.
3. **An operator changes the cadence (`goal/liveness`) mid-window:** with no guard, the old ticks are judged against the new period and give one false `#stalled`. The fix is to re-mark without judging. Pinned in Task 2, `TestSweepReMarksWhenTheCadenceMoves`.
4. **Millisecond jitter on a healthy node:** a node a few ms off its period must never read `#stalled`. The boundary is exactly one period. Pinned in Task 2, `TestSweepDoesNotStallANodeOnCadence`.
5. **A store opened by the pre-#272 binary** (the live gateway db at `~/porta-v2`): the new columns arrive NULL. NULL is "no mark", so the first sweep says `#unknown` once and then judges normally. Pinned in Task 1, `TestMarkIsAbsentUntilSet`.

---

## File map

| File | Change |
|---|---|
| `porta/internal/space/schema.go` | `addColumns` += `nodes.mark_wak`, `nodes.mark_ms`, `nodes.mark_cad` |
| `porta/internal/space/store.go` | `PathHealth` const beside `PathDeclared` |
| `porta/internal/space/progress.go` (new) | `Sample`, `Mark`, `LatestSample`, `MarkOf`, `SetMark` — the store half of the rate rule |
| `porta/internal/space/progress_test.go` (new) | store-half tests |
| `porta/internal/space/sweep.go` | `livenessStalled`; `verdictFor` returns a `judgement`; the rate block; `Sweep` writes the mark |
| `porta/internal/space/sweep_test.go` | `fakeNode` helper, `goLive`; existing tests that expect `#live` driven through two samples; seven new tests |
| `protocol/MANIFEST.cddl`, `protocol/descriptors/records.yaml` | `gw-verdict` gains `#6.39("stalled")` |
| `protocol/PROTOCOL-NSL-RECORDS.md`, `porta/docs/ARCHITECTURE.md` | the `gw/liveness` row and the verdict table: five verdicts (promotion) |

---

### Task 1: The store half — the latest sample, and the mark

**Files:**
- Modify: `porta/internal/space/schema.go` (the `addColumns` list)
- Modify: `porta/internal/space/store.go:280` (beside `PathDeclared`)
- Create: `porta/internal/space/progress.go`
- Test: `porta/internal/space/progress_test.go`

**Interfaces:**
- Produces: `type Sample struct{ Wak, AtMs int64 }`; `type Mark struct{ Sample; CadMs int64 }`; `func (s *Store) LatestSample(nodeID string) (Sample, bool, error)`; `func (s *Store) MarkOf(nodeID string) (Mark, bool, error)`; `func (s *Store) SetMark(nodeID string, m Mark) error`; `const PathHealth = "obs/health"`.

- [ ] **Step 1: Write the failing tests** — `porta/internal/space/progress_test.go`:

```go
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
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cd porta && go test ./internal/space/ -run 'LatestSample|MarkIsAbsent' -count=1`
Expected: build FAIL — `undefined: PathHealth`, `undefined: Sample`, `st.LatestSample undefined`.

- [ ] **Step 3: Implement**

In `store.go`, directly below `const PathDeclared = "obs/liveness"` (line 280):

```go
// PathHealth is the node's own health record. porta reads exactly one field
// of it, `wak` — the node's tick count — and only to judge progress (#272).
const PathHealth = "obs/health"
```

In `schema.go`, append to `addColumns`:

```go
	// rete#272 — the rate rule's MARK: the obs/health sample porta judged
	// from at the previous sweep, and the telCad it was judged under. NULL
	// until the first sweep that sees a sample; NULL reads as "no mark",
	// which is true of every row written before the column existed.
	`ALTER TABLE nodes  ADD COLUMN mark_wak INTEGER`,
	`ALTER TABLE nodes  ADD COLUMN mark_ms  INTEGER`,
	`ALTER TABLE nodes  ADD COLUMN mark_cad INTEGER`,
```

Create `porta/internal/space/progress.go`:

```go
// Copyright (c) 2026 Ekorau LLC

// internal/space/progress.go
package space

import (
	"database/sql"

	"github.com/davidg238/porta/internal/cval"
)

// Sample is one reading of a node's progress counter: obs/health's `wak` (the
// bridge's tick count, one per declared period) and the moment porta stored it.
type Sample struct {
	Wak  int64
	AtMs int64
}

// Mark is the sample the previous sweep judged from, and the telCad it was
// judged under. The next window runs from here to the latest sample (rete#272;
// the model's `markAge`/`seenDelta`, docs/spikes/2026-08-27-liveness-model).
type Mark struct {
	Sample
	CadMs int64
}

// LatestSample reads wak out of the node's obs/health cell. ok is false when
// there is no cell, or the record carries no integer wak — never an error, for
// DeclaredLiveness' reason: one garbled record must not cost the fleet its sweep.
func (s *Store) LatestSample(nodeID string) (Sample, bool, error) {
	var raw []byte
	var at int64
	err := s.db.QueryRow(
		`SELECT value_cbor, ts_ms FROM cells WHERE node_id = ? AND path = ?`,
		nodeID, PathHealth).Scan(&raw, &at)
	if err == sql.ErrNoRows {
		return Sample{}, false, nil
	}
	if err != nil {
		return Sample{}, false, err
	}
	v, err := cval.Decode(raw)
	if err != nil {
		return Sample{}, false, nil
	}
	rec, ok := v.(cval.Record)
	if !ok || rec.Tag != "Health" {
		return Sample{}, false, nil
	}
	for _, p := range rec.Fields {
		if k, ok := p.K.(cval.Symbol); ok && k == "wak" {
			if n, ok := p.V.(int64); ok {
				return Sample{Wak: n, AtMs: at}, true, nil
			}
		}
	}
	return Sample{}, false, nil
}

// MarkOf reads the node's mark. ok is false while any of the three is NULL.
func (s *Store) MarkOf(nodeID string) (Mark, bool, error) {
	var w, at, cad sql.NullInt64
	err := s.db.QueryRow(
		`SELECT mark_wak, mark_ms, mark_cad FROM nodes WHERE id = ?`, nodeID).Scan(&w, &at, &cad)
	if err == sql.ErrNoRows {
		return Mark{}, false, nil
	}
	if err != nil {
		return Mark{}, false, err
	}
	if !w.Valid || !at.Valid || !cad.Valid {
		return Mark{}, false, nil
	}
	return Mark{Sample: Sample{Wak: w.Int64, AtMs: at.Int64}, CadMs: cad.Int64}, true, nil
}

// SetMark moves the node's mark.
func (s *Store) SetMark(nodeID string, m Mark) error {
	_, err := s.db.Exec(
		`UPDATE nodes SET mark_wak = ?, mark_ms = ?, mark_cad = ? WHERE id = ?`,
		m.Wak, m.AtMs, m.CadMs, nodeID)
	return err
}
```

- [ ] **Step 4: Run them to verify they pass**

Run: `cd porta && go test ./internal/space/ -run 'LatestSample|MarkIsAbsent' -count=1 -v`
Expected: three PASS. If `TestLatestSampleReadsWakAndWhenItLanded` reads `AtMs` ≠ 12000, read how `Ingest` stamps `cells.ts_ms` (`store.go:197`) before changing the test. The rule depends on `ts_ms` being the arrival time.

- [ ] **Step 5: Full package, then commit**

Run: `cd porta && go test ./internal/space/ -count=1` → `ok`.

```bash
git add porta/internal/space/schema.go porta/internal/space/store.go porta/internal/space/progress.go porta/internal/space/progress_test.go
git commit -m "space(#272): the rate rule's store half — the latest wak sample, and the mark

refs #272"
```

---

### Task 2: The rule — `#stalled` in `verdictFor`, the mark in `Sweep`

**Files:**
- Modify: `porta/internal/space/sweep.go` (verdict constants; `LivenessChange` comment; `Sweep` loop; `verdictFor`; the doc comments that say "four verdicts" and "one line")
- Modify: `porta/internal/space/sweep_test.go`

**Interfaces:**
- Consumes: `Sample`, `Mark`, `LatestSample`, `MarkOf`, `SetMark`, `PathHealth`, `pushHealth` (Task 1).
- Produces: `livenessStalled`; `type judgement struct{ want []byte; verdict string; since int64; d Declaration; reason string; mark *Mark }`; `func (s *Store) verdictFor(nodeID string, lastSeen sql.NullInt64, stored []byte, now int64) (judgement, error)`.

**The rule, in evaluation order** (the first three are unchanged):

1. `!lastSeen.Valid` → `#neverSeen`. No declaration, or `bud < SweepTick` → `#unknown`.
2. `since > bud` → `#unknown` if deaf, else `#silent`. **The mark does not move.**
3. *(new — everything below is inside the budget)* No `wak` sample → `#unknown` ("declares a cadence but its obs/health carries no wak").
4. `lastSeen − latest.AtMs ≥ telCad` → `#unknown` ("arriving, but obs/health has not landed for N ms"). This guards Review Focus 1. It is outside the model, which assumed every arrival carries a sample. **The mark does not move.**
5. No mark → `#unknown` ("one sample says nothing about a rate"). **Mark := latest.**
6. `mark.CadMs ≠ telCad` → keep the stored verdict. **Mark := latest** under the new cadence.
7. `latest.AtMs == mark.AtMs` (no new sample since the mark) → keep the stored verdict. **The mark does not move.** This is the model's empty-window fix: re-marking here let a stalled node flip back to `#live`.
8. `elapsed − Δwak×telCad ≥ telCad` → `#stalled`. Otherwise `#live`. **Mark := latest.**

"Keep the stored verdict" means `want = stored`, so `Sweep`'s `bytes.Equal` skips the write. If `stored` is empty (no verdict was ever written), answer `#unknown` instead.

- [ ] **Step 1: Write the test helpers and the failing tests.** Append to `sweep_test.go`:

```go
// fakeNode is a node that ticks the way PortaBridgeNode does: every tick is one
// check-in (pull + push) whose push carries obs/health with the tick count.
// seq starts at 1 because `declare` used seq 1.
type fakeNode struct {
	id        string
	boot, seq int64
	wak       int64
}

func (n *fakeNode) tick(t *testing.T, st *Store, clock *int64, gap int64) {
	t.Helper()
	checkIn(t, st, clock, n.id, gap)
	n.seq++
	n.wak++
	pushHealth(t, st, n.id, n.boot, n.seq, n.wak)
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
	n := &fakeNode{id: id, boot: 41, seq: 1}
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
	n := &fakeNode{id: "n1", boot: 41, seq: 1}
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

// Review Focus 1: datagrams keep landing while obs/health does not (the
// batch-cap overflow of 2026-08). Without this guard the empty-window rule would
// hold #live forever.
func TestSweepArrivingWithoutProgressIsUnknown(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	listening(t, st)
	goLive(t, st, &clock, "n1", 2_000)
	for i := 0; i < 3; i++ {
		checkIn(t, st, &clock, "n1", 2_000) // exchanges, no obs/health
	}
	got := sweepOnce(t, st)
	if len(got) != 1 || got[0].Verdict != "unknown" {
		t.Fatalf("changes = %+v, want one #unknown", got)
	}
	if r := lastReason(t, st, "n1"); !strings.Contains(r, "has not landed for 6000 ms") {
		t.Fatalf("reason = %q", r)
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
	re := &fakeNode{id: "n1", boot: 42, seq: 0} // a new boot: seq and wak from zero
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
	n.seq++
	if _, _, err := st.Ingest("n1", n.boot, []Entry{{Seq: n.seq, Path: PathDeclared, Kind: cval.KindCell, Value: rec}}); err != nil {
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
```

Also extend `TestVerdictEncodingsAreTheAlgebrasSymbols`: encode `cval.Symbol("stalled")` and require `bytes.Equal(stalled, livenessStalled)`.

- [ ] **Step 2: Run them to verify they fail**

Run: `cd porta && go test ./internal/space/ -count=1 2>&1 | tail -30`
Expected: build FAIL (`undefined: livenessStalled`). Once that symbol alone is added, the new tests fail on their verdicts: `goLive`'s first sweep returns `#live`, not `#unknown`.

- [ ] **Step 3: Implement in `sweep.go`**

Add `livenessStalled = mustEncode(cval.Symbol("stalled"))` to the verdict `var` block. Rewrite that block's comment as "FIVE AND NOT TWO", adding the line: *#stalled is arriving but behind the cadence it declared — it is talking and not getting anywhere; go find what is blocking its tick (rete#272 D3).* Change the `LivenessChange.Verdict` comment to `live | silent | neverSeen | unknown | stalled`.

Add the type:

```go
// judgement is verdictFor's answer. mark is non-nil when the sweep must move
// the node's mark — written even when the verdict itself is unchanged.
type judgement struct {
	want    []byte
	verdict string
	since   int64
	d       Declaration
	reason  string
	mark    *Mark
}
```

In `Sweep`, replace the per-node body:

```go
	for _, n := range nodes {
		j, err := s.verdictFor(n.id, n.lastSeen, n.stored, now)
		if err != nil {
			errs = append(errs, fmt.Errorf("node %s: %w", n.id, err))
			continue
		}
		if !bytes.Equal(n.stored, j.want) {
			if err := s.WriteGateway(n.id, PathLiveness, j.want, j.reason); err != nil {
				errs = append(errs, fmt.Errorf("node %s: write verdict: %w", n.id, err))
				continue // the mark judged this verdict; it must not move without it
			}
			changes = append(changes, LivenessChange{
				NodeID: n.id, Verdict: j.verdict, SinceMs: j.since,
				BudgetMs: j.d.BudgetMs(), TelCadMs: j.d.TelCadMs, Misses: j.d.Misses,
			})
		}
		if j.mark != nil {
			if err := s.SetMark(n.id, *j.mark); err != nil {
				errs = append(errs, fmt.Errorf("node %s: move mark: %w", n.id, err))
			}
		}
	}
```

Change `verdictFor`'s signature to `(nodeID string, lastSeen sql.NullInt64, stored []byte, now int64) (judgement, error)`. Convert every existing `return x, "v", since, d, reason, nil` to `return judgement{want: x, verdict: "v", since: since, d: d, reason: reason}, nil`, and every error return to `return judgement{}, fmt.Errorf(...)`. Then replace the `if since <= bud { return live... }` block with:

```go
	if since <= bud {
		return s.judgeProgress(nodeID, lastSeen.Int64, stored, since, d)
	}
```

and add, below `verdictFor`:

```go
// judgeProgress is rete#272 D3, for a node already inside its budget: is it
// ticking at the rate it declared? The rule is the model-checked one
// (docs/spikes/2026-08-27-liveness-model, `Behind` and `DoSweep`, config
// sound-rate.cfg): between the mark and the latest sample, short by one whole
// declared period or more is #stalled. The one period is the only slack, and it
// is not a tunable — it is the smallest that absorbs a real node's ms jitter.
//
// THE ORDER IS LOAD-BEARING, and two branches exist because TLC refuted the
// version without them: a sweep with no new sample keeps the verdict AND the
// mark (re-marking let a stalled node flip back to #live, unbounded), and a
// node with no mark is #unknown (one sample says nothing about a rate).
func (s *Store) judgeProgress(nodeID string, lastSeen int64, stored []byte, since int64, d Declaration) (judgement, error) {
	j := judgement{since: since, d: d}
	keep := func(why string) (judgement, error) {
		if len(stored) == 0 {
			j.want, j.verdict, j.reason = livenessUnknown, "unknown", "unknown: "+why
			return j, nil
		}
		j.want, j.reason = stored, why
		return j, nil
	}
	latest, ok, err := s.LatestSample(nodeID)
	if err != nil {
		return judgement{}, fmt.Errorf("latest sample: %w", err)
	}
	if !ok {
		j.want, j.verdict = livenessUnknown, "unknown"
		j.reason = fmt.Sprintf("unknown: %d ms since the last exchange, but this node's obs/health carries no wak — porta cannot judge progress", since)
		return j, nil
	}
	if gap := lastSeen - latest.AtMs; gap >= d.TelCadMs {
		j.want, j.verdict = livenessUnknown, "unknown"
		j.reason = fmt.Sprintf("unknown: arriving, but obs/health has not landed for %d ms (telCad %d ms)", gap, d.TelCadMs)
		return j, nil
	}
	mark, hasMark, err := s.MarkOf(nodeID)
	if err != nil {
		return judgement{}, fmt.Errorf("mark: %w", err)
	}
	next := &Mark{Sample: latest, CadMs: d.TelCadMs}
	if !hasMark {
		j.mark = next
		j.want, j.verdict = livenessUnknown, "unknown"
		j.reason = "unknown: one sample says nothing about a rate — judged from the next sweep"
		return j, nil
	}
	if mark.CadMs != d.TelCadMs {
		j.mark = next
		return keep(fmt.Sprintf("telCad moved %d -> %d ms: re-marked, not judged", mark.CadMs, d.TelCadMs))
	}
	if latest.AtMs == mark.AtMs {
		return keep("no new sample since the mark")
	}
	j.mark = next
	elapsed := latest.AtMs - mark.AtMs
	ticks := latest.Wak - mark.Wak
	if short := elapsed - ticks*d.TelCadMs; short >= d.TelCadMs {
		j.want, j.verdict = livenessStalled, "stalled"
		j.reason = fmt.Sprintf("stalled: %d ticks in %d ms between samples, declared one per %d ms — %.1f periods behind",
			ticks, elapsed, d.TelCadMs, float64(short)/float64(d.TelCadMs))
		if ticks < 0 {
			j.reason += fmt.Sprintf("; wak went backwards (%d -> %d): the node restarted inside the window", mark.Wak, latest.Wak)
		}
		return j, nil
	}
	missed := float64(since) / float64(d.TelCadMs)
	j.want, j.verdict = livenessLive, "live"
	j.reason = fmt.Sprintf("live: %d ms since the last exchange, %.1f missed check-ins of %d ms, %d tolerated; %d ticks in %d ms",
		since, missed, d.TelCadMs, d.Misses, ticks, elapsed)
	return j, nil
}
```

Finally, rewrite the `Sweep` doc comment's `THE RULE IS ONE LINE…` paragraph to two rules. Arrival judges silence (`now − last_seen > telCad × misses`); inside the budget, the rate judges progress (`judgeProgress`). Keep the paragraph's history and add one sentence citing #272 D3.

- [ ] **Step 4: Bring the existing tests onto the rule**

Every existing test that expects `#live` after `checkIn` + `declare` now gets `#unknown`, because porta holds no `wak` sample. Replace each such opening with `n := goLive(t, st, &clock, "n1", <telCad>)` (with `listening(t, st)` before it). Use `n.tick` wherever the test later expects the node to have spoken. Where a test counts rows (`livenessRows`) or changes, add the one `#unknown` row `goLive` writes, and say so in a comment. Read each test before editing it. The intent stays the same; only the route to `#live` changes. Expect at least: `TestSweepJudgesAgainstTheDeclaredBudget`, `TestSweepDoesNotCallAFastNodeSilentForOrdinaryBlockingWork`, `TestSweepWritesOnlyOnTransition`, `TestSweepRecordsTheReturnToLife`, `TestSweepVerdictSurvivesARestart`, `TestSweepIsNotStoppedByOneNodesFailure`, and `TestSweepReasonCountsTheMisses` (its reason text is unchanged for `#silent`).

`TestSweepJudgesAgainstTheDeclaredBudget` has a boundary at exactly `bud`. Keep it, and measure `lastSeen` from goLive's last tick.

- [ ] **Step 5: Run the package**

Run: `cd porta && go test ./internal/space/ -count=1 -v 2>&1 | grep -E '^(--- |ok|FAIL)'`
Expected: every test PASS, including the seven new ones.

- [ ] **Step 6: Falsify the two refutation guards**

The guard must fail for the right reason. First, in `judgeProgress`, temporarily move `j.mark = next` above the `latest.AtMs == mark.AtMs` test. Run `-run EmptyWindow -count=1`. Expected: FAIL "empty window moved the mark". Restore. Second, temporarily delete the `!hasMark` branch. Run `-run OneSample -count=1`. Expected: FAIL (a verdict other than `#unknown`, or a panic on the zero mark). Restore, rerun the package → `ok`. Record both outputs in the commit body.

- [ ] **Step 7: Whole module, then commit**

Run: `cd porta && go test ./... -count=1` → every package `ok`. (`-count=1` matters: memory `go-test-cache-reads-as-a-pass`.)

```bash
git add porta/internal/space/sweep.go porta/internal/space/sweep_test.go
git commit -m "space(#272): #stalled — arriving, but behind the cadence it declared

<falsification outputs from Step 6, two lines>
refs #272"
```

---

### Task 3: The contract and the mainline docs

**Files:**
- Modify: `protocol/MANIFEST.cddl:355`, `protocol/descriptors/records.yaml` (`gw-verdict` entry, ~line 954)
- Modify: `protocol/PROTOCOL-NSL-RECORDS.md` (the `gw/liveness` row), `porta/docs/ARCHITECTURE.md` (the "Four verdicts" table, ~line 247)

**Interfaces:**
- Consumes: the five verdict names from Task 2.

- [ ] **Step 1: The manifest**

```cddl
gw-verdict = #6.39("live") / #6.39("silent") / #6.39("neverSeen") / #6.39("unknown") / #6.39("stalled")
```

In `records.yaml`'s `gw-verdict.meaning`, start with `#live, #silent, #neverSeen, #unknown or #stalled`. Add the sentence: *Inside the budget, #stalled says the node's obs/health.wak moved slower than one tick per telCad between porta's last two samples (rete#272 D3).* Replace the `validity` sentence "A verdict about SILENCE, not about health" with "Silence and rate, not correctness". Keep the `ref` advice. Append `rete#272` to `refs`.

- [ ] **Step 2: The manifest gates, in order**

```bash
python3 protocol/tools/manifest_lint.py
cd porta && go test ./internal/manifest/ ./internal/gate/ -count=1
```
Expected: lint clean, both `ok`. (`TestDescriptorsCoverManifest` is the fourth reader. It goes RED if `records.yaml` drifts from the manifest.)

- [ ] **Step 3: The prose of record (promotion)**

`porta/docs/ARCHITECTURE.md`: change "Four verdicts" to "Five verdicts" and add this row to the table:

`| #stalled | heard from inside its budget, but its tick count (obs/health.wak) moved slower than its declared telCad between porta's last two samples | find what is blocking its tick — a replyless uplink (#271) doubles the period; a reboot reads as wak going backwards |`

Extend the `#unknown` row's causes: "…, its obs/health carries no `wak` or has stopped landing, or porta holds only one sample". Add one paragraph: the detection bound is `bud + SweepTick + telCad` (TLC's, `sound-rate.cfg`; 32 + 30 + 2 = 64 s on a bench node). An empty window keeps the verdict rather than re-judging it. Cite the model README.

`protocol/PROTOCOL-NSL-RECORDS.md`, in the `gw/liveness` row: list the five symbols. Add one sentence: `#stalled` = arriving, but `obs/health.wak` behind the declared `telCad` (rete#272 D3), with its reason naming the ticks, the elapsed ms and the period.

- [ ] **Step 4: The seam and suite gates**

Run `cd nodus/oevm && bash tests/porta_sync_host.sh` → `PORTA SYNC OK`. Then run the nsl suite in the background with `-u` to a file. Never run them concurrently.

```bash
cd nodus/nsl && python3 -u tools/run_tests_oevm.py > /tmp/claude-1000/nsl-272.log 2>&1
```
Expected: `Failed: 0` **and** an aux line with every gate PASS (`nsl-tools-py` reads the manifest). A SKIP is a finding.

- [ ] **Step 5: Commit**

```bash
git add protocol/MANIFEST.cddl protocol/descriptors/records.yaml protocol/PROTOCOL-NSL-RECORDS.md porta/docs/ARCHITECTURE.md
git commit -m "protocol(#272): gw-verdict gains #stalled; the verdict table is five

refs #272"
```

---

### Task 4: Close-out (David-gated where marked)

- [ ] **Step 1:** `tools/docmap.sh`; commit if `review/DOCMAP.md` is the only change **and** David agrees. `review/` is his tree, so list the diff for him instead of committing it.
- [ ] **Step 2:** File a P3 issue `found-during #272`: "liveness-stalled end-to-end gate — a node whose replies are dropped, observed reading `#stalled` through a real porta (host or `c6spare`)". D3 requires reachability on a real node, not only in the Go tests.
- [ ] **Step 3 (David):** deploy to the gateway (`~/porta-v2` on gw85224-01, memory `porta-gateway-operational-state`). Build, stop, and `serve --manifest` with the new manifest (the store gates from its **stored** copy). Watch `porta get <c6bench> gw` across two sweeps: expect `#unknown` ("one sample"), then `#live`.
- [ ] **Step 4:** Comment on #272 (≤5 lines): the commits, the test names, and the mainline docs that absorbed the knowledge (`porta/docs/ARCHITECTURE.md`, `protocol/PROTOCOL-NSL-RECORDS.md`, the model README). Leave the issue open until Step 3's observation is recorded.
