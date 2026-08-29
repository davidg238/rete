# W2a.1 — Float-Arena GC Trigger Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `float_alloc` trigger `gc_collect` at a soft slab-count cap so float-dominated workloads bound their RSS (mandelbrot@750 @256 KB cap: 2.35 GB → single-digit MB), genuinely closing GH #5/#3.

**Architecture:** At the one point where the float arena grows (free-list empty AND bump slab full → about to `malloc` a new slab), if total slabs have reached a fixed `-D`-tunable budget, run `gc_collect` (whose W2a `float_sweep` refills the free-list) and reuse a reclaimed cell instead of growing; only `malloc` if the live set genuinely needs it. This mirrors `gc_allocate`'s collect-on-exhaustion logic and has a fixed point (no creep). Fast paths are untouched.

**Tech Stack:** C99, single-file change in `src/gc.c`; unit tests built with `gcc` directly; conformance via `ns-oevm/tools/run_tests_oevm.py`; RSS acceptance via `bench.sh` build + `/usr/bin/time -v`.

## Global Constraints

- All implementation changes confined to the float-arena section of `src/gc.c` plus `tests/test_float_gc.c`. **No change** to `struct Float`, `oop`, the W2a sweep/free-list mechanics, or anything the compiler emits.
- The budget gate is the **existing** `static size_t float_slab_count` (total slabs). **No new counter; `float_sweep` is unchanged.**
- Budget macro: `OEVM_FLOAT_ARENA_SLAB_BUDGET`, default `16`, overridable via `-D`.
- The trigger engages **only** at the slab-grow boundary — the free-list-draw and in-slab-bump fast paths must stay zero-overhead.
- Non-moving; cell free-list only (no slab freeing) — unchanged from W2a.
- `gc_in_progress` guard around the `float_alloc`-triggered `gc_collect`, and `!gc_in_progress` checked before triggering.
- Post-collect free-list draw uses the existing overlay-`value` unthread idiom; it does NOT get the bump-path markbit clear (free-list cells already carry a clear bit).
- `src/solidified_image.c` is a generated artifact — the harness overwrites it; `git restore src/solidified_image.c` before any commit, and never commit it.
- Conformance must stay **47 pass / 0 fail / 1 skip** on Model A + Model B + `-m32`.
- Commit trailer: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

---

## File Structure

- `src/gc.c` — float-arena section (~lines 207–259): add the `OEVM_FLOAT_ARENA_SLAB_BUDGET` macro and the `float_arena_slab_budget()` accessor; insert the collect-at-cap block at the top of `float_alloc`'s new-slab branch.
- `tests/test_float_gc.c` — add a self-triggering-bound test (the regression that would have caught the gap).

### Interfaces produced

```c
oop    float_alloc(oop vtable);          /* existing signature; grows via collect-at-cap */
size_t float_arena_slab_budget(void);    /* NEW test-only: returns the compiled budget macro */
```

---

## Task 1: Collect-at-cap trigger in `float_alloc` + self-triggering bound test

The whole feature is one cohesive change to `float_alloc` plus the test that proves the arena now self-bounds. It is a single reviewable unit.

**Files:**
- Modify: `src/gc.c` (float-arena section ~lines 207–262)
- Test: `tests/test_float_gc.c`

**Interfaces:**
- Consumes (already in `src/gc.c`): `float_freelist`, `float_slab`, `float_slab_used`, `float_slab_count`, `FLOAT_ARENA_SLAB_CELLS`, `struct FloatSlab`, the `gc_in_progress` static (line ~96), the `void gc_collect(void);` forward decl (line ~130), `float_arena_slab_count()`, the overlay-`value` free-list idiom.
- Produces: `float_arena_slab_budget()`; a `float_alloc` that triggers `gc_collect` at the cap.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_float_gc.c` (function body above `main`, call inside `main` before the final `printf("ALL TESTS PASSED\n")`). Also add the `extern` for the new accessor near the other `extern` declarations at the top of the file:

```c
extern size_t float_arena_slab_budget(void);
```

```c
/* W2a.1: the float arena self-triggers a collection at its soft cap and stays
   bounded WITHOUT any explicit gc_collect call. Without the trigger, the slab
   count grows without limit. Conservative scan disabled so the dropped floats
   are deterministically unreachable (precise roots don't cover them). */
__attribute__((noinline))
static void churn_floats_no_collect(size_t n) {   /* locals die at return */
    volatile double sink = 0.0;
    for (size_t i = 0; i < n; i++) {
        oop f = float_alloc(VT);
        ((struct Float *)f)->value = (double)i;
        sink += ((struct Float *)f)->value;
    }
    (void)sink;
}
static void test_arena_self_triggers_bound(void) {
    void *saved = vm_c_stack_base;
    vm_c_stack_base = NULL;                 /* dropped floats unreachable, deterministic */
    size_t budget = float_arena_slab_budget();
    /* Allocate FAR more than budget*4096 cells, all dropped. With the trigger the
       arena collects-and-reuses at the cap; without it, it grows ~linearly. */
    churn_floats_no_collect((budget + 8) * FLOAT_ARENA_SLAB_CELLS_T * 4);
    assert(float_arena_slab_count() <= budget + 2);
    vm_c_stack_base = saved;
    printf("test_float_gc: arena self-triggers + bounded OK (%zu/%zu slabs)\n",
           float_arena_slab_count(), budget);
}
```

Add `test_arena_self_triggers_bound();` in `main` before `printf("ALL TESTS PASSED\n");`.

(`FLOAT_ARENA_SLAB_CELLS_T` and `VT` already exist in this test file from W2a.)

- [ ] **Step 2: Run the test to verify it fails**

Run:
```bash
cd /home/david/workspace/oevm
gcc -std=c99 -Isrc tests/test_float_gc.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -o /tmp/tfgc 2>&1 | head
```
Expected: FAIL to link — `undefined reference to float_arena_slab_budget`. (After adding just the accessor in Step 3, the test would then *run* but its bound assertion would fire, because nothing triggers a collect yet — the slab count reaches ~`(budget+8)*4*…` ≫ `budget+2`.)

- [ ] **Step 3: Add the budget macro and accessor in `src/gc.c`**

In the float-arena section, just after the `#define FLOAT_ARENA_SLAB_CELLS 4096` line (~line 207), add the budget macro:

```c
#ifndef OEVM_FLOAT_ARENA_SLAB_BUDGET
#define OEVM_FLOAT_ARENA_SLAB_BUDGET 16   /* soft cap on total slabs (~1 MB host); -D lower for embedded */
#endif
```

Just after the `float_arena_slab_count` accessor (~line 262), add:

```c
/* W2a.1 test-only: the compiled slab budget, so bound tests adapt to -D overrides. */
size_t float_arena_slab_budget(void) { return OEVM_FLOAT_ARENA_SLAB_BUDGET; }
```

- [ ] **Step 4: Insert the collect-at-cap block in `float_alloc`**

In `float_alloc` (~line 232), the new-slab branch currently begins:

```c
        if (float_slab_used >= FLOAT_ARENA_SLAB_CELLS) {
            struct FloatSlab *slab = malloc(sizeof(struct FloatSlab));
```

Insert the collect-at-cap block **between** the `if (...) {` and the `malloc`, so the branch becomes:

```c
        if (float_slab_used >= FLOAT_ARENA_SLAB_CELLS) {
            /* W2a.1: at the soft cap, reclaim before growing (mirror gc_allocate). */
            if (!gc_in_progress && float_slab_count >= OEVM_FLOAT_ARENA_SLAB_BUDGET) {
                gc_in_progress = 1;
                gc_collect();                 /* float_sweep refills float_freelist */
                gc_in_progress = 0;
                if (float_freelist != NULL) { /* reclaim worked — draw and return, no growth */
                    struct Float *r = float_freelist;
                    float_freelist = *(struct Float **)&r->value;
                    r->base.vtable = vtable;
                    return (oop)r;
                }
                /* else: live set exceeds the cap — fall through and grow */
            }
            struct FloatSlab *slab = malloc(sizeof(struct FloatSlab));
```

Leave the rest of the branch (the `malloc` NULL check, `memset` of markbits, `float_slab = slab`, `float_slab_used = 0`, `float_slab_count++`, span update) and the subsequent bump + markbit-clear exactly as they are.

- [ ] **Step 5: Run the test to verify it passes**

Run:
```bash
cd /home/david/workspace/oevm
gcc -std=c99 -Isrc tests/test_float_gc.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -o /tmp/tfgc && /tmp/tfgc
```
Expected: all `test_float_gc:` lines print, including
`test_float_gc: arena self-triggers + bounded OK (16/16 slabs)` (or `≤ 18`), then `ALL TESTS PASSED`.

- [ ] **Step 6: Verify the budget `-D` override tightens the bound**

Run (smaller budget → tighter bound; the test reads the budget via the accessor, so it adapts):
```bash
cd /home/david/workspace/oevm
gcc -std=c99 -Isrc -DOEVM_FLOAT_ARENA_SLAB_BUDGET=4 tests/test_float_gc.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -o /tmp/tfgc4 && /tmp/tfgc4 | grep "self-triggers"
```
Expected: `... arena self-triggers + bounded OK (4/4 slabs)` (or `≤ 6`).

- [ ] **Step 7: Verify W2a + W1 GC tests still pass (no regression)**

Run:
```bash
cd /home/david/workspace/oevm
gcc -std=c99 -Isrc -DOEVM_HEAP_SIZE=65536 tests/test_gc_trigger.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -o /tmp/tgct && /tmp/tgct | tail -1
gcc -std=c99 -Isrc tests/test_float_arena.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -o /tmp/tfa && /tmp/tfa
```
Expected: `ALL TESTS PASSED` (test_gc_trigger) and `test_float_arena: OK (...)`.

- [ ] **Step 8: Commit**

```bash
cd /home/david/workspace/oevm
git restore src/solidified_image.c 2>/dev/null
git add src/gc.c tests/test_float_gc.c
git commit -m "feat(gc): float_alloc collect-at-cap trigger bounds the arena (W2a.1, GH#5)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: RSS acceptance + full regression + docs

Proves the real goal end-to-end (mandelbrot RSS bounded), runs the conformance + portability gates, records the result.

**Files:**
- Modify: `nodus/oevm/docs/optimizations.md` (W2 row / GH note), `nodus/oevm/docs/archive/ROADMAP.md` (GP0 step)

**Interfaces:**
- Consumes: the trigger from Task 1.

- [ ] **Step 1: Measure mandelbrot RSS at a 256 KB main-heap cap (the acceptance gate)**

Run (Model A bench build with the embedded cap; `mandelbrot` shipped main is @750):
```bash
cd /home/david/workspace/oevm
python3 /home/david/workspace/ns-oevm/tools/ns_compile_oevm.py \
    ../ns-tests/05_somns_benchmarks/04_mandelbrot.ns src/solidified_image.c >/dev/null
gcc -O2 -DOEVM_APPLIANCE -DOEVM_HEAP_SIZE=262144 -std=c99 \
    src/vm.c src/gc.c src/dispatch.c src/solidified_image.c src/oevm_main.c -o /tmp/oevm_bench_cap -lm
/usr/bin/time -v /tmp/oevm_bench_cap >/dev/null 2> /tmp/mandel_rss.txt
grep "Maximum resident" /tmp/mandel_rss.txt
```
Expected: **Maximum resident set size** is single-digit MB (a few thousand KB) — down from the pre-W2a.1 2.35 GB (2,352,808 KB). If it is still hundreds of MB or more, STOP and report BLOCKED — the trigger is not firing in the real workload.

- [ ] **Step 2: Confirm the program still produces correct output under the cap**

Run:
```bash
cd /home/david/workspace/oevm
/tmp/oevm_bench_cap
```
Expected: mandelbrot prints its normal checksum/`true` verification line (it is self-verifying) — i.e. the bounded run is still correct, not OOM-aborted or wrong.

- [ ] **Step 3: Run the full float GC suite once more**

Run:
```bash
cd /home/david/workspace/oevm
git restore src/solidified_image.c 2>/dev/null
gcc -std=c99 -Isrc tests/test_float_gc.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -o /tmp/tfgc && /tmp/tfgc | tail -1
```
Expected: `ALL TESTS PASSED`.

- [ ] **Step 4: Run the `-m32` portability gate**

Run:
```bash
cd /home/david/workspace/oevm
bash tests/test_portability_32.sh
```
Expected: passes as before. (The change adds only a `size_t` comparison and a pointer-overlay draw already exercised by W2a, so it is pointer-width-neutral.) If 32-bit multilib is not installed, record that as an environment limitation and note `test_portability_32.sh` is the authoritative gate.

- [ ] **Step 5: Run full conformance on Model A + B**

Run:
```bash
cd /home/david/workspace/ns-oevm
OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | grep -E "Passed|Failed|Skipped"
```
Expected: `Passed: 47`, `Failed: 0`, `Skipped: 1`.

- [ ] **Step 6: Update `nodus/oevm/docs/optimizations.md`**

In the weakness table, update the **W1** and/or **W2** rows (whichever records GH #3) to note the float-arena trigger: the float arena now participates in the GC trigger (collect-at-cap in `float_alloc`), so float-dominated workloads are bounded too — record the mandelbrot@750 @256 KB figure (2.35 GB → the measured single-digit-MB value from Step 1) and reference GH #5. Keep the W2b/unboxing item as still-deferred (small measured speed ceiling).

- [ ] **Step 7: Update `nodus/oevm/docs/archive/ROADMAP.md`**

Mark **W2a.1 (float-arena GC trigger) complete** with the mandelbrot RSS result, and record that **W2b (float unboxing) remains deferred** (measured speed ceiling sub-second; revisit only if profiling justifies it). Set the next GP0 step to the pre-Zephyr **GH #4 (iterative-mark conversion)** unless re-prioritized.

- [ ] **Step 8: Commit**

```bash
cd /home/david/workspace/oevm
git restore src/solidified_image.c 2>/dev/null
git add nodus/oevm/docs/optimizations.md nodus/oevm/docs/archive/ROADMAP.md
git commit -m "docs(gp0): W2a.1 done — float arena bounded under cap (mandelbrot 2.35GB -> bounded, GH#5)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review

**Spec coverage:**
- §Design 1 (budget macro + `float_arena_slab_budget` accessor, no new counter) → Task 1 Step 3. ✓
- §Design 2 (collect-at-cap in `float_alloc` new-slab branch; gate on `float_slab_count >= BUDGET`; `gc_in_progress` guard; post-collect free-list draw with overlay-`value` idiom, no markbit clear; fall through to grow if reclaim frees nothing) → Task 1 Step 4. ✓
- §Design 3 (rooting correctness — relies on W1, no new roots) → no code; exercised implicitly by Task 2 conformance + the unit test's collect under precise roots. ✓
- §Testing (self-triggering bound; budget override; mandelbrot RSS acceptance; conformance; m32; existing GC tests) → Task 1 Steps 1/5/6/7, Task 2 Steps 1/3/4/5. ✓
- §Acceptance criteria 1–5 → trigger+reuse (Task 1 Step 4), bounded slab count (Task 1 Step 5), mandelbrot RSS (Task 2 Step 1), conformance/m32 (Task 2 Steps 4–5), fast paths untouched (the insert is inside the new-slab branch only — Task 1 Step 4). ✓

**Placeholder scan:** No TBD/TODO-as-work. Every code step shows complete code; every run step shows the exact command and expected output. ✓

**Type consistency:** `float_alloc(oop)->oop`, `float_arena_slab_budget(void)->size_t`, `float_arena_slab_count(void)->size_t`, gate `float_slab_count >= OEVM_FLOAT_ARENA_SLAB_BUDGET`, free-list draw `*(struct Float **)&r->value` — all consistent with the existing `src/gc.c` idioms and used identically across Tasks 1–2 and the test. The test reads the budget via `float_arena_slab_budget()` so it tracks any `-D` override (asserted bound `≤ budget + 2`). ✓
