# G3 — GC strategy: non-moving mark-sweep, ratified

**Status:** DECIDED 2026-07-04 (ratified by David; https://github.com/davidg238/nsl-tuvm/issues/3). Resolves the G3 gap
(`tuvm/review/reliability-gaps.md` §G3) at the A→B boundary, before Milestone B lands
the allocation-heavy paths (message values, tuple space). Includes the G4 line-table
provision rider.

## The decision

The GC Milestone A shipped is **ratified as the strategy**, not treated as an interim:
**non-moving mark-sweep** over a fixed static arena, with bump allocation until
exhaustion, then exact-size segregated free-list bins with a cross-bin first-fit
fallback; bounded mark stack with overflow-rescan recovery (`tuvm/src/gc.c`).

Fragmentation — the known failure mode of non-moving collectors on years-long uptimes —
is managed by the deliberately adopted mitigation package M1–M6 below, with a written
revisit trigger (M5) instead of an open-ended "we'll see".

## Constraints that decided it

1. **~100–150 KB heap budget** (256 KB nRF52840 minus Zephyr + OpenThread). A semi-space
   copier needs 2× space — not viable.
2. **Address-derived identity is load-bearing.** `identityHash` and Symbol hash are
   pointer-derived (`dispatch.c` `PRIM_hash`). A moving collector changes them →
   every Dictionary keyed on identity breaks, unless every object grows a stored hash
   field (+1 word/object — real money at this scale) or all tables rehash at GC.
3. **The interpreter holds raw C pointers into the heap.** `vm_stack` caches
   `Stack->data`; primitives hold oops across allocations via `gc_temp_roots`, which
   are *marked*, not *updated*. Compaction turns every such site into a fixup
   obligation across `vm.c`/`dispatch.c` — an invasive rewrite of a working,
   sanitizer-clean, cross-validated engine.
4. **Differential-test determinism (T0.2).** Address-stable objects keep hash and
   iteration behavior stable across runs and platforms; a compactor makes Dictionary
   iteration order GC-schedule-dependent — noise in the frozen-oracle harness.
5. **L7 boxing pressure points toward bins, not compaction.** On the 32-bit target,
   u32 register values, ms-timestamps, and u32×u32 products box to `LargeInteger`
   routinely (see the L7 decision). But those boxes are **same-size and die young**:
   exact-size bins give perfect reuse, so integer churn is fragmentation-*neutral*.
   What fragments a non-moving heap is mixed-lifetime, variable-size data — strings,
   byte buffers, variable-arity structures. That is what M1/M3 target.
6. **Deep sleep retains RAM** on the target, so "reboot as hygiene" costs live state;
   fragmentation must be managed in place — which is why M2's telemetry is not
   optional garnish but the early-warning system (David: "metrics KEY").

MicroPython is the existence proof that this design survives years on 100 KB-class
MCUs — and its field history is the warning: it holds **when the big-buffer discipline
is followed** and fragments when it is not. Hence M1 is a rule, not a habit.

## The mitigation package (all six are part of the ratified decision)

- **M1 — Big-buffers-in-C rule** (in force now). Large or long-lived byte buffers —
  OTA staging, radio buffers, CBOR staging — live OUTSIDE the VM heap in C arenas,
  surfaced to the language as capability objects. This goes into the Milestone B and
  Milestone C specs as a stated rule.
- **M2 — Heap telemetry from the first allocation path** (landing now, as B pre-work,
  per David). `oevm_heap_stats()` exports: `heap_size`, `live_bytes`, `high_water`
  (monotonic), `largest_free_block` (computed on demand: max of free-bin blocks and
  the untouched bump region), `gc_cycles`. Largest-free-block is the only signal that
  catches fragmentation — leak-plateau tests (T-D.5) cannot. `%sys/heap` reporting
  rides Milestone D; the counters exist from now so the numbers are trustworthy by then.
- **M3 — The message algebra designs for the bins.** Milestone B should prefer
  bounded/small tuple arities so message values fall into exact-size bins the way
  boxed integers do. This is a stated design input to B planning.
- **M4 — Pin: no NEW heap-address-derived persistent state.** Today's address hashing
  is grandfathered (interned Symbols are blob-resident and never move regardless);
  nothing new may derive durable state from heap addresses. This keeps a future
  compactor retrofittable without re-litigating.
- **M5 — Revisit trigger, written down:** if the `largest_free_block` trend on real
  workloads decays below **25% of heap size sustained over a multi-day soak**, the
  fallback is **idle-time mark-compact at the reactor-idle safe point** — designed
  then, not now. Until that trigger fires, compaction proposals are out of scope.
- **M6 — C-boundary handle table** (adopt with the Zephyr seam work order). When C
  code (drivers, ISR→channel pulse, timer callbacks) must hold a reference to a VM
  object, it holds an index into a small VM-managed root table (dozens of entries,
  JNI-global-ref style), never a raw oop. This solves C-held-reference safety today
  and guarantees C never sees heap addresses — strengthening M4/M5's escape hatch.
  (This is the useful fragment of the object-table idea; see rejected alternatives.)

**Compatible future optimization (telemetry-driven, not now):** a header diet.
`HeapHeader` is 3 words (12 bytes on target; a boxed integer costs ~32 bytes all-in on
32-bit). The mark bit can move to a side bitmap and `next` is only needed while free.
Worth doing only if M2 numbers show header overhead matters.

## Rejected alternatives (recorded so they are not re-litigated)

- **Semi-space copying.** 2× space at a 100–150 KB budget: fatal. No further analysis
  needed.
- **Eager mark-compact (sliding).** Fits in one space, but: invasive pointer-fixup
  rewrite of a verified engine (constraint 3), breaks address-derived hashing
  (constraint 2), degrades differential-test determinism (constraint 4), and adds
  whole-heap pauses against the watchdog safe-point model — all to eagerly solve a
  problem M1–M3 largely design away. Retained only as M5's *idle-time* fallback behind
  the 25% trigger.
- **Per-job heaps (Erlang-style).** Jobs share the tuple space by design; there is no
  isolation boundary to exploit, and per-heap overheads don't fit the budget.
- **Classic object table (ST-80-style indirection)** — raised by David 2026-07-04,
  rejected with numbers. What it buys: compaction becomes trivial (move body, patch
  one slot) and identity survives moves — it would dissolve constraints 2 and 3. Why
  it loses here:
  1. *The table eats the budget it protects:* ~100 KB heap ÷ ~24–32 B average object
     ≈ 3–4 K live objects → 12–16 KB of table (≈10–15% of the whole heap), permanently,
     plus a hard max-object-count sizing decision and the table's own free management.
  2. *Every access pays forever:* one extra load per object touch in an interpreter
     that touches objects constantly — a tax on 100% of accesses to cheapen a rare
     event. (On a cache-less M4 the locality argument is weaker than on desktops, but
     the tax is still real; direct-pointer VMs won this argument historically.)
  3. *It fights XIP:* `oop == address` is load-bearing (SMI tagging, `is_in_blob`,
     blob ref-words resolving to flash addresses). Flash objects would need table
     entries too, or a hybrid direct/indirect scheme checked on every access.
     Retrofitting an OT is a bigger rewrite than the mark-compact it would enable.
  The useful 5% of the idea survives as **M6** (a handle table at the C boundary only).

## G4 rider — .oimg line-table provision (made explicit)

Milestone A verified the v8 header change forecloses nothing (additive-compatible).
The provision is now explicit in the format CONTRACT comment
(`nsl-tuvm/tools/oimg_serialize.py`): a future format bump may append a line-table
ref at the header tail and/or a `LINETABLE` cluster kind at the end of `KIND_ORDER`;
blob content unreachable from the resolved root graph is ignored by the loader, so
the addition is structurally additive. The compiler-side work (emitting line tables;
CompileError line numbers) is #139 and lands on its own schedule; the format
door is what this rider keeps open. The porta-side symbolizer-keyed-by-image-hash
alternative from G4 remains open as a cheaper v1.

## Test hooks

- `tuvm/tests/test_heap_stats.c` (landed with M2): property-based pins on
  `oevm_heap_stats()` — live_bytes falls after collecting garbage, high_water is
  monotonic, largest_free_block reflects both bins and bump region, counters reset
  with the arena.
- Future (Milestone D): `%sys/heap` record carries the M2 fields; T-D soak charts
  `largest_free_block` against the M5 25% line.
