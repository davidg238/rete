# GC & Embedded Memory Management — Design Scope (GH #3)

**Date:** 2026-06-18
**Status:** SCOPE (not yet a build plan — author a plan via brainstorming →
writing-plans before implementing)
**Driver:** Migration to **Zephyr OS** (resident OEVM on nRF52840-class, ~256 KB
RAM). This is the gating item for the Zephyr target — see
`../../optimizations.md` (weaknesses W1+W4+W2) and `../../../ns-tests/00_perf/RESULTS.md`.

## Problem

`gc_collect()` (`src/gc.c:238`) is a **complete mark-and-sweep** — root scan
(VM stack frames sequential + per-fiber, global vtables, `current_fiber`,
additional-roots hook), recursive mark with special cases (Vtable bindings,
Float, Fiber stacks), sweep of unmarked `heap_list` blocks, and read-only guards
(`is_in_blob`, `is_read_only`) so Model B's read-only image is never touched.

**It is never called from the running VM** (only `tests/test_*.c`). Every
allocation grows the heap until process exit. On a desktop the OS reclaims on
exit, so it has gone unnoticed; on a resident embedded VM it is fatal. Measured:
mandelbrot peak RSS **4,558 MB** (vs Soup 6 MB).

The blocker decomposes into three problems plus cross-cutting correctness.

## Problem 1 — No trigger

`gc_allocate` (`src/gc.c:49`) has no byte/count accounting and no threshold.
Nothing calls `gc_collect`.

**Direction:** add heap accounting (bytes live / bytes since last GC) and a
threshold trigger in the allocation path(s). Useless alone — see Problem 2.

## Problem 2 — Floats are unreclaimable, and dominate

The float bump arena (`float_alloc`, `src/gc.c:90`) hands out **headerless**
`struct Float` cells from slabs; they are **not on `heap_list`**, so mark/sweep
cannot see them (by design — `is_valid_heap_object` returns 0, `mark` early-outs).
This is ~99% of the allocation pressure (mandelbrot's 4.5 GB ≈ floats).

Two routes, not mutually exclusive:
- **2a — make the arena collectable.** Per-slab mark **bitmap** (cells are
  headerless, so a side bitmap, not a per-cell header — preserves the ~2-insn
  alloc) + sweep: rebuild a per-slab free-list, or free fully-dead slabs. Keeps
  the boxed model; adds mark + sweep cost proportional to live floats.
- **2b — stop boxing floats** (Opt #6, deferred). Runtime-guarded
  arithmetic-tree fusion: fold a pure `+−×÷` subtree into one fused op computing
  in raw C doubles, boxing only the root, with leaf type-guards and an exact-`/`
  / overflow / DNU slow path. If intermediates aren't boxed, there's nothing to
  collect. Bigger compiler change; also the headline perf lever.

For Zephyr, **2a is the minimum to not-OOM**; **2b** is the better long-term and
reduces GC frequency. They compose: 2a makes the current model safe today, 2b
shrinks what 2a must reclaim.

## Problem 3 — Allocator is not embedded-grade

`gc_allocate` = `malloc` per object + singly-linked `heap_list`; **mark is
O(n²)** because `is_valid_heap_object` (`src/gc.c:37`) linearly walks the whole
list for *every* pointer marked. A resident VM on 256 KB cannot use unbounded
libc `malloc`, and O(n²) mark on a real heap is too slow to trigger.

**Direction:** a **fixed pre-allocated managed heap** (one static arena sized to
the RAM budget): bump-allocate within it; membership becomes an **O(1) range
check** against the arena bounds (kills the O(n²)); reclaim via mark-sweep with a
size-binned free-list, or **mark-compact** if fragmentation bites (compaction is
harder — must update all references, including reified stack frames). Removes the
`malloc` dependency Zephyr disfavors and gives a hard RAM ceiling.

## Cross-cutting correctness (the part that makes this real work)

- **GC safe-points / rooting.** Triggering inside `gc_allocate` means any oop
  held only in a **C local** across the trigger can be collected mid-construction
  (e.g. `dispatch.c` assembling a Message/`LargeInteger`/Symbol from args before
  it is reachable from a frame register). Requires an **audit of every allocation
  site** for live-oops-in-C-locals, and either a conservative C-stack scan or
  explicit temporary roots / allocation ordering. This is the classic GC-safety
  pass and the main correctness risk.
- **Conservative vs precise tracing.** `trace_object` scans payloads at word
  granularity; today the O(n) `is_valid_heap_object` validation is what keeps
  that *safe* (a `LargeInteger.mag` word that looks like a pointer fails the
  membership test). Replacing it with a fast range check yields conservative
  **false-retention** (safe but leaky) unless **precise per-vtable field maps**
  are added. Decision: accept conservative retention on a small heap, or invest
  in field maps. (Floats and Fibers are already special-cased in `mark`.)
- **Model A vs Model B parity.** Both must collect identically. `is_in_blob`
  (Model B read-only `.oimg`) and `is_read_only` (Model A flash range) already
  fence the image; the new arena must sit clearly outside both so the guards keep
  working. Verify under `OEVM_PATH=AB` and `-m32`.
- **Reified stack frames.** Frames are layout-compatible heap-like objects linked
  by `prev_frame`; the GC already walks them. A *compacting* collector (3) would
  have to relocate/patch them — a strong reason to prefer non-moving mark-sweep
  first.

## Recommended sequencing

1. **Allocator foundation (W4):** fixed static arena + bump alloc + O(1)
   range-check membership. No behavior change yet; existing `gc_collect` keeps
   passing its unit tests on the new structure.
2. **Trigger + safe-points (W1):** byte-threshold trigger in the alloc path;
   audit/roots pass; long-running heap-capped `gc_stress` asserting bounded RSS
   (GH #3 acceptance point).
3. **Float reclamation (W2a):** arena mark-bitmap + sweep, so the dominant
   allocator participates in collection.
4. **Float unboxing (W2b / Opt #6):** follow-on perf + memory; reduces GC load.

Each step is independently testable (unit tests + conformance `OEVM_PATH=AB` +
`-m32` must stay green; the no-GC steady-state numbers in RESULTS.md become the
*before*, and are expected to regress slightly once collection runs).

## Acceptance

- A heap-capped, long-running container (float-heavy + object-heavy) holds
  **bounded RSS** indefinitely (the GH #3 point 3 validation).
- Conformance 47/0/1 on Model A + B + `-m32` unchanged.
- A documented RAM ceiling that fits the nRF52840 budget.

## Open decisions (resolve in the plan)

- Mark-sweep + free-list **vs** mark-compact (fragmentation tolerance on 256 KB).
- Conservative tracing **vs** precise field maps.
- 2a-only **vs** 2a+2b for the first Zephyr-runnable milestone.
- Arena sizing / multi-arena (per-container isolation under nodus?).
