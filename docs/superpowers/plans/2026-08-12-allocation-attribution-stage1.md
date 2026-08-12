# Allocation Attribution — Stage 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add per-method allocated-bytes columns to the existing flag-gated mprof table, hooked at both heaps, and produce the measurement that sizes stage 2's always-on table.

**Architecture:** The allocating method is *derived*, never stored: `current_frame->closure` is live at every allocation site, and `closure_entry()` on it yields the identical `CompiledMethod*` that `push_frame` already hands `mprof_bump`. `gc.c` learns nothing about methods — it calls one macro with a heap selector and a byte count; `vm.c` owns the key derivation and the table. Stage 1 rides entirely inside `#ifdef OEVM_METHOD_PROFILE`, so it costs production builds nothing and defers every RAM decision to stage 2.

**Tech Stack:** C99, the tuvm VM (`nodus/tuvm/src/`), Python 3 for the resolver, the nsl suite as the gate driver.

## Global Constraints

- Design of record: `docs/design/2026-08-12-allocation-attribution-design.md`. D1–D9.
- Copyright header `/* Copyright (c) 2026 Ekorau LLC */` on every new file.
- **Stage 1 adds nothing outside `#ifdef OEVM_METHOD_PROFILE`.** A build without the flag must be byte-identical in behaviour; the call sites in `gc.c` compile to `((void)0)`.
- **`struct MethodContext` gains no field.** Its layout is pinned (`oevm.h:139-157`); the 32/64-bit `gen_tag` union exists to hold offsets identical across word sizes.
- **Overflow is counted, never absorbed** (`vm.c:390`): a full table must not read as "this method never allocated".
- **Do not extend the existing `mprof:` output line.** `nodus/tuvm/tools/mprof_resolve.py:27` is `re.compile(r"mprof:\s+(@?\S+)\s+(\d+)\s*$", re.M)` — anchored, exactly two fields. Appending columns makes it match zero lines and print "no 'mprof:' lines found". New rows get a new prefix.
- Never run two nsl suites concurrently (#167); they share `/tmp` paths.
- Read the suite's **aux gate line**, not just `Passed: N / Failed: 0` (#63).

---

### Task 1: The byte columns and the `mprof_alloc` entry point

**Files:**
- Modify: `nodus/tuvm/src/vm.c:384-475` (the mprof block)
- Modify: `nodus/tuvm/src/oevm.h:395-400` (the `OEVM_METHOD_PROFILE` declarations)
- Test: `nodus/tuvm/tests/test_mprof_gate.c` (extend)

**Interfaces:**
- Produces: `void mprof_alloc(unsigned heap, size_t bytes);` — declared in `oevm.h` under `#ifdef OEVM_METHOD_PROFILE`. `heap` is `MPROF_HEAP_ARENA` (0) or `MPROF_HEAP_CHEAP` (1).
- Produces: `#define MPROF_ALLOC(h, n)` — the call form `gc.c` uses; expands to `((void)0)` when the flag is off.
- Produces: `extern unsigned long mprof_bytes_for_test(unsigned heap);` — test-only shim, total bytes attributed to real frames on that heap.
- Produces: `extern unsigned long mprof_noframe_bytes_for_test(unsigned heap);` — test-only shim.
- Consumes: `mprof_bump_for_test`, `mprof_used_for_test`, `mprof_start`, `mprof_stop`, `mprof_is_running` — all already exist.

- [ ] **Step 1: Extend the C test with the failing cases**

Append to `nodus/tuvm/tests/test_mprof_gate.c`, immediately before the final `printf`:

```c
    /* 5. Allocation attribution is OFF outside a window, like activations. */
    mprof_start();
    assert(mprof_bytes_for_test(MPROF_HEAP_ARENA) == 0);
    mprof_stop();
    mprof_alloc(MPROF_HEAP_ARENA, 64);
    assert(mprof_bytes_for_test(MPROF_HEAP_ARENA) == 0);

    /* 6. With no current frame there is nothing to attribute to, so the bytes
          land in the no-frame bucket rather than being dropped or crashing.
          This test runs with current_frame == NULL, which is exactly the boot
          and prim-context case the design's risk section names. */
    mprof_start();
    mprof_alloc(MPROF_HEAP_ARENA, 64);
    mprof_alloc(MPROF_HEAP_CHEAP, 32);
    assert(mprof_noframe_bytes_for_test(MPROF_HEAP_ARENA) == 64);
    assert(mprof_noframe_bytes_for_test(MPROF_HEAP_CHEAP) == 32);
    assert(mprof_bytes_for_test(MPROF_HEAP_ARENA) == 0);

    /* 7. The two heaps do not bleed into each other. */
    assert(mprof_noframe_bytes_for_test(MPROF_HEAP_CHEAP) == 32);

    /* 8. start() zeroes the byte columns too — a window that inherited the
          previous one's bytes would report a sum that never happened. */
    mprof_start();
    assert(mprof_noframe_bytes_for_test(MPROF_HEAP_ARENA) == 0);
    assert(mprof_noframe_bytes_for_test(MPROF_HEAP_CHEAP) == 0);
    mprof_stop();
```

- [ ] **Step 2: Run it and confirm it fails to build**

```bash
cd nodus/tuvm && gcc -Wall -std=c99 -DOEVM_METHOD_PROFILE -Isrc \
  src/*.c tests/test_mprof_gate.c -o /tmp/t1 -lm 2>&1 | head -5
```

Expected: FAIL — `implicit declaration of function 'mprof_alloc'` and `MPROF_HEAP_ARENA undeclared`.

- [ ] **Step 3: Declare the interface in `oevm.h`**

Replace the `#ifdef OEVM_METHOD_PROFILE` block at `oevm.h:396-400` with:

```c
#ifdef OEVM_METHOD_PROFILE
void mprof_start(void);
void mprof_stop(void);
int  mprof_is_running(void);
/* rete: allocation attribution (design 2026-08-12, D1/D3). The allocating method
   is DERIVED from current_frame inside vm.c — gc.c passes only a heap selector
   and a byte count, so the allocator never learns what a method is. */
void mprof_alloc(unsigned heap, size_t bytes);
#define MPROF_ALLOC(h, n) mprof_alloc((h), (n))
#else
#define MPROF_ALLOC(h, n) ((void)0)
#endif
#define MPROF_HEAP_ARENA 0u
#define MPROF_HEAP_CHEAP 1u
#define MPROF_HEAP_N     2u
```

Note the `MPROF_HEAP_*` constants sit **outside** the `#ifdef`: `gc.c` names them at every call site whether or not the flag is on.

- [ ] **Step 4: Factor the slot lookup, so both counters intern keys identically**

In `vm.c`, insert this immediately **before** `static OEVM_HOT void mprof_bump` (currently line 453):

```c
/* Find or create the row for `m`. Returns the slot, or -1 when the table is
   full — the caller COUNTS that rather than absorbing it (vm.c's rule above).
   Interning is shared so an activation and an allocation for the same method
   can never land in different rows. */
static OEVM_HOT int mprof_slot(const void *m) {
    uint32_t h = (uint32_t)(((uintptr_t)m >> 4) * 2654435761u) & (OEVM_MPROF_SLOTS - 1);
    uint32_t start = h;
    for (;;) {
        if (g_mprof_key[h] == m) return (int)h;
        if (g_mprof_key[h] == NULL) { g_mprof_key[h] = m; g_mprof_used++; return (int)h; }
        h = (h + 1) & (OEVM_MPROF_SLOTS - 1);
        if (h == start) return -1;
    }
}
```

Then replace the body of `mprof_bump` (everything after the `if (!g_mprof_enabled) return;` line, i.e. the hash comment and the `for (;;)` loop) with:

```c
    int s = mprof_slot(m);
    if (s < 0) { g_mprof_overflow++; return; }
    g_mprof_n[s]++;
}
```

**This is behaviour-preserving only because the arrays are zero-initialised.** The old code wrote `g_mprof_n[h] = 1` on creation; the new code creates the row with `n` still 0 and then increments. `g_mprof_n` is static (zero-init) and `mprof_start()` zeroes it, so both paths reach 1. Step 7 re-runs cases 1–4 to hold that.

- [ ] **Step 5: Add the columns and the entry point**

In `vm.c`, immediately after `static uint32_t g_mprof_used = 0;` (line 396):

```c
/* Allocation attribution (design 2026-08-12). Columns on the SAME rows as the
   activation counter, so bytes-per-activation needs no join. Stage 1 only:
   these live inside OEVM_METHOD_PROFILE and cost a production build nothing. */
static uint32_t g_mprof_bytes[MPROF_HEAP_N][OEVM_MPROF_SLOTS];
static uint32_t g_mprof_acnt[MPROF_HEAP_N][OEVM_MPROF_SLOTS];
static uint32_t g_mprof_alloc_overflow = 0;      /* allocations dropped: table full */
/* Allocations with no frame to attribute to — boot, and prim contexts. Reported
   separately rather than folded into a sentinel row: if this bucket dominates,
   the attribution is not yet answering the question, and that must be visible. */
static uint32_t g_mprof_nf_bytes[MPROF_HEAP_N];
static uint32_t g_mprof_nf_cnt[MPROF_HEAP_N];
```

Then, immediately after `mprof_bump`'s closing brace, add:

```c
/* Not OEVM_HOT and not static: gc.c calls it across a translation unit, so it
   cannot inline. Acceptable for a stage-1 spike — and the cost of that call is
   one of the numbers this stage exists to measure. */
void mprof_alloc(unsigned heap, size_t bytes) {
    if (!g_mprof_enabled) return;
    if (heap >= MPROF_HEAP_N) return;
    struct MethodContext *ctx = current_frame;
    if (ctx == NULL || ctx->closure == NULL) {
        g_mprof_nf_bytes[heap] += (uint32_t)bytes;
        g_mprof_nf_cnt[heap]++;
        return;
    }
    const void *m = (const void *)closure_entry((struct Closure *)ctx->closure);
    int s = mprof_slot(m);
    if (s < 0) { g_mprof_alloc_overflow++; return; }
    g_mprof_bytes[heap][s] += (uint32_t)bytes;
    g_mprof_acnt[heap][s]++;
}
```

- [ ] **Step 6: Zero the new columns in `mprof_start`, and add the test shims**

In `mprof_start()` (currently `vm.c:512`), extend the zeroing loop body and add a second loop:

```c
void mprof_start(void) {
    int i; unsigned h;
    for (i = 0; i < OEVM_MPROF_SLOTS; i++) { g_mprof_key[i] = NULL; g_mprof_n[i] = 0; }
    for (h = 0; h < MPROF_HEAP_N; h++) {
        for (i = 0; i < OEVM_MPROF_SLOTS; i++) { g_mprof_bytes[h][i] = 0; g_mprof_acnt[h][i] = 0; }
        g_mprof_nf_bytes[h] = 0;
        g_mprof_nf_cnt[h] = 0;
    }
    g_mprof_used = 0;
    g_mprof_overflow = 0;
    g_mprof_alloc_overflow = 0;
    g_mprof_enabled = 1;
}
```

Beside the existing `mprof_bump_for_test` shim (`vm.c:528`), add:

```c
unsigned long mprof_bytes_for_test(unsigned heap) {
    unsigned long t = 0; int i;
    if (heap >= MPROF_HEAP_N) return 0;
    for (i = 0; i < OEVM_MPROF_SLOTS; i++) t += g_mprof_bytes[heap][i];
    return t;
}
unsigned long mprof_noframe_bytes_for_test(unsigned heap) {
    return (heap < MPROF_HEAP_N) ? (unsigned long)g_mprof_nf_bytes[heap] : 0ul;
}
```

Declare both at the top of `tests/test_mprof_gate.c` beside the existing externs:

```c
extern unsigned long mprof_bytes_for_test(unsigned heap);
extern unsigned long mprof_noframe_bytes_for_test(unsigned heap);
```

- [ ] **Step 7: Run the test**

```bash
cd nodus/tuvm && gcc -Wall -std=c99 -DOEVM_METHOD_PROFILE -Isrc \
  src/*.c tests/test_mprof_gate.c -o /tmp/t1 -lm && /tmp/t1
```

Expected: `test_mprof_gate: PASSED`, and **no new warnings** — `-Wall` is on and an unused-variable or sign-compare warning here is a real defect.

- [ ] **Step 8: Prove the flag-off build is untouched**

```bash
cd nodus/tuvm && gcc -Wall -std=c99 -Isrc src/*.c tests/test_arena.c -o /tmp/t2 -lm && /tmp/t2
```

Expected: PASS with no warnings. This is the constraint that stage 1 costs production nothing; it is cheap to check and silent to break.

- [ ] **Step 9: Commit**

```bash
git add nodus/tuvm/src/vm.c nodus/tuvm/src/oevm.h nodus/tuvm/tests/test_mprof_gate.c
git commit -m "feat(alloc-attr): per-method byte columns and the mprof_alloc entry point

The allocating method is DERIVED from current_frame->closure, not stored --
MethodContext's layout is pinned and this design must not touch it. gc.c gets a
macro taking a heap selector and a byte count; key derivation stays in vm.c.

Stage 1 of docs/design/2026-08-12-allocation-attribution-design.md, entirely
inside OEVM_METHOD_PROFILE."
```

---

### Task 2: Hook both heaps

**Files:**
- Modify: `nodus/tuvm/src/gc.c:80` (`oevm_malloc`), `gc.c:100` (`oevm_realloc`), `gc.c:119` (`heap_stats_on_alloc`)
- Test: `nodus/tuvm/tests/test_mprof_alloc.c` (create)

**Interfaces:**
- Consumes: `MPROF_ALLOC(h, n)`, `MPROF_HEAP_ARENA`, `MPROF_HEAP_CHEAP`, `mprof_bytes_for_test`, `mprof_noframe_bytes_for_test` from Task 1.

Both heaps have exactly one accounting choke point each, which is why this task is three lines:

- `heap_stats_on_alloc(size_t need)` is already called from `gc_allocate`'s **two** success paths (`gc.c:309`, `gc.c:317`) with *"the exact bytes that just became live"*.
- `oevm_malloc` and `oevm_realloc` are the C-heap seam; both already bump `cheap.allocs`.

- [ ] **Step 1: Write the failing test**

Create `nodus/tuvm/tests/test_mprof_alloc.c`:

```c
/* Copyright (c) 2026 Ekorau LLC */
/* Allocation attribution reaches the table from BOTH heaps. This test runs with
   no VM frame, so every byte must land in the no-frame bucket -- which is the
   point: it proves the hook fires and that a frameless allocation is counted
   rather than dropped or dereferenced. Per-method attribution needs a running
   VM and is gated on target, not here. */
#include <assert.h>
#include <stdio.h>
#include "oevm.h"

extern unsigned long mprof_noframe_bytes_for_test(unsigned heap);

int main(void) {
    void *p;

    mprof_start();

    /* C heap: oevm_malloc's payload size is what gets attributed. */
    p = oevm_malloc(100);
    assert(p != NULL);
    assert(mprof_noframe_bytes_for_test(MPROF_HEAP_CHEAP) == 100);

    /* realloc counts the NEW size as a fresh attribution: cumulative bytes are
       "bytes this method caused to be allocated", not a live-set delta (D2). */
    p = oevm_realloc(p, 250);
    assert(p != NULL);
    assert(mprof_noframe_bytes_for_test(MPROF_HEAP_CHEAP) == 350);

    /* A free attributes nothing -- D2 is cumulative, not outstanding. */
    oevm_free(p);
    assert(mprof_noframe_bytes_for_test(MPROF_HEAP_CHEAP) == 350);

    /* Arena: any gc_allocate moves the arena column and NOT the C-heap one. */
    assert(mprof_noframe_bytes_for_test(MPROF_HEAP_ARENA) == 0);
    p = gc_allocate(NULL, 64);
    assert(p != NULL);
    assert(mprof_noframe_bytes_for_test(MPROF_HEAP_ARENA) > 0);
    assert(mprof_noframe_bytes_for_test(MPROF_HEAP_CHEAP) == 350);

    mprof_stop();
    printf("test_mprof_alloc: PASSED\n");
    return 0;
}
```

Note the arena assertion is `> 0`, not `== 64`: `heap_stats_on_alloc` receives `sizeof(struct HeapHeader) + rounded payload`, so the exact figure is an internal detail this test must not pin.

- [ ] **Step 2: Run it to verify it fails**

```bash
cd nodus/tuvm && gcc -Wall -std=c99 -DOEVM_METHOD_PROFILE -Isrc \
  src/*.c tests/test_mprof_alloc.c -o /tmp/t3 -lm && /tmp/t3
```

Expected: FAIL on the first C-heap assertion — the hook does not exist yet, so the bucket stays 0.

- [ ] **Step 3: Add the three call sites**

In `gc.c`, in `oevm_malloc`, immediately after `cheap.allocs++;`:

```c
    MPROF_ALLOC(MPROF_HEAP_CHEAP, n);
```

In `oevm_realloc`, immediately after its `cheap.allocs++;`:

```c
    MPROF_ALLOC(MPROF_HEAP_CHEAP, n);
```

In `heap_stats_on_alloc`, as the **first** statement of the function body:

```c
    MPROF_ALLOC(MPROF_HEAP_ARENA, need);
```

Confirm `gc.c` includes `oevm.h` (it does — it uses `struct OevmCHeapStats`). If the macro is not visible, the build fails loudly rather than silently no-op'ing, because `MPROF_ALLOC` is undefined rather than empty.

- [ ] **Step 4: Run the test**

```bash
cd nodus/tuvm && gcc -Wall -std=c99 -DOEVM_METHOD_PROFILE -Isrc \
  src/*.c tests/test_mprof_alloc.c -o /tmp/t3 -lm && /tmp/t3
```

Expected: `test_mprof_alloc: PASSED`.

- [ ] **Step 5: Register it in the suite**

In `nodus/nsl/tools/run_tests_oevm.py`, after the `test_mprof_gate` block ends at line 168 (`print("C unit test test_mprof_gate: PASSED")`) and **before** `return True`, insert a block copying that one exactly, substituting `test_mprof_alloc` for `test_mprof_gate` in all five places (the source path, the `-o` path, the two failure messages, and the run path).

- [ ] **Step 6: Run the full suite**

```bash
cd nodus/nsl && python3 tools/run_tests_oevm.py
```

Expected: `Passed: 258 / Failed: 0`, the aux line green, and **`C unit test test_mprof_alloc: PASSED` visible in it**. `footprint: SKIP (no arm-zephyr-eabi toolchain)` is pre-existing. Read the aux line — it can be red while the count reads zero failures (#63).

- [ ] **Step 7: Commit**

```bash
git add nodus/tuvm/src/gc.c nodus/tuvm/tests/test_mprof_alloc.c nodus/nsl/tools/run_tests_oevm.py
git commit -m "feat(alloc-attr): hook both heaps at their single accounting choke points

Each heap already had exactly one: heap_stats_on_alloc for the arena (called
from gc_allocate's two success paths) and oevm_malloc/oevm_realloc for the C
heap. Three call sites, no new plumbing.

realloc attributes its NEW size as a fresh allocation and free attributes
nothing -- D2 is cumulative, not outstanding, and the test pins that."
```

---

### Task 3: Report the rows, and teach the resolver to read them

**Files:**
- Modify: `nodus/tuvm/src/vm.c:489-505` (`mprof_report`)
- Modify: `nodus/tuvm/tools/mprof_resolve.py:27` and its output path
- Test: `nodus/tuvm/tests/test_mprof_alloc.c` (extend — Task 2 created it)

**Interfaces:**
- Consumes: everything from Tasks 1 and 2.
- Produces: a new stderr line type per row, `mprofa: <key> <arenaBytes> <arenaCount> <cheapBytes> <cheapCount>`, and one summary line `mprofa[<why>]: noframe_arena=<b>/<n> noframe_cheap=<b>/<n> allocOverflow=<n>`.

**The existing `mprof:` line must not change.** `mprof_resolve.py:27` anchors on exactly two fields; appending columns makes it match nothing and report "no 'mprof:' lines found" — a silent false negative in the one tool that reads this output.

- [ ] **Step 1: Extend the C test to assert the output shape**

Append to `nodus/tuvm/tests/test_mprof_alloc.c`, before its final `printf`, replacing the `mprof_stop();` line:

```c
    /* The report must emit an mprofa: row for the no-frame bucket and must NOT
       disturb the mprof: line shape mprof_resolve.py anchors on. Captured by
       the caller in Step 3 rather than asserted here -- stderr parsing belongs
       in the shell gate, not in a C unit test. */
    mprof_stop();
```

- [ ] **Step 2: Emit the new lines**

In `mprof_report`, after the existing per-slot loop closes, add:

```c
    fprintf(stderr, "mprofa[%s]: noframe_arena=%u/%u noframe_cheap=%u/%u allocOverflow=%u\n",
            why, g_mprof_nf_bytes[MPROF_HEAP_ARENA], g_mprof_nf_cnt[MPROF_HEAP_ARENA],
            g_mprof_nf_bytes[MPROF_HEAP_CHEAP], g_mprof_nf_cnt[MPROF_HEAP_CHEAP],
            g_mprof_alloc_overflow);
    for (i = 0; i < OEVM_MPROF_SLOTS; i++) {
        if (!g_mprof_key[i]) continue;
        if (g_mprof_bytes[MPROF_HEAP_ARENA][i] == 0 &&
            g_mprof_bytes[MPROF_HEAP_CHEAP][i] == 0) continue;   /* rows that never allocated */
        if (is_in_blob(g_mprof_key[i])) {
            fprintf(stderr, "mprofa: %lu %u %u %u %u\n",
                    (unsigned long)((const uint8_t *)g_mprof_key[i] - oimg_blob_base),
                    g_mprof_bytes[MPROF_HEAP_ARENA][i], g_mprof_acnt[MPROF_HEAP_ARENA][i],
                    g_mprof_bytes[MPROF_HEAP_CHEAP][i], g_mprof_acnt[MPROF_HEAP_CHEAP][i]);
        } else {
            fprintf(stderr, "mprofa: @%p %u %u %u %u\n", g_mprof_key[i],
                    g_mprof_bytes[MPROF_HEAP_ARENA][i], g_mprof_acnt[MPROF_HEAP_ARENA][i],
                    g_mprof_bytes[MPROF_HEAP_CHEAP][i], g_mprof_acnt[MPROF_HEAP_CHEAP][i]);
        }
    }
```

**Skipping never-allocated rows is deliberate and is the measurement.** The count of `mprofa:` lines *is* the distinct-allocating-method population that sizes stage 2 (D8). Emitting zero rows would bury it inside the 224-row activation dump.

- [ ] **Step 3: Verify both line shapes at once**

```bash
cd nodus/tuvm && gcc -Wall -std=c99 -DOEVM_METHOD_PROFILE -Isrc \
  src/*.c tests/test_mprof_alloc.c -o /tmp/t3 -lm && /tmp/t3 2>/tmp/t3.err; \
  grep -c '^mprofa' /tmp/t3.err; grep '^mprofa\[' /tmp/t3.err
```

Expected: at least the `mprofa[window]:` summary line, showing `noframe_cheap=350/2` and a non-zero `noframe_arena`. There will be **no** `mprofa:` per-method rows in this test, because it has no frames — that is correct, and it is why the summary line carries the no-frame bucket.

- [ ] **Step 4: Teach the resolver the new rows**

In `nodus/tuvm/tools/mprof_resolve.py`, beside `DUMP_RE` at line 27:

```python
# rete (design 2026-08-12): allocation attribution rows. A SEPARATE prefix, not
# extra fields on `mprof:` -- DUMP_RE is anchored at two fields, so widening that
# line would make it match nothing and report "no mprof: lines found".
ALLOC_RE = re.compile(r"mprofa:\s+(@?\S+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*$", re.M)
```

Add a `resolve_alloc(text, sidecar)` alongside the existing `resolve()`, joining on the same key column and returning `(name, arenaBytes, arenaCount, cheapBytes, cheapCount)` sorted by `arenaBytes + cheapBytes` descending. In `main`, print that table under a `--- allocation attribution ---` heading after the existing output, plus a final line:

```python
print(f"distinct allocating methods: {len(rows)}   (stage-2 sizing input)")
```

- [ ] **Step 5: Run the suite**

```bash
cd nodus/nsl && python3 tools/run_tests_oevm.py
```

Expected: `Passed: 258 / Failed: 0`, aux line green, both C unit tests named in it.

- [ ] **Step 6: Commit**

```bash
git add nodus/tuvm/src/vm.c nodus/tuvm/tools/mprof_resolve.py nodus/tuvm/tests/test_mprof_alloc.c
git commit -m "feat(alloc-attr): mprofa: rows, and the resolver that reads them

A SEPARATE line prefix, not extra fields on mprof:. mprof_resolve.py's DUMP_RE
anchors on exactly two fields, so widening that line would make it match zero
lines and print 'no mprof: lines found' -- a silent false negative in the only
tool that reads this output.

Rows that never allocated are skipped, because the COUNT of mprofa: lines is
the distinct-allocating-method population that sizes stage 2."
```

---

### Task 4: The host measurement

**Files:**
- Create: `docs/results/2026-08-12-allocation-attribution-host.md`

**Interfaces:**
- Consumes: the `mprofa:` output and `mprof_resolve.py --alloc` from Task 3.

- [ ] **Step 1: Drive a real window under the host bridge**

`tests/porta_sync_host.sh` already builds with `-DOEVM_METHOD_PROFILE` (line 58) and leg 8 drives a real `dbg/req` profile window. Run it and keep stderr:

```bash
cd nodus/tuvm && bash tests/porta_sync_host.sh 2>/tmp/alloc-host.err; echo "RC=$?"
grep -c '^mprofa: ' /tmp/alloc-host.err
grep '^mprofa\[' /tmp/alloc-host.err
```

Expected: `PORTA SYNC OK`. **Check `RC=` explicitly** — a pipe would have reported the wrong command's status (`hardware-gate-false-greens`).

- [ ] **Step 2: Resolve the rows to names**

```bash
cd nodus/tuvm && python3 tools/mprof_resolve.py --alloc /tmp/alloc-host.err
```

Expected: a ranked table, and the `distinct allocating methods: N` line.

- [ ] **Step 3: Write the result**

Create `docs/results/2026-08-12-allocation-attribution-host.md` in the pattern of `docs/results/2026-08-09-cbor-encode-shape.md`. It must record, verbatim:

- the exact command and the build id it ran against
- **`distinct allocating methods: N`** — the number that sizes stage 2 (D8)
- the top rows by total bytes, both heaps shown separately
- **the no-frame bucket as a fraction of total attributed bytes** — the design's §8 risk. If it dominates, say so plainly: the attribution is not yet answering the question.
- `allocOverflow` — if non-zero, the host window already exceeded 256 slots and stage 2's sizing question is answered in the wrong direction

- [ ] **Step 4: Commit**

```bash
git add docs/results/2026-08-12-allocation-attribution-host.md
git commit -m "docs(alloc-attr): the host measurement — N distinct allocating methods"
```

---

### Task 5: The target measurement, and the close-out

**Files:**
- Create: `docs/results/2026-08-12-allocation-attribution-c6.md`
- Modify: `review/tests/CLAIMS.md`, `docs/next-steps-overview.md`

**Interfaces:**
- Consumes: Task 4's host figure, as the number the target run is compared against.

**Board discipline:** `c6spare` is the attached disposable board and `TARGET_DEFAULT_BOARD`. **Never `c6`** — that is the ship's bell, flashing it is a physical act in a house, and a failed flash rings it until someone unplugs the cable (#201). Resolve boards through the collector env files, never a table.

- [ ] **Step 1: Build and flash `c6spare` with the flag on**

```bash
export OEVM_METHOD_PROFILE=1
```

Then build and flash `MethodProfileNodeC6.nsl` per `nodus/tuvm/zephyr/`'s normal recipe. Run the build in this session, not a subagent — subagents background long builds and wait forever.

- [ ] **Step 2: Drive a window under steady-state bridging**

Let the node reach steady-state bridging on the real mesh, then drive a `dbg/req profile <ms>` window as `nodus/tests/23_mprof/02_dbg_req_profile.nsl` does. Read the rows from the **collector's log**, not by opening the port — on the C6 the collector holds the CP2102N permanently and two readers split the stream.

- [ ] **Step 3: Resolve against the matching sidecar**

```bash
cd nodus/tuvm && python3 tools/mprof_resolve.py --alloc <collector-log>
```

The `.oimg.methods.json` sidecar must be the one for the running build — `platform buildId` names it, which is what #74 closed. A mismatched sidecar resolves offsets to the wrong names and reads as a plausible answer.

- [ ] **Step 4: Write the result and compare the two**

Create `docs/results/2026-08-12-allocation-attribution-c6.md` with the same fields as Task 4, plus:

- **`distinct allocating methods` on target vs host.** Host runs the union of every path the suite takes; target runs one app in steady state. If target is much smaller, stage 2's table can be small. If it is *larger*, the host number was not the bound it looked like.
- **`gc_allocate`'s call frequency** — arena `mprofa:` counts summed over the window, against wall-clock. This is the number D4 admits it does not have.
- Whether the measured cost matches D4's ~0.02% projection, or refutes it.

- [ ] **Step 5: Promote to mainline, and file what the measurement earned**

A rung is not closed until its knowledge leaves `docs/superpowers/`:

- Add a claim row to `review/tests/CLAIMS.md` for each of `test_mprof_gate`'s new cases and `test_mprof_alloc`, naming the gate that holds it.
- Update `docs/next-steps-overview.md` Part 2 thread 5 (the mprof paragraph) with stage 1's outcome and the stage-2 sizing input.
- **File stage 2 as its own issue**, carrying the measured allocating-method population as its sizing input and citing the design's D8. If the numbers refuted D4 or D5, say so in the issue body — that is the falsifiable deferral the design promised.

- [ ] **Step 6: Commit**

```bash
git add docs/results/2026-08-12-allocation-attribution-c6.md review/tests/CLAIMS.md docs/next-steps-overview.md
git commit -m "docs(alloc-attr): the target measurement, and stage 2's sizing input"
```

---

## Self-Review

**Spec coverage.** D1 → Task 1 Step 5 (key derived from `current_frame->closure`, no `MethodContext` change). D2 → Task 2 Step 1 (realloc attributes new size, free attributes nothing). D3 → Task 2 Step 3 (both heaps). D4 → Task 5 Step 4 (the projection is measured, not assumed). D5/D6 → out of stage 1 by construction; D6's overflow counter is Task 1 Step 5 and reported in Task 3 Step 2. D7 → Tasks 4 and 5 (the two numbers). D8 → Task 5 Step 5 (stage 2 filed with its sizing input). D9 → Task 3 (existing `dbg/req` route, blob-relative offsets, no prim id). §8's risks → the no-frame bucket is tested (Task 1 Step 1 case 6), reported (Task 3 Step 2), and its dominance is a required field of both results docs.

**Placeholder scan.** No TBD/TODO. Every code step carries the actual code. Task 5's build recipe points at the board's existing procedure rather than restating it, which is a reference to a real document, not a placeholder.

**Type consistency.** `mprof_alloc(unsigned heap, size_t bytes)` — same signature in `oevm.h` (T1S3), the definition (T1S5), and every call (T2S3). `MPROF_HEAP_ARENA`/`MPROF_HEAP_CHEAP`/`MPROF_HEAP_N` defined once (T1S3), used in T1S5, T1S6, T2S1, T2S3, T3S2. `mprof_bytes_for_test`/`mprof_noframe_bytes_for_test` return `unsigned long` in the definition (T1S6), the `oevm.h`-adjacent externs (T1S6), and both tests. `mprof_slot` returns `int` with `-1` for full, checked as `< 0` at both call sites.

**One known gap, deliberate.** No test asserts *per-method* attribution — both C tests run without a VM frame, so they exercise the no-frame path only. Per-method attribution is proven by Task 5's target run producing named rows, not by a unit test, because standing up a real frame in a C unit test would test the harness rather than the hook. If Task 5's run yields zero `mprofa:` rows, that is the failure this gap would have caught, and it fails loudly.
