# W2a — Float-Arena Reclaim Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the malloc-backed float bump arena reclaimable under runtime GC (now triggered by W1), bounding RSS for float-heavy code without losing the Opt #5 bump-allocation speed win.

**Architecture:** Keep the separate float slab pool. Add a per-slab side mark bitmap and a cell free-list. `mark()` recognizes float-arena pointers and sets the cell's bit (floats are immutable leaves); a `float_sweep()` run from `gc_collect` rebuilds the free-list from every unmarked cell; `float_alloc` draws free-list → bump → new slab. Non-moving, so it stays compatible with W1's conservative C-stack roots.

**Tech Stack:** C99, single-file change in `src/gc.c`; unit tests built with `gcc` directly; conformance via `ns-oevm/tools/run_tests_oevm.py`.

## Global Constraints

- All implementation changes are confined to `src/gc.c` (float-arena section + `mark()` + `gc_collect`) plus a new test file. **No change to `struct Float`** (`src/oevm.h`) or anything the compiler emits.
- `FLOAT_ARENA_SLAB_CELLS = 4096` (unchanged).
- Reclaim scope is **cell free-list only** — slabs are never freed back to malloc.
- Non-moving: never relocate a Float cell (conservative roots cannot be rewritten).
- Pointer-width-correct: bitmap is byte-indexed; all cell math uses `sizeof(struct Float)` (must pass `-m32`).
- Conformance must stay **47 pass / 0 fail / 1 skip** on Model A + Model B + `-m32`.
- `src/solidified_image.c` is a generated artifact — the test harness overwrites it; `git restore src/solidified_image.c` before any commit.
- Commit message trailer:
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

---

## File Structure

- `src/gc.c` — float-arena section (~lines 191–230): add bitmap, free-list, span tracking, `is_in_float_arena`, `float_mark`, `float_sweep`, test-introspection hooks; `mark()` (~line 246): add float branch; `gc_collect` (~line 515): call `float_sweep()`.
- `tests/test_float_gc.c` — **new** unit test for reclaim/reuse, conservative survival, mixed liveness, and bounded-churn acceptance. Modeled on `tests/test_gc_trigger.c` (setup) and `tests/test_float_arena.c` (fake vtable).

### Interfaces produced (used across tasks and by tests)

```c
/* Allocation (existing signature, behavior extended) */
oop  float_alloc(oop vtable);

/* GC integration (internal to gc.c) */
static int   is_in_float_arena(oop ptr);   /* 1 if ptr is a cell-aligned float-arena address */
static void  float_mark(oop ptr);          /* set the cell's mark bit (leaf; no tracing) */
static void  float_sweep(void);            /* rebuild free-list from unmarked cells; clear bits */

/* Test-only introspection (unconditionally compiled, parallel to gc_is_live) */
size_t float_arena_slab_count(void);       /* number of slabs ever allocated */
size_t float_arena_free_count(void);       /* cells currently on the free-list */
int    float_is_live(oop ptr);             /* 1 if cell is in use (not on free-list), else 0 */
```

---

## Task 1: Bitmap, free-list, span tracking, and introspection hooks

Adds the data structures and the free-list-aware allocator, plus the test hooks. No reclaim yet (free-list stays empty until Task 2), so allocation behavior is unchanged — this task is verified structurally.

**Files:**
- Modify: `src/gc.c` (float-arena section, ~lines 207–230)
- Test: `tests/test_float_gc.c` (new)

**Interfaces:**
- Consumes: existing `struct Float` (`{ struct Object base; double value; }`), `struct FloatSlab`, `float_alloc`.
- Produces: `float_arena_slab_count`, `float_arena_free_count`, `float_is_live`, and the extended `float_alloc` (free-list draw first).

- [ ] **Step 1: Write the failing test**

Create `tests/test_float_gc.c`:

```c
/* Unit test for W2a float-arena reclaim (src/gc.c).
   Build:
     gcc -std=c99 -Isrc tests/test_float_gc.c \
         src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
         -lm -o /tmp/tfgc && /tmp/tfgc */
#include "../src/oevm.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

extern oop    float_alloc(oop vtable);
extern void   gc_collect(void);
extern void   heap_init(void);
extern void   gc_set_stack_base(void *p);
extern void  *vm_c_stack_base;
extern void   mark_object(oop);
extern void (*gc_additional_roots_hook)(void);

extern size_t float_arena_slab_count(void);
extern size_t float_arena_free_count(void);
extern int    float_is_live(oop ptr);

static int fake_vtable;
#define VT ((oop)&fake_vtable)

/* Task 1: introspection hooks report a coherent picture of a fresh arena. */
static void test_hooks_basic(void) {
    size_t slabs0 = float_arena_slab_count();
    oop c = float_alloc(VT);
    assert(c != NULL);
    assert(((struct Float *)c)->base.vtable == VT);
    assert(float_arena_slab_count() >= 1);
    assert(float_arena_slab_count() > slabs0 || slabs0 >= 1);
    assert(float_arena_free_count() == 0);   /* nothing reclaimed yet */
    assert(float_is_live(c) == 1);           /* freshly handed out => in use */
    printf("test_float_gc: hooks basic OK\n");
}

int main(void) {
    int base_anchor;
    gc_set_stack_base(&base_anchor);
    heap_init();

    test_hooks_basic();

    printf("ALL TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cd /home/david/workspace/oevm
gcc -std=c99 -Isrc tests/test_float_gc.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -o /tmp/tfgc 2>&1 | head
```
Expected: FAIL to link — `undefined reference to float_arena_slab_count` / `float_arena_free_count` / `float_is_live`.

- [ ] **Step 3: Add the bitmap, free-list, span tracking, and hooks in `src/gc.c`**

Replace the float-arena section (currently lines ~207–230, from `#define FLOAT_ARENA_SLAB_CELLS 4096` through the end of `float_alloc`) with:

```c
#define FLOAT_ARENA_SLAB_CELLS 4096
struct FloatSlab {
    struct FloatSlab *next;
    struct Float cells[FLOAT_ARENA_SLAB_CELLS];
    uint8_t markbits[FLOAT_ARENA_SLAB_CELLS / 8];   /* W2a: 1 bit/cell, 512 B */
};
static struct FloatSlab *float_slab = NULL;
/* Initialised at the slab capacity so the first allocation forces a slab. */
static size_t float_slab_used = FLOAT_ARENA_SLAB_CELLS;

/* W2a reclaim state. A dead cell stores the free-list next pointer overlaid on
   its unused `value` field. The [lo,hi) span across all slabs gives mark() an
   O(1) fast reject for the common non-float pointer. */
static struct Float *float_freelist = NULL;
static char *float_arena_lo = NULL;     /* min cell address across slabs */
static char *float_arena_hi = NULL;     /* one-past max cell address */

static size_t float_slab_count = 0;     /* slabs ever allocated (test hook) */

oop float_alloc(oop vtable) {
    struct Float *f;
    if (float_freelist != NULL) {
        f = float_freelist;
        /* unthread: next pointer was stored overlaid on value */
        float_freelist = *(struct Float **)&f->value;
    } else {
        if (float_slab_used >= FLOAT_ARENA_SLAB_CELLS) {
            struct FloatSlab *slab = malloc(sizeof(struct FloatSlab));
            if (!slab) {
                fprintf(stderr, "VM Float Arena Error: Out of Memory\n");
                abort();
            }
            memset(slab->markbits, 0, sizeof slab->markbits);
            slab->next = float_slab;
            float_slab = slab;
            float_slab_used = 0;
            float_slab_count++;
            /* extend the address span to cover this slab's cells */
            {
                char *clo = (char *)&slab->cells[0];
                char *chi = (char *)&slab->cells[FLOAT_ARENA_SLAB_CELLS];
                if (float_arena_lo == NULL || clo < float_arena_lo) float_arena_lo = clo;
                if (float_arena_hi == NULL || chi > float_arena_hi) float_arena_hi = chi;
            }
        }
        f = &float_slab->cells[float_slab_used++];
    }
    f->base.vtable = vtable;
    return (oop)f;
}

/* --- W2a test-only introspection (parallel to gc_is_live) --- */
size_t float_arena_slab_count(void) { return float_slab_count; }

size_t float_arena_free_count(void) {
    size_t n = 0;
    for (struct Float *p = float_freelist; p != NULL;
         p = *(struct Float **)&p->value) n++;
    return n;
}

/* 1 if the cell is currently in use (not reclaimed). A cell is "dead" iff it is
   on the free-list. Pristine bump-tail cells are never passed here by tests. */
int float_is_live(oop ptr) {
    for (struct Float *p = float_freelist; p != NULL;
         p = *(struct Float **)&p->value) {
        if ((oop)p == ptr) return 0;
    }
    return 1;
}
```

Ensure `<string.h>` is included for `memset` (check the top of `src/gc.c`; add `#include <string.h>` if absent).

- [ ] **Step 4: Run test to verify it passes**

Run:
```bash
cd /home/david/workspace/oevm
gcc -std=c99 -Isrc tests/test_float_gc.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -o /tmp/tfgc && /tmp/tfgc
```
Expected: prints `test_float_gc: hooks basic OK` then `ALL TESTS PASSED`.

- [ ] **Step 5: Verify the existing float-arena test still passes (no behavior regression)**

Run:
```bash
cd /home/david/workspace/oevm
gcc -std=c99 -Isrc tests/test_float_arena.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -o /tmp/tfa && /tmp/tfa
```
Expected: `test_float_arena: OK (10000 cells across slab boundaries)`.

- [ ] **Step 6: Commit**

```bash
cd /home/david/workspace/oevm
git restore src/solidified_image.c 2>/dev/null
git add src/gc.c tests/test_float_gc.c
git commit -m "feat(gc): float-arena bitmap + free-list + introspection hooks (W2a step 1)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Mark recognition + sweep reclaim

Wires reclamation: `mark()` pins reachable Floats, `float_sweep()` (called from `gc_collect`) rebuilds the free-list from unmarked cells, and `float_alloc` reuses them. This is the task that actually reclaims memory.

**Files:**
- Modify: `src/gc.c` — float-arena section (add `is_in_float_arena`, `float_mark`, `float_sweep`), `mark()` (~line 246, add float branch), `gc_collect` (~line 515, call `float_sweep()`).
- Test: `tests/test_float_gc.c` (extend)

**Interfaces:**
- Consumes: Task 1's `float_freelist`, `float_slab`, `float_slab_used`, `float_arena_lo/hi`, per-slab `markbits`, and the introspection hooks.
- Produces: `is_in_float_arena`, `float_mark`, `float_sweep` (all `static`); float-reclaiming `gc_collect`.

- [ ] **Step 1: Write the failing tests**

Append these to `tests/test_float_gc.c` (add the function bodies above `main`, and the calls inside `main` after `test_hooks_basic();`):

```c
/* (2a) A Float referenced only by a C local survives a collection
   (conservative C-stack pinning reaches the arena). */
__attribute__((noinline))
static int float_local_survives(void) {
    oop f = float_alloc(VT);
    ((struct Float *)f)->value = 3.5;
    gc_collect();                 /* f rooted ONLY by this C local */
    return float_is_live(f) && ((struct Float *)f)->value == 3.5;
}

/* (2b) Dead Floats are reclaimed and reused; the slab count does not grow on a
   second wave that fits within the reclaimed cells. Conservative scan disabled
   so dropped cells are not pinned by stale stack slots. */
__attribute__((noinline))
static void alloc_wave(size_t n) {       /* locals die when this returns */
    for (size_t i = 0; i < n; i++) { oop f = float_alloc(VT); ((struct Float *)f)->value = (double)i; }
}
static void test_reclaim_reuse(void) {
    void *saved = vm_c_stack_base;
    vm_c_stack_base = NULL;              /* precise roots only: nothing roots the wave */
    size_t want = 3 * FLOAT_ARENA_SLAB_CELLS_T;   /* force ~3 slabs */
    alloc_wave(want);
    size_t slabs_after_wave = float_arena_slab_count();
    gc_collect();                        /* all of the wave is unreachable */
    assert(float_arena_free_count() > 0);
    size_t slabs_before_reuse = float_arena_slab_count();
    alloc_wave(want / 2);                /* fits in reclaimed cells */
    assert(float_arena_slab_count() == slabs_before_reuse);  /* reused, no growth */
    (void)slabs_after_wave;
    vm_c_stack_base = saved;
    printf("test_float_gc: reclaim + reuse OK\n");
}

/* (2c) Mixed liveness: marked Floats survive, unmarked are reclaimed. */
#define KEEP 4
static oop kept[KEEP];
static void keep_roots(void) { for (int i = 0; i < KEEP; i++) mark_object(kept[i]); }
static void test_mixed_liveness(void) {
    void *saved = vm_c_stack_base;
    vm_c_stack_base = NULL;
    gc_additional_roots_hook = keep_roots;
    for (int i = 0; i < KEEP; i++) { kept[i] = float_alloc(VT); ((struct Float *)kept[i])->value = 100.0 + i; }
    oop dead = float_alloc(VT);          /* unreferenced */
    ((struct Float *)dead)->value = -1.0;
    gc_collect();
    for (int i = 0; i < KEEP; i++) {
        assert(float_is_live(kept[i]) == 1);
        assert(((struct Float *)kept[i])->value == 100.0 + i);
    }
    assert(float_is_live(dead) == 0);    /* reclaimed */
    gc_additional_roots_hook = NULL;
    vm_c_stack_base = saved;
    printf("test_float_gc: mixed liveness OK\n");
}
```

Add near the top of the file (after the `#define VT` line) a sized constant mirroring the arena's slab size so the test does not depend on a macro private to `gc.c`:

```c
#define FLOAT_ARENA_SLAB_CELLS_T ((size_t)4096)
```

And in `main`, after `test_hooks_basic();`, add:

```c
    assert(float_local_survives() == 1);
    printf("test_float_gc: conservative float survival OK\n");
    test_reclaim_reuse();
    test_mixed_liveness();
```

- [ ] **Step 2: Run tests to verify they fail**

Run:
```bash
cd /home/david/workspace/oevm
gcc -std=c99 -Isrc tests/test_float_gc.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -o /tmp/tfgc && /tmp/tfgc
```
Expected: FAIL — `float_local_survives` may pass spuriously, but `test_reclaim_reuse` aborts on `assert(float_arena_free_count() > 0)` because nothing reclaims cells yet (sweep not implemented).

- [ ] **Step 3: Add `is_in_float_arena`, `float_mark`, `float_sweep` to the float-arena section**

Insert immediately after the `float_is_live` definition from Task 1 (still above `mark()`):

```c
/* W2a: 1 if ptr is a cell-aligned address inside some slab's cells[]. The
   [lo,hi) span fast-rejects the overwhelmingly common non-float pointer. */
static int is_in_float_arena(oop ptr) {
    char *p = (char *)ptr;
    if (p < float_arena_lo || p >= float_arena_hi) return 0;
    for (struct FloatSlab *s = float_slab; s != NULL; s = s->next) {
        char *base = (char *)&s->cells[0];
        char *end  = (char *)&s->cells[FLOAT_ARENA_SLAB_CELLS];
        if (p >= base && p < end) {
            size_t off = (size_t)(p - base);
            return (off % sizeof(struct Float)) == 0;   /* must be a cell start */
        }
    }
    return 0;
}

/* W2a: set the mark bit for the cell at ptr. Caller has verified
   is_in_float_arena(ptr). Floats are leaves, so nothing else to trace. */
static void float_mark(oop ptr) {
    char *p = (char *)ptr;
    for (struct FloatSlab *s = float_slab; s != NULL; s = s->next) {
        char *base = (char *)&s->cells[0];
        char *end  = (char *)&s->cells[FLOAT_ARENA_SLAB_CELLS];
        if (p >= base && p < end) {
            size_t idx = (size_t)(p - base) / sizeof(struct Float);
            s->markbits[idx >> 3] |= (uint8_t)(1u << (idx & 7));
            return;
        }
    }
}

/* W2a: rebuild the free-list from every unmarked allocated cell, then clear all
   bits for the next cycle. Mirrors the main heap's rebuild-free_bins-from-scratch
   sweep. Head slab is swept only over [0, float_slab_used); older slabs in full. */
static void float_sweep(void) {
    float_freelist = NULL;
    for (struct FloatSlab *s = float_slab; s != NULL; s = s->next) {
        size_t limit = (s == float_slab) ? float_slab_used : FLOAT_ARENA_SLAB_CELLS;
        for (size_t i = 0; i < limit; i++) {
            uint8_t bit = s->markbits[i >> 3] & (uint8_t)(1u << (i & 7));
            if (bit) {
                s->markbits[i >> 3] &= (uint8_t)~(1u << (i & 7));  /* live: clear for next cycle */
            } else {
                struct Float *cell = &s->cells[i];                 /* dead: thread onto free-list */
                *(struct Float **)&cell->value = float_freelist;
                float_freelist = cell;
            }
        }
    }
}
```

- [ ] **Step 4: Add the float branch to `mark()`**

In `mark()` (~line 246), immediately **after** the `if (is_read_only(ptr)) return;` line and **before** `if (!is_valid_heap_object(ptr)) { return; }`, insert:

```c
    if (is_in_float_arena(ptr)) {
        float_mark(ptr);        /* leaf object: set bit, no tracing */
        return;
    }
```

- [ ] **Step 5: Call `float_sweep()` from `gc_collect`**

At the very end of `gc_collect` (after the main-heap sweep loop closes, ~line 515, just before the function's closing brace), add:

```c
    /* W2a: reclaim dead Float cells (separate malloc-backed arena). */
    float_sweep();
```

- [ ] **Step 6: Run tests to verify they pass**

Run:
```bash
cd /home/david/workspace/oevm
gcc -std=c99 -Isrc tests/test_float_gc.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -o /tmp/tfgc && /tmp/tfgc
```
Expected:
```
test_float_gc: hooks basic OK
test_float_gc: conservative float survival OK
test_float_gc: reclaim + reuse OK
test_float_gc: mixed liveness OK
ALL TESTS PASSED
```

- [ ] **Step 7: Verify W1 + arena unit tests still pass (no GC regression)**

Run:
```bash
cd /home/david/workspace/oevm
gcc -std=c99 -Isrc -DOEVM_HEAP_SIZE=65536 tests/test_gc_trigger.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -o /tmp/tgct && /tmp/tgct
gcc -std=c99 -Isrc tests/test_float_arena.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -o /tmp/tfa && /tmp/tfa
```
Expected: `ALL TESTS PASSED` (test_gc_trigger) and `test_float_arena: OK`.

- [ ] **Step 8: Commit**

```bash
cd /home/david/workspace/oevm
git restore src/solidified_image.c 2>/dev/null
git add src/gc.c tests/test_float_gc.c
git commit -m "feat(gc): float-arena mark + sweep reclaim wired into gc_collect (W2a step 2)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Bounded-churn acceptance + full regression + docs

Proves the RSS goal (bounded slab count under sustained churn), runs the full conformance + portability gates, and records the result.

**Files:**
- Test: `tests/test_float_gc.c` (extend with the acceptance test)
- Modify: `nodus/oevm/docs/optimizations.md` (W2 row), `nodus/oevm/docs/archive/ROADMAP.md` (W2a → done, W2b next)

**Interfaces:**
- Consumes: everything from Tasks 1–2.

- [ ] **Step 1: Write the bounded-churn acceptance test**

Append to `tests/test_float_gc.c` (body above `main`, call at the end of `main` before the final print):

```c
/* Acceptance: under sustained churn with a bounded live-set, the slab count
   stops growing once steady state is reached (the float analogue of the W1
   gc_stress RSS bound). Without reclaim this would grow without limit. */
__attribute__((noinline))
static void churn_round(size_t n) {       /* all allocations die at return */
    volatile double sink = 0.0;
    for (size_t i = 0; i < n; i++) {
        oop f = float_alloc(VT);
        ((struct Float *)f)->value = (double)i;
        sink += ((struct Float *)f)->value;
    }
    (void)sink;
}
static void test_bounded_churn(void) {
    void *saved = vm_c_stack_base;
    vm_c_stack_base = NULL;              /* precise roots only */
    /* Warm up to steady state, then measure that slabs stop growing. */
    for (int r = 0; r < 3; r++) { churn_round(50000); gc_collect(); }
    size_t slabs_steady = float_arena_slab_count();
    for (int r = 0; r < 10; r++) { churn_round(50000); gc_collect(); }
    assert(float_arena_slab_count() == slabs_steady);  /* bounded: no growth */
    vm_c_stack_base = saved;
    printf("test_float_gc: bounded churn OK (steady at %zu slabs)\n", slabs_steady);
}
```

Add `test_bounded_churn();` in `main` before `printf("ALL TESTS PASSED\n");`.

- [ ] **Step 2: Run the full float GC test**

Run:
```bash
cd /home/david/workspace/oevm
gcc -std=c99 -Isrc tests/test_float_gc.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -o /tmp/tfgc && /tmp/tfgc
```
Expected: all `test_float_gc:` lines print, ending with `bounded churn OK (steady at N slabs)` and `ALL TESTS PASSED`. N must be small (single digits), proving reclaim.

- [ ] **Step 3: Run the `-m32` portability config**

Run:
```bash
cd /home/david/workspace/oevm
bash tests/test_portability_32.sh
```
Expected: passes as before. (If the script does not already build `test_float_gc`, additionally run the build below with `-m32`; install `gcc-multilib` if missing.)

```bash
gcc -std=c99 -m32 -Isrc tests/test_float_gc.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -o /tmp/tfgc32 && /tmp/tfgc32
```
Expected: `ALL TESTS PASSED` under `-m32` (validates byte-indexed bitmap + `sizeof(struct Float)` cell math at 32-bit pointer width).

- [ ] **Step 4: Run full conformance on Model A + B**

Run:
```bash
cd /home/david/workspace/ns-oevm
OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | grep -E "Passed|Failed|Skipped"
```
Expected: `Passed: 47`, `Failed: 0`, `Skipped: 1`.

- [ ] **Step 5: Update `nodus/oevm/docs/optimizations.md`**

In the weakness table, update the **W2** row to reflect that the float arena is now reclaimable: change its status to note "arena reclaim DONE (W2a): per-slab mark bitmaps + cell free-list, swept from `gc_collect`; bounded slab count under sustained churn; unboxing (W2b/Opt #6) still pending." Keep the W2b/unboxing item as the remaining work.

- [ ] **Step 6: Update `nodus/oevm/docs/archive/ROADMAP.md`**

Mark **W2a (float-arena reclaim) complete** and set **W2b (float unboxing)** as the next GP0 step.

- [ ] **Step 7: Commit**

```bash
cd /home/david/workspace/oevm
git restore src/solidified_image.c 2>/dev/null
git add tests/test_float_gc.c nodus/oevm/docs/optimizations.md nodus/oevm/docs/archive/ROADMAP.md
git commit -m "test+docs(gp0): W2a float-arena reclaim done; bounded under sustained churn (W2a step 3)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review

**Spec coverage:**
- §Design 1 (data structures: bitmap, free-list, span) → Task 1 Step 3. ✓
- §Design 2 (`float_alloc` free-list → bump → new slab) → Task 1 Step 3. ✓
- §Design 3 (mark inside `mark()`, `is_in_float_arena` with `[lo,hi)` reject + cell-boundary check, leaf) → Task 2 Steps 3–4. ✓
- §Design 4 (`float_sweep` rebuild-from-scratch, head-slab limit, clear bits) → Task 2 Step 3. ✓
- §Design 5 (call `float_sweep` after main-heap sweep in `gc_collect`) → Task 2 Step 5. ✓
- §Testing (reclaim/reuse, conservative survival, mixed liveness, slab-boundary, bounded churn, conformance, m32) → Task 1 Step 5 (slab-boundary via existing test), Task 2 Step 1, Task 3 Steps 1–4. ✓
- §Acceptance criteria 1–5 → Task 3 (bounded churn = 1, conservative survival = 2 in Task 2, unit tests = 3, conformance/m32 = 4, fast path preserved = free-list-first in Task 1) . ✓
- §Scope boundaries (no whole-slab free, no unboxing, no struct change, no iterative-mark) → respected; nothing in any task touches them. ✓

**Placeholder scan:** No TBD/TODO-as-work. Every code step shows complete code; every run step shows the exact command and expected output. ✓

**Type consistency:** `float_alloc(oop)`, `is_in_float_arena(oop)->int`, `float_mark(oop)`, `float_sweep(void)`, `float_arena_slab_count(void)->size_t`, `float_arena_free_count(void)->size_t`, `float_is_live(oop)->int` are used identically across Tasks 1–3 and the tests. Free-list threading uses `*(struct Float **)&cell->value` consistently in alloc, sweep, and the introspection hooks. The test's `FLOAT_ARENA_SLAB_CELLS_T` (4096) mirrors `gc.c`'s private `FLOAT_ARENA_SLAB_CELLS`; if that constant ever changes, both must move together (noted in the test comment). ✓
