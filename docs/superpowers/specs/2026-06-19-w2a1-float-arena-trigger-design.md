# W2a.1 — Float-Arena GC Trigger (design)

**Date:** 2026-06-19
**Workstream:** GP0 (GC / embedded memory, Zephyr gate) — float-arena trigger gap
**Depends on:** W1 (GC trigger + conservative rooting), W2a (float-arena reclaim: per-slab markbits + cell free-list + `float_sweep`)
**Tracks:** GH #5 (float arena never triggers GC); relates to #3 (which closed the main-heap half)

## Problem

`gc_collect` — and therefore the W2a `float_sweep` it calls — is triggered **only**
by main-heap exhaustion (the W1 collect-and-retry path in `gc_allocate`). Float
cells are allocated from a **separate malloc-backed slab arena** via `float_alloc`,
which has **no trigger of its own** and never calls `gc_collect`. A workload that
allocates almost entirely floats therefore never collects, and the float arena
grows without bound — the original GH #3 symptom, still present after W1 + W2a.

### Evidence (Model A, `-O2 -DOEVM_APPLIANCE`, post W1+W2a, best of 3)

| Workload | Main-heap cap | float allocs | GC collects | Peak RSS |
|---|---|---|---|---|
| nbody@250k | 256 KB | 80.0 M | 53 | 25.8 MB |
| mandelbrot@750 | 256 KB | 149.3 M | **0** | **2.35 GB** |

mandelbrot does **0 collections even at a 256 KB main-heap cap** and peaks at
**2.35 GB**: its allocation is almost entirely floats, so the main heap never fills,
the W1 trigger never fires, and `float_sweep` never runs. nbody stays bounded only
incidentally — it also allocates main-heap objects, so its 53 main-heap-triggered
collects piggyback the float sweep.

The W2a `test_bounded_churn` unit test passed only because it calls `gc_collect()`
**explicitly** every round; nothing calls it for real pure-float execution.

## Approach

Make `float_alloc` participate in the GC trigger, mirroring `gc_allocate`'s
collect-and-retry: when the arena is about to **grow** (free-list empty and the
bump slab is full → about to `malloc` a new slab), if growth since the last collect
already grown to a soft cap (total slabs ≥ budget), run `gc_collect` first (its
`float_sweep` refills the free-list), then retry the free-list; only `malloc` a new
slab if reclaim did not free anything (the live set genuinely needs the memory).
This bounds the arena to ~`max(budget, live-set high-water)` slabs.

**The budget is a soft cap on _total_ slabs, not slabs-since-collect.** A
since-collect counter creeps: because reclaimed slabs are never freed (cell
free-list only), after each collect the large free-list drains, then the arena
grows by another `budget` slabs before the next collect — so total slabs climb by
`budget` every drain cycle, unbounded. Gating on the **total** slab count is exactly
`gc_allocate`'s collect-on-exhaustion logic and has a fixed point: once total slabs
reach the budget, every further exhaustion collects-and-reuses instead of growing
(for a live-set that fits the budget), so the count stabilizes with no creep. It
also needs **no new counter and no `float_sweep` change** — the existing
`float_slab_count` static is the gate.

Rejected alternatives (from brainstorming): folding float bytes into the main-heap
exhaustion accounting (more coupling, less direct); periodic/time-based triggers
(hacky); adaptive grow-rate budgets (more state, nondeterministic to test). A
**fixed, `-D`-tunable slab budget** was chosen — it mirrors the existing explicit
`OEVM_HEAP_SIZE` knob and gives predictable bounds.

## Design

All changes are confined to the float-arena section of `src/gc.c` plus tests. No
change to `struct Float`, `oop`, the W2a sweep/free-list mechanics, or anything the
compiler emits.

### 1. Budget knob

```c
#ifndef OEVM_FLOAT_ARENA_SLAB_BUDGET
#define OEVM_FLOAT_ARENA_SLAB_BUDGET 16   /* slabs (~1 MB host); -D lower for embedded */
#endif
```

The gate is the **existing** `static size_t float_slab_count` (total slabs ever
allocated). No new counter is introduced. (A test-only accessor
`float_arena_slab_budget()` returning the compiled macro is added so the bound test
adapts to `-D` overrides.)

### 2. `float_alloc` — collect-at-cap-before-grow

The fast paths are unchanged (zero overhead on the common case):
1. Free-list non-empty → draw a cell, return.
2. Bump slab has room → bump the cursor, return.

The trigger engages **only** at the grow boundary (free-list empty *and* bump slab
full), and only once the arena has reached the soft cap. Inserted at the top of the
existing new-slab branch, before the `malloc`:

```c
if (float_slab_used >= FLOAT_ARENA_SLAB_CELLS) {
    /* W2a.1: at the soft cap, try to reclaim before growing (mirror gc_allocate) */
    if (!gc_in_progress && float_slab_count >= OEVM_FLOAT_ARENA_SLAB_BUDGET) {
        gc_in_progress = 1;
        gc_collect();             /* float_sweep refills float_freelist */
        gc_in_progress = 0;
        if (float_freelist != NULL) {   /* reclaim succeeded — draw and return, no growth */
            struct Float *r = float_freelist;
            float_freelist = *(struct Float **)&r->value;
            r->base.vtable = vtable;
            return (oop)r;
        }
        /* else: live set exceeds the cap — fall through and grow (honor live data) */
    }
    struct FloatSlab *slab = malloc(sizeof(struct FloatSlab));   /* existing code */
    ...
    float_slab_count++;
}
... existing bump + markbit-clear ...
```

Notes:
- The gate is `float_slab_count >= BUDGET` (total slabs). Below the cap, the arena
  grows freely with no collect — the first `BUDGET` slabs accrue, then every further
  exhaustion collects-and-reuses. For a live-set within the cap the count stabilizes
  at `BUDGET`; for a larger live-set the collect frees too little, growth proceeds,
  and the count stabilizes at the true live-set size — bounded either way, no creep.
- The `gc_in_progress` guard prevents a nested re-trigger if `float_alloc` is ever
  reached during a collection (it is not today, but the guard is consistent with
  `gc_allocate` and defensive).
- The post-collect free-list draw reuses the **exact** free-list-draw idiom already
  at the top of `float_alloc` (overlay-`value` unthread, set vtable). It does **not**
  need the bump-path markbit clear (that invariant — W2a `d3310e0` — applies only to
  pristine bump cells; free-list cells were swept and carry a clear bit).
- `float_sweep` is **unchanged** — no counter to reset.

### 3. Rooting correctness

A `float_alloc`-triggered `gc_collect` runs mid-arithmetic with the operands
(`op1`/`op2` in `OP_ADD/…`; the receiver in `PRIM_asFloat`/`PRIM_sqrt`) live. They
are rooted by `scan_vm_stack` (they reside in `ctx->regs`) and additionally by W1's
conservative C-stack scan (they are also C locals across the `float_alloc` call).
The result Float is not yet allocated. No live Float is unrooted — this is exactly
the domain W1's rooting audit established; **no new roots are required**. In
contexts without a captured `vm_c_stack_base` (e.g. some unit tests) only precise
roots run, which still cover register-resident operands.

## Testing

Unit tests (extend `tests/test_float_gc.c`):

1. **Self-triggering bound (the regression that would have caught this):** allocate
   far more than `BUDGET × FLOAT_ARENA_SLAB_CELLS` floats in a loop, dropping each,
   **without ever calling `gc_collect` explicitly**; assert `float_arena_slab_count()`
   stays bounded (≤ `BUDGET` + a small constant). Set `vm_c_stack_base = NULL`
   (save/restore) so dropped floats are deterministically unreachable. Without the
   trigger this count grows without limit.
2. **Budget override:** built with a smaller `-DOEVM_FLOAT_ARENA_SLAB_BUDGET`, the
   bound is correspondingly tighter (a separate small build/run, or a documented
   check — the macro is compile-time so it cannot vary within one binary).

Acceptance (documented measurement, not a C unit assertion):
- mandelbrot@750 under a 256 KB main-heap cap: **peak RSS single-digit MB** (down
  from 2.35 GB) and **collections > 0**. Measured via the `bench.sh` Model A build
  with `/usr/bin/time -v`.

Gates:
- Conformance **47 pass / 0 fail / 1 skip** on Model A + Model B + `-m32`, unchanged.
- `test_float_gc`, `test_gc_trigger`, `test_float_arena` all still pass.

## Scope boundaries (YAGNI)

- Only the **trigger** changes — no change to the W2a sweep/free-list mechanics.
- Not float unboxing (W2b) — its measured speed ceiling is small (sub-second; see
  the re-measure table above), deferred.
- No change to `struct Float`, `oop`, or the image format.
- No whole-slab return to malloc (still cell-free-list only; the trigger bounds
  growth, which is what the embedded target requires).

## Acceptance criteria

1. `float_alloc` triggers `gc_collect` at the grow boundary past the slab budget and
   reuses reclaimed cells instead of growing when possible.
2. A float-only allocation loop (no explicit `gc_collect`) keeps `float_arena_slab_count()`
   bounded ≤ `BUDGET` + small constant.
3. mandelbrot@750 under a 256 KB cap: peak RSS single-digit MB, collections > 0.
4. Conformance 47/0/1 on A + B + `-m32`; existing GC unit tests still pass.
5. Fast paths (free-list draw, in-slab bump) unchanged — no per-allocation overhead
   added off the grow boundary.
