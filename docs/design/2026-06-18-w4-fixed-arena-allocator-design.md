# W4 — Fixed-Arena Allocator Foundation — Design

**Date:** 2026-06-18
**Status:** APPROVED (ready for writing-plans)
**Parent scope:** `2026-06-18-gc-embedded-memory-design.md` (GP0, step 1 of 4).
**Driver:** Migration to Zephyr OS on nRF52840-class hardware (~256 KB RAM). W4 is
the structural foundation; it does **not** add a GC trigger (W1), float
reclamation (W2a), or unboxing (W2b).

## Goal

Replace the general-object allocator — `malloc` per object plus a singly-linked
`heap_list` (`src/gc.c:49`, `:13`) — with a **fixed pre-allocated static arena**,
and replace the O(n) `is_valid_heap_object` linear walk (`src/gc.c:37`, called per
traced pointer → O(n²) mark) with an **O(1) range check plus object-start
validation**.

**No behavior change.** The three GC unit tests that call `gc_collect()`
directly — `tests/test_oimg.c`, `tests/test_fibers.c`, `tests/test_mirrors.c` —
keep passing unchanged. Conformance stays **47/0/1** on Model A + Model B + `-m32`.

`float_alloc` (`src/gc.c:90`, separate slab arena) and `gc_collect`'s
root-scanning logic (`scan_vm_stack`, global vtable roots, fiber roots, the
additional-roots hook) are **out of scope** and untouched.

## Decisions (resolved during brainstorming)

1. **Scope:** W4 (allocator foundation) only. W1/W2a/W2b get their own specs.
2. **Reclaim model:** non-moving **mark-sweep + size-binned free-list**. Chosen
   over mark-compact because reified stack frames (linked by `prev_frame`),
   BoundClass outers, and vtable bindings would all need relocation/patching
   under a moving collector. Non-moving keeps every reference valid.
3. **Tracing precision:** **O(1) range check + object-start validation** (via an
   alloc bitmap), not a pure range check (would allow conservative false
   retention) and not per-vtable precise field maps (large cross-cutting change,
   deferred). Preserves today's precise membership semantics.
4. **Sizing / exhaustion:** compile-time `OEVM_HEAP_SIZE`. Host default ~16 MB
   (dev/benchmarks unaffected). Embedded nRF52840 target **16 KB**, validated by
   building the host binary with `-DOEVM_HEAP_SIZE=16384` and running the full
   conformance suite under that cap. In W4 there is no auto-GC, so arena
   exhaustion is a **fatal handler + diagnostic** (`abort`). Benchmarks and stress
   tests are exempt from the 16 KB cap.

## Current structure (as of `src/gc.c`, master 7ac0824)

```c
struct HeapHeader { struct HeapHeader *next; size_t size; int marked; };
struct HeapHeader *heap_list = NULL;            /* global list head */

oop gc_allocate(oop vtable, size_t size) {       /* malloc + push heap_list */ }
static int is_valid_heap_object(oop ptr) {       /* O(n) walk of heap_list   */ }
static void mark(oop ptr) { ... is_valid_heap_object(ptr) ... header->marked ... }
void gc_collect(void) { /* reset marks (walk list); scan roots; sweep list */ }
```

Every object is `(oop)(header + 1)`; `mark`/sweep read `header->marked` and
`header->size`. Sweep specially `free()`s Fiber stacks (`fib->stack`, malloc'd at
`src/vm.c:790`, **not** a `gc_allocate` object).

## Design

### Component 1 — Arena layout

A single static, maximally-aligned buffer:

```c
#ifndef OEVM_HEAP_SIZE
#define OEVM_HEAP_SIZE (16u * 1024 * 1024)      /* host default; -D override for embedded */
#endif
static _Alignas(max_align_t) uint8_t oevm_heap[OEVM_HEAP_SIZE];
static uint8_t *heap_base;                       /* = oevm_heap */
static uint8_t *heap_top;                        /* bump cursor; grows toward heap_end */
static uint8_t *heap_end;                        /* = oevm_heap + OEVM_HEAP_SIZE */
```

The region `[heap_base, heap_top)` is **fully tiled** by headered blocks, live or
freed, each `HeapHeader + payload`. A block's successor is at
`(uint8_t*)block + sizeof(HeapHeader) + block->size`, so the whole region can be
walked linearly with no auxiliary list.

`HeapHeader` keeps `size` and `marked`; the `next` field is **repurposed** to link
free blocks within their size-class bin (the global `heap_list` head is removed).

`heap_init()` runs once at startup (before first allocation): sets the three
pointers, zeroes the alloc bitmap, asserts non-overlap with the image ranges (see
Component 5).

### Component 2 — Allocation (`gc_allocate`, signature unchanged)

`oop gc_allocate(oop vtable, size_t size)`:

1. Round `size` up to the 8-byte granule.
2. **Free-list reuse:** index the size-class bin for the rounded size; first-fit a
   block whose capacity ≥ requested. On hit: unlink from the bin, set the alloc
   bitmap bit at its payload-start granule, set `vtable`, return. (A reused block
   keeps its original `size`/capacity; the slack is tolerated — no splitting in
   W4.)
3. **Bump:** if `heap_top + sizeof(HeapHeader) + rounded ≤ heap_end`, carve a new
   block at `heap_top`, advance `heap_top`, set header (`size = rounded`,
   `marked = 0`), set the bitmap bit, set `vtable`, return.
4. **Exhaustion:** neither path fits → call the **overridable OOM handler**
   `oevm_oom(size)`. The default handler prints used / capacity / requested bytes
   to `stderr`, then `abort()`s. It is invoked through a swappable function
   pointer (`oop (*oevm_oom_handler)(size_t)`, defaulting to the abort handler) so
   W1 can install a "call `gc_collect`, retry steps 2–3, then abort" handler
   **without touching the `gc_allocate` call site**. In W4 the default (hard fail)
   is in effect.

**Size-class bins:** a small fixed array of segregated free-lists (e.g.
power-of-two or fixed-step classes spanning typical object sizes; exact class
table chosen in the plan). No coalescing of adjacent free blocks in W4 — a
documented fragmentation risk; coalescing is a cheap later add because the linear
walk already visits blocks in address order.

### Component 3 — Membership & tracing (the O(n²) kill)

**Alloc bitmap:** one bit per 8-byte granule of the arena —
`OEVM_HEAP_SIZE / 8 / 8` bytes (256 KB at 16 MB host; 256 B at 16 KB embedded),
a static array. A bit is **set** at an object's payload-start granule on
allocation and **cleared** when the block is swept-free.

`is_valid_heap_object(ptr)` becomes O(1):

```c
static int is_valid_heap_object(oop ptr) {
    uint8_t *p = (uint8_t *)ptr;
    if (p < heap_base + sizeof(struct HeapHeader) || p >= heap_top) return 0;
    if (((uintptr_t)p & 7u) != 0) return 0;                 /* 8-aligned payload start */
    size_t granule = (size_t)(p - heap_base) >> 3;
    return bitmap_test(granule);                            /* set only at live payload starts */
}
```

This preserves today's precise semantics: a payload word that happens to look like
a pointer but lands mid-object, or into a freed block, has no bit set and is
rejected — exactly what the O(n) walk did, now in O(1). `trace_object`'s
word-granularity scan and `mark`'s special cases (Vtable bindings, Float, Fiber
stacks) are **unchanged**.

### Component 4 — Collection (`gc_collect`)

Three phases; only iteration mechanics change, not semantics:

1. **Mark-reset:** linear walk `heap_base → heap_top`, clear each block's `marked`
   (replaces the `heap_list` walk).
2. **Mark:** **identical** to today — `scan_vm_stack`, global vtable roots, fiber
   vtable roots, `current_fiber`, `gc_additional_roots_hook`.
3. **Sweep:** linear walk `heap_base → heap_top`. Unmarked block → clear its
   bitmap bit, push onto its size-class bin (retaining the special
   `free(fib->stack)` for Fiber objects). Marked block → leave in place
   (non-moving; references stay valid).

### Component 5 — Model A / Model B parity

- `heap_init()` asserts `oevm_heap` does not overlap `[flash_rom_start,
  flash_rom_end)` (Model A XIP flash range) nor the Model B `.oimg` blob. The
  arena lives in BSS, distinct from both.
- `mark` already calls `is_in_blob(ptr)` and `is_read_only(ptr)` **before**
  `is_valid_heap_object`, so the read-only image is fenced regardless of the
  membership implementation. (`test_oimg.c` documents this ordering; it stays
  valid — a blob pointer fails the range check anyway.)
- Verify under `OEVM_PATH=AB` and `-m32`. Bitmap index math must be width-clean
  on 32-bit (`size_t` shifts, no 64-bit assumptions).

## Explicitly out of scope (deferred, noted so they are not forgotten)

- **Fiber stacks** still `malloc` at `src/vm.c:790` and `free()` in sweep —
  moving them to a static/pooled allocation is a later embedded step.
- **`float_alloc` slabs** untouched until W2a (float reclamation).
- **No GC trigger** — W1.
- **No coalescing / no compaction** — fragmentation tolerated on a small heap;
  revisit only if it bites.

## Testing & acceptance

- **TDD — new `tests/test_arena.c`:** alloc/free/reuse round-trips; bitmap
  validation rejects mid-object and freed-block pointers; free-list reuse before
  bump; exhaustion reaches `oevm_oom`; bump-then-reuse ordering.
- **Regression:** `tests/test_oimg.c`, `tests/test_fibers.c`,
  `tests/test_mirrors.c` stay green (behavior unchanged).
- **Conformance:** `python3 ns-oevm/tools/run_tests_oevm.py` from
  `~/workspace`, `OEVM_PATH=AB` → **47/0/1** unchanged; `-m32`
  (`oevm/tests/test_portability_32.sh`) green.
- **Acceptance gate (the embedded ceiling):** build the host binary with
  `-DOEVM_HEAP_SIZE=16384` and run all 47 conformance tests. Pass ⇒ 16 KB is the
  documented nRF52840 general-object ceiling. Fail ⇒ raise to the smallest
  passing power-of-two and document the actual figure.

## Open decisions for the plan

- Exact size-class table for the segregated free-lists (class boundaries + count).
