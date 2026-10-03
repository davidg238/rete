# #272 — the tick count rides the pull — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** porta judges `#live` / `#stalled` / `#silent` from one sample per check-in — a count the node puts in every pull request — so the rate rule's model-checked assumption is true on the wire.

**Architecture:** The kernel bridge counts its own `checkIn`s and sends the count as `#wak` in the pull request, which is built fresh every tick. porta stores (count, arrival time) on `nodes` and the verdict reads nothing else: silence is the age of that sample, progress is the rate between the mark and it. This continues branch `272-progress-verdict`, which already holds the mark columns, `#stalled`, and the five-verdict docs (`4513b81`, `4b6820b`, `614f501`); it replaces that code's sample source and deletes its `obs/health` reader.

**Tech Stack:** nsl (`nodus/nsl/lib/kernel.nsl`), Go (`porta/internal/wire`, `porta/internal/space`), sqlite, the hand-rolled `cval` codec.

**Spec:** `docs/design/2026-09-14-liveness-two-cadences-design.md` §D3.3 (the decision), with §D3.2 for the rule. Model: `docs/spikes/2026-08-27-liveness-model/README.md` §"What the cached frame found (2026-10-03)" (`fresh-clear.cfg`, `fresh-lossy.cfg`). Paths are relative to the repo root `~/workspace/rete`.

## Global Constraints

- **`#wak` is required in a pull.** No optional field, no fallback to arrival-only, no dual read (root `CLAUDE.md`, design posture). An image without it reads `#unknown` until reflashed.
- **One sample.** The verdict reads `nodes.wak` / `nodes.wak_ms` and the mark. It does not read `obs/health`, `nodes.last_seen_ms` (except to tell `#neverSeen` from "no count yet"), or `boot_id`.
- **No invented tunables.** The only slack is one declared period. Nothing in "Left to the fleet" (D3.3) is decided here.
- **Delete what this replaces in the same commit:** `LatestSample`, `PathHealth`, `pushHealth`, the "health stopped landing" guard and its test.
- Every verdict stays pre-encoded and compared as bytes (D4.1).
- `cd porta && gofmt -l .` prints nothing before every commit. Issue references are `refs #272`, never a closing keyword.
- Gates (root `CLAUDE.md`): `cd porta && go test ./... -count=1`; the nsl suite (~20 min, background, `python3 -u … > file`, never under `timeout`, read the aux line); `cd nodus/oevm && bash tests/porta_sync_host.sh`. Never two of the last two at once.
- **A `kernel.nsl` edit needs a board** (memory `host-suite-blind-to-isr-seam`): Task 5 is not optional.

## Review Focus

1. **A node on the old image** (the C6 bell until its next flash): its pull has no `#wak`. It must be refused with a reason a person can act on, and its verdict must read `#unknown` naming the missing count — never `#live` on arrival alone, never `#neverSeen`. Pinned: Task 2 `TestPullWithoutACountIsRefused`, Task 3 `TestSweepNodeWithoutACountIsUnknown`.
2. **A new node against the porta that is deployed today** (nodes are flashed before the gateway is restarted): the old handler must serve a pull that carries the extra field. Pinned: Task 2 Step 1, a test that must PASS before the handler is changed.
3. **porta restarts** (every deploy does this): no 2 s node may read `#stalled` because it ticked at 4 s while porta was down. Pinned: Task 3 `TestSweepDoesNotBlameANodeForPortasOutage`.
4. **A node is unplugged and replugged:** `#silent`, then `#unknown` (one sample), then `#live`. Never `#stalled`. Pinned: Task 3 `TestSweepReturnAfterSilenceIsJudgedAfresh`.
5. **The live gateway store** (`~/porta-v2`): rows with a stored `#live`, NULL `wak`, NULL mark. First sweep after the node's first new pull says `#unknown` once, then judges. Pinned: Task 3 `TestSweepStoredLiveWithNoMarkIsUnknownOnce`.

---

## File map

| File | Change |
|---|---|
| `nodus/nsl/lib/kernel.nsl` | `Map`/`Record` gain the five-pair constructor; `PortaBridge` gains `wakes`, counted in `checkIn`, sent in `pullFrame` |
| `nodus/tests/22_spacesync/03_bridge_frames.nsl` | two `expect:` lines for the count |
| `porta/internal/wire/message.go`, `server.go` | `EncodePull` and `handlePull` carry `wak` |
| `porta/internal/space/schema.go` | `addColumns` += `nodes.wak`, `nodes.wak_ms` |
| `porta/internal/space/store.go` | `NoteCheckIn` stores the sample; `PathHealth` deleted |
| `porta/internal/space/progress.go` | `LatestSample` deleted; `ForgetMark` added |
| `porta/internal/space/sweep.go` | the verdict reads the sample; guard deleted; deaf window; forget on past-budget |
| `porta/internal/space/*_test.go`, `porta/internal/wire/*_test.go`, `porta/internal/portacli/loopback_test.go` | call sites; the tests below |
| `protocol/PROTOCOL-NSL.md`, `protocol/PROTOCOL-NSL-RECORDS.md`, `protocol/descriptors/records.yaml`, `porta/docs/ARCHITECTURE.md`, `porta/docs/GETTING-STARTED.md`, `docs/tutorials/03-porta.md` | the prose of record |
| `nodus/nsl/tools/fixtures/manifest_goldens.nsl` | a `#stalled` golden |

---

### Task 1: The node — count the check-in, send it in the pull

**Files:**
- Modify: `nodus/nsl/lib/kernel.nsl` (`Map` class side near `:483`, `Record` class side at `:549`, `PortaBridge` slots at `:1623`, `pullFrame` at `:1861`, `checkIn` at `:2308`)
- Test: `nodus/tests/22_spacesync/03_bridge_frames.nsl`

**Interfaces:**
- Produces: the pull request `#pull { #id, #cursor, #bid, #hw, #wak }`, `#wak` an integer that is 0 before the first `checkIn` of a boot and increases by one per `checkIn`.

- [ ] **Step 1: Write the failing test.** In `03_bridge_frames.nsl`, add two lines to the `expect:` block, directly after `pull cursor 0`:

```
pull wak 0
pull wak after one check-in 1
```

and, directly after the existing `('pull cursor ' , …) out.` line:

```smalltalk
    ('pull wak ' , (pull at: #wak ifAbsent: [ 0 - 1 ]) printString) out.
    (* socket: nil, so checkIn returns before any send — but the tick still happened,
       and the count is of ticks, not of datagrams (rete#272 D3.3). *)
    bridge checkIn.
    pull:: platform Cbor decode: bridge pullFrame ifInvalid: [ :m | nil ].
    ('pull wak after one check-in ' , (pull at: #wak ifAbsent: [ 0 - 1 ]) printString) out.
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd nodus/nsl && python3 tools/run-one.py ../tests/22_spacesync/03_bridge_frames.nsl B`
(`B` is mandatory — the default bakes Path A over the tracked fixture; memory `nsl-single-test-runner`.)
Expected: FAIL, the output shows `pull wak -1`.

- [ ] **Step 3: Implement in `kernel.nsl`**

`Map` class side — add the five-pair constructor directly below the four-pair one, with the same body extended by one pair (read the four-pair body at `:483` and mirror it exactly; it is the only way a five-field record is built without an intermediate collection).

`Record` class side, below line 549:

```smalltalk
    public tag: aTag with: a with: b with: c with: d with: e = ( ^ self tag: aTag fields: (Map with: a with: b with: c with: d with: e) )
```

`PortaBridge` slots (line 1623), add `wakes ::= 0.` to the list.

`pullFrame`:

```smalltalk
    (* #wak — rete#272 D3.3. The count of this boot's check-ins, sent on the one
       request that is built fresh every tick. porta judges progress from it, and
       it CANNOT ride the push: pushFrame re-sends the same bytes until an ack is
       heard (rete#199), so a count in there is the count from when the frame was
       built — a deaf node would report no progress at all and a lost frame would
       land an old count at a new time. *)
    public pullFrame = (
      ^ Cbor new encode: (Record tag: #pull
          with: #id -> nodeId
          with: #cursor -> cursor
          with: #bid -> self bootId
          with: #hw -> acked
          with: #wak -> wakes) )
```

`checkIn` — the first statement, ABOVE `socket == nil ifTrue: [ ^ self ]`:

```smalltalk
      (* Counted before the socket test: the count is of TICKS the reactor ran, and
         a node with no socket still ticked (rete#272 D3.3). *)
      wakes:: wakes + 1.
```

- [ ] **Step 4: Run it to verify it passes**

Run: `cd nodus/nsl && python3 tools/run-one.py ../tests/22_spacesync/03_bridge_frames.nsl B`
Expected: PASS. A SKIP proves nothing; a CRASH discards every line already printed.
Then run the two other tests that build a bridge: `…/22_spacesync/06_reply_tag_match.nsl B` and `…/11_bridge_span_brackets.nsl B` → PASS.

- [ ] **Step 5: Restore and commit**

`git status --short` must show only `kernel.nsl` and the test (a bake can leave `.oimg`/`.stmap` beside a source; remove any that appear and are untracked).

```bash
git add nodus/nsl/lib/kernel.nsl nodus/tests/22_spacesync/03_bridge_frames.nsl
git commit -m "kernel(#272): the bridge counts its check-ins and sends the count in the pull

refs #272"
```

---

### Task 2: porta — the pull's count becomes the stored sample

**Files:**
- Modify: `porta/internal/wire/message.go` (`EncodePull`), `porta/internal/wire/server.go` (`handlePull`)
- Modify: `porta/internal/space/schema.go` (`addColumns`), `store.go` (`NoteCheckIn`, delete `PathHealth`), `progress.go`
- Modify: every caller of `EncodePull` (28, tests) and `NoteCheckIn` (10 tests + `server.go`)
- Test: `porta/internal/wire/wire_test.go`, `porta/internal/space/progress_test.go`

**Interfaces:**
- Consumes: Task 1's pull shape.
- Produces: `func EncodePull(id string, cursor, bootID, hw, wak int64) ([]byte, error)`; `func (s *Store) NoteCheckIn(nodeID string, cursor, nodeHW, bootID, wak int64) (restartedFrom int64, restarted bool, err error)`; columns `nodes.wak`, `nodes.wak_ms` (NULL until the node's first counted pull); `func (s *Store) ForgetMark(nodeID string) error`. `Sample`, `Mark`, `MarkOf`, `SetMark` are unchanged. `LatestSample`, `PathHealth` and `pushHealth` no longer exist.

- [ ] **Step 1: Review Focus 2 — prove the deployed handler tolerates the new field, BEFORE touching it.** Append to `wire_test.go` (use the file's existing server/store helper — read the first test in the file and build the server the same way):

```go
// rete#272 D3.3, deploy order: nodes are flashed before the gateway is restarted,
// so a handler that has never heard of #wak must still serve a pull that carries
// it. Written and run GREEN before handlePull was changed; after the change it
// is the ordinary happy path.
func TestPullCarryingACountIsServed(t *testing.T) {
	srv, _ := testServer(t)
	req, err := cval.Encode(cval.Record{Tag: "pull", Fields: cval.Map{
		{K: cval.Symbol("id"), V: "n1"},
		{K: cval.Symbol("cursor"), V: int64(0)},
		{K: cval.Symbol("bid"), V: int64(41)},
		{K: cval.Symbol("hw"), V: int64(0)},
		{K: cval.Symbol("wak"), V: int64(7)},
	}})
	if err != nil {
		t.Fatal(err)
	}
	if _, _, err := DecodeBatch(srv.Handle(req)); err != nil {
		t.Fatalf("a pull carrying #wak was not answered with a batch: %v", err)
	}
}
```

Run: `cd porta && go test ./internal/wire/ -run PullCarryingACount -count=1 -v`
Expected: **PASS** on the unchanged handler (probed 2026-10-03 while writing this plan: it does). If it fails, STOP: the deploy order in Task 5 is wrong and the plan must be re-read with David.

- [ ] **Step 2: Write the failing tests.** In `wire_test.go`:

```go
// Review Focus 1: closed decoding. A peer that cannot name its count is
// mis-versioned, exactly as one that cannot name its watermark is.
func TestPullWithoutACountIsRefused(t *testing.T) {
	srv, _ := testServer(t)
	req, _ := cval.Encode(cval.Record{Tag: "pull", Fields: cval.Map{
		{K: cval.Symbol("id"), V: "n1"},
		{K: cval.Symbol("cursor"), V: int64(0)},
		{K: cval.Symbol("bid"), V: int64(41)},
		{K: cval.Symbol("hw"), V: int64(0)},
	}})
	if msg := decodeErrReply(t, srv.Handle(req)); !strings.Contains(msg, "wak") { // refuse_test.go's reader
		t.Fatalf("reply = %q, want an #err naming wak", msg)
	}
}
```

Replace the whole of `porta/internal/space/progress_test.go` with:

```go
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
```

- [ ] **Step 3: Run them to verify they fail**

Run: `cd porta && go test ./internal/space/ ./internal/wire/ -count=1 2>&1 | tail -20`
Expected: build FAIL — `too many arguments in call to st.NoteCheckIn`, `st.ForgetMark undefined`.

- [ ] **Step 4: Implement**

`schema.go`, append to `addColumns` above the mark columns' comment:

```go
	// rete#272 D3.3 — the SAMPLE: the count the node sent in its latest pull, and
	// when porta heard it. The verdict reads these two and the mark, and nothing
	// else. NULL until the node's first counted pull.
	`ALTER TABLE nodes  ADD COLUMN wak    INTEGER`,
	`ALTER TABLE nodes  ADD COLUMN wak_ms INTEGER`,
```

`store.go`: `NoteCheckIn` gains the trailing parameter `wak int64`, and its final `UPDATE` becomes

```go
		`UPDATE nodes SET cursor_acked = ?, boot_id = ?, last_seen_ms = ?,
		        wak = ?, wak_ms = ?,
		        cursor_hw = MAX(cursor_hw, MIN(?, served_hw))
		  WHERE id = ?`,
		cursor, bootID, now, wak, now, cursor, nodeID); err != nil {
```

Delete `const PathHealth` and its comment.

`progress.go`: delete `LatestSample` and the `cval` import; rewrite `Sample`'s comment to "the count the node sent in a pull (`#wak`, one per check-in) and the moment porta heard it"; add:

```go
// ForgetMark drops the node's mark. A verdict past the budget does this: the
// conversation the mark belonged to is over, and a node that returns is judged
// from its first new sample, never across the silence (rete#272 D3.3).
func (s *Store) ForgetMark(nodeID string) error {
	_, err := s.db.Exec(
		`UPDATE nodes SET mark_wak = NULL, mark_ms = NULL, mark_cad = NULL WHERE id = ?`, nodeID)
	return err
}
```

`message.go`: `EncodePull(id string, cursor, bootID, hw, wak int64)` adds `{K: cval.Symbol("wak"), V: wak}`.

`server.go` `handlePull`, after the `hw` field is read:

```go
	// rete#272 D3.3 — the node's count of this boot's check-ins. Required for the
	// same reason hw is: decoding is closed, and a peer that cannot name it is an
	// image from before the verdict could judge progress.
	wak, err := intField(r.Fields, "wak")
	if err != nil {
		return nil, err
	}
```

and pass it: `s.st.NoteCheckIn(id, cursor, nodeHW, bootID, wak)`.

Call sites: every `EncodePull(a, b, c, d)` in tests becomes `EncodePull(a, b, c, d, 0)` and every test `NoteCheckIn(a, b, c, d)` becomes `NoteCheckIn(a, b, c, d, 0)` — a mechanical edit; `sweep_test.go` will not compile until Task 3 (its `pushHealth` is gone), so this task's package run excludes it by `-run`.

- [ ] **Step 5: Run to verify they pass**

Run: `cd porta && go vet ./internal/wire/ && go test ./internal/wire/ ./internal/portacli/ -count=1` → `ok` twice.
Run: `cd porta && go test ./internal/space/ -run 'NoteCheckIn|RecordExchangeLeaves|MarkIsAbsent' -count=1 -v` → three PASS. (`./internal/space/` as a whole does not build until Task 3 rewrites `sweep_test.go`; if the build blocks even `-run`, do Task 3 Step 1 first and commit the two tasks together — ledger the ruling.)

- [ ] **Step 6: Commit** (with Task 3 if the package could not build alone)

```bash
git add porta/internal/wire porta/internal/space porta/internal/portacli
git commit -m "porta(#272): the pull carries #wak, and it is the stored sample

refs #272"
```

---

### Task 3: The rule reads the sample

**Files:**
- Modify: `porta/internal/space/sweep.go` (`Sweep`'s query and loop, `verdictFor`, `judgeProgress`, the doc comments)
- Modify: `porta/internal/space/sweep_test.go`, `runs_test.go`

**Interfaces:**
- Consumes: `nodes.wak`/`wak_ms`, `ForgetMark`, `NoteCheckIn(…, wak)` (Task 2).
- Produces: `judgement` gains `forget bool`; `func (s *Store) verdictFor(nodeID string, lastSeen sql.NullInt64, latest *Sample, stored []byte, now int64) (judgement, error)`.

**The rule, in evaluation order:**

1. `!lastSeen.Valid` → `#neverSeen`. No declaration, or `bud < SweepTick` → `#unknown` (unchanged; `since` here is `now − lastSeen`).
2. `latest == nil` → `#unknown`: "checks in, but its pulls carry no tick count — this image predates rete#272".
3. `since := now − latest.AtMs`. `since > bud` → `#unknown` if deaf, else `#silent`. **The mark is forgotten.**
4. No mark → `#unknown` ("one sample says nothing about a rate"). **Mark := latest.**
5. `mark.CadMs ≠ telCad` → keep the stored verdict. **Mark := latest.**
6. `latest.AtMs == mark.AtMs` → keep the stored verdict. **The mark does not move.**
7. `WasDeaf(mark.AtMs, latest.AtMs)` → keep the stored verdict. **Mark := latest.** ("porta was not listening for part of the window: re-marked, not judged")
8. `elapsed − Δwak×telCad ≥ telCad` → `#stalled`, else `#live`. **Mark := latest.**

"Keep" with an empty stored verdict answers `#unknown`.

- [ ] **Step 1: Rewrite the test helpers.** In `sweep_test.go`, `fakeNode.tick` becomes the real shape of a check-in — a counted pull, then the two exchange rows:

```go
func (n *fakeNode) tick(t *testing.T, st *Store, clock *int64, gap int64) {
	t.Helper()
	*clock += gap
	n.wak++
	if _, _, err := st.NoteCheckIn(n.id, 0, 0, n.boot, n.wak); err != nil {
		t.Fatalf("NoteCheckIn: %v", err)
	}
	checkIn(t, st, clock, n.id, 0) // the pull and push exchange rows
}
```

Delete `fakeNode.seq` and its uses (the count no longer rides a seq'd entry); `goLive` keeps its body minus `seq: 1`. `declare` is unchanged.

- [ ] **Step 2: Write the failing tests.** Delete `TestSweepArrivingWithoutProgressIsUnknown`. Add:

```go
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

// Review Focus 3: porta is down 60 s; the 2 s node spins both reply budgets and
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
```

Also add `TestMarkSurvivesAReopen`: copy the open/close shape of `TestSweepVerdictSurvivesARestart`, call `goLive`, read `MarkOf`, close and reopen the store on the same path, and require the same `Mark`.

- [ ] **Step 3: Bring the existing tests onto the rule.** Every sweep test's node now speaks through `fakeNode` — a bare `checkIn` is an uncounted image and reads `#unknown`. Read each test before editing; the intent stays, the route to `#live`/`#silent` changes:

| Test | Change |
|---|---|
| `TestSweepRecordsTheReturnToLife` | drop `n.wak += 20`; after `#silent` the return is a NEW `fakeNode` (boot 42): first sweep `#unknown`, ticks, second sweep `#live`. Its claim becomes "the return to life is recorded", two transitions |
| `TestSweepAcrossAGatewayOutageIsUnknownAndNotSilent` (`runs_test.go`) | drop the `bell.wak +=` line; after the `#unknown`, `bell.tick` + sweep → no change (stored `#unknown`, one sample), `bell.ticks(…, 2, 30_000)` + sweep → `#live` |
| `TestSweepDoesNotCallAFastNodeSilentForOrdinaryBlockingWork` | unchanged in claim (6.5 s inside a 32 s budget is not `#silent`); it already uses `goLive` |
| `TestSweepWritesOnlyOnTransition`, `TestSweepVerdictSurvivesARestart`, `TestSweepIsNotStoppedByOneNodesFailure`, `TestSweepReasonCountsTheMisses`, `TestSweepDistinguishesNeverSeenFromSilent`, `TestSweepRefusesABudgetBelowItsOwnTick` | replace `checkIn`+`declare` openings with `goLive` (or, where the test wants `#silent` only, `declare` + one `fakeNode.tick`); add the `#unknown` row `goLive` writes to any row/changes count, with a comment |
| `TestSweepCatchesARebootAsStalled` | `re := &fakeNode{id: "n1", boot: 42}` (no `seq`) |

- [ ] **Step 4: Run to verify the new tests fail**

Run: `cd porta && go test ./internal/space/ -count=1 2>&1 | tail -30`
Expected: build FAIL (`too many arguments in call to s.verdictFor` or `j.forget undefined` once referenced), then — with only the signature stubbed — `TestSweepNodeWithoutACountIsUnknown` fails on its reason and `TestSweepReturnAfterSilenceIsJudgedAfresh` fails on "a silent verdict kept its mark".

- [ ] **Step 5: Implement in `sweep.go`**

`Sweep`'s query and row:

```go
	rows, err := s.db.Query(`
		SELECT n.id, n.last_seen_ms, n.wak, n.wak_ms, COALESCE(c.value_cbor, x'')
		FROM nodes n
		LEFT JOIN cells c ON c.node_id = n.id AND c.path = ?
		ORDER BY n.id`, PathLiveness)
```

with `wak, wakMs sql.NullInt64` scanned into the row, and in the loop:

```go
		var latest *Sample
		if n.wak.Valid && n.wakMs.Valid {
			latest = &Sample{Wak: n.wak.Int64, AtMs: n.wakMs.Int64}
		}
		j, err := s.verdictFor(n.id, n.lastSeen, latest, n.stored, now)
```

After the verdict write, replace the mark block:

```go
		switch {
		case j.forget:
			if err := s.ForgetMark(n.id); err != nil {
				errs = append(errs, fmt.Errorf("node %s: forget mark: %w", n.id, err))
			}
		case j.mark != nil:
			if err := s.SetMark(n.id, *j.mark); err != nil {
				errs = append(errs, fmt.Errorf("node %s: move mark: %w", n.id, err))
			}
		}
```

`judgement` gains `forget bool // the sweep must drop the node's mark`.

`verdictFor` — the head is unchanged through the `bud < tick` test (those two `#unknown`s keep `since := now - lastSeen.Int64`). Then:

```go
	// rete#272 D3.3: ONE sample. Everything below reads the count the node sent
	// in its latest pull and the moment porta heard it — and nothing else.
	if latest == nil {
		return judgement{want: livenessUnknown, verdict: "unknown", since: since, d: d,
			reason: "unknown: checks in, but its pulls carry no tick count — this image predates rete#272; reflash it"}, nil
	}
	since = now - latest.AtMs
	if since > bud {
		missed := float64(since) / float64(d.TelCadMs)
		deaf, err := s.WasDeaf(now-since, now)
		if err != nil {
			return judgement{}, fmt.Errorf("deaf intervals: %w", err)
		}
		// Past the budget the conversation the mark belonged to is over: forget
		// it, so a node that returns is judged from its first new sample.
		if deaf {
			return judgement{want: livenessUnknown, verdict: "unknown", since: since, d: d, forget: true,
				reason: fmt.Sprintf("unknown: %d ms since the last check-in, budget %d ms, and porta was not listening for part of it", since, bud)}, nil
		}
		return judgement{want: livenessSilent, verdict: "silent", since: since, d: d, forget: true,
			reason: fmt.Sprintf("silent: %d ms since the last check-in — %.1f missed check-ins of %d ms, %d tolerated", since, missed, d.TelCadMs, d.Misses)}, nil
	}
	return s.judgeProgress(nodeID, *latest, stored, since, d)
```

`judgeProgress(nodeID string, latest Sample, stored []byte, since int64, d Declaration)`: delete the `LatestSample` call, the `!ok` branch and the `gap >= d.TelCadMs` guard; after the `latest.AtMs == mark.AtMs` branch add:

```go
	// porta's own absence is not evidence about the node: a 2 s node ticks at
	// 4 s while nothing answers it (#271), and a window that spans an interval
	// porta was not listening would blame the node for the gateway's outage.
	deaf, err := s.WasDeaf(mark.AtMs, latest.AtMs)
	if err != nil {
		return judgement{}, fmt.Errorf("deaf intervals: %w", err)
	}
	if deaf {
		j.mark = next
		return keep("porta was not listening for part of the window: re-marked, not judged")
	}
```

Rewrite the comments that this makes false: the `Sweep` doc's rate paragraph ("the count the node sends in every pull"), `judgeProgress`'s header (the sample's source; D3.3), `verdictFor`'s "a node heard from inside its budget is #live" (`sweep.go:195`), and `store.go:272`'s "`#live` or `#silent`".

- [ ] **Step 6: Run the package**

Run: `cd porta && go test ./internal/space/ -count=1 -v 2>&1 | grep -E '^(--- |ok|FAIL)' | grep -v PASS; go test ./internal/space/ -count=1`
Expected: no FAIL line, then `ok`.

- [ ] **Step 7: Falsify the three new guards.** Each must fail for its own reason; restore after each and record the three outputs in the commit body.
  1. Delete `forget: true` from the `#silent` return → `-run ReturnAfterSilence` FAILS "a silent verdict kept its mark".
  2. Disable the deaf branch (`if false && deaf`) → `-run PortasOutage` FAILS with a `#stalled` change.
  3. Change `since = now - latest.AtMs` to `since = now - lastSeen.Int64` → `-run PushesWithoutPulls` FAILS (no `#silent`).

- [ ] **Step 8: Whole module, then commit**

Run: `cd porta && gofmt -l . ; go test ./... -count=1` → nothing, then every package `ok`.

```bash
git add porta/internal/space
git commit -m "space(#272): the verdict reads one sample — the pull's count

<the three falsification outputs, one line each>
refs #272"
```

---

### Task 4: The prose of record, the golden, and the three gates

**Files:**
- Modify: `protocol/PROTOCOL-NSL.md` (§3 step 1 "Pull", ~line 306), `protocol/PROTOCOL-NSL-RECORDS.md` (the `gw/liveness` row), `protocol/descriptors/records.yaml` (`gw-verdict`), `porta/docs/ARCHITECTURE.md` (the verdict table and the `#stalled` paragraph), `porta/docs/GETTING-STARTED.md:197`, `docs/tutorials/03-porta.md:317`
- Modify: `nodus/nsl/tools/fixtures/manifest_goldens.nsl`

**Interfaces:**
- Consumes: the behaviour Tasks 1–3 built. Every sentence written here must be one a test above pins.

- [ ] **Step 1: The protocol.** `PROTOCOL-NSL.md` §3 step 1: the pull declares `cursor`, `hw` **and `wak`** — "the count of this boot's check-ins, required; porta judges progress from it and from nothing else (rete#272 D3.3)". Say in one sentence why it is not in the push: the push frame is re-sent unchanged until acked (rete#199).

- [ ] **Step 2: The verdict's rows.** In `PROTOCOL-NSL-RECORDS.md`, `records.yaml` and `ARCHITECTURE.md`, replace every "`obs/health.wak`" in the `gw/liveness` text with "the count in the node's pull (`#wak`)". `#unknown`'s causes become: no declaration; a budget below the sweep tick; a gateway outage across the window; **no tick count in the node's pulls (an image from before rete#272)**; only one sample so far. Delete "its `obs/health` carries no `wak` or has stopped landing". `ARCHITECTURE.md`'s `#silent` row: "no counted pull for longer than its budget". Add to its `#stalled` paragraph: a verdict past the budget forgets the mark (replug = `#silent → #unknown → #live`), and a window spanning a porta outage is re-marked, not judged.
  `records.yaml` `gw-verdict.validity`: rewrite the sentence "A node uniformly refused by porta reads #live right up to its death" to what Task 3 makes true — a node whose **pulls** are refused sends no sample and reads `#silent` past its budget; one refused only on its **pushes** still reads `#live`. Keep the `ref` advice.

- [ ] **Step 3: The two guides.** `GETTING-STARTED.md:197` and `docs/tutorials/03-porta.md:317` list four verdicts: make them five, and add beside the tutorial's transcript (`:324`) that a node's first verdict is `#unknown` (one sample) and `#live` follows at the next sweep. The transcript is dated evidence; do not edit it.

- [ ] **Step 4: The golden.** In `manifest_goldens.nsl`, add a second `gw/liveness` instance encoding `#stalled` beside the `#live` one, with its `GOLDEN gw/liveness d827677374616c6c6564` expect line (the bytes are `cval.Encode(Symbol("stalled"))`; confirm against `TestVerdictEncodingsAreTheAlgebrasSymbols`).

- [ ] **Step 5: The gates, in order, none concurrently**

```bash
python3 protocol/tools/manifest_lint.py protocol/MANIFEST.cddl          # rc 0
cd porta && go test ./... -count=1                                       # every package ok
cd nodus/oevm && bash tests/porta_sync_host.sh                           # PORTA SYNC OK
cd nodus/nsl && python3 -u tools/run_tests_oevm.py > /tmp/claude-1000/nsl-272b.log 2>&1   # background
```

Expected: the seam gate's `gw/liveness reads #live` line — a real host node reaching `#live` through the count in its pull; the suite `Failed: 0` and every aux gate PASS.

- [ ] **Step 6: Commit**

```bash
git add protocol porta/docs docs/tutorials nodus/nsl/tools/fixtures/manifest_goldens.nsl
git commit -m "docs(#272): the count rides the pull — protocol, catalogue, architecture, guides

refs #272"
```

---

### Task 5: A board, then the fleet (David-gated where marked)

- [ ] **Step 1: The board gate.** `kernel.nsl` changed, so: `nodus/oevm/tests/target/run.sh bridge-health` on `c6bench` (it is on gw85224-01; resolve the board through its collector env file). Expected: the scenario's TAPs and its observation window pass. This is the first time the new pull crosses a real radio.
- [ ] **Step 2: Nodes first.** Flash the bridge image to `c6bench`, then `nrf4ec0` and `nrf92c2` (Task 2 Step 1 is the proof the deployed gateway serves them). Read each node's `porta get <id> obs` after the flash: it must still be checking in.
- [ ] **Step 3 (David): the gateway.** Open memory `porta-gateway-operational-state` and copy its three-call restart sequence; never a kill and a start in one ssh call. Deploy the new binary to `~/porta-v2`. Watch `porta log` for `gw/liveness` across three sweeps: every reflashed node `#unknown` (one sample) → `#live`; **no `#stalled` from the restart itself** (Review Focus 3, now on real nodes).
- [ ] **Step 4 (David): the bells.** The C6 bell (`ShipsBellAlarmNodeC6`) reads `#unknown … predates rete#272; reflash it` from Step 3 until it is flashed. Until then every one of its pulls is refused (`#err` "missing #wak"), so **its cursor never advances and no `goal/**` write reaches it**, and porta logs one "rejecting datagram" line per pull (~2.9k/day at 30 s). Flash it — daylight, `zephyr/flash-bell.sh`, re-read `flashing-an-actuator-node` first. The MagTag bell has no porta bridge today, so it is not on this list; putting it on porta is its own question (a node that cold-boots every wake restarts its count each time).
- [ ] **Step 5: What the fleet says.** After 24 h: `porta log` filtered to `gw/liveness`, one line per node — rows by verdict, and for each `#stalled` its reason. Post the table on #272. This is the datum D3.3's "left to the fleet" list is waiting for.

### Task 6: Close-out

- [ ] **Step 1:** `tools/docmap.sh`; `review/DOCMAP.md` is David's tree — list the diff for him, do not commit it.
- [ ] **Step 2:** Update #325 (the end-to-end `#stalled` gate): with the count on the pull a node with no reply path now delivers samples, so the gate is buildable as written.
- [ ] **Step 3:** Comment on #272 (≤5 lines): commits, the test names, and the mainline docs that absorbed the knowledge (`protocol/PROTOCOL-NSL.md` §3, `porta/docs/ARCHITECTURE.md`, the model README, design D3.3). Leave it open until Task 5 Step 5's table is posted.
