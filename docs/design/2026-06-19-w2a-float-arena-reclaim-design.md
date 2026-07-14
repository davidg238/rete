# W2a — Float-Arena Reclaim (design)

**Date:** 2026-06-19
**Workstream:** GP0 (GC / embedded memory, Zephyr gate) — step W2a
**Depends on:** W1 (GC trigger + conservative rooting, merged `d252e9d`), W4 (fixed-arena allocator)
**Parent spec:** `docs/design/2026-06-18-gc-embedded-memory-design.md`
**Tracks:** GH #3 (closed by W1; float half addressed here)

## Problem

`float_alloc()` (`src/gc.c`) hands out `struct Float` cells by bumping a cursor
within malloc-backed slabs (`struct FloatSlab`, `FLOAT_ARENA_SLAB_CELLS = 4096`
cells each). It was introduced in Opt #5 to remove the per-op `malloc` + heap-list
cost that dominated nbody/mandelbrot (nbody 1.53×, mandelbrot 1.89×).

The arena is **deliberately never reclaimed**: cells are not on `heap_list`, so
mark/sweep never sees them (`is_valid_heap_object()` returns 0 for them and
`mark()` early-returns). Before W1 there was no runtime GC trigger, so this only
matched the existing "never collect" profile. After W1, `gc_collect` runs under
memory pressure — but float slabs still grow without bound under sustained float
churn (mandelbrot peak RSS ~4.5 GB). On the nRF52840 target (256 KB) this is fatal.

**W2a makes the float arena reclaimable** so float-heavy code has bounded RSS,
while preserving the Opt #5 bump-allocation speed win.

## Approach (chosen)

**Reclaim the separate arena in place** — keep the malloc-backed slab pool; add
per-slab mark bitmaps + a cell free-list. `mark()` pins reachable Floats; a sweep
threads dead cells onto the free-list; `float_alloc` draws free-list → bump → new
slab. **Non-moving**, so it stays compatible with W1's conservative C-stack roots.

Rejected alternatives:
- **Re-merge into the W4 main arena** (allocate Floats via `gc_allocate`): simplest,
  but routes high float churn through the *fixed* `OEVM_HEAP_SIZE` arena → far more
  frequent collections, and loses the 2-instruction bump (likely regresses
  nbody/mandelbrot). The separate malloc-backed arena keeps float churn out of the
  constrained main heap — better for the embedded goal, not just speed.
- **Scavenge / copy survivors** (semispace): reclaims whole slabs but **moves**
  Floats — fundamentally incompatible with W1's conservative scan, which finds
  float pointers in C locals it cannot safely rewrite. Reject.

**Reclaim scope (chosen): cell free-list only.** Dead cells are reused by future
`float_alloc`; slabs are never freed back to malloc once allocated. This bounds RSS
at the high-water mark of concurrently-live Floats — enough to close the
unbounded-growth concern. Whole-slab return to the OS is a deferred follow-up if the
high-water mark proves too high in practice.

## Design

All changes are confined to `src/gc.c` (float-arena section + `gc_collect`) plus
tests. No change to `struct Float` (`src/oevm.h`) and nothing the compiler emits.

### 1. Data structures

```c
#define FLOAT_ARENA_SLAB_CELLS 4096
struct FloatSlab {
    struct FloatSlab *next;
    struct Float cells[FLOAT_ARENA_SLAB_CELLS];
    uint8_t markbits[FLOAT_ARENA_SLAB_CELLS / 8];   /* NEW: side bitmap, 512 B */
};
static struct FloatSlab *float_slab = NULL;          /* head (bump) slab */
static size_t float_slab_used = FLOAT_ARENA_SLAB_CELLS;
static struct Float *float_freelist = NULL;          /* NEW: reclaimed cells */

/* NEW: address span across all slabs, for a cheap is_in_float_arena() reject. */
static char *float_arena_lo = NULL;   /* min cell address seen */
static char *float_arena_hi = NULL;   /* one-past max cell address seen */
```

`struct Float` is `{ struct Object base; double value; }`. A dead cell stores the
free-list `next` pointer by overlaying `value` (the cell is dead, so `value` is
unused). The mark bitmap is **zeroed on slab allocation** (the cells themselves
need not be zeroed).

### 2. Allocation — `float_alloc(oop vtable)`

Draw order: **free-list → bump (head slab) → new slab.**

```
if float_freelist != NULL:
    f = float_freelist
    float_freelist = *(struct Float **)&f->value   /* unthread */
else:
    if float_slab_used >= FLOAT_ARENA_SLAB_CELLS:
        slab = malloc(sizeof(struct FloatSlab))     /* abort on NULL, as today */
        zero slab->markbits
        slab->next = float_slab; float_slab = slab; float_slab_used = 0
        update float_arena_lo / float_arena_hi to include slab->cells[0..N)
    f = &float_slab->cells[float_slab_used++]
f->base.vtable = vtable
return (oop)f
```

The bump cursor is **never rewound**; reclaimed cells return only via the
free-list. Pristine bump-tail cells in the head slab are therefore never on the
free-list.

### 3. Mark — inside `mark()` (the shared choke-point)

`mark_object()` → `mark()`, and both the precise roots and the W1 conservative
C-stack scan funnel through `mark()`. Adding float recognition here covers **both**
root sources, so a Float held only in a C local is pinned. Floats are immutable
**leaves** — the only outgoing pointer is the shared `FloatVtable`, already a global
root — so marking a Float sets its bit and returns; no tracing.

Insert **before** the `is_valid_heap_object()` early-return, after the cheap
SMI/NULL/blob/read-only guards:

```c
if (is_in_float_arena(ptr)) {
    /* compute (slab, cell index); set markbit; return (leaf) */
    float_mark(ptr);
    return;
}
```

`is_in_float_arena(ptr)`:
1. Fast reject: `ptr < float_arena_lo || ptr >= float_arena_hi` → not ours.
2. Walk the slab list; for each slab, if `ptr` is within
   `&slab->cells[0] .. &slab->cells[FLOAT_ARENA_SLAB_CELLS]` **and** aligned to a
   cell boundary (`(ptr - cells) % sizeof(struct Float) == 0`), it is cell
   `index = (ptr - cells) / sizeof(struct Float)` → set `slab->markbits[index/8] |=
   1 << (index%8)`. A pointer into the middle of a cell is not a valid Float oop and
   is ignored.

GC is exhaustion-triggered (rare), so the per-slab walk is acceptable. The
`[lo,hi)` span makes the common "not a float" case O(1).

### 4. Sweep — `float_sweep()`, called from `gc_collect`

Called **after** the main-heap sweep. Rebuild the free-list from scratch each cycle
(mirrors how the main heap rebuilds `free_bins`):

```
float_freelist = NULL
for each slab in float_slab list:
    limit = (slab == head) ? float_slab_used : FLOAT_ARENA_SLAB_CELLS
    for index in [0, limit):
        if markbit(slab, index) set:
            clear the bit            /* live: ready for next cycle */
        else:
            cell = &slab->cells[index]
            *(struct Float **)&cell->value = float_freelist   /* thread */
            float_freelist = cell
            /* bit already clear */
```

After sweep every bit is 0, ready for the next mark phase. Cells already on the
free-list from a prior cycle are simply re-threaded (their bit is clear → treated as
dead), so rebuilding from scratch is correct and self-consistent — no double-insert,
no self-cycling. Pristine bump-tail cells in the head slab (`index >=
float_slab_used`) are skipped.

### 5. `gc_collect` integration

- **Mark reset:** none needed for floats beyond the per-cycle bit clearing the sweep
  already does (bits start at 0 from slab zero-init; sweep restores 0). The main
  heap's start-of-collect mark reset is unchanged.
- **Sweep order:** call `float_sweep()` immediately after the existing main-heap
  sweep loop in `gc_collect`, before returning.

## Testing

Unit tests (extend `tests/test_gc_trigger.c`; reuse `tests/test_float_arena.c`
slab-boundary coverage):

1. **Reclaim/reuse:** allocate many Floats (force ≥2 slabs), drop all references,
   trigger `gc_collect`, then allocate another wave — assert the slab count does
   **not** grow (cells came from the free-list). Verifies dead cells are reclaimed.
2. **Conservative survival:** hold a Float by value only in a C local (no heap/frame
   reference), trigger `gc_collect`, assert the cell is still live (bit set / not on
   free-list) and its `value` intact. Verifies W1 conservative pinning reaches the
   arena.
3. **Mixed liveness:** some Floats reachable via a marked heap object's field, others
   dead — assert only the dead ones are reclaimed and the live ones' values survive.
4. **Slab-boundary correctness** (existing `test_float_arena.c`) still passes.

Acceptance gate:
- A heap-capped, sustained float-churn loop shows a **bounded slab count** (stops
  growing once the live-set stabilizes) — the float analogue of the W1 `gc_stress`
  RSS bound.
- Conformance **47 pass / 0 fail / 1 skip** unchanged on Model A + Model B + the
  `-m32` portability config (bitmap is byte-indexed and cell math uses
  `sizeof(struct Float)`, so it is pointer-width-correct).

## Scope boundaries (YAGNI)

- **No** whole-slab return to malloc — cell free-list only. Follow-up if RSS
  high-water proves too high.
- **No** float unboxing — that is W2b (Opt #6).
- **No** change to `struct Float` layout.
- **No** change to the recursive-mark structure — the iterative-mark conversion is
  tracked separately as GH #4 (pre-Zephyr).

## Acceptance criteria

1. Float cells dead at a collection are reclaimed and reused (slab count bounded
   under sustained churn with a stable live-set).
2. A Float reachable only via a C local survives a collection (conservative pinning).
3. New unit tests (reclaim/reuse, conservative survival, mixed liveness) pass;
   `test_float_arena.c` and `test_gc_trigger.c` still pass.
4. Conformance 47/0/1 on Model A + B + `-m32`.
5. Opt #5 bump-allocation fast path preserved (no regression to the
   free-list-empty common case).
