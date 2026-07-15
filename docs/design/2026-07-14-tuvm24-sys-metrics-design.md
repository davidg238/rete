# tuvm#24 — the sys/metrics story: heap + handles + queue telemetry as a cell

**Date:** 2026-07-14 · **Status:** approved (David, section-by-section) · **Scope ruling:** full
one-sys/metrics-story per David's 2026-07-14 scope comment on tuvm#24 — closes #24 and the
telemetry residues of #23 (handle counts) and #20 (queue high-water + push-side record) in one rung.

## Corrected premise (verified empirically, per the tuvm#18 lesson)

The issue body's "no primitive exposes it" is **stale**: `oevm_heap_stats()` → prim 104
(`heapStats`) → `kernel heapStats` → the nsl `HeapStats` class landed with rung 4 (2026-07-10)
and is tested (`nsl-tests/15_heap_stats/01_invariants.ns`). Prim 108 (`spaceOverruns:`, pull
side) landed with #20. What is actually missing:

1. **Handle telemetry** — `src/handles.c` has no live-count or high-water counters (#23 residue).
2. **Queue high-water** — `src/space.c` counts overruns but not ring-occupancy high-water (#20 residue).
3. **The `sys/metrics` record published as a cell** — the push side (#24 proper; WHY-AGENTS rider:
   metrics as cells so an agent can `peek:` without draining a stream another consumer counts on).
4. **Strict T-D.5 plateau** — `nsl-tests/11_supervision/10_soak_leak.ns` still asserts the weak
   completion proxy its own comment says to replace once a gauge is wired.

## Decisions taken (with David, this session)

- **Scope:** full sys/metrics story (not minimal-#24, not the no-new-C middle option).
- **Layout:** **one `sys/metrics` cell** holding one flat record — one schema, one peek, one
  atomic snapshot. Not per-subsystem cells; not cell-plus-sub-cells (YAGNI until a narrow
  consumer exists).
- **Seam:** **one new prim beside 104** ("Approach 1"), kernel composes, **app publishes**.
  Rejected: widening prim 104's array (churns a landed, tested seam; `HeapStats` stops meaning
  heap) and a kernel-owned `publishMetrics` (the kernel would need ambient access to the space —
  against the Milestone-C capabilities-as-constructor-args rule).

## 1 — C-side counters (tuvm)

Incrementally maintained on already-instrumented paths; nothing computed on demand; no behavior
change; a couple of integer ops per event.

- **`src/handles.c`**: `handles_live` — increment in `handle_new` when a slot goes live;
  decrement in `handle_free` **only if the slot was live** (a double-free must not skew the
  gauge). `handles_high_water` — monotonic max of `handles_live`. Accessor exposed via
  `handles.h`.
- **`src/space.c`**: `q_high_water` — monotonic max of `q_count`, bumped at post time.
  Documented nuance: `q_count` includes lazily-dead not-yet-compacted slots, so this measures
  **ring-slot pressure** (slots unavailable to a burst — exactly what G2 sizing needs), not
  logical queue depth.

## 2 — One new prim + the kernel surface (tuvm + nsl-tuvm)

- New 0-arg platform prim **`sysStats`** — id arrived at by **counting PRIM_NAMES entries**
  (never last+1 arithmetic), append-only. Answers a 4-slot SMI array
  `[handlesLive, handlesHighWater, queueHighWater, totalOverruns]` — the prim-104 pattern,
  same GC-safety shape (copy all scalars into C locals **before** the single allocating
  `indexable_new`; stores are SMIs, so no heap oop is held across an allocation, no
  `gc_temp_root` pin). All values < arena/table sizes → SMI-representable on host and target.
- **All five forwarder sites** (the standing checklist): `prim_ids.h` define · `dispatch.c` enum
  member · `dispatch.c` `PRIM_NAMES[]` string · `platform.c` case · **`kernel.ns` public Kernel
  forwarder** (the `platform` arg tests receive is the Kernel instance; forgetting site 5 is a
  DNU that mimics a dispatch-table bug).
- **`kernel metrics`** composes prim 104 + `sysStats` into one flat immutable **Record** — the
  `sys/metrics` schema:

  ```
  { heapSize. liveBytes. heapHighWater. largestFreeBlock. gcCycles.
    handlesLive. handlesHighWater. queueHighWater. overruns }
  ```

  Nine integer fields, ~150–190 B in CBOR (the symbol field names dominate; corrected
  2026-07-14 from an earlier ~70 B estimate that ignored key text) — under the 256 B/value
  radio cap (`PROTOCOL-NSL-RECORDS.md` size law). `HeapStats` and prim 104 are untouched. Float-slab
  count is **deferred** (natural extension, same record, later — YAGNI now).

## 3 — The `sys/metrics` cell + publisher policy

- The record is published onto a **cell** named `sys/metrics` (rung-9 `put:value:` /
  `peek:ifAbsent:`): any grantee — agent, porta bridge, second job — reads current state **by
  name**, non-destructively, without draining a stream another consumer counts on.
- **Publishing is the app's act**: a job holding the granted `sys/metrics` channel does
  `cell put: '' value: kernel metrics` on whatever cadence it wired. **No periodic publisher
  ships this rung** (G9: no unrequested periodic work — a future keeper/app wires a timer
  reaction when it wants cadence). T-D.5 and the schema test pull explicitly.
- **Schema pinned in `porta/docs/PROTOCOL-NSL-RECORDS.md`** as part of this rung (2026-07-11
  addendum rule: per-prefix record schemas pinned before first implementation).

## 4 — Strict T-D.5 + tests + gates

> **CORRECTED 2026-07-14 (execution finding, ruled by David).** The original gauge —
> "highWater plateaus unless there's a leak" — is **false on the host**: GC in this VM fires
> only on arena exhaustion (or `OEVM_GC_STRESS`, which the behavioral suite does not set), and
> the soak workload allocates ~9 MB against the 16 MB host arena, so **no collection ever
> runs** and `liveBytes`/`highWater` just track cumulative allocation (measured: highWater
> climbed a deterministic 9,121,280 bytes across the 90k post-warm-up iterations in a healthy
> run — three identical runs). Ruling: **force a collection at each sample point by spinning
> droppable allocations until `gcCycles` increments, then sample `liveBytes`** (= true live at
> the sweep boundary, plus a few spin-tail allocations the tolerance absorbs). No VM change. A
> separate issue is filed for a future diagnostic `gcCollect` prim (also relevant to #19's
> idle-time work); the spin is the no-new-surface bridge until then.

- **`nsl-tests/11_supervision/10_soak_leak.ns`**: keep the completion+responsiveness invariant;
  add the strict plateau — at warm-up (i=10,000) and at the end, force a sweep (allocation
  spin until `gcCycles` bumps) and sample `liveBytes`; assert end ≤ warm-up + a named
  tolerance. The tolerance is a named constant tuned once against measured host noise, with
  the tuning rule stated in-file (must sit well below the smallest leak the test should
  catch). Output stays deterministic — print `plateau true`, never raw byte counts.
- **New test** `nsl-tests/15_heap_stats/02_sys_metrics.ns`: build the record, `put:`/`peek:` it
  through the `sys/metrics` cell, assert all nine fields present and sane, cross-check
  `overruns` against prim 108's `spaceOverruns: ''`.
- **Gates**: full suite both paths (Path A solidified / Path B `.oimg`; floor 193/0/10 rises
  with the new test), all five named gates including the MCU footprint ratchet — expect a few
  dozen bytes of text; re-bank with the reason. Watch tuvm#31 (the footprint gate silently
  skips without the ARM toolchain) — verify the gate actually *ran*.
- **Close-out promotion (per the standing rule)**: `review/reliability-gaps.md` G3/G2 status
  notes; `review/tests/CLAIMS.md`; tick #24 in the tuvm#26 tracker; close out the residue notes
  on #23 and #20; ladder entry at the next milestone-boundary update.

## Error handling

The prim is 0-arg with no failure modes; counters are plain unsigned integers far from overflow
at any plausible node lifetime (a uint32 handle counter at 1 kHz churn overflows after ~49 days
of *continuous* pinning — and it counts live handles, not cumulative pins). `handle_free`'s
was-live guard is the one correctness edge. Cell semantics (kind guard, wrong-verb raise) are
rung 9's, unchanged.
