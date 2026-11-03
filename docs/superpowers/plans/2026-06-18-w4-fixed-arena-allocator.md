# W4 Fixed-Arena Allocator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the general-object `malloc`+`heap_list` allocator in `src/gc.c` with a fixed static arena (bump + size-binned free-list) and an O(1) bitmap membership check, with no behavior change.

**Architecture:** A single static, C99-aligned byte buffer (`OEVM_HEAP_SIZE`, host default 16 MB, embedded via `-D`). Objects are bump-allocated as `HeapHeader + payload` blocks tiling `[heap_base, heap_top)`. A side **alloc bitmap** (one bit per `sizeof(oop)` granule, set at each live payload start) makes `is_valid_heap_object` an O(1) range-check + object-start test, replacing the O(n) `heap_list` walk (and thus the O(n²) mark). Collection is non-moving mark-sweep over a linear walk of the arena; freed blocks return to size-class bins for reuse. `float_alloc` and `gc_collect`'s root-scanning are untouched.

**Tech Stack:** C99, gcc. No external deps. Tests are standalone C `main()`s compiled directly with gcc; conformance via `ns-oevm/tools/run_tests_oevm.py`.

## Global Constraints

- **C99 only.** Build uses `gcc -std=c99`. No C11 (`_Alignas`, `max_align_t`, `<stdalign.h>`). Use a union-typed buffer for max alignment.
- **Width-clean for `-m32`.** `sizeof(oop)` is 4 on `-m32`, 8 on host. All granule/bitmap math must use `sizeof(oop)` and `size_t`/`uintptr_t`, never hardcoded 8.
- **No behavior change.** `tests/test_oimg.c`, `tests/test_fibers.c`, `tests/test_mirrors.c` must stay green; conformance stays **47/0/1** on `OEVM_PATH=AB`; `-m32` (`tests/test_portability_32.sh`) green.
- **Scope fences:** do NOT touch `float_alloc`/`FloatSlab` (`src/gc.c:81-104`); do NOT change `gc_collect`'s root set (`scan_vm_stack`, global vtable roots, fiber roots, `gc_additional_roots_hook`); do NOT add a GC *trigger* (that is W1).
- **Spec:** `docs/design/2026-06-18-w4-fixed-arena-allocator-design.md`.
- **Build/run a single C unit test** (from `oevm/`):
  `gcc -std=c99 -Isrc tests/<t>.c src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c -lm -o /tmp/<t> && /tmp/<t>`
- **Run conformance** (from `~/workspace`): `python3 ns-oevm/tools/run_tests_oevm.py` (defaults to `OEVM_PATH=AB`). Expected tail: `47 pass / 0 fail / 1 skip` per path.

---

## File Structure

- `src/gc.c` — **modified**. Add arena buffer, bitmap, bins, helpers, `heap_init`, overridable OOM handler; rewrite `gc_allocate`, `is_valid_heap_object`, and the mark-reset/sweep loops in `gc_collect`. Remove `heap_list`. `float_alloc`, `mark`, `trace_object`, `scan_vm_stack` unchanged (except `mark` keeps calling the new `is_valid_heap_object`).
- `tests/test_arena.c` — **new**. Unit tests for bump alloc, bitmap membership precision, exhaustion via overridable handler, and (Task 2) free-list reuse.
- `ns-oevm/tools/run_tests_oevm.py` — **modified** (Task 3). Append `OEVM_EXTRA_CFLAGS` env var to both gcc invocations so the 16 KB acceptance build is runnable.
- `nodus/oevm/docs/archive/ROADMAP.md`, `nodus/oevm/docs/optimizations.md` — **modified** (Task 3). Mark W4 done; record the validated embedded ceiling.

---

## Task 1: Fixed arena + bump alloc + bitmap membership + linear mark/sweep + overridable OOM

**Files:**
- Modify: `src/gc.c` (header section + `gc_allocate` `:48-63` + `is_valid_heap_object` `:36-46` + `gc_collect` reset loop `:240-244` and sweep loop `:289-310`; remove `heap_list` `:12-13`)
- Test: `tests/test_arena.c` (create)

**Interfaces:**
- Consumes: existing `struct HeapHeader { struct HeapHeader *next; size_t size; int marked; }` (file-local in `gc.c`), `oop` (= `struct Object *`), `is_in_blob`/`is_read_only`, `flash_rom_start/end`, `oimg_blob_base/end`.
- Produces (for tests and Task 2):
  - `oop gc_allocate(oop vtable, size_t size)` — unchanged signature; now arena-backed, bump-only.
  - `void gc_collect(void)` — unchanged signature.
  - `extern oop (*oevm_oom_handler)(size_t size);` — swappable; default aborts. `gc_allocate` returns `oevm_oom_handler(size)` on exhaustion.
  - `void heap_init(void)` — idempotent arena/bitmap init; lazy-called on first `gc_allocate`/`gc_collect`. (Exported non-static so tests can re-init.)
  - `int gc_is_live(oop ptr)` — test-only introspection: non-static wrapper returning `is_valid_heap_object(ptr)` (1 if `ptr` is a live payload start in the arena, else 0). Lets tests assert retention/sweep precisely.

- [ ] **Step 1: Write the failing test** — create `tests/test_arena.c`:

```c
/* Unit test for the fixed-arena general-object allocator (src/gc.c, W4).
   Build:
     gcc -std=c99 -Isrc tests/test_arena.c \
         src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
         -lm -o /tmp/ta && /tmp/ta
   Verifies: distinct bump-allocated blocks; bitmap membership accepts live
   payload starts and rejects mid-object / interior / out-of-range pointers;
   exhaustion routes through the overridable OOM handler. */
#include "../src/oevm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <setjmp.h>

extern oop  gc_allocate(oop vtable, size_t size);
extern void gc_collect(void);
extern void heap_init(void);
extern int  gc_is_live(oop);
extern oop (*oevm_oom_handler)(size_t);
extern void (*gc_additional_roots_hook)(void);
extern void mark_object(oop);

/* ---- membership is exercised through the public mark/sweep path ---- */
/* A root object whose payload word points either at the START of a target
   (should retain) or into its INTERIOR (should NOT retain). */
static oop g_root = NULL;
static void root_hook(void) { mark_object(g_root); }

/* ---- exhaustion handler: flag + longjmp instead of abort ---- */
static jmp_buf oom_jmp;
static volatile int oom_fired = 0;
static oop test_oom(size_t size) { (void)size; oom_fired = 1; longjmp(oom_jmp, 1); }

int main(void) {
    heap_init();

    /* 1. Distinct, writable bump allocations. */
    oop a = gc_allocate(NULL, sizeof(struct Object) + 2 * sizeof(oop));
    oop b = gc_allocate(NULL, sizeof(struct Object) + 2 * sizeof(oop));
    assert(a && b && a != b);

    /* 2. Membership precision: object-start validation, observed via gc_is_live.
       A root object's payload holds a START pointer to `b` (must retain b) and
       an INTERIOR pointer to `c` (must NOT retain c). `a` is referenced by
       nobody (must be swept). */
    oop c = gc_allocate(NULL, sizeof(struct Object) + 2 * sizeof(oop));
    oop *root_slots = (oop *)gc_allocate(NULL, sizeof(struct Object) + 2 * sizeof(oop));
    g_root = (oop)root_slots;
    /* payload starts right after the Object header (vtable word) */
    oop *payload = (oop *)((char *)root_slots + sizeof(struct Object));
    payload[0] = b;                                   /* start pointer  -> retains b */
    payload[1] = (oop)((char *)c + sizeof(oop));      /* interior ptr   -> must NOT retain c */

    gc_additional_roots_hook = root_hook;
    gc_collect();                                     /* a, c unreachable -> swept; b retained */
    gc_additional_roots_hook = NULL;

    assert(gc_is_live(b)  && "start pointer must retain b");
    assert(!gc_is_live(c) && "interior pointer must NOT retain c");
    assert(!gc_is_live(a) && "unreferenced object must be swept");

    /* 3. Exhaustion routes through the overridable handler (no abort). */
    oevm_oom_handler = test_oom;
    if (setjmp(oom_jmp) == 0) {
        for (;;) { gc_allocate(NULL, 4096); }         /* must eventually OOM */
        assert(0 && "unreachable");
    }
    assert(oom_fired);

    printf("test_arena: OK\n");
    return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails to build/link**

Run: `gcc -std=c99 -Isrc tests/test_arena.c src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c -lm -o /tmp/ta && /tmp/ta`
Expected: **link/compile error** — `heap_init` and `oevm_oom_handler` are undefined (not yet added to `gc.c`).

- [ ] **Step 3: Add arena state, bitmap, helpers, and OOM handler to `src/gc.c`**

Replace the header section (the `struct HeapHeader` block and `struct HeapHeader *heap_list = NULL;`, `src/gc.c:5-13`) with — keep the `HeapHeader` struct, drop `heap_list`, add the arena. Also add `#include <string.h>` near the top includes:

```c
/* Heap Header prepended to every general-object allocation. The `next` field
   links FREE blocks within their size-class bin (Task 2); it is unused while a
   block is live. */
struct HeapHeader {
    struct HeapHeader *next;
    size_t size;            /* payload capacity, rounded up to sizeof(oop)      */
    int marked;
};

/* ---- Fixed managed arena (W4) ---------------------------------------- */
#ifndef OEVM_HEAP_SIZE
#define OEVM_HEAP_SIZE (16u * 1024u * 1024u)   /* host default; -D override for embedded */
#endif

/* C99 max-aligned backing store (no C11 _Alignas / max_align_t). */
typedef union { long double ld; void *p; long long ll; } oevm_align_cell;
#define OEVM_HEAP_CELLS (((size_t)OEVM_HEAP_SIZE + sizeof(oevm_align_cell) - 1) / sizeof(oevm_align_cell))
static oevm_align_cell oevm_heap_store[OEVM_HEAP_CELLS];

static uint8_t *heap_base = NULL;   /* = oevm_heap_store; NULL until heap_init */
static uint8_t *heap_top  = NULL;   /* bump cursor */
static uint8_t *heap_end  = NULL;   /* one past the arena */

/* Alloc bitmap: 1 bit per sizeof(oop) granule, set at each live payload start. */
#define OEVM_GRANULE      (sizeof(oop))
#define OEVM_GRANULE_BITS (OEVM_HEAP_SIZE / OEVM_GRANULE)
static uint8_t heap_bitmap[OEVM_GRANULE_BITS / 8u + 1u];

static void bm_set(size_t g)   { heap_bitmap[g >> 3] |= (uint8_t)(1u << (g & 7u)); }
static void bm_clear(size_t g) { heap_bitmap[g >> 3] &= (uint8_t)~(1u << (g & 7u)); }
static int  bm_test(size_t g)  { return (heap_bitmap[g >> 3] >> (g & 7u)) & 1u; }
static size_t bm_granule(oop obj) { return (size_t)((uint8_t *)obj - heap_base) / OEVM_GRANULE; }

/* Size-class free-list bins are introduced in Task 2; declared here as a no-op
   anchor so heap_init can reset them once they exist. */

void heap_init(void) {
    heap_base = (uint8_t *)oevm_heap_store;
    heap_top  = heap_base;
    heap_end  = heap_base + OEVM_HEAP_SIZE;
    memset(heap_bitmap, 0, sizeof heap_bitmap);
    /* Model A/B parity guard: the arena must not overlap the read-only image
       ranges, or mark()'s is_read_only/is_in_blob fences would mis-classify it.
       Ranges may be unset (0/NULL) at first init; only check when set. */
    if (flash_rom_end > flash_rom_start) {
        uintptr_t a0 = (uintptr_t)heap_base, a1 = (uintptr_t)heap_end;
        if (!(a1 <= flash_rom_start || a0 >= flash_rom_end)) {
            fprintf(stderr, "VM GC Error: arena overlaps flash ROM range\n");
            abort();
        }
    }
}

/* Overridable out-of-memory handler. Default aborts; W1 will install a
   collect-and-retry handler without touching the gc_allocate call site. */
static oop default_oom(size_t size) {
    fprintf(stderr, "VM GC Error: Out of Memory (requested %zu, used %td of %u)\n",
            size, (ptrdiff_t)(heap_top - heap_base), (unsigned)OEVM_HEAP_SIZE);
    abort();
    return NULL; /* unreached */
}
oop (*oevm_oom_handler)(size_t) = default_oom;
```

> Note: `<stddef.h>` (for `ptrdiff_t`) and `<stdint.h>` (`uint8_t`/`uintptr_t`) arrive via `oevm.h` (already used in this file). Add `#include <string.h>` for `memset`.

- [ ] **Step 4: Rewrite `gc_allocate` (bump-only) and `is_valid_heap_object` (O(1))**

Replace `gc_allocate` (`src/gc.c:48-63`):

```c
oop gc_allocate(oop vtable, size_t size) {
    if (heap_base == NULL) heap_init();

    size_t rounded = (size + (OEVM_GRANULE - 1)) & ~(size_t)(OEVM_GRANULE - 1);
    size_t need = sizeof(struct HeapHeader) + rounded;

    /* (Task 2 inserts size-binned free-list reuse here, before the bump.) */

    if (heap_top + need > heap_end) {
        return oevm_oom_handler(size);
    }
    struct HeapHeader *header = (struct HeapHeader *)heap_top;
    heap_top += need;
    header->next = NULL;
    header->size = rounded;
    header->marked = 0;

    oop obj = (oop)(header + 1);
    bm_set(bm_granule(obj));
    obj->vtable = vtable;
    return obj;
}
```

Replace `is_valid_heap_object` (`src/gc.c:36-46`):

```c
/* O(1): range-check against the arena, require sizeof(oop) alignment, then
   confirm the bitmap marks this exact word as a live payload start. */
static int is_valid_heap_object(oop ptr) {
    uint8_t *p = (uint8_t *)ptr;
    if (p < heap_base + sizeof(struct HeapHeader) || p >= heap_top) return 0;
    if (((uintptr_t)p % OEVM_GRANULE) != 0) return 0;
    return bm_test((size_t)(p - heap_base) / OEVM_GRANULE);
}

/* Test-only introspection: is `ptr` a live payload start in the arena? */
int gc_is_live(oop ptr) { return is_valid_heap_object(ptr); }
```

- [ ] **Step 5: Rewrite the mark-reset and sweep loops in `gc_collect`**

Replace the reset loop (`src/gc.c:240-244`) with a guarded linear walk:

```c
    // 1. Reset mark tags: linear walk of the arena
    if (heap_base == NULL) heap_init();
    {
        uint8_t *cur = heap_base;
        while (cur < heap_top) {
            struct HeapHeader *h = (struct HeapHeader *)cur;
            h->marked = 0;
            cur += sizeof(struct HeapHeader) + h->size;
        }
    }
```

Replace the sweep loop (`src/gc.c:289-310`, the `struct HeapHeader **pp = &heap_list; ...` block) with a linear walk that clears the bitmap for dead blocks and keeps the Fiber-stack free. (Task 2 adds the free-list push where marked.)

```c
    // 4. Sweep phase: linear walk; reclaim unmarked blocks
    extern oop FiberVtable;
    {
        uint8_t *cur = heap_base;
        while (cur < heap_top) {
            struct HeapHeader *h = (struct HeapHeader *)cur;
            uint8_t *next = cur + sizeof(struct HeapHeader) + h->size;
            if (!h->marked) {
                oop ptr = (oop)(h + 1);
                extern oop RunnableFiberVtable, SuspendedFiberVtable, DeadFiberVtable;
                if ((FiberVtable != NULL && ptr->vtable == FiberVtable) ||
                    (RunnableFiberVtable != NULL && ptr->vtable == RunnableFiberVtable) ||
                    (SuspendedFiberVtable != NULL && ptr->vtable == SuspendedFiberVtable) ||
                    (DeadFiberVtable != NULL && ptr->vtable == DeadFiberVtable)) {
                    struct Fiber *fib = (struct Fiber *)ptr;
                    extern oop boot_stack[];
                    if (fib->stack != NULL && fib->stack != boot_stack) {
                        free(fib->stack);
                        fib->stack = NULL;
                    }
                }
                bm_clear(bm_granule(ptr));   /* dead: stop validating as live */
                /* (Task 2 pushes h onto its size-class bin here.) */
            }
            cur = next;
        }
    }
```

> The `extern oop FiberVtable;` already exists earlier in `gc_collect`; if the compiler warns about the duplicate `extern`, drop the local re-declaration and reuse the existing one. The other fiber-vtable externs mirror the originals.

- [ ] **Step 6: Build and run the new test — verify it passes**

Run: `gcc -std=c99 -Isrc tests/test_arena.c src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c -lm -o /tmp/ta && /tmp/ta`
Expected: `test_arena: OK`

- [ ] **Step 7: Run the existing GC unit tests — verify no behavior change**

Run each (from `oevm/`):
```bash
for t in test_oimg test_fibers test_mirrors; do
  gcc -std=c99 -Isrc tests/$t.c src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c -lm -o /tmp/$t && /tmp/$t || { echo "FAIL $t"; break; }
done
```
Expected: each prints its own success line, no assertion failures. (If a test needs extra source files, add them — but these three only touch the GC/boot path above.)

- [ ] **Step 8: Run full conformance + 32-bit**

Run (from `~/workspace`): `python3 ns-oevm/tools/run_tests_oevm.py`
Expected: `47 pass / 0 fail / 1 skip` on both Path A and Path B.
Run (from `oevm/`): `bash tests/test_portability_32.sh`
Expected: `32-bit portability OK`. (Requires `gcc-multilib`.)

- [ ] **Step 9: Commit**

```bash
git add src/gc.c tests/test_arena.c
git commit -m "feat(gc): fixed-arena bump allocator + O(1) bitmap membership (W4 step 1)

Replace malloc/heap_list with a static OEVM_HEAP_SIZE arena (host 16MB,
embedded via -D). is_valid_heap_object is now an O(1) range-check + alloc
bitmap object-start test, killing the O(n^2) mark. gc_collect reset/sweep
become linear arena walks; dead blocks clear their bitmap bit. Overridable
oevm_oom_handler (default aborts). No free-list reuse yet (Task 2). No
behavior change: gc unit tests + conformance 47/0/1 + -m32 hold.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Size-binned free-list (reuse on alloc, push on sweep)

**Files:**
- Modify: `src/gc.c` (add bins + `bin_index`; insert reuse in `gc_allocate`; insert push in the sweep loop; reset bins in `heap_init`)
- Test: `tests/test_arena.c` (extend)

**Interfaces:**
- Consumes: `gc_allocate`, `gc_collect`, `heap_init`, `HeapHeader.next/size`, `bm_set`, `bm_granule` from Task 1.
- Produces: same public signatures; freed blocks are now reused before the bump cursor advances. No new exported symbols.

- [ ] **Step 1: Write the failing test** — append to `tests/test_arena.c`'s `main()` before the final `printf`, a reuse check:

```c
    /* 4. Free-list reuse: after a collection frees a block, the next
       same-size allocation must REUSE it (address comes back) rather than
       always bumping. We allocate a uniquely-sized object, drop it, collect,
       then allocate the same size and expect the freed address back. */
    heap_init();                          /* fresh arena for a deterministic check */
    oevm_oom_handler = default_oom_ref(); /* restore non-longjmp behaviour */
    {
        size_t sz = sizeof(struct Object) + 5 * sizeof(oop); /* distinct size class */
        oop x = gc_allocate(NULL, sz);
        /* x is unreachable (no root) -> next collect frees it */
        g_root = NULL;
        gc_additional_roots_hook = root_hook;   /* roots nothing */
        gc_collect();
        gc_additional_roots_hook = NULL;
        oop y = gc_allocate(NULL, sz);
        assert(y == x && "freed block of same size class must be reused");
    }
```

Add a tiny accessor near the top of the test file so it can restore the default handler after the longjmp test:

```c
extern oop (*oevm_oom_handler)(size_t);
/* capture the default the library installed at startup */
static oop (*g_default_oom)(size_t);
static oop (*default_oom_ref(void))(size_t) { return g_default_oom; }
```
and at the very start of `main()` add: `g_default_oom = oevm_oom_handler;`

- [ ] **Step 2: Run the test — verify the reuse assertion fails**

Run: `gcc -std=c99 -Isrc tests/test_arena.c src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c -lm -o /tmp/ta && /tmp/ta`
Expected: **FAIL** at `freed block of same size class must be reused` (Task 1 is bump-only, so `y != x`).

- [ ] **Step 3: Add bins + `bin_index`, reset in `heap_init`**

Add after the bitmap helpers in `src/gc.c` (replacing the Task 1 "no-op anchor" comment):

```c
/* Segregated free-lists. Bin k (k < FREE_BIN_COUNT-1) holds blocks of capacity
   exactly k*sizeof(oop). The last bin is a catch-all for larger blocks, served
   first-fit by capacity. */
#define FREE_BIN_COUNT 64
static struct HeapHeader *free_bins[FREE_BIN_COUNT];

static size_t bin_index(size_t rounded) {
    size_t k = rounded / OEVM_GRANULE;
    return (k < (FREE_BIN_COUNT - 1)) ? k : (FREE_BIN_COUNT - 1);
}
```

In `heap_init`, after the `memset(heap_bitmap, ...)` line, add:

```c
    for (size_t i = 0; i < FREE_BIN_COUNT; i++) free_bins[i] = NULL;
```

- [ ] **Step 4: Insert free-list reuse in `gc_allocate`**

Replace the `/* (Task 2 inserts ... reuse here ...) */` comment in `gc_allocate` with:

```c
    {
        size_t bi = bin_index(rounded);
        struct HeapHeader **pp = &free_bins[bi];
        while (*pp != NULL) {
            if ((*pp)->size >= rounded) {            /* exact bins: first fits; catch-all: first-fit */
                struct HeapHeader *h = *pp;
                *pp = h->next;                        /* unlink */
                h->next = NULL;
                h->marked = 0;
                oop obj = (oop)(h + 1);
                bm_set(bm_granule(obj));
                obj->vtable = vtable;
                return obj;
            }
            pp = &(*pp)->next;
        }
    }
```

- [ ] **Step 5: Insert the free-list push in the sweep loop**

Replace the `/* (Task 2 pushes h onto its size-class bin here.) */` comment (inside the `if (!h->marked)` block, after `bm_clear(...)`) with:

```c
                size_t bi = bin_index(h->size);
                h->next = free_bins[bi];
                free_bins[bi] = h;
```

- [ ] **Step 6: Build and run — verify the reuse test passes**

Run: `gcc -std=c99 -Isrc tests/test_arena.c src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c -lm -o /tmp/ta && /tmp/ta`
Expected: `test_arena: OK`

- [ ] **Step 7: Re-run GC unit tests + conformance + m32**

Run the Step-7 loop and the conformance/m32 commands from Task 1.
Expected: all green; conformance `47 pass / 0 fail / 1 skip`; `32-bit portability OK`.

- [ ] **Step 8: Commit**

```bash
git add src/gc.c tests/test_arena.c
git commit -m "feat(gc): size-binned free-list reuse for the fixed arena (W4 step 2)

Sweep pushes dead blocks onto segregated size-class bins; gc_allocate
reuses a bin block (first-fit by capacity) before bumping. Non-moving, no
coalescing/splitting yet (documented fragmentation tolerance). gc unit
tests + conformance 47/0/1 + -m32 hold.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: 16 KB acceptance gate (harness CFLAGS passthrough) + docs

**Files:**
- Modify: `ns-oevm/tools/run_tests_oevm.py` (append `OEVM_EXTRA_CFLAGS` to both gcc commands)
- Modify: `oevm/docs/archive/ROADMAP.md`, `oevm/docs/optimizations.md`

**Interfaces:**
- Consumes: the arena from Tasks 1–2; `OEVM_HEAP_SIZE` compile-time macro.
- Produces: a reproducible command to build+run conformance under a capped heap; the documented embedded ceiling.

- [ ] **Step 1: Add `OEVM_EXTRA_CFLAGS` passthrough to the harness**

In `ns-oevm/tools/run_tests_oevm.py`, near the top (after `OEVM_PATH = ...`), add:

```python
OEVM_EXTRA_CFLAGS = os.environ.get("OEVM_EXTRA_CFLAGS", "").split()
```

Then append `OEVM_EXTRA_CFLAGS` to the gcc argument lists in **both** build sites:
- the Path B image-VM build (the `["gcc", "-Wall", "-std=c99", "src/vm.c", ... "-lm"]` list around line 69) — insert `*OEVM_EXTRA_CFLAGS` before `"-lm"`.
- the Path A runner build (`cmd_build = ["gcc", "-Wall", "-Wextra", "-std=c99", "-DOEVM_APPLIANCE", ...]` around line 175) — insert `*OEVM_EXTRA_CFLAGS` before `"-lm"`.

Example for the Path A list:
```python
    cmd_build = [
        "gcc", "-Wall", "-Wextra", "-std=c99",
        "-DOEVM_APPLIANCE",
        "src/vm.c", "src/gc.c", "src/dispatch.c", "src/solidified_image.c", "src/oevm_main.c",
        "-o", "oevm_runner",
        *OEVM_EXTRA_CFLAGS,
        "-lm",
    ]
```

- [ ] **Step 2: Sanity-run conformance unchanged (no extra flags)**

Run (from `~/workspace`): `python3 ns-oevm/tools/run_tests_oevm.py`
Expected: `47 pass / 0 fail / 1 skip` (passthrough is empty → identical to before).

- [ ] **Step 3: Run the 16 KB acceptance gate**

Run (from `~/workspace`):
`OEVM_EXTRA_CFLAGS=-DOEVM_HEAP_SIZE=16384 python3 ns-oevm/tools/run_tests_oevm.py`
Expected (target): `47 pass / 0 fail / 1 skip`.
**Contingency:** if any test now fails with the OOM diagnostic (`VM GC Error: Out of Memory`), re-run doubling the cap (`32768`, `65536`, …) until 47/0/1, and record the smallest passing value as the embedded ceiling. The relevant tests run with no GC trigger in W4, so the cap must hold the test's full live + dead object set.

- [ ] **Step 4: Record the validated ceiling in docs**

In `oevm/docs/optimizations.md`, update the W4 row of the weakness table to "DONE", noting the validated ceiling (e.g. "general-object heap fits in 16 KB; conformance 47/0/1 under `-DOEVM_HEAP_SIZE=16384`").
In `oevm/docs/archive/ROADMAP.md` GP0 section, mark step 1 (W4) done and point at the smallest passing cap. Keep W1/W2a/W2b as next.

- [ ] **Step 5: Commit (both repos)**

```bash
# ns-oevm
cd ~/workspace/ns-oevm
git add tools/run_tests_oevm.py
git commit -m "test(harness): OEVM_EXTRA_CFLAGS passthrough for capped-heap runs

Lets the W4 acceptance gate build conformance with -DOEVM_HEAP_SIZE to
validate the embedded general-object ceiling.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
# oevm
cd ~/workspace/oevm
git add nodus/oevm/docs/archive/ROADMAP.md nodus/oevm/docs/optimizations.md
git commit -m "docs(gp0): W4 fixed-arena allocator done; record embedded heap ceiling

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review

**Spec coverage:**
- Component 1 (arena layout) → Task 1 Step 3. ✓
- Component 2 (bump alloc + rounding + exhaustion handler) → Task 1 Steps 3–4. ✓
- Component 2 (size-binned free-list, no split/coalesce) → Task 2. ✓
- Component 3 (bitmap O(1) membership + object-start) → Task 1 Steps 3–4 + test Step 1. ✓
- Component 4 (linear mark-reset + sweep; roots unchanged) → Task 1 Step 5. ✓
- Component 5 (Model A/B parity assert; mark fences unchanged; m32) → Task 1 Step 3 (`heap_init` guard) + Steps 8 / Task 2 Step 7 (m32). ✓
- Decision: overridable `oevm_oom_handler` → Task 1 Step 3 + test Step 1. ✓
- Acceptance gate (16 KB via `-D`, conformance, m32, documented ceiling) → Task 3. ✓
- Out-of-scope fences (float arena, no trigger, fiber stacks, no coalescing) → Global Constraints + Task comments. ✓

**Placeholder scan:** No "TBD/TODO/handle edge cases" — every code step shows full code; the only "(Task 2 inserts …)" markers are deliberate anchors that Task 2 replaces with shown code.

**Type consistency:** `oevm_oom_handler` is `oop (*)(size_t)` everywhere; `bm_granule(oop)`, `bin_index(size_t)`, `heap_init(void)`, `HeapHeader.size` = rounded payload capacity used consistently in alloc/reuse/sweep; granule is `sizeof(oop)` throughout (width-clean). `is_valid_heap_object` unchanged signature/semantics consumed by the untouched `mark`.
