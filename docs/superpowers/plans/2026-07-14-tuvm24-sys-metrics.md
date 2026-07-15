# tuvm#24 sys/metrics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Export handle/queue/overrun telemetry beside the existing heap telemetry, compose one flat `Metrics` record in the kernel, publish it via a `sys/metrics` **cell**, and tighten T-D.5 to a strict heap-high-water plateau — closing tuvm#24 and the residues of #23 and #20.

**Architecture:** Two new incrementally-maintained C counters (`handles.c` live/high-water, `space.c` ring-occupancy high-water) surface through **one new 0-arg prim `sysStats`** (the prim-104 pattern). `kernel metrics` composes prim 104 + `sysStats` into a 9-field immutable Record; a job holding the granted `sys/metrics` channel publishes it with `put:value:` (rung-9 cell). No periodic publisher ships (G9). Spec: `nsl-tuvm/docs/design/2026-07-14-tuvm24-sys-metrics-design.md`.

**Tech Stack:** C99 (tuvm VM), nsl (`lib/kernel.ns`), nsl-tests harness, porta protocol docs.

## Global Constraints

- **Four sibling repos, all on `master`** (P0-sweep precedent — no feature branch): `~/workspace/tuvm` (C VM), `~/workspace/nsl-tuvm` (kernel + tools), `~/workspace/nsl-tests` (suite), `~/workspace/porta` (protocol docs). Commit per repo, message prefix `feat(tuvm#24):` / `test(tuvm#24):` / `docs(tuvm#24):`.
- **Prim ids are append-only.** The new id is arrived at by **COUNTING the `PRIM_NAMES[]` entries in `tuvm/src/dispatch.c`** (108 entries exist today, `"spaceOverruns:"` last ⇒ the new prim is the 109th entry, id 109) — never by last+1 arithmetic. Enum position and `PRIM_NAMES` position must match 1:1.
- **A new platform prim has FIVE sites, not four:** `tuvm/src/prim_ids.h` define · `tuvm/src/dispatch.c` enum member · `tuvm/src/dispatch.c` `PRIM_NAMES[]` string · `tuvm/src/platform.c` case · **`nsl-tuvm/lib/kernel.ns` public Kernel forwarder**. Forgetting site 5 = runtime DNU that mimics a dispatch-table bug.
- **Suite invocation rules** (all have burned us): run the gate from the **nsl-tuvm** cwd (`OEVM_PATH=AB python3 tools/run_tests_oevm.py`); first line must say `Discovered ~200` (92 means you ran `tuvm/tools/`'s copy against the frozen oracle); **never** run concurrently with a `west build`; **never** wrap in `timeout` or pipe to `head`/`tail` — redirect to a file and poll; it has **no single-test filter**.
- **Single-.ns fast check** (Path A) — compile in place, build the runner, ALWAYS restore the clobbered image:
  ```bash
  cd /home/david/workspace/nsl-tuvm
  python3 tools/ns_compile_oevm.py ../nsl-tests/<path>.ns /home/david/workspace/tuvm/src/solidified_image.c
  cd /home/david/workspace/tuvm
  gcc -Wall -std=c99 -Isrc -DOEVM_APPLIANCE \
    src/vm.c src/gc.c src/dispatch.c src/platform.c src/space.c src/handles.c \
    src/serial.c src/udp.c src/mirror.c \
    src/backend/posix/platform_posix.c src/backend/posix/serial_posix.c \
    src/backend/posix/udp_posix.c src/backend/posix/pulse_posix.c \
    src/backend/posix/timer_posix.c \
    src/solidified_image.c src/oevm_main.c -o /tmp/runner -lm
  timeout 10 /tmp/runner ; echo "exit: $?"
  git checkout -- src/solidified_image.c src/solidified_image.stmap
  ```
  nsl-tests is a **sibling** repo: paths from nsl-tuvm cwd are `../nsl-tests/...` (a wrong path silently runs the restored fixture, which prints `42`).
- **Test file convention:** inline `(* expect: *)` header + `class X packageUsing: manifest` + `println`; deterministic output only — **never print raw byte counts** (they vary host/target/run).
- **No behavior change from counters.** The C counters are a couple of integer ops on paths already instrumented; no new allocation, no policy.
- **No periodic publisher** (G9 — no unrequested periodic work). Publishing is an explicit app act.
- **The footprint gate must be seen to RUN** (tuvm#31: it silently exits 77/SKIP without the ARM toolchain). A gate that didn't run is not a pass.
- **nsl gotcha:** a reified `whenever:do:` block cannot inline a cross-class call needing outer/`platform` resolution (`Invalid receiver for LD_SLOT`) — hoist to a `self`-send helper. (No task below registers such a block, but T-D.5 edits live near ones that do.)

---

### Task 1: Handle live/high-water counters (tuvm, C)

**Files:**
- Modify: `~/workspace/tuvm/src/handles.c` (counters + accessors)
- Modify: `~/workspace/tuvm/src/handles.h` (declarations)
- Test: `~/workspace/tuvm/tests/test_handles.c` (new counters block)

**Interfaces:**
- Consumes: existing `handle_new(oop) -> int`, `handle_free(int)`.
- Produces: `unsigned handle_live_count(void)` and `unsigned handle_high_water(void)` — Task 3's prim reads exactly these two names.

- [ ] **Step 1: Write the failing test**

In `~/workspace/tuvm/tests/test_handles.c`, inside `main`, immediately **before** the overflow `expect_abort` call at the end (the block is net-zero on live handles, so the `OEVM_HANDLE_TABLE_MAX=4` overflow trigger still needs exactly 5 pins):

```c
    /* tuvm#24: live/high-water counters track pin + release exactly. Relative
       assertions only — earlier properties may hold pins of their own. */
    {
        unsigned live0 = handle_live_count();
        unsigned hw0   = handle_high_water();
        int a = handle_new(make_str());
        int b = handle_new(make_str());
        assert(handle_live_count() == live0 + 2 && "two pins raise live by 2");
        assert(handle_high_water() >= hw0 && "high water never decreases");
        assert(handle_high_water() >= live0 + 2 && "high water covers the new peak");
        unsigned hw1 = handle_high_water();
        handle_free(a);
        assert(handle_live_count() == live0 + 1 && "free lowers live");
        handle_free(a);  /* double-free must not skew the gauge */
        assert(handle_live_count() == live0 + 1 && "double-free is idempotent on live count");
        assert(handle_high_water() == hw1 && "high water is monotonic, unaffected by free");
        handle_free(b);
        assert(handle_live_count() == live0 && "back to baseline");
        printf("test_handles: counters PASS\n");
    }
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd ~/workspace/tuvm && tools/run_c_tests.sh 2>&1 | tail -20`
Expected: `test_handles` **fails to compile** — `implicit declaration of function 'handle_live_count'`.

- [ ] **Step 3: Implement the counters**

`~/workspace/tuvm/src/handles.h` — append before the `#endif`:

```c
/* tuvm#24 telemetry: currently-live pins, and the monotonic max ever observed.
   Maintained incrementally in handle_new/handle_free (no scan). Read by the
   sysStats prim (platform.c) for the sys/metrics record. */
unsigned handle_live_count(void);
unsigned handle_high_water(void);
```

`~/workspace/tuvm/src/handles.c` — beside the table:

```c
static struct HandleSlot handles[OEVM_HANDLE_TABLE_MAX];
/* tuvm#24: pin telemetry (see handles.h). */
static unsigned handles_live = 0;
static unsigned handles_high_water = 0;
```

In `handle_new`, the slot-goes-live branch becomes:

```c
        if (!handles[i].live) {
            handles[i].value = o;
            handles[i].live = 1;
            handles_live++;
            if (handles_live > handles_high_water) handles_high_water = handles_live;
            return i;
        }
```

`handle_free` gains the was-live guard (the one correctness edge — a double-free must not underflow the gauge):

```c
void handle_free(int idx) {
    if (idx < 0 || idx >= OEVM_HANDLE_TABLE_MAX) return;
    if (handles[idx].live) handles_live--;
    handles[idx].live = 0;
    handles[idx].value = NULL;
}
```

At the end of the file:

```c
unsigned handle_live_count(void) { return handles_live; }
unsigned handle_high_water(void) { return handles_high_water; }
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd ~/workspace/tuvm && tools/run_c_tests.sh 2>&1 | tail -20`
Expected: `test_handles: counters PASS` and every pre-existing line still PASS.

- [ ] **Step 5: Commit (tuvm)**

```bash
cd ~/workspace/tuvm && git add src/handles.c src/handles.h tests/test_handles.c
git commit -m "feat(tuvm#24): handle live/high-water counters — the #23 telemetry residue"
```

---

### Task 2: Ring-occupancy high-water (tuvm, C)

**Files:**
- Modify: `~/workspace/tuvm/src/space.c` (counter + accessor; the single `q_count++` enqueue site is at `space.c:223`)
- Modify: `~/workspace/tuvm/src/space.h` (declaration, beside `space_overruns` at `space.h:75`)
- Test: `~/workspace/tuvm/tests/test_space.c` (new property block)

**Interfaces:**
- Consumes: existing `space_post(const char *path, size_t len, oop value)`, `space_take(const char *path, size_t len)`.
- Produces: `uint32_t space_queue_high_water(void)` — Task 3's prim reads exactly this name.

- [ ] **Step 1: Write the failing test**

In `~/workspace/tuvm/tests/test_space.c`, append a new property block at the end of `main`, after the last existing property and before any final summary print:

```c
    /* tuvm#24: queue high-water is monotonic and covers any simultaneous
       occupancy. Relative assertions — earlier properties already used the ring. */
    {
        uint32_t hw0 = space_queue_high_water();
        space_post("hw/probe", 8, TAG_SMI(1));
        space_post("hw/probe", 8, TAG_SMI(2));
        space_post("hw/probe", 8, TAG_SMI(3));
        uint32_t hw1 = space_queue_high_water();
        assert(hw1 >= hw0 && "high water never decreases");
        assert(hw1 >= 3 && "three simultaneously-queued events must register");
        while (space_take("hw/probe", 8) != NULL) { }
        assert(space_queue_high_water() == hw1 && "draining does not lower high water");
        printf("test_space: queue high-water PASS\n");
    }
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd ~/workspace/tuvm && tools/run_c_tests.sh 2>&1 | tail -20`
Expected: `test_space` **fails to compile** — `implicit declaration of function 'space_queue_high_water'`.

- [ ] **Step 3: Implement the counter**

`~/workspace/tuvm/src/space.c` — beside `total_overruns` (~line 104):

```c
/* tuvm#24: monotonic max of q_count ever observed. NOTE this counts ring SLOTS
   — q_count includes lazily-dead not-yet-compacted slots (see the file-header
   staleness note) — so it measures ring-slot PRESSURE (slots unavailable to a
   burst, the number G2 depth-sizing needs), not logical queue depth. */
static uint32_t q_high_water = 0;
```

At the single enqueue site (after `q_count++;` at line 223):

```c
    q_count++;
    if (q_count > q_high_water) q_high_water = (uint32_t)q_count;
```

At the end of the file, beside `space_overruns`:

```c
uint32_t space_queue_high_water(void) { return q_high_water; }
```

`~/workspace/tuvm/src/space.h` — beside the `space_overruns` declaration (line 75):

```c
/* tuvm#24: monotonic max ring occupancy (q_count) ever observed — ring-slot
   pressure for G2 sizing, not logical depth (q_count counts lazily-dead
   not-yet-compacted slots too). Read by the sysStats prim. */
uint32_t space_queue_high_water(void);
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd ~/workspace/tuvm && tools/run_c_tests.sh 2>&1 | tail -20`
Expected: `test_space: queue high-water PASS` and every pre-existing line still PASS.

- [ ] **Step 5: Commit (tuvm)**

```bash
cd ~/workspace/tuvm && git add src/space.c src/space.h tests/test_space.c
git commit -m "feat(tuvm#24): ring-occupancy high-water counter — the #20 telemetry residue"
```

---

### Task 3: `sysStats` prim + `kernel metrics` + the sys/metrics cell test (tuvm + nsl-tuvm + nsl-tests)

One TDD cycle across three repos: the failing `.ns` test drives all five prim sites plus the record composition, because the raw prim array is only readable kernel-side (`basicAt:` is kernel-emitted, not app surface — everything app-visible goes through `kernel metrics`).

**Files:**
- Test (create): `~/workspace/nsl-tests/15_heap_stats/02_sys_metrics.ns`
- Modify: `~/workspace/tuvm/src/prim_ids.h` (append after `PRIM_pf_spaceOverruns 108`)
- Modify: `~/workspace/tuvm/src/dispatch.c` (enum tail + `PRIM_NAMES[]` tail)
- Modify: `~/workspace/tuvm/src/platform.c` (case beside `PRIM_pf_heapStats` at ~line 710; check `#include "handles.h"` is present, add if not — `space.h` already is, since the `spaceOverruns` case calls `space_overruns`)
- Modify: `~/workspace/nsl-tuvm/lib/kernel.ns` (forwarder beside `spaceOverruns:` at line 1988; `metrics` beside `heapStats` at line 2028)

**Interfaces:**
- Consumes: Task 1's `handle_live_count()`/`handle_high_water()`, Task 2's `space_queue_high_water()`, existing `space_overruns("", 0)`, existing `indexable_new`, kernel `HeapStats`/`Record`/`Map fromPairs:`/`OrderedCollection`.
- Produces: prim selector `sysStats` (0-arg, 4-slot SMI array `[handlesLive, handlesHighWater, queueHighWater, totalOverruns]`); `kernel metrics` answering `Record tag: #Metrics` with fields `#heapSize #liveBytes #heapHighWater #largestFreeBlock #gcCycles #handlesLive #handlesHighWater #queueHighWater #overruns` — Task 4 (T-D.5) uses `platform heapStats highWater`; Task 5 pins this exact field list as the wire schema.

- [ ] **Step 1: Write the failing test**

Create `~/workspace/nsl-tests/15_heap_stats/02_sys_metrics.ns`:

```
(*
expect:
true
true
true
true
true
true
true
true
done
*)
(* tuvm#24: the sys/metrics story end-to-end — prim 109 (sysStats) + kernel
   metrics (the 9-field flat Record, schema pinned in
   porta/docs/PROTOCOL-NSL-RECORDS.md) + the sys/metrics CELL (rung 9): a
   reader peeks current state by NAME, non-destructively, without draining a
   stream another consumer counts on (WHY-AGENTS rider). Absolute values vary
   run-to-run, so this asserts structural invariants only (01_invariants
   precedent). Publishing is the app's act — this test IS the publisher (G9:
   no unrequested periodic work; no periodic publisher exists). Both-path
   A==B gate. *)
class SysMetricsTest packageUsing: manifest = () (
public main: platform args: args = (
  | m cell got |
  m:: platform metrics.
  (m tag = #Metrics) printString println.
  ((m at: #heapSize ifAbsent: [ -1 ]) > 0) printString println.
  ((m at: #heapHighWater ifAbsent: [ -1 ]) >= (m at: #liveBytes ifAbsent: [ 0 ])) printString println.
  ((m at: #handlesHighWater ifAbsent: [ -1 ]) >= (m at: #handlesLive ifAbsent: [ 0 ])) printString println.
  ((m at: #overruns ifAbsent: [ -1 ]) = (platform spaceOverruns: '')) printString println.

  cell:: platform space channel: 'sys/metrics'.
  cell put: '' value: m.
  got:: cell peek: '' ifAbsent: [ nil ].
  (got = m) printString println.
  ((got at: #queueHighWater ifAbsent: [ -1 ]) >= 0) printString println.

  (* the put above enqueued a reaction event, so a FRESH snapshot must show
     the ring has been occupied at least once *)
  ((platform metrics at: #queueHighWater ifAbsent: [ -1 ]) >= 1) printString println.
  'done' println.
)
) : (
)
```

- [ ] **Step 2: Run to verify it fails**

Use the Global Constraints single-.ns recipe with `../nsl-tests/15_heap_stats/02_sys_metrics.ns`.
Expected: `Message 'metrics' not understood` (and restore the image afterward).

- [ ] **Step 3: The four C sites (tuvm)**

**First count the `PRIM_NAMES[]` entries** in `~/workspace/tuvm/src/dispatch.c` (e.g. transcribe the array and count strings — expect 108, `"spaceOverruns:"` last; if the count differs, the new id is that count + 1 everywhere below).

`~/workspace/tuvm/src/prim_ids.h` — append after `#define PRIM_pf_spaceOverruns 108`:

```c
/* tuvm#24: sys-telemetry leaf (`sysStats`), appended after spaceOverruns: — no
   pre-existing id shifts. 109 arrived at by COUNTING PRIM_NAMES (108 entries
   before this append), not by 108+1 arithmetic, per the standing lesson above.
     sysStats -> 4-slot Array of SMIs [handlesLive, handlesHighWater,
                 queueHighWater, totalOverruns] (heapStats' shape, prim 104).
                 The push-side sys/metrics record is composed in nsl
                 (kernel metrics) and published by the APP onto a cell — the
                 kernel never writes the space (capability rule). */
#define PRIM_pf_sysStats 109
```

`~/workspace/tuvm/src/dispatch.c` — enum tail, after `PRIM_pf_spaceOverruns,`:

```c
    /* tuvm#24: sys-telemetry leaf — the 109th entry (counted). */
    PRIM_pf_sysStats,
```

`PRIM_NAMES[]` tail, after `"spaceOverruns:",`:

```c
    "sysStats",
```

`~/workspace/tuvm/src/platform.c` — new case directly after the `PRIM_pf_heapStats` case (and add `#include "handles.h"` to the file's includes if absent):

```c
    case PRIM_pf_sysStats: {
        /* tuvm#24: `platform sysStats` — snapshot handle/ring/overrun telemetry
           into a 4-slot Array of SMIs, in field order [handlesLive,
           handlesHighWater, queueHighWater, totalOverruns]. Same GC-safety
           shape as heapStats above: all four scalars are copied into C locals
           BEFORE the single allocating call (indexable_new); the stores are
           SMIs (immediates, no allocation), so no heap oop is held across an
           allocation and no gc_temp_root pin is required. Every value is far
           below SMI range on host and target alike (table/ring/counter
           magnitudes). args ignored (0-arg selector). */
        unsigned hl = handle_live_count();
        unsigned hh = handle_high_water();
        uint32_t qh = space_queue_high_water();
        uint32_t ov = space_overruns("", 0);
        oop r = indexable_new(4);
        ((struct ObjectInstance *)r)->slots[1] = TAG_SMI((intptr_t)hl);
        ((struct ObjectInstance *)r)->slots[2] = TAG_SMI((intptr_t)hh);
        ((struct ObjectInstance *)r)->slots[3] = TAG_SMI((intptr_t)qh);
        ((struct ObjectInstance *)r)->slots[4] = TAG_SMI((intptr_t)ov);
        *out = r; return 1;
    }
```

- [ ] **Step 4: The fifth site + the record composition (nsl-tuvm/lib/kernel.ns)**

Beside `public spaceOverruns: p` (line 1988):

```
  public sysStats = ( ^ platform sysStats )
```

Directly after `public heapStats` (line 2028):

```
  (* tuvm#24: the sys/metrics snapshot — prim 104 (heapStats) + prim 109
     (sysStats) composed into ONE flat immutable Record, the sys/metrics
     cell's wire schema (pinned in porta/docs/PROTOCOL-NSL-RECORDS.md §4.4).
     The kernel only BUILDS the record; publishing it onto a cell is the
     app's act, on a user-requested cadence (G9), with the channel arriving
     as a capability — the kernel never conjures space access (Milestone-C
     constructor-args rule). Raw prim arrays are read with basicAt:, the
     HeapStats precedent. *)
  public metrics = ( | h s pairs |
    h:: platform heapStats.
    s:: platform sysStats.
    pairs:: OrderedCollection new.
    pairs add: #heapSize -> (h basicAt: 1).
    pairs add: #liveBytes -> (h basicAt: 2).
    pairs add: #heapHighWater -> (h basicAt: 3).
    pairs add: #largestFreeBlock -> (h basicAt: 4).
    pairs add: #gcCycles -> (h basicAt: 5).
    pairs add: #handlesLive -> (s basicAt: 1).
    pairs add: #handlesHighWater -> (s basicAt: 2).
    pairs add: #queueHighWater -> (s basicAt: 3).
    pairs add: #overruns -> (s basicAt: 4).
    ^ Record tag: #Metrics fields: (Map fromPairs: pairs) )
```

(The tag is `#Metrics`, capitalized — the CBOR tag-27 record name IS the tag
symbol, and the PROTOCOL-NSL-RECORDS catalogue names records capitalized
(`Reset`, `Overrun`, `BlobRef`); the wire name and the nsl tag must match
byte-for-byte.)

```
```

(Note `metrics` bypasses the `HeapStats` wrapper and reads prim 104's raw array directly — one composition point, no double wrapping.)

- [ ] **Step 5: Run to verify it passes**

Single-.ns recipe again with `../nsl-tests/15_heap_stats/02_sys_metrics.ns`.
Expected: eight `true` lines + `done`, exit 0. Restore the image.

- [ ] **Step 6: C unit tests still green**

Run: `cd ~/workspace/tuvm && tools/run_c_tests.sh 2>&1 | tail -5`
Expected: all PASS (guards against a slipped id shifting an existing prim).

- [ ] **Step 7: Commit (all three repos)**

```bash
cd ~/workspace/tuvm && git add src/prim_ids.h src/dispatch.c src/platform.c
git commit -m "feat(tuvm#24): sysStats prim (109) — handle/ring/overrun telemetry leaf"
cd ~/workspace/nsl-tuvm && git add lib/kernel.ns
git commit -m "feat(tuvm#24): kernel metrics — the 9-field sys/metrics Record + sysStats forwarder"
cd ~/workspace/nsl-tests && git add 15_heap_stats/02_sys_metrics.ns
git commit -m "test(tuvm#24): sys/metrics end-to-end — record schema + cell put/peek"
```

---

### Task 4: Strict T-D.5 plateau (nsl-tests)

> **AMENDED mid-execution (2026-07-14, David's ruling — see the design doc's §4 correction).**
> The highWater gauge below is superseded: GC never fires on the host arena for this workload,
> so highWater = cumulative allocation (measured 9,121,280 B deterministic climb in a healthy
> run). The strict plateau instead samples **`liveBytes` after forcing a sweep at each sample
> point** — spin droppable allocations until `platform heapStats gcCycles` increments, then
> read `platform heapStats liveBytes`; warm-up sample at i=10,000, end sample after the final
> drain; assert end − warm-up ≤ the named tolerance (same tuning rule and falsification step
> as written below, with `hw`/`highWater` read as `liveBytes`-at-sweep). A future `gcCollect`
> prim is filed as tuvm#35; the spin is the no-VM-change bridge.

**Files:**
- Modify: `~/workspace/nsl-tests/11_supervision/10_soak_leak.ns`

**Interfaces:**
- Consumes: `platform heapStats highWater` (prim 104 via the Kernel forwarder — the gauge whose absence forced the weak invariant).
- Produces: the suite's standing leak detector; expect-block line `plateau true`.

- [ ] **Step 1: Add the sampling + assertion (initially instrumented)**

Three edits to `10_soak_leak.ns`:

(a) Instance-variable list gains `hwWarm ::= 0.` (house style — loop-mutated state is instance vars here):

```
class SoakLeak packageUsing: manifest = ( | h1Hits ::= 0. h2Hits ::= 0. h3Hits ::= 0.
                                            crashCycles ::= 0. restarts ::= 0. finalHits ::= 0.
                                            cycleCounter ::= 0. hwWarm ::= 0. | ) (
```

(b) Inside the `1 to: n do: [ :i | ... ]` loop, immediately before the closing `space drain ].` line, add the warm-up sample:

```
      (i = 10000) ifTrue: [ hwWarm:: platform heapStats highWater ].
```

(c) After the final-responsiveness block (after `space drain.` that follows `final out: '' value: #ping.`) and before the print block, add — **temporarily instrumented** with the raw delta for step 2's tuning:

```
    (* T-D.5 STRICT PLATEAU (tuvm#24 — the assertion this test was designed
       around, enabled now that prim 104 exists): heap highWater is sampled at
       iteration 10,000 (past warm-up: caches, supervisor state, ring growth
       all settled) and again here, after 90,000 further iterations and ~4,500
       further crash/restart cycles. highWater is the right gauge — liveBytes
       carries floating-garbage noise (live-at-last-sweep + allocations
       since), while any real leak forces the monotonic high-water upward.
       TOLERANCE: 65536 bytes to start; step 2 tightens it to >=4x the measured
       cross-run noise (and rewrites this sentence with the observed numbers) —
       orders of magnitude below the smallest leak this test must catch
       (16 B/iteration x 90k = 1.44 MB). Verdict only, never raw bytes
       (host/target/run vary). *)
    ('hw delta ' , ((platform heapStats highWater) - hwWarm) printString) out.
    ('plateau ' , (((platform heapStats highWater) - hwWarm) <= 65536) printString) out.
```

And the expect block gains, between `final 1` and `done`:

```
plateau true
```

(The `hw delta` line is temporary and will make the expect check fail — that is fine for the tuning runs; use the runner exit output, not the harness verdict, in step 2.)

- [ ] **Step 2: Tune the tolerance empirically**

Run the single-.ns recipe (Path A) with `../nsl-tests/11_supervision/10_soak_leak.ns` **three times** (bump the runner timeout: this file drives 300k firings — use `timeout 120`). Record the three `hw delta` values. Set the tolerance constant in the `plateau` line to `max(4 × largest observed delta, 4096)`, rounded up to a power of two, and update the `<FILLED-IN-STEP-2>` comment text with the observed numbers. It must stay ≤ 65536 (well below the 1.44 MB signal); if the observed delta exceeds 16 KB, stop and reconsider the gauge — do not widen past 65536 to make it pass.

- [ ] **Step 3: Falsify the gate (silent-gates law — prove it CAN fail)**

Temporarily add a deliberate leak: instance var `leak ::= nil.`, at the top of `main:` `leak:: platform OrderedCollection new.` (app code reaches kernel classes through `platform`, the 05_gc_root precedent), and inside the loop `leak add: i printString.` (90k retained strings ≈ several MB). Run once: expected output line `plateau false`. **A `plateau true` here means the gate is broken — stop and fix before proceeding.** Then remove all three leak edits.

- [ ] **Step 4: De-instrument and verify green**

Delete the temporary `('hw delta ' , ...) out.` line. Run the single-.ns recipe once more.
Expected: byte-exact match with the expect block (all original lines + `plateau true`), exit 0. Restore the image.

- [ ] **Step 5: Commit (nsl-tests)**

```bash
cd ~/workspace/nsl-tests && git add 11_supervision/10_soak_leak.ns
git commit -m "test(tuvm#24): T-D.5 strict heap-high-water plateau — the leak detector, armed"
```

---

### Task 5: Pin the wire schema (porta)

**Files:**
- Modify: `~/workspace/porta/docs/PROTOCOL-NSL-RECORDS.md` §4.4 (the `sys/**` catalogue table)

**Interfaces:**
- Consumes: Task 3's exact field list (the schema and `kernel metrics` must agree symbol-for-symbol).
- Produces: the pinned `sys/metrics` row future bridge/porta work codes against.

- [ ] **Step 1: Add the catalogue row + the cell-kind note**

In §4.4's table, add:

```markdown
| `sys/metrics` | **C (cell)** | `Metrics { heapSize: int, liveBytes: int, heapHighWater: int, largestFreeBlock: int, gcCycles: int, handlesLive: int, handlesHighWater: int, queueHighWater: int, overruns: int }` — tuvm#24; ~70 B encoded, within §2.2 |
```

After the table's existing delivery-class paragraph, add:

```markdown
**Kind C is a cell** (tuvm#32 option A, rung 9): the pigeonhole holds the latest
letter. A reader — porta, an agent, a second job — `peek:`s current state *by
name*, non-destructively, without draining a stream some other consumer is
counting on (the WHY-AGENTS rider, folded in here). The node app is the writer,
on a user-requested cadence (G9: no unrequested periodic work); as of tuvm#24
no periodic publisher exists — the record is built by `kernel metrics` and
published explicitly.
```

- [ ] **Step 2: Commit (porta)**

```bash
cd ~/workspace/porta && git add docs/PROTOCOL-NSL-RECORDS.md
git commit -m "docs(tuvm#24): pin sys/metrics cell schema — 9-field Metrics record"
```

---

### Task 6: Gates, re-bank, promote, close

**Files:**
- Modify: `~/workspace/tuvm/review/reliability-gaps.md` (G3 + G2 status notes)
- Modify: `~/workspace/tuvm/review/tests/CLAIMS.md` (T-D.5 strict + the new test)
- Modify: `~/workspace/tuvm/tools/footprint-mcu-baseline.txt` (re-bank with reason)
- Modify: `~/workspace/nsl-tuvm/.superpowers/sdd/progress.md` (ledger entry)
- GitHub: tick #24 in the tuvm#26 tracker body; close tuvm#24

**Interfaces:**
- Consumes: everything above, landed and committed.
- Produces: the closed P0 and the promoted record (the close-out is not done until the mainline docs name it — the standing promotion rule).

- [ ] **Step 1: Full suite, both paths**

```bash
cd ~/workspace/nsl-tuvm
OEVM_PATH=AB python3 tools/run_tests_oevm.py > /tmp/claude-1000/-home-david-workspace-nsl-tuvm/bd772cbd-5325-4ca8-816e-f16b92a24882/scratchpad/suite-tuvm24.log 2>&1
tail -25 /tmp/claude-1000/-home-david-workspace-nsl-tuvm/bd772cbd-5325-4ca8-816e-f16b92a24882/scratchpad/suite-tuvm24.log
```

Verify the log's FIRST line says `Discovered ~200` (not 92). Expected: **194 / 0 / 10** (floor 193 + `02_sys_metrics.ns`), zero regressions, and the named gates all printed PASS (grant, debugger-integration, portability-32, boot-gc-asan — none SKIP without a stated environmental reason). No `west build` may run concurrently.

- [ ] **Step 2: Footprint — host + the MCU ratchet, and see it RUN**

```bash
cd ~/workspace/tuvm && tools/footprint.sh | tail -5
source ~/workspace/ns-zephyr/tools/env.sh
FOOTPRINT_CONFIG=mcu tools/footprint.sh | tail -8; echo "mcu exit: $?"
```

**Exit 77 = the gate did not run** (tuvm#31) — that is not a pass; fix the env and rerun. Expected: a small text increase (the prim case + counters, order of +100–300 B; zero new RAM beyond the four static counters ≈ 16 B BSS). Re-bank `tools/footprint-mcu-baseline.txt` with the new figure and a one-line reason (`tuvm#24 sysStats prim + telemetry counters`), and commit it with the review-doc edits in Step 3.

- [ ] **Step 3: Promote to the mainline docs**

- `~/workspace/tuvm/review/reliability-gaps.md`: update **G3**'s Milestone-D status note (the flagged gap): the gauge is exported end-to-end (prim 104 + prim 109 + `kernel metrics` + `sys/metrics` cell) and T-D.5 asserts the strict high-water plateau. Update **G2**: queue high-water landed (pull: prim 109; the push record exists as `sys/metrics`); overruns already noted from #20.
- `~/workspace/tuvm/review/tests/CLAIMS.md`: T-D.5's claim upgrades from completion-proxy to strict plateau; add `15_heap_stats/02_sys_metrics.ns` (schema + cell round-trip + overruns cross-check).
- `~/workspace/nsl-tuvm/.superpowers/sdd/progress.md`: append a dated tuvm#24 entry — scope ruling, the 6 tasks, suite/footprint numbers, falsification result (Task 4 Step 3).

```bash
cd ~/workspace/tuvm && git add review/reliability-gaps.md review/tests/CLAIMS.md tools/footprint-mcu-baseline.txt
git commit -m "docs(tuvm#24): promote — G3/G2 status, CLAIMS, footprint re-bank"
cd ~/workspace/nsl-tuvm && git add .superpowers/sdd/progress.md && git commit -m "docs(tuvm#24): SDD ledger entry"
```

- [ ] **Step 4: Issue bookkeeping**

- Edit the tuvm#26 tracker body: tick #24 in the P0 table and the execution-order list.
- Close tuvm#24 with a comment naming: the commits (per repo), the design doc (`nsl-tuvm/docs/design/2026-07-14-tuvm24-sys-metrics-design.md`), which mainline docs absorbed the knowledge (reliability-gaps G3/G2, CLAIMS.md, PROTOCOL-NSL-RECORDS §4.4), the suite/footprint numbers, and the falsification evidence for the plateau gate.

```bash
cd ~/workspace/tuvm && gh issue close 24 --comment "<the close-out comment>"
```

- [ ] **Step 5: Push (all four repos) — after David's go-ahead if session policy requires**

```bash
for r in tuvm nsl-tuvm nsl-tests porta; do (cd ~/workspace/$r && git push); done
```
