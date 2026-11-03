# Bounded span-capture sessions on a `diag/` channel family — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the job-latency figures trustworthy to quote by accumulating them over a
bounded window that excludes the act of reporting them, controlled by a `diag/spans/ctl`
cell and reported on a `diag/spans/result` cell.

**Architecture:** A new nullary prim exposes the already-existing `span_reset_all()`. A new
`SpanCapture` class in the kernel library reacts to a control cell: on `#running` it stamps
`t0` then resets; on `#stopped` it snapshots the span table into a Record and puts it on a
result cell. Because a reaction runs during `space drain` and an HTTP response renders
synchronously in its own request, the report is always rendered on a later request than the
one that closed the window — so a report can never land inside the window it describes.

**Tech Stack:** C (the VM and its prim seam), nsl (the kernel library and the node),
Zephyr on an ESP32 WROOM-32 for device verification.

**Design:** `~/workspace/tuvm/docs/design/2026-07-22-diag-span-capture-design.md`
**Tracking issue:** tuvm#72

## Global Constraints

- **Repos.** VM + node: `~/workspace/tuvm`. Kernel library: `~/workspace/nsl-tuvm`. Tests:
  `~/workspace/nsl-tests`. All three are separate git repos and are committed separately.
- **Prim ids are counted, never computed.** A new id is `(number of entries in PRIM_NAMES) + 1`
  arrived at by **counting the list**, and it is appended — no existing id may shift.
- **Path B only.** Run the suite with `OEVM_DIR=~/workspace/tuvm python3 tools/run_tests_oevm.py`
  from `~/workspace/nsl-tuvm`. Do **not** enable Path A; the per-test gcc rebuild is too slow.
- **Read the gate line, not just the failure count.** The suite prints `Passed/Failed/Skipped`
  and then a gate line (`grant · debugger-integration · portability-32 · boot-gc-asan · footprint`).
  A run can print `Failed: 0` while a gate is red. `footprint: SKIP` is expected in this
  environment (no `arm-zephyr-eabi` toolchain, tuvm#31).
- **Suite floor is 207 passed / 0 failed / 14 skipped** before this work. Each task that adds
  a test raises the floor by one; state the new floor in the commit message.
- **Leaf vocabulary is closed** — `ctl`, `result`, `sample`, `event`. No other leaf names.
- **Never run two nsl suites at once** (they share `/tmp` paths).

---

## File Structure

| File | Repo | Responsibility |
|---|---|---|
| `src/prim_ids.h` | tuvm | The new `PRIM_pf_spanReset` id |
| `src/dispatch.c` | tuvm | The prim enum entry, the `PRIM_NAMES` string, and the enum/names parity check |
| `src/platform.c` | tuvm | The prim case calling `span_reset_all()` |
| `lib/kernel.nsl` | nsl-tuvm | `Spans>>resetAll` (Task 1) and the new `SpanCapture` class (Task 2) |
| `19_latency/06_span_reset.nsl` | nsl-tests | Reset zeroes C data *and* nsl failure counts, keeps names |
| `19_latency/07_span_capture.nsl` | nsl-tests | Both ordering invariants, plus the three edge cases |
| `zephyr/ThermalHttpNode.nsl` | tuvm | Starts the capture; two routes drive the cell; the result table + legend |

`SpanCapture` goes in the kernel library, **not** the node file — a class nested in a
zephyr node file is unreachable from `nsl-tests/`, which would make the host tests in Task 2
impossible. Design §5.1 records this and its cost.

---

## Task 1: The `spanReset` prim and `Spans>>resetAll`

**Files:**
- Modify: `~/workspace/tuvm/src/prim_ids.h` (append after `PRIM_pf_spanRecord`, line ~218)
- Modify: `~/workspace/tuvm/src/dispatch.c` (enum ~line 763, `PRIM_NAMES` ~line 815, parity check ~line 826)
- Modify: `~/workspace/tuvm/src/platform.c` (new case beside `PRIM_pf_spanRecord`, ~line 900)
- Modify: `~/workspace/nsl-tuvm/lib/kernel.nsl` (`Spans` class, after `noteFailure:`/`failuresAt:`, ~line 1097)
- Test: `~/workspace/nsl-tests/19_latency/06_span_reset.nsl`

**Interfaces:**
- Consumes: `span_reset_all(void)` — already exists at `~/workspace/tuvm/src/span.c:90`, declared in `src/span.h:36`. No change to it.
- Produces: nsl `Spans>>resetAll` — zeroes the C span table **and** the nsl-side failure
  counts, preserves the name↔slot table, answers `self`. Task 2 calls exactly this.

**The catch that makes this task non-trivial:** `span_reset_all()` zeroes only the C side.
Failure counts live in nsl, in the `Spans` class's `fails` OrderedCollection
(`~/workspace/nsl-tuvm/lib/kernel.nsl:1095`), because a failure is an app-level concept the
C seam cannot detect. If `resetAll` forgets them, every capture reports failures cumulative
since boot beside samples that are windowed — a silently wrong table. The test asserts this
directly.

- [ ] **Step 1: Write the failing test**

Create `~/workspace/nsl-tests/19_latency/06_span_reset.nsl`:

```smalltalk
(*
expect:
true
true
true
true
true
true
done
*)
(* tuvm#72: `resetAll` is what makes a bounded capture window possible. It must
   clear BOTH halves of a span's state — the C sample table (span_reset_all) and
   the nsl-side failure counts, which C cannot see. A reset that cleared only the
   C half would report failures cumulative-since-boot beside windowed samples.
   Names must SURVIVE: a reset clears data, not the slot table, or every holder
   of a slot index from `register:` would be pointing at the wrong span. *)
class SpanResetTest packageUsing: manifest = () (
public main: platform args: args = (
  | spans a s |
  spans:: platform Spans new.
  a:: spans register: #acquire.

  spans recordSlot: a us: 500.
  spans noteFailure: a.
  s:: spans statsAt: a.
  ((s basicAt: 1) = 1) printString println.            (* true — one sample banked *)
  ((spans failuresAt: a) = 1) printString println.     (* true — one failure banked *)

  spans resetAll.

  s:: spans statsAt: a.
  ((s basicAt: 1) = 0) printString println.            (* true — n zeroed      *)
  ((s basicAt: 3) = 0) printString println.            (* true — max zeroed    *)
  ((spans failuresAt: a) = 0) printString println.     (* true — nsl side too  *)
  ((spans nameAt: a) = #acquire) printString println.  (* true — name survives *)

  'done' println.
)
) : (
)
```

- [ ] **Step 2: Run the test and verify it fails**

```bash
cd ~/workspace/nsl-tuvm && OEVM_DIR=~/workspace/tuvm python3 tools/run-one.py ~/workspace/nsl-tests/19_latency/06_span_reset.nsl
```

Expected: FAIL — a doesNotUnderstand on `resetAll`. **A SKIP is not a failure and proves
nothing** (the runner returns before compiling); if you see SKIP, fix the test header before
going on.

- [ ] **Step 3: Add the prim id**

In `~/workspace/tuvm/src/prim_ids.h`, immediately after `#define PRIM_pf_spanRecord 121`:

```c
/* tuvm#72 diag span capture: spanReset — a bounded capture window needs the
   accumulator zeroed at the start of it. Wraps span_reset_all(), which already
   existed for tests (src/span.c:90). Appended after spanRecord; no pre-existing
   id shifts. 122 was arrived at by COUNTING PRIM_NAMES (121 entries before this
   append), not by 121+1 arithmetic, per the standing rule above. Nullary: the
   reset is all-slots, because a per-slot reset would let a caller clear half a
   window and publish the result as if it were whole. */
#define PRIM_pf_spanReset  122
```

Before writing `122`, **count the entries in `PRIM_NAMES` in `src/dispatch.c`** and confirm
the count is 121. If it is not, use the count you got.

- [ ] **Step 4: Add the enum entry, the name, and update the parity check**

In `~/workspace/tuvm/src/dispatch.c`, in the prim enum (~line 763), after `PRIM_pf_spanRecord,`:

```c
    PRIM_pf_spanReset,
```

In `PRIM_NAMES` (~line 815), after `"spanRecord:us:",`:

```c
    "spanReset",
```

Update the parity check (~line 826) to name the new last prim:

```c
typedef char prim_names_enum_parity_check[(N_PRIM_NAMES == PRIM_pf_spanReset) ? 1 : -1];
```

That typedef is a compile-time assertion that the enum and the name list have not drifted.
If you skipped the name or the enum entry, the build fails here rather than at runtime —
let it do its job; do not "fix" it by changing the comparison.

- [ ] **Step 5: Implement the prim**

In `~/workspace/tuvm/src/platform.c`, after the `PRIM_pf_spanRecord` case (~line 907):

```c
    case PRIM_pf_spanReset:
        /* tuvm#72: nullary — all slots at once. Answers the platform capability
           like spanBegin:/spanEnd:, not a snapshot. */
        span_reset_all();
        *out = platform_object();
        return 1;
```

- [ ] **Step 6: Implement `resetAll` in the kernel**

In `~/workspace/nsl-tuvm/lib/kernel.nsl`, in the `Spans` class, after `failuresAt:` (~line 1097):

```smalltalk
    (* tuvm#72: clear the accumulator for a bounded capture window. BOTH halves —
       the C sample table via the prim, and the nsl-side failure counts, which C
       cannot see. Clearing only the C half would publish failures
       cumulative-since-boot beside windowed samples, which is a wrong table that
       looks right. The name<->slot table is deliberately NOT cleared: every
       caller holds a slot index handed out by register:, and moving those indices
       under them would silently mis-attribute every subsequent sample. *)
    public resetAll = (
      | i n |
      self ensureTables.
      platform spanReset.
      i:: 1. n:: fails size.
      [ i <= n ] whileTrue: [ fails at: i put: 0. i:: i + 1 ].
      ^ self )
```

- [ ] **Step 7: Run the test and verify it passes**

```bash
cd ~/workspace/nsl-tuvm && OEVM_DIR=~/workspace/tuvm python3 tools/run-one.py ~/workspace/nsl-tests/19_latency/06_span_reset.nsl
```

Expected: PASS.

- [ ] **Step 8: Run the whole suite**

```bash
cd ~/workspace/nsl-tuvm && OEVM_DIR=~/workspace/tuvm python3 tools/run_tests_oevm.py 2>&1 | tail -8
```

Expected: `Passed: 208  Failed: 0  Skipped: 14`, and the gate line green except
`footprint: SKIP`. If any *other* gate is red, stop — a new prim id that collides or shifts
shows up as a `portability-32` or `boot-gc-asan` failure, not as a test failure.

- [ ] **Step 9: Commit (three repos, separately)**

```bash
cd ~/workspace/tuvm && git add src/prim_ids.h src/dispatch.c src/platform.c && git commit -m "feat(#72): spanReset prim — expose the existing span_reset_all to nsl

A bounded capture window needs the accumulator zeroed at its start. Nullary and
all-slots: a per-slot reset would let a caller clear half a window and publish
the result as whole. Id 122 counted from PRIM_NAMES, appended, nothing shifts.

Refs #72, #61"

cd ~/workspace/nsl-tuvm && git add lib/kernel.nsl && git commit -m "feat(#72): Spans>>resetAll clears the nsl failure counts too

span_reset_all() zeroes only the C table. Failure counts live in nsl because a
failure is an app-level concept C cannot detect, so a reset that forgot them
would report failures cumulative-since-boot beside windowed samples. Names
survive: callers hold slot indices from register:.

Refs #72"

cd ~/workspace/nsl-tests && git add 19_latency/06_span_reset.nsl && git commit -m "test(#72): resetAll clears both halves and keeps the name table

Suite floor 207 -> 208.

Refs #72"
```

---

## Task 2: The `SpanCapture` class

**Files:**
- Modify: `~/workspace/nsl-tuvm/lib/kernel.nsl` (new class after `Spans`, before `ReadStream` ~line 1169)
- Test: `~/workspace/nsl-tests/19_latency/07_span_capture.nsl`

**Interfaces:**
- Consumes: `Spans>>resetAll`, `Spans>>slotCount`, `Spans>>statsAt:`, `Spans>>recordAt:`
  (all in `lib/kernel.nsl`); `Channel>>put:value:`, `Channel>>peek`, `Channel>>whenever:do:`;
  `Record tag:with:with:with:`; `List fromCollection:`; the `millis` prim as `0 millis`.
- Produces:
  - `SpanCapture class>>onSpace: aSpace spans: aSpans` — answers a new instance.
  - `SpanCapture>>start` — registers the ctl reaction and normalises a stale `#running`; answers `self`.
  - Channels `diag/spans/ctl` (cell, `{state:}`) and `diag/spans/result` (cell,
    `#SpanCapture` Record with `#n`, `#ms`, `#slots`).
  - `#slots` is a `List` of the `#LatencySpan` Records that `Spans>>recordAt:` already
    builds — do **not** invent a second per-span schema.

**Reuse note:** `Spans>>recordAt:` (`lib/kernel.nsl:1149`) already produces exactly the
per-slot Record wanted here, including the deliberately short field names. Use it.

- [ ] **Step 1: Write the failing test**

Create `~/workspace/nsl-tests/19_latency/07_span_capture.nsl`:

```smalltalk
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
(* tuvm#72: the two ordering invariants of a bounded capture window, asserted
   rather than commented — reset LAST on start (so samples banked before the
   window are excluded) and snapshot FIRST on stop (so samples banked after it
   are excluded). Values are FED with recordSlot:us:, never timed, so nothing
   here is timing-dependent. Both-path A==B gate. *)
class SpanCaptureTest packageUsing: manifest = () (
public main: platform args: args = (
  | space spans cap a ctl res r slots row |
  space:: platform Space new.
  spans:: platform Spans new.
  a::     spans register: #acquire.
  cap::   platform SpanCapture onSpace: space spans: spans.
  cap start.
  ctl:: space channel: 'diag/spans/ctl'.
  res:: space channel: 'diag/spans/result'.

  (* A sample banked BEFORE the window opens must not survive it. *)
  spans recordSlot: a us: 500.

  ctl put: '' value: (platform Record tag: #SpanCtl with: #state -> #running).
  space drain.

  (* Two samples INSIDE the window. *)
  spans recordSlot: a us: 100.
  spans recordSlot: a us: 200.

  ctl put: '' value: (platform Record tag: #SpanCtl with: #state -> #stopped).
  space drain.

  (* A sample banked AFTER the window closes must not appear in the report. *)
  spans recordSlot: a us: 900.

  r:: res peek.
  (r isNil not) printString println.                        (* true — a report was published *)
  ((r at: #n ifAbsent: [ -1 ]) = 2) printString println.    (* true — exactly the two inside  *)

  slots:: r at: #slots ifAbsent: [ nil ].
  row:: slots at: a + 1.
  ((row at: #n ifAbsent: [ -1 ]) = 2) printString println.      (* true *)
  ((row at: #minUs ifAbsent: [ -1 ]) = 100) printString println. (* true — not 500 *)
  ((row at: #maxUs ifAbsent: [ -1 ]) = 200) printString println. (* true — not 900 *)

  (* #stopped while already stopped must not overwrite a good capture with an
     empty one — a stray refresh would otherwise destroy the measurement. *)
  ctl put: '' value: (platform Record tag: #SpanCtl with: #state -> #stopped).
  space drain.
  r:: res peek.
  ((r at: #n ifAbsent: [ -1 ]) = 2) printString println.    (* true — unchanged *)

  (* #running while ALREADY running restarts the window rather than extending it:
     writing the cell is a statement of intent, and "start again" is the only sane
     reading. Assert it, because the alternative (ignore a redundant start) is
     equally plausible to a future reader and would silently change what a capture
     means. *)
  ctl put: '' value: (platform Record tag: #SpanCtl with: #state -> #running).
  space drain.
  spans recordSlot: a us: 300.
  ctl put: '' value: (platform Record tag: #SpanCtl with: #state -> #running).
  space drain.
  spans recordSlot: a us: 400.
  ctl put: '' value: (platform Record tag: #SpanCtl with: #state -> #stopped).
  space drain.
  r:: res peek.
  ((r at: #n ifAbsent: [ -1 ]) = 1) printString println.    (* true — only the 400 *)

  (* A retained #running is normalised to #stopped at start-up: a window whose
     start cannot be named is not a measurement. *)
  ctl put: '' value: (platform Record tag: #SpanCtl with: #state -> #running).
  space drain.
  (platform SpanCapture onSpace: space spans: spans) start.
  space drain.
  (((ctl peek) at: #state ifAbsent: [ nil ]) = #stopped) printString println.  (* true *)

  'done' println.
)
) : (
)
```

- [ ] **Step 2: Run the test and verify it fails**

```bash
cd ~/workspace/nsl-tuvm && OEVM_DIR=~/workspace/tuvm python3 tools/run-one.py ~/workspace/nsl-tests/19_latency/07_span_capture.nsl
```

Expected: FAIL — `SpanCapture` does not exist. Again: a SKIP proves nothing.

- [ ] **Step 3: Implement the class**

In `~/workspace/nsl-tuvm/lib/kernel.nsl`, after the `Spans` class's closing `) : (\n  )` and
before `class ReadStream`:

```smalltalk
  (* tuvm#72: a BOUNDED span-capture session, controlled by a cell.

     Why this exists: `egress` records one sample per emitted HTTP chunk for every
     route (src/http_core.c), and the /stats page emits ~24 chunks, so a
     cumulative span table partly measures the rendering of the span table. Over a
     300-request run that is noise for n and for the decade percentiles, but max
     is a single-sample statistic and a /stats chunk is a much larger string than
     an SVG row — so the max may be an artefact of the reporting.

     The invariant is ORDERING: reset LAST on start, snapshot FIRST on stop. In
     this shape it is structural rather than hand-maintained — a reaction runs
     during `space drain` while an HTTP response renders synchronously in its own
     request, so the report is always rendered on a LATER request than the one
     that closed the window, and a report can never land inside the window it
     describes.

     Deliberately generic: it knows about a Space and a Spans and nothing else —
     no sensor, no HTTP, no device. That is why it lives here beside Spans rather
     than in a node file, and it is also what makes it host-testable. *)
  class SpanCapture = ( | space spans ctl result t0 running | ) (
    onSpace: sp spans: sn = (
      space::  sp.
      spans::  sn.
      running:: false.
      ctl::    sp channel: 'diag/spans/ctl'.
      result:: sp channel: 'diag/spans/result'.
      ^ self )

    public start = (
      (* A self-send: a reified block must not name a class. *)
      ctl whenever: '' do: [ :c | self apply: c ].
      self normalise.
      ^ self )

    (* Registering a reaction does NOT replay a retained value, so a ctl left at
       #running by a crash mid-capture would sit there claiming a capture is
       running forever and a remote reader would believe it. Peek and correct.
       No result is published: t0 did not survive the restart, and a window whose
       start cannot be named is not a measurement. *)
    normalise = (
      | c |
      c:: ctl peek.
      c isNil ifTrue: [ ^ self ].
      ((c at: #state ifAbsent: [ #stopped ]) = #running) ifTrue: [
        ctl put: '' value: (Record tag: #SpanCtl with: #state -> #stopped) ].
      ^ self )

    apply: c = (
      | st |
      st:: c at: #state ifAbsent: [ #stopped ].
      st = #running ifTrue: [ ^ self beginWindow ].
      st = #stopped ifTrue: [ ^ self endWindow ].
      ^ self )

    (* #running while already running RESTARTS the window. Writing the cell is a
       statement of intent and "start again" is the only sane reading of it. *)
    beginWindow = (
      t0:: 0 millis.
      spans resetAll.
      running:: true.
      ^ self )

    (* #stopped while already stopped is ignored — otherwise a stray refresh
       overwrites a good capture with a zero-length one. *)
    endWindow = (
      | ms |
      running ifFalse: [ ^ self ].
      ms:: 0 millis - t0.
      (* `millis` tags into a 31-bit SMI and wraps at ~12.4 days of uptime. A
         negative elapsed is published as -1 rather than as a plausible wrong
         number: silent-wrong is the failure mode this whole class exists to
         remove. *)
      ms < 0 ifTrue: [ ms:: -1 ].
      result put: '' value: (self reportMs: ms).
      running:: false.
      ^ self )

    (* Reuses Spans>>recordAt:, which already builds the per-slot #LatencySpan
       Record with the short field names porta's size law wants. `n` is the TOTAL
       across slots, deliberately not "cycles": this class must not know which
       span represents a job. The per-job count is that job's row. *)
    reportMs: ms = (
      | i n slots total |
      slots:: OrderedCollection new.
      total:: 0.
      i:: 0. n:: spans slotCount.
      [ i < n ] whileTrue: [
        slots add: (spans recordAt: i).
        total:: total + ((spans statsAt: i) basicAt: 1).
        i:: i + 1 ].
      ^ Record tag: #SpanCapture
          with: #n     -> total
          with: #ms    -> ms
          with: #slots -> (List fromCollection: slots) )
  ) : (
    public onSpace: aSpace spans: aSpans = (
      ^ self new onSpace: aSpace spans: aSpans )
  )
```

- [ ] **Step 4: Run the test and verify it passes**

```bash
cd ~/workspace/nsl-tuvm && OEVM_DIR=~/workspace/tuvm python3 tools/run-one.py ~/workspace/nsl-tests/19_latency/07_span_capture.nsl
```

Expected: PASS. If the `#slots` row assertions fail, print `r` to check whether `recordAt:`
field names differ from those used above — they are `#n`, `#fails`, `#minUs`, `#maxUs`,
`#sumUsLo`, `#sumUsHi`, `#b0`..`#b5`, `#span` (`lib/kernel.nsl:1149`).

- [ ] **Step 5: Run the whole suite**

```bash
cd ~/workspace/nsl-tuvm && OEVM_DIR=~/workspace/tuvm python3 tools/run_tests_oevm.py 2>&1 | tail -8
```

Expected: `Passed: 209  Failed: 0  Skipped: 14`, gates green except `footprint: SKIP`.

- [ ] **Step 6: Measure what the shared kernel cost**

Design §5.1 requires this to be measured rather than assumed, because every node's image now
carries the class:

```bash
cd ~/workspace/tuvm && ls -l src/solidified_image.c && git diff --stat src/solidified_image.c 2>/dev/null | tail -2
```

Record the byte delta of the regenerated image in the commit message. If the image is not
regenerated in this environment, say so in the commit message rather than reporting a number
you did not measure. (tuvm#16 already tracks the committed image being stale versus a fresh
regen — do not let this task's number be taken from a stale file.)

- [ ] **Step 7: Commit**

```bash
cd ~/workspace/nsl-tuvm && git add lib/kernel.nsl && git commit -m "feat(#72): SpanCapture — a bounded, cell-controlled span-capture window

Reset last on start, snapshot first on stop. Structural rather than
hand-maintained: the reaction runs in drain and the report renders on a later
request, so a report cannot land inside the window it describes.

Lives beside Spans rather than in the node file — it knows only a Space and a
Spans, and a class nested in a zephyr node file is unreachable from nsl-tests.

Refs #72"

cd ~/workspace/nsl-tests && git add 19_latency/07_span_capture.nsl && git commit -m "test(#72): both ordering invariants, plus the three edge cases

Samples before the window and after it are excluded; a second #stopped does not
overwrite a good capture; a retained #running is normalised at start-up. Values
are fed, never timed. Suite floor 208 -> 209.

Refs #72"
```

---

## Task 3: Wire the capture into the node and drive it from `/stats`

**Files:**
- Modify: `~/workspace/tuvm/zephyr/ThermalHttpNode.nsl` — `main:` (~line 215-236), and the
  stats page methods (`statsPageOn:toId:spans:` ~line 258, `latencyTableOn:toId:spans:` ~line 303)

**Interfaces:**
- Consumes: `SpanCapture class>>onSpace:spans:` and `SpanCapture>>start` from Task 2.
- Produces: routes `/diag/spans/on` and `/diag/spans/off`; the node holds a `capture` temp in
  `main:` so the instance is not collected.

**Note on a GET that mutates:** these two routes change node state on a GET, so a browser
prefetch or a double-request can start or stop a capture. That is accepted for a bench
instrument and recorded in design §3.2. Do not add confirmation dialogs or JavaScript — the
page is served from a 31-bit VM on a WROOM-32 and stays plain HTML.

- [ ] **Step 1: Construct and start the capture in `main:`**

In `~/workspace/tuvm/zephyr/ThermalHttpNode.nsl`, add `capture` to `main:`'s temporaries and,
immediately after the `sRender:: spans register: #render.` line (~line 223):

```smalltalk
    (* tuvm#72: the capture session. Constructed here, like every other job, so
       the capabilities arrive as constructor args (the Milestone-C rule). Held in
       a temp for the lifetime of main: — a collected capture would silently stop
       reacting and the slider would appear to do nothing. *)
    capture:: platform SpanCapture onSpace: space spans: spans.
    capture start.
```

- [ ] **Step 2: Add the two routes**

After the `/stats` route registration (~line 233):

```smalltalk
    (* tuvm#72: the slider. Writing the cell is the whole mechanism — porta will
       write the same cell, so the page is a CLIENT of the facility and not the
       facility itself. Sliding off does not hand back the report: the reaction
       has not run yet at render time, and that is exactly what keeps the report
       out of the window it describes. Read it from /stats afterwards. *)
    http route: '/diag/spans/on' do: [ :id :plat |
        (space channel: 'diag/spans/ctl')
            put: '' value: (plat Record tag: #SpanCtl with: #state -> #running).
        plat httpChunkId: id data:
          '<html><body style=''font-family:system-ui,sans-serif;margin:3rem''>',
          '<p><b>capturing.</b> Run the load, then stop.</p>',
          '<p><a href=''/diag/spans/off''>stop capture</a> &middot; ',
          '<a href=''/stats''>&larr; stats</a></p></body></html>' ].
    http route: '/diag/spans/off' do: [ :id :plat |
        (space channel: 'diag/spans/ctl')
            put: '' value: (plat Record tag: #SpanCtl with: #state -> #stopped).
        plat httpChunkId: id data:
          '<html><body style=''font-family:system-ui,sans-serif;margin:3rem''>',
          '<p><b>captured.</b> The report is on the stats page.</p>',
          '<p><a href=''/stats''>&larr; stats</a></p></body></html>' ].
```

- [ ] **Step 3: Verify the node still compiles**

```bash
cd ~/workspace/nsl-tuvm && OEVM_DIR=~/workspace/tuvm python3 tools/run_tests_oevm.py 2>&1 | tail -8
```

Expected: `Passed: 209  Failed: 0  Skipped: 14`. The node file is not covered by the suite,
so this only proves nothing else broke. **Real verification is Step 4.**

- [ ] **Step 4: Flash and verify by hand**

The board is a WROOM-32. **Never trust a `ttyUSB` number** — three CP210x boards are on the
bench; identify the right one by MAC (`30:ae:a4:1e:7b:84`):

```bash
cd ~/workspace/tuvm/zephyr && python3 -c "
import subprocess,glob
for p in sorted(glob.glob('/dev/ttyUSB*')):
    try: print(p, subprocess.run(['esptool','--port',p,'read_mac'],capture_output=True,text=True,timeout=20).stdout.split('MAC: ')[-1].split()[0])
    except Exception as e: print(p,'err',e)
"
bash run.sh --board esp32 --node ThermalHttpNode.nsl --model B --pristine
```

Then, with the node on the network:

```bash
curl -s http://192.0.2.40:18080/diag/spans/on | head -3
curl -s http://192.0.2.40:18080/thermal > /dev/null
curl -s http://192.0.2.40:18080/diag/spans/off | head -3
curl -s http://192.0.2.40:18080/stats | grep -i -A3 'capture'
```

Expected: the `on` page says "capturing", the `off` page says "captured", and `/stats` shows
a capture result whose `n` reflects roughly the one `/thermal` fetch — **not** thousands of
samples accumulated since boot. If `n` looks like a since-boot number, the reset is not
firing; check that `capture start` is reached and that `space drain` runs in the main loop.

- [ ] **Step 5: Commit**

```bash
cd ~/workspace/tuvm && git add zephyr/ThermalHttpNode.nsl && git commit -m "feat(#72): drive the capture window from /stats

Two routes write diag/spans/ctl; the page is a client of the facility, not the
facility. Sliding off deliberately does not return the report — the reaction has
not run at render time, which is what keeps the report out of its own window.

Refs #72"
```

---

## Task 4: The result table, the legend, and the four readability fixes

**Files:**
- Modify: `~/workspace/tuvm/zephyr/ThermalHttpNode.nsl` — `statsPageOn:toId:spans:`,
  `latencyTableOn:toId:spans:`, `latencyRow:toId:on:spans:`, and two new helpers

**Interfaces:**
- Consumes: `diag/spans/result` (the `#SpanCapture` Record from Task 2).
- Produces: `groupDigits: anInteger on: platform` (String with thousands separators) and
  `spanLegendOn: platform toId: id` — both private to the page class.

**Why the cosmetics live here and not earlier:** they add chunks and bytes, and `egress`
records one sample per chunk. On the cumulative table that is contamination; on the capture
result table it is free, because that page renders outside every window. This ordering is
the point of the whole issue — do not move these edits earlier.

- [ ] **Step 1: Add the digit-grouping helper**

Zephyr has no locale support (picolibc/newlib), so `%'d` is unavailable and a C route would
mean writing the same loop in C plus a new prim. In nsl it is ten lines and no new surface.
Add to the page class, beside `statRow:value:toId:on:`:

```smalltalk
  (* tuvm#72: thousands separators. `String at:` answers a 1-based byte SMI and
     `platform string:` builds a String from an Array of bytes — the same
     construction prefixOf:upTo: uses. Walks from the right so the leading group
     may be 1, 2 or 3 digits. A leading '-' is left alone: it is not a digit and
     must not attract a separator. 44 is $,  and 45 is $- . *)
  groupDigits: anInteger on: platform = (
    | s n out i digits |
    s:: anInteger printString.
    n:: s size.
    out:: platform OrderedCollection new.
    digits:: 0.
    i:: n.
    [ i >= 1 ] whileTrue: [
      ((s at: i) = 45)
        ifTrue:  [ out add: 45 ]
        ifFalse: [
          ((digits > 0) and: [ digits \\ 3 = 0 ]) ifTrue: [ out add: 44 ].
          out add: (s at: i).
          digits:: digits + 1 ].
      i:: i - 1 ].
    ^ platform string: (self reversedArrayOf: out on: platform) )

  (* out was built right-to-left; hand back a left-to-right Array. *)
  reversedArrayOf: aCollection on: platform = (
    | n arr i |
    n:: aCollection size.
    arr:: platform basicNew: Array indexable: n.
    i:: 1. [ i <= n ] whileTrue: [
      arr at: i put: (aCollection at: n - i + 1). i:: i + 1 ].
    ^ arr )
```

**`on: platform` is not optional.** `ThermalHttpNode` is declared `= ()` — the outer class
has **no instance variables**, and every page method receives `platform` as a parameter.
A `groupDigits:` that referenced a bare `platform` would not compile.
```

- [ ] **Step 2: Home link above the `job latency` header**

In `latencyTableOn:toId:spans:` (~line 305), the chunk currently starts `'<h2>job latency</h2>'`.
Change that first line to put the link first, and keep the existing one at the page foot:

```smalltalk
      '<p><a href=''/''>&larr; home</a></p>',
      '<h2>job latency</h2>',
```

- [ ] **Step 3: Middle dot between histogram buckets**

In `latencyRow:toId:on:spans:` (~line 356), replace the bucket-joining loop:

```smalltalk
    b:: ''. j:: 6. [ j <= 11 ] whileTrue: [
      j > 6 ifTrue: [ b:: b , ' &middot; ' ].
      b:: b , (self groupDigits: (s basicAt: j) on: platform).
      j:: j + 1 ].
```

- [ ] **Step 4: Group the digits in every numeric cell**

Still in `latencyRow:toId:on:spans:`, replace each `(s basicAt: N) printString` and
`(spans failuresAt: slot) printString` with `(self groupDigits: (s basicAt: N) on: platform)`
and `(self groupDigits: (spans failuresAt: slot) on: platform)` respectively — fields 1, 2,
3, 4, 5. Do the same for `v printString` in `statRow:value:toId:on:` (~line 390), which
becomes `(self groupDigits: v on: platform)`.

Leave `(spans nameAt: slot) printString` alone — it is a symbol, not a number.

- [ ] **Step 5: The legend**

Add to the page class, and call it from `latencyTableOn:` immediately after the closing
`'</tbody></table>'`:

```smalltalk
  (* tuvm#72: the legend documents the COLUMNS and what each SPAN represents. A
     column legend alone leaves jobThermal versus acquire as a guess, which is the
     harder question and the one a reader at the bench actually has. Boundaries
     are pinned in docs/design/2026-07-21-job-latency-instrumentation-design.md
     §5 — this must not drift from it.
     Emitted as ONE chunk on purpose: egress records one sample per chunk, so a
     legend split across paragraphs would be a dozen extra samples. It is free
     here only because this page renders outside every capture window. *)
  spanLegendOn: platform toId: id = (
    platform httpChunkId: id data:
      '<h3>what the columns mean</h3>',
      '<dl style=''max-width:44rem''>',
      '<dt><b>n</b></dt><dd>samples recorded in the window.</dd>',
      '<dt><b>fails</b></dt><dd>jobs that ended without producing a sample. Counted ',
      'separately because a failure that vanished would silently IMPROVE the ',
      'percentiles &mdash; the run would look faster because the slow cases stopped ',
      'being measured.</dd>',
      '<dt><b>p50 / p95</b></dt><dd>the bucket DECADE a sample falls in, never a number ',
      'interpolated inside one. No p95 below n&nbsp;&ge;&nbsp;200.</dd>',
      '<dt><b>min / max &micro;s</b></dt><dd>exact, not bucketed.</dd>',
      '<dt><b>sumLo / sumHi</b></dt><dd>the sum, split at 1e9 as the C side keeps it. ',
      'Reassembling it here would overflow a 31-bit integer once sumHi reaches 2, ',
      'which is why the split exists. Divide it off the device.</dd>',
      '<dt><b>histogram</b></dt><dd>the six decade buckets, &lt;100&micro;s to &ge;1s.</dd>',
      '</dl>',
      '<h3>what the spans mean</h3>',
      '<dl style=''max-width:44rem''>',
      '<dt><b>tickQueue</b></dt><dd>pulse post &rarr; reaction entry: the reactor delay.</dd>',
      '<dt><b>acquire</b></dt><dd>the I2C read of the Grid-EYE.</dd>',
      '<dt><b>vmCompute</b></dt><dd>the 64-element raw&rarr;List copy.</dd>',
      '<dt><b>publish</b></dt><dd>the put onto the sample cell.</dd>',
      '<dt><b>jobThermal</b></dt><dd><b>Outer span, nesting the four above.</b> Its time is ',
      'not additional to theirs &mdash; summing the thermal rows double-counts.</dd>',
      '<dt><b>httpQueue</b></dt><dd>accept &rarr; take: how long a request waited to be served.</dd>',
      '<dt><b>render</b></dt><dd>the route block.</dd>',
      '<dt><b>egress</b></dt><dd>the socket writes, one sample per emitted chunk.</dd>',
      '</dl>',
      '<p style=''max-width:44rem''><b>render and egress overlap by design.</b> ',
      'Formatting happens in nsl and the write happens in C inside the same call, so ',
      'render necessarily contains the writes. Render-compute is <b>render &minus; egress</b>, ',
      'a DERIVED figure &mdash; the table does not measure it directly.</p>'.
    ^ self )
```

- [ ] **Step 6: Render the capture result table**

In `statsPageOn:toId:spans:`, after the existing `self latencyTableOn: platform toId: id spans: spans.`
call (~line 292):

```smalltalk
    (* tuvm#72: the clean table. The one above is cumulative since boot and
       partly measures its own rendering; this one covers a bounded window that
       excluded the reporting of it. Figures quoted in review/ or in the deck come
       from THIS table only. *)
    self captureTableOn: platform toId: id result: capture.
    self spanLegendOn: platform toId: id.
```

**The result Record is passed in, not fetched.** The page methods are pure formatters that
hold no capabilities — the file states this rule explicitly about `ThermalView`, and the
outer class has no `space` to reach anyway. So thread it from the route block, which does
have `space` in scope. Change the `/stats` route (Task 3) and the two forwarders:

```smalltalk
    http route: '/stats' do: [ :id :plat |
        self showStatsTo: id on: plat spans: spans
             capture: ((space channel: 'diag/spans/result') peek) ].
```

```smalltalk
  showStatsTo: id on: platform spans: spans capture: capture = (
    ^ self statsPageOn: platform toId: id spans: spans capture: capture )
```

and widen `statsPageOn:toId:spans:` to `statsPageOn:toId:spans:capture:`, passing `capture`
through to the new method:

```smalltalk
  captureTableOn: platform toId: id result: r = (
    | slots i row |
    platform httpChunkId: id data:
      '<h2>last capture</h2>',
      '<p style=''max-width:44rem''><a href=''/diag/spans/on''>start capture</a> &middot; ',
      '<a href=''/diag/spans/off''>stop capture</a></p>'.
    r isNil ifTrue: [
      platform httpChunkId: id data:
        '<p style=''max-width:44rem''>No capture yet. Start one, run the load, stop it, ',
        'then reload this page.</p>'.
      ^ self ].
    platform httpChunkId: id data:
      '<p style=''max-width:44rem''>',
      (self groupDigits: (r at: #n ifAbsent: [ 0 ]) on: platform), ' samples over ',
      (self groupDigits: (r at: #ms ifAbsent: [ -1 ]) on: platform), ' ms.</p>',
      '<table style=''border-collapse:collapse;width:100%''>',
      '<thead><tr><th style=''text-align:left;border-bottom:2px solid #ccc;padding:.4rem''>span</th>',
      '<th style=''text-align:right;border-bottom:2px solid #ccc;padding:.4rem''>n</th>',
      '<th style=''text-align:right;border-bottom:2px solid #ccc;padding:.4rem''>fails</th>',
      '<th style=''text-align:right;border-bottom:2px solid #ccc;padding:.4rem''>min &micro;s</th>',
      '<th style=''text-align:right;border-bottom:2px solid #ccc;padding:.4rem''>max &micro;s</th>',
      '</tr></thead><tbody>'.
    slots:: r at: #slots ifAbsent: [ nil ].
    i:: 1. [ i <= slots size ] whileTrue: [
      row:: slots at: i.
      platform httpChunkId: id data:
        '<tr><td style=''padding:.35rem;border-bottom:1px solid #eee''>',
        (row at: #span ifAbsent: [ #? ]) printString,
        '</td><td style=''padding:.35rem;border-bottom:1px solid #eee;text-align:right''>',
        (self groupDigits: (row at: #n ifAbsent: [ 0 ]) on: platform),
        '</td><td style=''padding:.35rem;border-bottom:1px solid #eee;text-align:right''>',
        (self groupDigits: (row at: #fails ifAbsent: [ 0 ]) on: platform),
        '</td><td style=''padding:.35rem;border-bottom:1px solid #eee;text-align:right''>',
        (self groupDigits: (row at: #minUs ifAbsent: [ 0 ]) on: platform),
        '</td><td style=''padding:.35rem;border-bottom:1px solid #eee;text-align:right''>',
        (self groupDigits: (row at: #maxUs ifAbsent: [ 0 ]) on: platform),
        '</td></tr>'.
      i:: i + 1 ].
    platform httpChunkId: id data: '</tbody></table>'.
    ^ self )
```

Every `groupDigits:` call carries `on: platform` — the page class has no instance variables,
so there is no other way to reach it.

- [ ] **Step 7: Suite, then flash and read the page**

```bash
cd ~/workspace/nsl-tuvm && OEVM_DIR=~/workspace/tuvm python3 tools/run_tests_oevm.py 2>&1 | tail -8
cd ~/workspace/tuvm/zephyr && bash run.sh --board esp32 --node ThermalHttpNode.nsl --model B --pristine
```

Then open `http://192.0.2.40:18080/stats` in a browser and confirm by eye: the home link
sits above `job latency`, numbers carry separators, buckets are dot-separated, the legend
reads correctly, and the capture table is present.

- [ ] **Step 8: Commit**

```bash
cd ~/workspace/tuvm && git add zephyr/ThermalHttpNode.nsl && git commit -m "feat(#72): the capture result table, the legend, and four readability fixes

Home link above the job-latency header, thousands separators, middle dots
between histogram buckets, and a legend documenting the columns AND what each
span represents (jobThermal is outer and summing double-counts; render/egress
overlap and render-compute is derived).

These land on the capture table, where they are free: that page renders outside
every window, so prose and separators cannot move a figure.

Refs #72, #61"
```

---

## Close-out

Per `CLAUDE.md`, an issue is not closed until its durable knowledge is promoted to a mainline
document of record. For this work:

- [ ] **Promote to `~/workspace/tuvm/review/program-sequence.md`** — the 2026-07-22 addendum
  on the job-latency rung currently says the HTTP figures should be re-taken. Extend it with
  the reason the re-take must use a capture window, and state the standing rule: **figures
  quoted in `review/` or in the deck come from the capture table only.**
- [ ] **`~/workspace/tuvm/review/tests/CLAIMS.md`** — no row moves. This work fixed a
  measurement defect and added no capability claim. Say so explicitly in the close-out rather
  than leaving it unstated.
- [ ] **Close tuvm#72** with the merge SHA, the measured image-size delta from Task 2 Step 6,
  and the new suite floor (209).
- [ ] **Leave #120 open.** §7 of the design is input to it; nothing in this plan
  discharges it.

## Then, and only then: the #61 re-take

This plan exists so the re-take is worth doing. Re-take the #61 table on this build using a
capture window, and update the deck figures (#70) from it. Do not re-take before Task 4 is
merged — the point is that the numbers come from a page whose rendering is outside them.
