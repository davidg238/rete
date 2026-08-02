# On-Target Measurement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the VM real (not host-proxy) on-target footprint numbers and expose live GC heap telemetry to nsl, so the nRF52840-DK's static image cost and dynamic arena behaviour can both be measured.

**Architecture:** One append-only platform leaf prim (104 `heapStats`) snapshots the existing C-side `oevm_heap_stats()` into a 5-slot Array; an nsl `HeapStats` value class names the fields. A layered footprint gate adds a CI-gated MCU cross-compile size ratchet (new named gate) plus a banked, DK-captured real `ram_report`/`rom_report` reference. A DK measurement node exercises the prim under an allocation soak and its output is banked alongside the image reports.

**Tech Stack:** C99 (tuvm engine), Newspeak dialect (`lib/kernel.ns`, `.ns` tests), Python both-path harness (`tools/run_tests_oevm.py`), Zephyr/west + `arm-zephyr-eabi` toolchain (NCS SDK), nRF52840-DK.

## Global Constraints

- **Prim ids are append-only; COUNT `PRIM_NAMES`, never compute.** The last prim is 103 (`asSymbol:`); this plan adds exactly one: **104 `heapStats`**. `dispatch.c` enum, `dispatch.c` `PRIM_NAMES[]`, and `src/prim_ids.h` `#define` must all agree, and the id must be arrived at by counting `PRIM_NAMES` entries.
- **GC is non-moving mark-sweep and does NOT scan the C stack.** Any heap oop held live across an allocation must be pinned via `gc_temp_root_push`/`gc_temp_roots_clear`. (Prim 104 avoids this: it reads the stats struct into C locals and does a single allocation with only SMI stores after it — no pin needed.)
- **AB full-suite runs CLOBBER `tuvm/src/solidified_image.c` + `.stmap`.** Always `git checkout -- src/solidified_image.c src/solidified_image.stmap` in `tuvm` before committing there. Baking a DK node also clobbers them — restore before any tuvm commit.
- **The controller owns full-suite runs:** `cd nsl-tuvm && OEVM_DIR=/home/david/workspace/tuvm OEVM_PATH=AB python3 tools/run_tests_oevm.py` (~10 min). Baseline before this rung is **178 passed / 0 failed / 12 skipped + 4 named gates**; this rung takes it to **179 / 0 / 12 + 5 named gates**.
- **Repo order: `tuvm` → `nsl-tuvm` → `nsl-tests`** (leaf prim before kernel class before test). All three are on branch `rung4-on-target-measurement` (create it in each repo if absent; it exists in `nsl-tuvm`).
- **Commit trailers** (every commit):
  ```
  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_015XawRA2Fw1zgPFQWCyXXmg
  ```
- **Absolute paths:** tuvm = `/home/david/workspace/tuvm`, nsl-tuvm = `/home/david/workspace/nsl-tuvm`, nsl-tests = `/home/david/workspace/nsl-tests`.

---

## File Structure

**tuvm (C engine + footprint + DK):**
- Modify `src/prim_ids.h` — add `#define PRIM_pf_heapStats 104` (after line 126, `PRIM_pf_asSymbol 103`).
- Modify `src/dispatch.c` — add `PRIM_pf_heapStats,` to the enum (after `PRIM_pf_asSymbol,`, ~line 704) and `"heapStats",` to `PRIM_NAMES[]` (after `"asSymbol:",`, ~line 748).
- Modify `src/platform.c` — add `case PRIM_pf_heapStats:` to `platform_dispatch` (after the `PRIM_pf_asSymbol` case, ~line 723).
- Modify `tools/footprint.sh` — MCU branch: skip-when-toolchain-absent (exit 77) + ratchet compare against baseline.
- Create `tools/footprint-mcu-baseline.txt` — banked `text` and `data_bss` totals.
- Create `zephyr/MeasureNodeDK.ns` — allocation-soak node that prints `heapStats`.
- Create `zephyr/FOOTPRINT_DK.md` — banked real DK `ram_report`/`rom_report` + soak readout.

**nsl-tuvm (kernel + harness):**
- Modify `lib/kernel.ns` — add `class HeapStats` (after the `Cbor` class, ~line 565) + two module accessors (`HeapStats`, `heapStats`) near the `Cbor` accessor (~line 1698).
- Modify `tools/run_tests_oevm.py` — add `run_footprint_gate()` (mirror `run_portability_gate`) and wire it into `suite_ok` + the summary line.

**nsl-tests (both-path test):**
- Create `15_heap_stats/01_invariants.ns` — invariant assertions, deterministic across Path A/B.

---

## Task 1: Prim 104 `heapStats` C leaf (tuvm)

**Files:**
- Modify: `/home/david/workspace/tuvm/src/prim_ids.h:126` (append after)
- Modify: `/home/david/workspace/tuvm/src/dispatch.c:704` (enum) and `:748` (`PRIM_NAMES`)
- Modify: `/home/david/workspace/tuvm/src/platform.c:723` (append case after `PRIM_pf_asSymbol`)

**Interfaces:**
- Consumes: `struct OevmHeapStats oevm_heap_stats(void)` (`src/oevm.h:414`, fields `heap_size, live_bytes, high_water, largest_free_block, gc_cycles`); `oop indexable_new(intptr_t n)` (non-static in `platform.c`; `slots[0]=TAG_SMI(n)`, values in `slots[1..n]`); `TAG_SMI`, `struct ObjectInstance`.
- Produces: platform prim **104 `heapStats`**, callable from nsl as `platform heapStats`, returning a 5-slot Array `[heapSize, liveBytes, highWater, largestFreeBlock, gcCycles]` (all SMIs).

- [ ] **Step 1: Verify the current prim count is 103**

Run: `grep -c '"' /dev/null; awk '/static const char \*const PRIM_NAMES/,/^};/' /home/david/workspace/tuvm/src/dispatch.c | grep -o '"[^"]*"' | wc -l`
Expected: `103` (the number of `PRIM_NAMES` entries before this append; the new id is 103+the-count-confirms-104).

- [ ] **Step 2: Add the `prim_ids.h` mirror define**

In `/home/david/workspace/tuvm/src/prim_ids.h`, immediately after line 126 (`#define PRIM_pf_asSymbol         103`), add:

```c
/* Rung 4 (on-target measurement) Task 1: heap-telemetry leaf (`heapStats`),
   appended after asSymbol: — no pre-existing id shifts. 104 was arrived at by
   COUNTING PRIM_NAMES (103 entries before this append), not by 103+1 arithmetic,
   per the standing lesson above. Platform-dispatched (mirrored here as 104).
   Returns a 5-slot Array [heapSize, liveBytes, highWater, largestFreeBlock,
   gcCycles] snapshotting src/gc.c oevm_heap_stats(); the nsl HeapStats class
   names the fields. */
#define PRIM_pf_heapStats 104
```

- [ ] **Step 3: Add the enum entry and `PRIM_NAMES` entry**

In `/home/david/workspace/tuvm/src/dispatch.c`, after `    PRIM_pf_asSymbol,` (~line 704), add:

```c
    /* Rung 4 (on-target measurement) Task 1: heap-telemetry leaf. Snapshots
       oevm_heap_stats() into a 5-slot Array of SMIs. Appended last — no
       pre-existing id shifts. Counted (not computed) to be id 104 — the 104th
       PRIM_NAMES entry. Platform-dispatched (mirrored in prim_ids.h as 104). */
    PRIM_pf_heapStats,
```

In the same file, after `    "asSymbol:",` (~line 748, the last `PRIM_NAMES` entry before `};`), add:

```c
    "heapStats",
```

- [ ] **Step 4: Add the `platform_dispatch` case**

In `/home/david/workspace/tuvm/src/platform.c`, after the closing `}` of the `case PRIM_pf_asSymbol:` block (~line 723) and before `case PRIM_pf_pumpEvents:`, add:

```c
    case PRIM_pf_heapStats: {
        /* Rung 4 Task 1: `platform heapStats` — snapshot GC heap telemetry
           (src/gc.c oevm_heap_stats) into a 5-slot Array of SMIs, in field order
           [heapSize, liveBytes, highWater, largestFreeBlock, gcCycles]. The nsl
           HeapStats class names them. Every field is < arena size (<= 48 KB on
           the 32-bit DK; the host default 16 MB arena still fits a 32-bit SMI),
           so each is SMI-representable on host and target alike. GC-safety: the
           whole struct is copied into the C local `st` BEFORE the single
           allocating call (indexable_new); the five stores are SMIs (immediates,
           no allocation), so no heap oop is ever held across an allocation and no
           gc_temp_root pin is required. args are ignored (0-arg selector). */
        struct OevmHeapStats st = oevm_heap_stats();
        oop r = indexable_new(5);
        ((struct ObjectInstance *)r)->slots[1] = TAG_SMI((intptr_t)st.heap_size);
        ((struct ObjectInstance *)r)->slots[2] = TAG_SMI((intptr_t)st.live_bytes);
        ((struct ObjectInstance *)r)->slots[3] = TAG_SMI((intptr_t)st.high_water);
        ((struct ObjectInstance *)r)->slots[4] = TAG_SMI((intptr_t)st.largest_free_block);
        ((struct ObjectInstance *)r)->slots[5] = TAG_SMI((intptr_t)st.gc_cycles);
        *out = r; return 1;
    }
```

- [ ] **Step 5: Compile the engine to verify it builds**

Run:
```bash
cd /home/david/workspace/tuvm && gcc -std=c99 -Isrc -c src/platform.c src/dispatch.c -o /dev/null 2>&1 || \
gcc -std=c99 -Isrc -fsyntax-only src/platform.c src/dispatch.c
```
Expected: no errors (clean compile). (The existing `test_heap_stats.c` already covers `oevm_heap_stats()` itself; the prim's Array mapping is verified end-to-end by the both-path test in Task 3, the project idiom for VM-dependent prims.)

- [ ] **Step 6: Run the existing C heap-stats unit test to confirm no regression**

Run: `cd /home/david/workspace/tuvm && bash tools/run_c_tests.sh 2>&1 | grep -E 'test_heap_stats|FAIL'`
Expected: `test_heap_stats` PASS, no `FAIL`.

- [ ] **Step 7: Commit**

```bash
cd /home/david/workspace/tuvm
git checkout -- src/solidified_image.c src/solidified_image.stmap 2>/dev/null || true
git add src/prim_ids.h src/dispatch.c src/platform.c
git commit -m "$(printf 'feat(oevm): heapStats prim 104 — GC heap telemetry to nsl\n\nAppend-only platform leaf 104: snapshots oevm_heap_stats() into a 5-slot\nArray [heapSize, liveBytes, highWater, largestFreeBlock, gcCycles]. Single\nalloc, SMI-only stores after it, no temp-root pin needed. Rung-4 Task 1.\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>\nClaude-Session: https://claude.ai/code/session_015XawRA2Fw1zgPFQWCyXXmg')"
```

---

## Task 2: MCU footprint ratchet + baseline (tuvm)

**Files:**
- Modify: `/home/david/workspace/tuvm/tools/footprint.sh:28-40` (MCU branch)
- Create: `/home/david/workspace/tuvm/tools/footprint-mcu-baseline.txt`

**Interfaces:**
- Consumes: `arm-zephyr-eabi-gcc`/`arm-zephyr-eabi-size` (NCS SDK on PATH via `source /home/david/workspace/ns-zephyr/tools/env.sh`).
- Produces: `FOOTPRINT_CONFIG=mcu bash tools/footprint.sh` exit codes — **0** = within baseline (PASS), **77** = cross toolchain absent (SKIP), **1** = regression (FAIL). `FOOTPRINT_UPDATE_BASELINE=1` rewrites the baseline. Consumed by Task 4's named gate.

- [ ] **Step 1: Replace the MCU branch with a skip-guard + ratchet**

In `/home/david/workspace/tuvm/tools/footprint.sh`, replace the `if [ "${FOOTPRINT_CONFIG:-host}" = "mcu" ]; then` block (lines 28-40) with:

```bash
if [ "${FOOTPRINT_CONFIG:-host}" = "mcu" ]; then
  XCC=${XCC:-arm-zephyr-eabi-gcc}
  # SKIP (exit 77) when the cross toolchain is absent, so the named gate reports
  # SKIP rather than FAIL on hosts without the NCS SDK sourced.
  command -v "$XCC" >/dev/null 2>&1 || {
    echo "footprint(mcu): SKIP — $XCC not on PATH (source ns-zephyr/tools/env.sh)" >&2
    exit 77; }
  MCPU="-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16"
  $XCC -Os -std=c99 -Isrc $MCPU -D__ZEPHYR__=1 -DOEVM_NO_FILE_IO -c \
    src/vm.c src/gc.c src/dispatch.c src/kernel_boot.c src/platform.c \
    src/serial.c src/udp.c src/mirror.c src/oimg_loader.c src/space.c src/handles.c || {
      echo "footprint(mcu): cross-compile failed" >&2; rm -f *.o; exit 1; }
  # TOTAL line from arm-zephyr-eabi-size -t: "  text  data  bss  dec  hex  filename".
  read -r CUR_TEXT CUR_DATA CUR_BSS _ < <(arm-zephyr-eabi-size -t \
    vm.o gc.o dispatch.o kernel_boot.o platform.o serial.o udp.o \
    mirror.o oimg_loader.o space.o handles.o | awk 'END { print $1, $2, $3 }')
  rm -f *.o
  CUR_DATA_BSS=$(( CUR_DATA + CUR_BSS ))
  echo "footprint(mcu): text=$CUR_TEXT data+bss=$CUR_DATA_BSS"

  BASELINE="tools/footprint-mcu-baseline.txt"
  if [ "${FOOTPRINT_UPDATE_BASELINE:-0}" = "1" ]; then
    printf 'text %s\ndata_bss %s\n' "$CUR_TEXT" "$CUR_DATA_BSS" > "$BASELINE"
    echo "footprint(mcu): baseline updated -> text=$CUR_TEXT data+bss=$CUR_DATA_BSS"
    exit 0
  fi
  if [ ! -f "$BASELINE" ]; then
    echo "footprint(mcu): FAIL — no baseline; run FOOTPRINT_UPDATE_BASELINE=1 first" >&2
    exit 1; fi
  BASE_TEXT=$(awk '/^text /{print $2}' "$BASELINE")
  BASE_DATA_BSS=$(awk '/^data_bss /{print $2}' "$BASELINE")
  RC=0
  if [ "$CUR_TEXT" -gt "$BASE_TEXT" ]; then
    echo "footprint(mcu): FAIL text $CUR_TEXT > baseline $BASE_TEXT" >&2; RC=1; fi
  if [ "$CUR_DATA_BSS" -gt "$BASE_DATA_BSS" ]; then
    echo "footprint(mcu): FAIL data+bss $CUR_DATA_BSS > baseline $BASE_DATA_BSS" >&2; RC=1; fi
  [ "$RC" -eq 0 ] && echo "footprint(mcu): PASS (text<=$BASE_TEXT data+bss<=$BASE_DATA_BSS)"
  exit "$RC"
fi
```

- [ ] **Step 2: Confirm the skip path works without the toolchain**

Run: `cd /home/david/workspace/tuvm && FOOTPRINT_CONFIG=mcu bash tools/footprint.sh; echo "exit=$?"`
Expected (no SDK sourced): `footprint(mcu): SKIP …` and `exit=77`. (If the SDK IS on PATH, expect either the FAIL-no-baseline message + `exit=1`, or a real size line — proceed to Step 3 to establish the baseline.)

- [ ] **Step 3: Establish the baseline (requires the cross toolchain)**

Run:
```bash
source /home/david/workspace/ns-zephyr/tools/env.sh
cd /home/david/workspace/tuvm
FOOTPRINT_UPDATE_BASELINE=1 FOOTPRINT_CONFIG=mcu bash tools/footprint.sh
cat tools/footprint-mcu-baseline.txt
```
Expected: prints `footprint(mcu): baseline updated -> text=<N> data+bss=<M>`, and `footprint-mcu-baseline.txt` contains two lines `text <N>` / `data_bss <M>` with real integers. **If the toolchain is unavailable on this machine, defer Steps 3–4 to the DK task (Task 5), which runs on the SDK-equipped host, and note the deferral in the commit.**

- [ ] **Step 4: Confirm the ratchet passes at baseline and fails on injected growth**

Run:
```bash
source /home/david/workspace/ns-zephyr/tools/env.sh
cd /home/david/workspace/tuvm
FOOTPRINT_CONFIG=mcu bash tools/footprint.sh; echo "pass_exit=$?"
# temporarily lower the baseline text by 1 to simulate a regression
awk '/^text /{print "text", $2-1; next} {print}' tools/footprint-mcu-baseline.txt > /tmp/bl && cp /tmp/bl tools/footprint-mcu-baseline.txt
FOOTPRINT_CONFIG=mcu bash tools/footprint.sh; echo "fail_exit=$?"
git checkout -- tools/footprint-mcu-baseline.txt
```
Expected: `pass_exit=0` then a `FAIL text …` line with `fail_exit=1`; the `git checkout` restores the true baseline.

- [ ] **Step 5: Commit**

```bash
cd /home/david/workspace/tuvm
git add tools/footprint.sh tools/footprint-mcu-baseline.txt
git commit -m "$(printf 'feat(footprint): MCU cross-size ratchet + baseline (tuvm#5)\n\nfootprint.sh mcu mode now SKIPs (exit 77) without the cross toolchain and\notherwise ratchets text + data+bss against tools/footprint-mcu-baseline.txt\n(regression = exit 1). FOOTPRINT_UPDATE_BASELINE=1 rebases. Rung-4 Task 2.\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>\nClaude-Session: https://claude.ai/code/session_015XawRA2Fw1zgPFQWCyXXmg')"
```

---

## Task 3: `HeapStats` class + both-path invariant test (nsl-tuvm + nsl-tests)

**Files:**
- Modify: `/home/david/workspace/nsl-tuvm/lib/kernel.ns` (add `class HeapStats` after the `Cbor` class ~line 565; add two accessors near ~line 1698)
- Create: `/home/david/workspace/nsl-tests/15_heap_stats/01_invariants.ns`

**Interfaces:**
- Consumes: `platform heapStats` (prim 104 from Task 1) → 5-slot Array.
- Produces: nsl `HeapStats` value class with accessors `heapSize`, `liveBytes`, `highWater`, `largestFreeBlock`, `gcCycles`, `printString`; class-side `fromList:`. Module accessors: `platform HeapStats` (the class) and `platform heapStats` (a fresh snapshot instance).

- [ ] **Step 1: Write the failing both-path test**

Create `/home/david/workspace/nsl-tests/15_heap_stats/01_invariants.ns`:

```
(*
expect:
true
true
true
true
*)
(* Rung 4 Task 3: heapStats runtime-telemetry invariants. Absolute field values
   vary run-to-run, so this asserts only the structural invariants that hold on
   every run and on BOTH paths (A solidified image, B .oimg): the arena is
   non-empty, high_water never trails live_bytes, the largest free extent never
   exceeds the arena, and gc_cycles is a non-negative count. Exercises prim 104
   -> the HeapStats class end-to-end. Both-path A==B gate. *)
class HeapStatsInvariantsTest packageUsing: manifest = () (
public main: platform args: args = (
  | s |
  s:: platform heapStats.
  (s heapSize > 0) printString println.
  (s highWater >= s liveBytes) printString println.
  (s largestFreeBlock <= s heapSize) printString println.
  (s gcCycles >= 0) printString println.
)
) : (
)
```

- [ ] **Step 2: Run it to verify it fails (heapStats not yet in kernel)**

Run:
```bash
cd /home/david/workspace/nsl-tuvm
OEVM_DIR=/home/david/workspace/tuvm OEVM_PATH=A python3 tools/run_tests_oevm.py 15_heap_stats/01_invariants 2>&1 | tail -20
```
Expected: FAIL — a DNU / does-not-understand on `heapStats` (the module accessor doesn't exist yet), or an output mismatch. (Exact harness single-test invocation: if the path filter differs, run the whole suite path `OEVM_PATH=A` and confirm `15_heap_stats` is red.)

- [ ] **Step 3: Add the `HeapStats` class to `lib/kernel.ns`**

In `/home/david/workspace/nsl-tuvm/lib/kernel.ns`, after the closing `)` of `class Cbor = () ( … )` (the Cbor class ends before the next `class …`; insert as a sibling class), add:

```
  class HeapStats = ( | heapSize liveBytes highWater largestFreeBlock gcCycles | ) (
    (* A read-only snapshot of GC heap telemetry (src/gc.c oevm_heap_stats),
       surfaced by the native `platform heapStats` leaf (prim 104) as a 5-element
       Array in field order. Construct via the class-side `fromList:`. *)
    public heapSize         = ( ^ heapSize )
    public liveBytes        = ( ^ liveBytes )
    public highWater        = ( ^ highWater )
    public largestFreeBlock = ( ^ largestFreeBlock )
    public gcCycles         = ( ^ gcCycles )
    setFromList: a = (
      heapSize:: a at: 1. liveBytes:: a at: 2. highWater:: a at: 3.
      largestFreeBlock:: a at: 4. gcCycles:: a at: 5. ^ self )
    public printString = (
      ^ 'live ', liveBytes printString, ' / hw ', highWater printString,
        ' / free ', largestFreeBlock printString, ' / gc ', gcCycles printString,
        ' / size ', heapSize printString )
  ) : (
    public fromList: a = ( ^ self new setFromList: a )
  )
```

- [ ] **Step 4: Add the module accessors**

In `/home/david/workspace/nsl-tuvm/lib/kernel.ns`, immediately after the `public Cbor              = ( ^ Cbor new )` line (~line 1698), add:

```
  public HeapStats         = ( ^ HeapStats )
  public heapStats         = ( ^ HeapStats fromList: (platform heapStats) )
```

- [ ] **Step 5: Run the test on Path A to verify it passes**

Run:
```bash
cd /home/david/workspace/nsl-tuvm
OEVM_DIR=/home/david/workspace/tuvm OEVM_PATH=A python3 tools/run_tests_oevm.py 15_heap_stats/01_invariants 2>&1 | tail -20
git -C /home/david/workspace/tuvm checkout -- src/solidified_image.c src/solidified_image.stmap
```
Expected: PASS — four `true` lines matching `expect:`.

- [ ] **Step 6: Run both paths to verify A == B**

Run:
```bash
cd /home/david/workspace/nsl-tuvm
OEVM_DIR=/home/david/workspace/tuvm OEVM_PATH=AB python3 tools/run_tests_oevm.py 15_heap_stats/01_invariants 2>&1 | tail -20
git -C /home/david/workspace/tuvm checkout -- src/solidified_image.c src/solidified_image.stmap
```
Expected: PASS on both A and B (A==B agreement).

- [ ] **Step 7: Commit (both repos)**

```bash
cd /home/david/workspace/nsl-tuvm
git add lib/kernel.ns
git commit -m "$(printf 'feat(kernel): HeapStats value class over platform heapStats (prim 104)\n\nNames the 5-slot heapStats Array [heapSize/liveBytes/highWater/\nlargestFreeBlock/gcCycles] + printString; module accessors HeapStats and\nheapStats (fresh snapshot). Rung-4 Task 3.\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>\nClaude-Session: https://claude.ai/code/session_015XawRA2Fw1zgPFQWCyXXmg')"

cd /home/david/workspace/nsl-tests
git checkout -b rung4-on-target-measurement 2>/dev/null || git checkout rung4-on-target-measurement
git add 15_heap_stats/01_invariants.ns
git commit -m "$(printf 'test(heap-stats): both-path heapStats invariants (15_heap_stats)\n\nStructural invariants that hold every run on A and B: heapSize>0,\nhighWater>=liveBytes, largestFreeBlock<=heapSize, gcCycles>=0. Rung-4 Task 3.\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>\nClaude-Session: https://claude.ai/code/session_015XawRA2Fw1zgPFQWCyXXmg')"
```

---

## Task 4: Footprint named gate in the harness (nsl-tuvm)

**Files:**
- Modify: `/home/david/workspace/nsl-tuvm/tools/run_tests_oevm.py` (add `run_footprint_gate` after `run_portability_gate` ~line 247; wire into `suite_ok` and the summary print ~line 618-627)

**Interfaces:**
- Consumes: `FOOTPRINT_CONFIG=mcu bash <OEVM_DIR>/tools/footprint.sh` (Task 2) — exit 0/77/1 = PASS/SKIP/FAIL.
- Produces: a `footprint` named gate in the standard suite, reported alongside `grant`, `debugger-integration`, `portability-32`, `boot-gc-asan`.

- [ ] **Step 1: Add the gate runner**

In `/home/david/workspace/nsl-tuvm/tools/run_tests_oevm.py`, after the end of `run_portability_gate()` (~line 247), add:

```python
def run_footprint_gate() -> str:
    """Gate the MCU cross-compile footprint ratchet (tuvm/tools/footprint.sh).

    Cross-compiles the engine core with arm-zephyr-eabi and ratchets text +
    data+bss against tools/footprint-mcu-baseline.txt: a size regression is a
    gate failure like a text regression (design addendum A2 / tuvm#5). Returns
    "PASS", "SKIP" (exit 77 — no cross toolchain on PATH), or "FAIL"."""
    script = OEVM_DIR / "tools" / "footprint.sh"
    try:
        proc = subprocess.run(
            ["bash", str(script)],
            cwd=str(OEVM_DIR), capture_output=True, text=True, timeout=300,
            env={**os.environ, "FOOTPRINT_CONFIG": "mcu"})
    except Exception as e:
        print(f"{COLOR_RED}footprint: FAIL{COLOR_RESET} (runner error: {e})")
        return "FAIL"
    if proc.returncode == 77:
        print(f"footprint: SKIP (no arm-zephyr-eabi toolchain)")
        return "SKIP"
    if proc.returncode != 0:
        print(f"{COLOR_RED}footprint: FAIL{COLOR_RESET}")
        out = (proc.stdout or "") + (proc.stderr or "")
        print("  " + "\n  ".join(out.strip().splitlines()[-30:]))
        return "FAIL"
    print(f"{COLOR_GREEN}footprint: PASSED{COLOR_RESET}")
    return "PASS"
```

- [ ] **Step 2: Confirm `os` is imported (the gate uses `os.environ`)**

Run: `grep -n '^import os\|^import ' /home/david/workspace/nsl-tuvm/tools/run_tests_oevm.py | head`
Expected: an `import os` line exists. If it does not, add `import os` near the other imports at the top of the file.

- [ ] **Step 3: Wire the gate into the suite result and summary**

In `/home/david/workspace/nsl-tuvm/tools/run_tests_oevm.py`, near where `portability = run_portability_gate()` is called (just before `suite_ok = …` ~line 618), add a call:

```python
    footprint = run_footprint_gate()
```

Then change the `suite_ok` expression (~line 618) to also require the footprint gate (SKIP is acceptable, like portability):

```python
    suite_ok = (failed == 0 and c_units_ok and grant_ok and dbg_int_ok
                and boot_gc_ok
                and portability != "FAIL"
                and footprint != "FAIL")
```

(Match the exact existing operands on that line; add `and footprint != "FAIL"` without dropping any existing condition.)

And extend the summary print (~line 626-627) to include footprint:

```python
    print(f"  grant: {'PASS' if grant_ok else 'FAIL'}  "
          f"debugger-integration: {'PASS' if dbg_int_ok else 'FAIL'}  "
          f"portability-32: {portability}  "
          f"boot-gc-asan: {'PASS' if boot_gc_ok else 'FAIL'}  "
          f"footprint: {footprint}")
```

- [ ] **Step 4: Syntax-check and dry-run the harness gate wiring**

Run: `cd /home/david/workspace/nsl-tuvm && python3 -c "import ast; ast.parse(open('tools/run_tests_oevm.py').read()); print('ok')"`
Expected: `ok`.

- [ ] **Step 5: Commit**

```bash
cd /home/david/workspace/nsl-tuvm
git add tools/run_tests_oevm.py
git commit -m "$(printf 'feat(harness): footprint named gate — MCU size ratchet in the suite\n\nrun_footprint_gate() runs FOOTPRINT_CONFIG=mcu footprint.sh; FAIL fails the\nsuite, SKIP tolerated (no cross toolchain), like portability-32. Reported in\nthe named-gate summary. Rung-4 Task 4.\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>\nClaude-Session: https://claude.ai/code/session_015XawRA2Fw1zgPFQWCyXXmg')"
```

---

## Task 5: DK capture — image reports + live heapStats soak (tuvm, on hardware)

> This is the Task-8-style manual acceptance bar. It runs on the SDK-equipped host with the nRF52840-DK plugged in. If the DK's on-board J-Link enters a firmware-replace loop (no `/dev/ttyACM0`), power-cycle the on-board-debug USB (see `zephyr/README.md`).

**Files:**
- Create: `/home/david/workspace/tuvm/zephyr/MeasureNodeDK.ns`
- Create: `/home/david/workspace/tuvm/zephyr/FOOTPRINT_DK.md`

**Interfaces:**
- Consumes: prim 104 + `HeapStats` (Tasks 1, 3); the config-D env + west recipe from `zephyr/README.md`; `west build -t ram_report` / `-t rom_report`.
- Produces: banked real DK `ram_report`/`rom_report` + a live under-load `HeapStats` line in `FOOTPRINT_DK.md`.

- [ ] **Step 1: Write the measurement node**

Create `/home/david/workspace/tuvm/zephyr/MeasureNodeDK.ns` (mirrors `PulseNodeDK.ns`'s class/main shape; prints via `… out`, the DK serial sink):

```
(*
MeasureNodeDK.ns — rung-4 on-target measurement node. Runs a bounded allocation
soak (build+drop short Lists to move live_bytes/high_water and trigger a few GC
cycles), then prints the heapStats snapshot over the DK console (uart0/VCOM) via
`… out`, exactly as PulseNodeDK.ns emits its line. Baked into the DK boot image
with (run from the nsl-tuvm repo):
  python3 tools/ns_compile_oevm.py \
    /home/david/workspace/tuvm/zephyr/MeasureNodeDK.ns \
    /home/david/workspace/tuvm/src/solidified_image.c
*)
class MeasureNodeDK packageUsing: manifest = () (
  public main: platform args: args = (
    | L acc |
    L:: platform List.
    1 to: 3000 do: [ :i | acc:: L with: i with: (i + 1) with: (i + 2) ].
    ('heapStats ' , (platform heapStats) printString) out.
  )
) : (
)
```

- [ ] **Step 2: Bake the node, build config-D, capture the image reports**

Run:
```bash
source /home/david/workspace/ns-zephyr/tools/env.sh
export OEVM_HEAP_SIZE=49152 METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256
export VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512
cd /home/david/workspace/nsl-tuvm
python3 tools/ns_compile_oevm.py \
  /home/david/workspace/tuvm/zephyr/MeasureNodeDK.ns \
  /home/david/workspace/tuvm/src/solidified_image.c
cd /home/david/workspace/tuvm/zephyr
west build -b nrf52840dk/nrf52840 -p always . -- -DCONF_FILE=prj_dk.conf
west build -t ram_report 2>&1 | tee /tmp/dk_ram_report.txt
west build -t rom_report 2>&1 | tee /tmp/dk_rom_report.txt
```
Expected: a successful build; `ram_report`/`rom_report` print the section tables ending in a total RAM/ROM figure (and %). Record the totals.

- [ ] **Step 3: Flash and capture the live heapStats line**

Run:
```bash
cd /home/david/workspace/tuvm/zephyr
west flash --skip-rebuild --runner jlink
# open the console (115200) and capture one boot's output:
timeout 15 cat /dev/ttyACM0 | tee /tmp/dk_heapstats.txt
```
Expected: after the boot banner, a line `heapStats live <N> / hw <N> / free <N> / gc <N> / size 49152`. Confirm `hw <= size` and `free <= size` and `gc >= 1` (the soak forced at least one collection).

- [ ] **Step 4: Restore the checked-in image (MANDATORY before any tuvm commit)**

Run:
```bash
cd /home/david/workspace/tuvm
git checkout -- src/solidified_image.c src/solidified_image.stmap
git status --short src/
```
Expected: no `src/solidified_image.*` shown as modified.

- [ ] **Step 5: Bank the numbers**

Create `/home/david/workspace/tuvm/zephyr/FOOTPRINT_DK.md` filling in the captured values (this is a data-recording step — paste the real numbers from Steps 2-3, do not leave placeholders):

```markdown
# nRF52840-DK on-target footprint + heap telemetry (rung-4)

Captured on the DK (`nrf52840dk/nrf52840`) with the recommended **config D**
(`OEVM_HEAP_SIZE=49152 METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256
VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512`), image baked from
`zephyr/MeasureNodeDK.ns`. Recipe: `zephyr/README.md` "Build + flash + run".

Commit: tuvm `<sha>`  ·  date 2026-07-10

## Static image (`west build -t ram_report` / `-t rom_report`)

| Region | Bytes | % of nRF52840 |
| ------ | ----- | ------------- |
| ROM    | <N>   | <P> %         |
| RAM    | <N>   | <P> %         |

(nRF52840: 1 MB flash / 256 KB SRAM.)

## Live heap telemetry under the allocation soak (prim 104 `heapStats`)

Console line (uart0/VCOM) after the 3000-iteration List soak:

```
heapStats live <N> / hw <N> / free <N> / gc <N> / size 49152
```

Invariants hold on hardware: `hw <= size`, `free <= size`, `gc >= 1`.
```

- [ ] **Step 6: Commit**

```bash
cd /home/david/workspace/tuvm
git add zephyr/MeasureNodeDK.ns zephyr/FOOTPRINT_DK.md
git commit -m "$(printf 'feat(zephyr): DK on-target measurement node + banked footprint/heap telemetry\n\nMeasureNodeDK.ns runs an allocation soak then prints heapStats over VCOM.\nFOOTPRINT_DK.md banks real config-D ram_report/rom_report + the live under-load\nHeapStats line captured on the nRF52840-DK. Rung-4 Task 5.\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>\nClaude-Session: https://claude.ai/code/session_015XawRA2Fw1zgPFQWCyXXmg')"
```

---

## Task 6: Full-suite gate + footprint banking + docs reconcile (all repos)

**Files:**
- Modify: `/home/david/workspace/nsl-tests/00_perf/RESULTS.md` (cross-link `FOOTPRINT_DK.md`)
- Modify: `/home/david/workspace/nsl-tuvm/docs/design/2026-07-10-on-target-measurement-design.md` (mark as-built if any deviations)

**Interfaces:**
- Consumes: everything from Tasks 1-5.
- Produces: green full both-path suite (179/0/12 + 5 named gates), reconciled docs, per-strand footprint delta banked.

- [ ] **Step 1: Run the full both-path suite (controller-owned)**

Run:
```bash
cd /home/david/workspace/nsl-tuvm
OEVM_DIR=/home/david/workspace/tuvm OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | tail -30
git -C /home/david/workspace/tuvm checkout -- src/solidified_image.c src/solidified_image.stmap
```
Expected: **179 passed / 0 failed / 12 skipped**; named-gate summary shows `grant: PASS  debugger-integration: PASS  portability-32: PASS|SKIP  boot-gc-asan: PASS  footprint: PASS|SKIP`. `footprint` may be SKIP if the cross toolchain isn't on this host's PATH — that is acceptable (it PASSes on the SDK host where Task 2/5 ran).

- [ ] **Step 2: Run the C unit suite**

Run: `cd /home/david/workspace/tuvm && bash tools/run_c_tests.sh 2>&1 | tail -15`
Expected: 0 failures; `test_heap_stats` PASS.

- [ ] **Step 3: Cross-link the DK footprint from RESULTS.md**

In `/home/david/workspace/nsl-tests/00_perf/RESULTS.md`, add a one-line pointer near the DK config-D table:

```markdown
> On-target static footprint + live heap telemetry (rung-4): see
> `tuvm/zephyr/FOOTPRINT_DK.md` (real `ram_report`/`rom_report` + `heapStats` soak).
```

- [ ] **Step 4: Reconcile the design doc to as-built**

Open `/home/david/workspace/nsl-tuvm/docs/design/2026-07-10-on-target-measurement-design.md`; if the implementation deviated (e.g. the both-path test lives at `15_heap_stats/01_invariants.ns`, no new C test since `test_heap_stats.c` already covered `oevm_heap_stats()`), add a short "As-built notes" section recording the final paths, the prim id (104), and the suite count (179/0/12 + 5 gates). If there were no deviations beyond these, record exactly that.

- [ ] **Step 5: Bank the footprint delta**

Append to the design doc's as-built notes the engine size delta for prim 104 (from `size` before/after on `platform.o`+`dispatch.o`) and the kernel image delta (the `HeapStats` class lines are image/ROM only, no RAM/bss). Compute with:
```bash
cd /home/david/workspace/tuvm && FOOTPRINT_CONFIG=host bash tools/footprint.sh
```
Record the numbers (this is a data-recording step, not a placeholder).

- [ ] **Step 6: Commit the docs (both repos)**

```bash
cd /home/david/workspace/nsl-tests
git add 00_perf/RESULTS.md
git commit -m "$(printf 'docs(perf): cross-link DK on-target footprint + heap telemetry (rung-4)\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>\nClaude-Session: https://claude.ai/code/session_015XawRA2Fw1zgPFQWCyXXmg')"

cd /home/david/workspace/nsl-tuvm
git add docs/design/2026-07-10-on-target-measurement-design.md
git commit -m "$(printf 'docs(rung4): reconcile on-target measurement design to as-built\n\nPrim 104, 15_heap_stats both-path test, 179/0/12 + 5 named gates, footprint\ndeltas banked. Rung-4 Task 6.\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>\nClaude-Session: https://claude.ai/code/session_015XawRA2Fw1zgPFQWCyXXmg')"
```

- [ ] **Step 7: Final whole-branch review**

Request a whole-branch review across all three repos (`rung4-on-target-measurement`) per the milestone pattern before any merge. Merge/push is David's call (do not merge or push without explicit instruction).

---

## Self-Review (author checklist — completed at write time)

- **Spec coverage:** §3 heapStats prim+class → Tasks 1, 3. §4.1 CI ratchet named gate → Tasks 2, 4. §4.2 banked DK reference → Task 5 (`FOOTPRINT_DK.md`). §5.1 both-path invariant test → Task 3. §5.2 C coverage → covered by the pre-existing `test_heap_stats.c` (Task 1 Step 6 re-runs it; noted as-built). §5.3 DK soak → Task 5. §6 repo order/sequencing/footprint → Tasks ordered tuvm→nsl-tuvm→nsl-tests; delta banked Task 6. §7 acceptance → Task 6.
- **Placeholder scan:** the `<N>`/`<P>`/`<sha>` tokens in Task 5 Step 5 and Task 6 Step 5 are explicit data-recording slots filled from captured command output, not deferred work — the surrounding steps state exactly which command produces each value.
- **Type consistency:** field order `[heapSize, liveBytes, highWater, largestFreeBlock, gcCycles]` is identical in the C prim (Task 1 Step 4 `slots[1..5]`), the nsl `setFromList:` (Task 3 Step 3 `a at: 1..5`), and the `printString`. Accessor names match between the class and the test. `run_footprint_gate` returns `"PASS"|"SKIP"|"FAIL"` consistent with the `suite_ok`/summary wiring.
</content>
