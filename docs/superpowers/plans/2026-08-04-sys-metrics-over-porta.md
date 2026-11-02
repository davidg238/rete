# sys/metrics over porta — v1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A node publishes its VM internals at `sys/metrics` on every porta check-in, and
`obs/health` stops duplicating them.

**Architecture:** Three changes and no more. The bridge's cell scan gains a second rooted
capability (`sys` beside `obs`), so capability confinement does the filtering with no new
code. `PortaBridge>>checkIn` composes `platform metrics` and writes it to `sys/metrics`
after applying porta's batch and before pushing, so the record describes the state porta
just asked for. `obs/health` drops the four fields that already live in `sys/metrics`.

**Tech Stack:** nsl (`nodus/nsl/lib/kernel.nsl`), the tuvm VM, porta (Go, unchanged —
`store.go:215` already upserts a cell at any path).

**Design:** `~/workspace/rete/docs/design/2026-08-04-metrics-over-porta-design.md`
(D1–D9). Read D6 before adding any cadence logic: escalation is **deliberately staged
out** of v1 and adding it back here is a scope violation, not initiative.

## Global Constraints

- **nsl dialect:** `[ ... ] whileTrue:`, never `repeat`. No `#( )` literal arrays. No
  modulo. State in instance slots so every reaction block is a one-line self-send (the
  reified-block `LD_SLOT` gotcha, rete#133).
- **`PortaBridgeNode.nsl` and `PortaBridgeNodeC6.nsl` are twins.** Their own header says
  *"A change to the body of either file is a change to both."* Every edit to one is an
  edit to the other.
- **No backward compatibility is required** (design D9). Change shapes outright; do not
  add versioned fields, dual-reads or deprecation windows.
- **Never run two nsl suites concurrently** — they share `/tmp` paths.
- **`run-one.py` dirties `nodus/tuvm/src/solidified_image.c` and `.stmap`.** Restore them
  with `git checkout --` before every commit.
- **Gates**, all three green before the final commit:
  - `cd nodus/nsl && python3 tools/run_tests_oevm.py` → `Passed: N / Failed: 0`, **and
    read the aux-gate line** — it can be red while the count reads zero failures.
    `footprint(T0.3): SKIP` is pre-existing and expected.
  - `cd nodus/tuvm && bash tests/porta_sync_host.sh` → `PORTA SYNC OK`
  - `cd porta && go test ./...`
- **Inner loop:** `nodus/nsl/tools/run-one.py <file.nsl>` runs ONE test.
  `run_tests_oevm.py` ignores argv and always globs the whole ~2–3 min suite. A `SKIP`
  proves nothing — it returns before compiling.
- **Test file convention:** expected stdout is an inline `(* expect: *)` block at the TOP
  of the `.nsl`; the test is a CLASS with `main: platform args:` printing via `println`
  or `out`. There are ZERO `.expected` files. Copy a sibling before writing one.

---

## File Structure

| file | responsibility | task |
|---|---|---|
| `nodus/nsl/lib/kernel.nsl` | `PortaBridge>>collectOutbound` scans `obs` + `sys`; `checkIn` publishes `sys/metrics` | 1, 2 |
| `nodus/tests/22_spacesync/07_sys_cells_push.nsl` | new — a `sys/` cell reaches the push frame, `goal/` does not | 1 |
| `nodus/tests/22_spacesync/08_metrics_on_checkin.nsl` | new — a check-in publishes the pinned `#Metrics` shape | 2 |
| `nodus/tuvm/zephyr/PortaBridgeNode.nsl` | `publishHealth` drops to 5 fields | 3 |
| `nodus/tuvm/zephyr/PortaBridgeNodeC6.nsl` | twin of the above | 3 |
| `protocol/PROTOCOL-NSL.md` | §4.2 push rule (D1) and the `obs/health` row (D3) | 1, 3 |
| `protocol/PROTOCOL-NSL-RECORDS.md` | the `obs/health` row (D2) | 3 |
| `nodus/tests/22_spacesync/09_batch_size.nsl` | new — batch size regression guard | 4 |
| `nodus/tuvm/tests/porta_sync_host.sh` | asserts `sys/metrics` lands and events accumulate | 5 |
| `review/tests/CLAIMS.md` | the promotion row (close-out) | 6 |

---

### Task 1: The cell scan reaches `sys/`

**Files:**
- Modify: `nodus/nsl/lib/kernel.nsl` — `PortaBridge>>collectOutbound`, currently at 1700–1712
- Modify: `protocol/PROTOCOL-NSL.md:248`
- Test: `nodus/tests/22_spacesync/07_sys_cells_push.nsl` (create)

**Interfaces:**
- Consumes: `Channel>>changedSince:max:` (answers cells only — `space.c:486`), rows shaped
  `(path, seq, value)`.
- Produces: `PortaBridge>>addCellsFrom:into:`, used again by nothing — it exists to keep
  `collectOutbound` readable with two prefixes. Wire rows stay `(seq, path, kind, value)`,
  which is what `applyEntry:` reads.

- [ ] **Step 1: Write the failing test**

Create `nodus/tests/22_spacesync/07_sys_cells_push.nsl`:

```
(*
porta space-sync — the cell scan covers the node's OWN authority, which is `sys/`
as well as `obs/`. Before this, RECORDS §4.4 pinned sys/metrics as a CELL while the
scan only ever looked at obs/**, so the value was unsendable by construction and no
node app ever published it (design 2026-08-04 §3, D1).

goal/ must NOT appear: it is porta's authority, and echoing porta's own writes back
at it is the one thing the direction rule forbids. Confinement does that filtering —
the bridge holds two ROOTED capabilities, so there is no prefix test in the code.
expect:
rows 2
has obs/health true
has sys/metrics true
has goal/name false
*)
class SysCellsPush packageUsing: manifest = () (
  has: rows path: p = (
    | i |
    i:: 1.
    [ i <= rows size ] whileTrue: [
      ((rows at: i) at: 2) = p ifTrue: [ ^ true ].
      i:: i + 1 ].
    ^ false )

  public main: platform args: args = (
    | space bridge rows |
    space:: platform space.
    space put: 'goal/name' value: 'bench'.
    space put: 'obs/health' value: 11.
    space put: 'sys/metrics' value: 22.

    bridge:: platform PortaBridge.
    bridge setSocket: nil space: space id: 'test' peer: '::1' port: 1.
    rows:: bridge collectOutbound.

    ('rows ' , rows size printString) println.
    ('has obs/health ' , (self has: rows path: 'obs/health') printString) println.
    ('has sys/metrics ' , (self has: rows path: 'sys/metrics') printString) println.
    ('has goal/name ' , (self has: rows path: 'goal/name') printString) println.
  )
) : (
)
```

- [ ] **Step 2: Run it and confirm it fails for the right reason**

```bash
cd ~/workspace/rete/nodus/nsl
python3 tools/run-one.py ../tests/22_spacesync/07_sys_cells_push.nsl
```

Expected: FAIL with `rows 1` and `has sys/metrics false`. If it fails any other way —
especially a compile error — fix that first; a test that fails for the wrong reason
proves nothing about the change you are about to make.

Note: `collectOutbound` is not currently `public`. If the test cannot reach it, mark it
`public` in the same edit as Step 3 — it is the seam under test.

- [ ] **Step 3: Widen the scan**

In `nodus/nsl/lib/kernel.nsl`, replace `collectOutbound` with:

```
    (* Both prefixes are the NODE's authority (PROTOCOL-NSL §4.1): obs/** is what it
       observes, sys/** is what it reports about itself. goal/** is porta's and is
       excluded by CONSTRUCTION rather than by a test — each scan runs against a
       capability rooted at its own prefix, so a rooted channel cannot learn that
       `goal/name` was written (22_spacesync/02). There is no prefix check here to
       get wrong.

       maxRows applies PER PREFIX now, so up to 2 x maxRows rows can enter before
       pushFrame trims by bytes. That is safe and it is not free: see the starvation
       note on pushFrame. *)
    public collectOutbound = (
      | rows i |
      rows:: OrderedCollection new.
      i:: 1.
      [ i <= pending size ] whileTrue: [ rows add: (pending at: i). i:: i + 1 ].
      self addCellsFrom: 'obs' into: rows.
      self addCellsFrom: 'sys' into: rows.
      ^ self sortedBySeq: rows )

    addCellsFrom: prefix into: rows = (
      | cells i r |
      cells:: (space channel: prefix) changedSince: acked max: self maxRows.
      i:: 1.
      [ i <= cells size ] whileTrue: [
        r:: cells at: i.
        (self valueFits: (r at: 3))
          ifTrue: [ rows add: (List with: (r at: 2) with: (r at: 1) with: 2 with: (r at: 3)) ]
          ifFalse: [ rejected:: rejected + 1 ].
        i:: i + 1 ].
      ^ self )
```

- [ ] **Step 4: Run the test and confirm it passes**

```bash
cd ~/workspace/rete/nodus/nsl
python3 tools/run-one.py ../tests/22_spacesync/07_sys_cells_push.nsl
```

Expected: PASS.

- [ ] **Step 5: Record the trim-starvation limitation next to the code that causes it**

Append to the `pushFrame` comment in `kernel.nsl` (the block beginning *"Then TRIM FROM
THE TAIL"*):

```
       Known and accepted for v1: trimming from the tail drops the HIGHEST seq first,
       and sys/metrics is written last on every check-in, so it always holds the
       highest seq — under batch pressure it is the first thing dropped. Measured
       headroom is 87 B of 512 with two cells and one tel/log row (design §9.1), so
       this does not bite today. It starts to bite when a node has many obs/ paths,
       and the fix then is a budget shared across prefixes, not a bigger cap.
```

- [ ] **Step 6: Correct the conformance target**

In `protocol/PROTOCOL-NSL.md`, line 248 currently reads:

```
2. **Push** — send cell `obs/**` values changed since the last acked push, plus
```

Replace with:

```
2. **Push** — send cell `obs/**` and `sys/**` values changed since the last acked push,
   plus
```

- [ ] **Step 7: Restore build artifacts and commit**

```bash
cd ~/workspace/rete
git checkout -- nodus/tuvm/src/solidified_image.c nodus/tuvm/src/solidified_image.stmap
git add nodus/nsl/lib/kernel.nsl nodus/tests/22_spacesync/07_sys_cells_push.nsl protocol/PROTOCOL-NSL.md
git commit -m "feat(#56): the cell scan reaches sys/, so a pinned record can finally be sent"
```

---

### Task 2: A check-in publishes `sys/metrics`

**Files:**
- Modify: `nodus/nsl/lib/kernel.nsl` — `PortaBridge>>checkIn`, currently at 1763–1771
- Test: `nodus/tests/22_spacesync/08_metrics_on_checkin.nsl` (create)

**Interfaces:**
- Consumes: `Kernel>>metrics` (`kernel.nsl:2970`) — answers `Record tag: #Metrics` with
  the 11 fields RECORDS §4.4 pins. Reached from inside `PortaBridge` as `outer metrics`,
  the same way `outer rankOf:` is reached today.
- Produces: `PortaBridge>>publishMetrics`, called by `checkIn`.

- [ ] **Step 1: Write the failing test**

Create `nodus/tests/22_spacesync/08_metrics_on_checkin.nsl`:

```
(*
porta space-sync — the bridge publishes its own VM internals. #56 capability (3):
"metrics die with the node", and /stats is a PULL, which is a human affordance —
an agent arrives after the corpse is cold.

Published by the BRIDGE, not the app (design D4): the bridge is handed `space` as a
constructor capability, so it does not conjure space access, and it already owns the
check-in — the wake everything must ride (D7). Every bridged app inherits metrics
without re-implementing anything.

The socket is nil, so checkIn returns before any I/O; publishMetrics is called
directly here, which is the seam under test.
expect:
tag Metrics
heapSize present true
queueHighWater present true
pulseDrops present true
*)
class MetricsOnCheckIn packageUsing: manifest = () (
  public main: platform args: args = (
    | space bridge m |
    space:: platform space.
    bridge:: platform PortaBridge.
    bridge setSocket: nil space: space id: 'test' peer: '::1' port: 1.
    bridge publishMetrics.

    m:: space peek: 'sys/metrics'.
    ('tag ' , m tag printString) println.
    ('heapSize present ' , ((m at: #heapSize ifAbsent: [ nil ]) == nil) not printString) println.
    ('queueHighWater present ' , ((m at: #queueHighWater ifAbsent: [ nil ]) == nil) not printString) println.
    ('pulseDrops present ' , ((m at: #pulseDrops ifAbsent: [ nil ]) == nil) not printString) println.
  )
) : (
)
```

If `space peek:` is not the reader on a rooted space in this dialect, copy the read used
by `nodus/tests/15_heap_stats/02_sys_metrics.nsl`, which already round-trips this exact
record through a cell.

- [ ] **Step 2: Run it and confirm it fails**

```bash
cd ~/workspace/rete/nodus/nsl
python3 tools/run-one.py ../tests/22_spacesync/08_metrics_on_checkin.nsl
```

Expected: FAIL — `publishMetrics` does not exist.

- [ ] **Step 3: Add `publishMetrics` and call it from `checkIn`**

In `nodus/nsl/lib/kernel.nsl`, add to `PortaBridge`:

```
    (* #56 capability (3). The record is built by the kernel and WRITTEN by the
       bridge, which holds `space` because the app granted it at construction —
       the kernel never conjures space access.

       v1 publishes on EVERY check-in. Escalating on degradation was designed and
       deliberately staged out (design D6): it needed five invented constants, and
       D7 already capped its worst case at exactly this — one record per check-in.
       The ceiling of the elaborate design is the simple design, and it is the one
       setting whose cost can be measured. Do not add cadence logic here without
       the data D6 names. *)
    publishMetrics = (
      space put: 'sys/metrics' value: outer metrics.
      ^ self )
```

Then in `checkIn`, publish AFTER the batch is applied and BEFORE the push:

```
    public checkIn = (
      | reply |
      socket == nil ifTrue: [ ^ self ].
      socket sendTo: peerHost port: peerPort data: self pullFrame.
      reply:: self awaitReply: #batch.
      reply == nil
        ifTrue: [ misses:: misses + 1 ]
        ifFalse: [ self applyBatch: reply ].
      (* In THIS order: applying goal/** first means the metrics this node publishes
         describe the state porta just asked for, not the one before it — the same
         reasoning PortaBridgeNodeC6>>tick already gives for publishHealth. *)
      self publishMetrics.
      ^ self pushOnce )
```

- [ ] **Step 4: Run the test and confirm it passes**

```bash
cd ~/workspace/rete/nodus/nsl
python3 tools/run-one.py ../tests/22_spacesync/08_metrics_on_checkin.nsl
```

Expected: PASS.

- [ ] **Step 5: Restore build artifacts and commit**

```bash
cd ~/workspace/rete
git checkout -- nodus/tuvm/src/solidified_image.c nodus/tuvm/src/solidified_image.stmap
git add nodus/nsl/lib/kernel.nsl nodus/tests/22_spacesync/08_metrics_on_checkin.nsl
git commit -m "feat(#56): the bridge reports its own internals, every check-in"
```

---

### Task 3: `obs/health` stops duplicating the VM internals

**Files:**
- Modify: `nodus/tuvm/zephyr/PortaBridgeNodeC6.nsl:110-125` (`publishHealth`)
- Modify: `nodus/tuvm/zephyr/PortaBridgeNode.nsl` (the twin — same method)
- Modify: `protocol/PROTOCOL-NSL-RECORDS.md:231`
- Modify: `protocol/PROTOCOL-NSL.md:327`

**Interfaces:**
- Produces: `Health { uptimeMs, bootId, wakes, pollTimeouts, streamDropped }` — 5 fields,
  87 B encoded (design §9.1). Task 4's size guard asserts that number.

- [ ] **Step 1: Slim `publishHealth` in BOTH node files**

Replace the body in `PortaBridgeNodeC6.nsl` **and** `PortaBridgeNode.nsl`:

```
  (* Liveness and delivery ONLY (design D2). heapFree/heapLargest/overruns LEFT this
     record on 2026-08-04 — they are not duplicated here, they are deleted, because
     every one of them already lives in sys/metrics and a node publishing two
     overlapping health records is how obs/health came to have three mutually
     inconsistent definitions.

     streamDropped stays: it counts information lost ON THE WAY OUT, which is the
     number that says the observability itself is lying — a liveness fact, not a VM
     internal. *)
  publishHealth = (
    | pairs |
    pairs:: platform OrderedCollection new.
    pairs add: #uptimeMs -> platform monotonicMillis.
    pairs add: #bootId -> 1.
    pairs add: #wakes -> ticks.
    pairs add: #pollTimeouts -> bridge misses.
    pairs add: #streamDropped -> bridge dropped.
    space put: 'obs/health' value:
      (platform Record tag: #Health fields: (platform Map fromPairs: pairs)).
    ^ self
  )
```

- [ ] **Step 2: Correct both protocol documents**

`protocol/PROTOCOL-NSL-RECORDS.md:231` becomes:

```
| `obs/health` | R | `Health { uptimeMs: int, bootId: int, wakes: int, pollTimeouts: int, streamDropped: int }` — liveness and delivery only; the VM internals live at `sys/metrics` (§4.4). **87 B encoded, MEASURED 2026-08-04** |
```

`protocol/PROTOCOL-NSL.md:327` becomes:

```
| `health` | `obs/health` | cell | `{uptimeMs:, bootId:, wakes:, pollTimeouts:, streamDropped:}` |
```

The unit change `uptimeUs` → `uptimeMs` is deliberate: D4.2 makes milliseconds the rule
and nothing downstream has ever used microseconds.

- [ ] **Step 3: Run the whole suite — this changes a record two gates read**

```bash
cd ~/workspace/rete/nodus/nsl && python3 tools/run_tests_oevm.py 2>&1 | tail -12
```

Expected: `Failed: 0`, and the aux-gate line all PASS except the expected
`footprint(T0.3): SKIP`. If a test asserts the old 8-field `obs/health`, update the test —
D9 says there is no compatibility to keep.

- [ ] **Step 4: Restore build artifacts and commit**

```bash
cd ~/workspace/rete
git checkout -- nodus/tuvm/src/solidified_image.c nodus/tuvm/src/solidified_image.stmap
git add nodus/tuvm/zephyr/PortaBridgeNode.nsl nodus/tuvm/zephyr/PortaBridgeNodeC6.nsl protocol/
git commit -m "feat(#56): obs/health is liveness again — the VM internals live at sys/metrics"
```

---

### Task 4: The batch size regression guard

**Files:**
- Test: `nodus/tests/22_spacesync/09_batch_size.nsl` (create)

A measured size protects nothing if nothing re-checks it. §9.1 took the measurement; this
lands it as a guard, the same job `19_latency/05_record_size.nsl` does for `#Metrics`.

- [ ] **Step 1: Write the test with the MEASURED numbers as assertions**

Create `nodus/tests/22_spacesync/09_batch_size.nsl` with exactly this content — the
numbers in the `expect:` block are the ones measured on 2026-08-04:

```
(*
expect:
health 87
metrics 195
batch 425
*)
(* #56 design §9.1, landed as a regression guard. NOT a sum of record sizes: the
   thing that must fit 512 B is the PUSH FRAME, envelope and rows included, so this
   builds the frame PortaBridge>>encodePush: builds and encodes it through the real
   Cbor path.

   Values are for a node that has been up THIRTY DAYS, because CBOR is shortest-form
   and a fresh node encodes narrower than a long-lived one — measuring the easy case
   is how an estimate becomes wrong in the field.

   `metrics 195` is the CONTROL: the value RECORDS §4.4 pinned on 2026-07-21 through
   a different test. If it moves, every other number here is suspect and the failure
   is not in this file.

   Note: `platform Cbor` answers an INSTANCE where `platform Spans` answers the
   CLASS — an asymmetry worth knowing before copying either line. *)
class BatchSize packageUsing: manifest = () (

  health: platform = (
    | pairs |
    pairs:: platform OrderedCollection new.
    pairs add: #uptimeMs -> 2592000000.      (* 30 days *)
    pairs add: #bootId -> 1.
    pairs add: #wakes -> 1296000.            (* 30 d at the 2 s cadence *)
    pairs add: #pollTimeouts -> 431.
    pairs add: #streamDropped -> 17.
    ^ platform Record tag: #Health fields: (platform Map fromPairs: pairs) )

  (* The eleven fields kernel.nsl builds, with the WROOM-32 values 05_record_size.nsl
     measured, so the 195 B control reproduces exactly. *)
  metrics: platform = (
    | pairs |
    pairs:: platform OrderedCollection new.
    pairs add: #heapSize -> 53248.
    pairs add: #liveBytes -> 36196.
    pairs add: #heapHighWater -> 53228.
    pairs add: #largestFreeBlock -> 18608.
    pairs add: #gcCycles -> 1523.
    pairs add: #handlesLive -> 1.
    pairs add: #handlesHighWater -> 1.
    pairs add: #queueHighWater -> 2.
    pairs add: #overruns -> 0.
    pairs add: #pulseDrops -> 0.
    pairs add: #pulseCoalesced -> 0.
    ^ platform Record tag: #Metrics fields: (platform Map fromPairs: pairs) )

  log: platform = (
    ^ platform Record tag: #Log with: #level -> #info
                              with: #text -> 'tick 1296000' )

  row: platform seq: s path: p value: v = (
    ^ platform List with: s with: p with: 2 with: v )

public main: platform args: args = (
  | cbor rows |
  cbor:: platform Cbor.
  ('health ' , (cbor encode: (self health: platform)) byteSize printString) println.
  ('metrics ' , (cbor encode: (self metrics: platform)) byteSize printString) println.

  rows:: platform OrderedCollection new.
  rows add: (self row: platform seq: 1296000 path: 'obs/health' value: (self health: platform)).
  rows add: (self row: platform seq: 1296001 path: 'sys/metrics' value: (self metrics: platform)).
  rows add: (self row: platform seq: 1296002 path: 'tel/log' value: (self log: platform)).
  ('batch ' , (cbor encode: (platform Record tag: #push
      with: #id -> 'c6-bench'
      with: #bootId -> 1
      with: #entries -> (platform List fromCollection: rows))) byteSize printString) println.
)
) : (
)
```

- [ ] **Step 2: Run it and confirm it passes first time**

```bash
cd ~/workspace/rete/nodus/nsl
python3 tools/run-one.py ../tests/22_spacesync/09_batch_size.nsl
```

Expected: PASS immediately — this guards a measurement already taken, so unlike every
other task here it is not expected to fail first. If it does fail, the numbers moved
between the measurement and Task 3, and THAT is the finding: do not edit the expected
values to match. Find out what changed.

- [ ] **Step 3: Restore build artifacts and commit**

```bash
cd ~/workspace/rete
git checkout -- nodus/tuvm/src/solidified_image.c nodus/tuvm/src/solidified_image.stmap
git add nodus/tests/22_spacesync/09_batch_size.nsl
git commit -m "test(#56): the batch is 425 B of 512, and now something re-checks it"
```

---

### Task 5: The wire gate asserts metrics arrive

**Files:**
- Modify: `nodus/tuvm/tests/porta_sync_host.sh`

The host gate already drives a real node against a real porta and prints
`obs/health is in porta's cell tree`. Two new assertions.

- [ ] **Step 1: Assert `sys/metrics` reaches porta's cell tree**

Add after the existing `obs/build` assertion (currently line 89), matching its style —
same `porta get`, same failure message shape, same dump-on-failure:

```bash
for _ in $(seq 1 50); do
  /tmp/porta get --db "$DB" "$NODE" sys 2>/dev/null | grep -q "sys/metrics" && break; sleep 0.2
done
/tmp/porta get --db "$DB" "$NODE" sys | grep -q "sys/metrics" || {
  echo "FAIL: sys/metrics never reached porta"; /tmp/porta get --db "$DB" "$NODE"; cat /tmp/porta_serve.log; exit 1; }
echo "  sys/metrics is in porta's cell tree"
```

A gate that passes whether or not the feature works is not a test: this asserts the
path is PRESENT rather than asserting no error occurred.

- [ ] **Step 2: Assert the run-up accumulates in the event log**

This is design §4's claim — verified by reading `store.go:220`, never asserted. Add
near the existing `tel/log` row counting (currently around line 116):

```bash
METRIC_ROWS=$(/tmp/porta log --db "$DB" "$NODE" --limit 200 | grep -c 'sys/metrics' || true)
[ "$METRIC_ROWS" -ge 2 ] || {
  echo "FAIL: sys/metrics appears $METRIC_ROWS time(s) in the event log, expected >= 2."
  echo "      A cell push must be APPENDED, not collapsed to latest-value — design §4"
  echo "      rests on that, and if it is false the run-up is not retained for free."
  /tmp/porta log --db "$DB" "$NODE" --limit 200; exit 1; }
echo "  sys/metrics accumulates in the event log: $METRIC_ROWS rows — the run-up is retained"
```

Guard the comparison: `[ "$METRIC_ROWS" -ge 2 ]` errors into a printed success if the
variable is empty or non-integer, which is one of the nine ways a hardware gate goes
green testing nothing. `|| true` on the `grep -c` keeps it `0` rather than unset.

If this assertion fails, §4's simplification is wrong and the design needs a history
mechanism after all. That is precisely why it is asserted rather than assumed.

- [ ] **Step 3: Run the gate**

```bash
cd ~/workspace/rete/nodus/tuvm && bash tests/porta_sync_host.sh
```

Expected: `PORTA SYNC OK`, with the two new lines printed.

- [ ] **Step 4: Commit**

```bash
cd ~/workspace/rete
git add nodus/tuvm/tests/porta_sync_host.sh
git commit -m "test(#56): the gate witnesses metrics arriving, and the run-up accumulating"
```

---

### Task 6: Gates and promotion

Per CLAUDE.md's promotion rule, this is not closed until the durable knowledge leaves
`docs/superpowers/` for a mainline document of record.

- [ ] **Step 1: Run all three gates from clean**

```bash
cd ~/workspace/rete/nodus/nsl && python3 tools/run_tests_oevm.py 2>&1 | tail -12
cd ~/workspace/rete/nodus/tuvm && bash tests/porta_sync_host.sh
cd ~/workspace/rete/porta && go test ./...
```

Read the aux-gate line, not only the pass count.

- [ ] **Step 2: Add the CLAIMS row**

Add a row to `review/tests/CLAIMS.md` naming: the claim (a node publishes its VM
internals at `sys/metrics` every check-in, and `obs/health` carries liveness only), the
tests that enforce it (`22_spacesync/07`, `08`, `09`, and `porta_sync_host.sh`), the code
that implements it, and the design document. Match the format of the existing tuvm#24
row, which is the nearest sibling.

- [ ] **Step 3: Note the deferred work on #56**

Comment on #56 recording that capability (3) has landed in its v1 form, that escalation
is staged with the three named observations that would earn it back (design D6), and
that capability (4) — the device test harness — is now unblocked.

- [ ] **Step 4: Commit**

```bash
cd ~/workspace/rete
git add review/tests/CLAIMS.md
git commit -m "docs(#56): promote the metrics claim to CLAIMS"
```

---

## Out of scope — do not build these

Named because each is a plausible-looking addition that the design deliberately excludes:

- **Escalating cadence** (D6). Five invented constants, staged until data earns them.
- **Retained RAM / flash-backed run-up** (§10). The v2 refinement, with a named blind
  spot: it survives a panic, an OOM or a watchdog, and not a brownout.
- **`bootId` as a real counter** (§5). Needs NVS.
- **Giving the bell node a bridge** (D8). The next change, not this one — the bell is
  mid-soak and is the only witness for the midnight slot wrap and the post-midnight NTP
  sync.
- **Any porta-side change.** `store.go:215` already upserts a cell at any path, and
  `store.go:220` already appends every entry. If you find yourself editing Go, stop and
  re-read design §3 and §4.
