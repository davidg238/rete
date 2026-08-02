# tuvm#20 — G2 overflow policy: the ring never kills the node (design)

**Status: approved by David 2026-07-14** (three policy forks decided below); implemented
same day. Replaces the `space_post_id` queue-full `abort()` — the last load-response
abort in the space seam — with the G2 policy. No separate plan doc (David: not needed;
single-seam fix, TDD'd directly).

## The rule

When the event ring is full: a **cell's** poke **coalesces**, a **stream's** post
**drops** — both **counted**, both observable. `abort()` remains only for structural
invariants (registry full, path too long, reactions full, kind mismatch), never as a
load response. A burst of events must be a contained, observable degradation — not a
node death that bypasses the crash boundary and supervisor entirely.

## Policy derives from kind — zero new registration API

G2 asked for "per-source overflow policy, chosen at registration". Rung 9 already
gave every name a declared kind, and the kind IS the policy choice:

- **CELL** (ring full at `put:`): its queued ring events are doorbell pokes — the
  value is already in the slot. Overwrite the cell's newest queued live event with
  the new value; if none is queued, drop the incoming poke. Either way the reader
  can always `peek:` the latest value and gets at least one poke: under overload,
  N puts degrade to one firing with the last value — exactly G2's coalesce /
  latest-value case. The `16_cells/04` fires-on-WRITE guarantee yields **only at
  overflow**, and the yield is counted.
- **STREAM** (ring full at post): events are distinct letters; merging them corrupts
  meaning (merged crash records!). **Drop newest + count**: the incoming event is
  dropped, already-queued events keep causal order — a reader sees history up to the
  overload point. (Drop-oldest was considered and rejected: ring surgery, and the
  crash that STARTED the storm would be the first record lost.)

Declarable per-source policy stays unbuilt — no customer; the kinds cover today's
sources. If one appears, the seam is the same and the declaration composes.

## Observability — never silent

- `struct SpaceChannel` gains `uint32_t overruns`; a static node `total_overruns`
  beside it. `SpaceEvent` is untouched, so tuvm#13's generation tags stay orthogonal.
- **Prim 108 `spaceOverruns:`** (appended after 107, nothing renumbered): path →
  per-channel count; `''` → node total. Kernel forwarder `platform spaceOverruns:`,
  same shape as `spaceDropReactionsOwnedBy:`.
- The **first** overrun on each channel prints one line — `channel overrun: <path>`
  — guarded by `overruns == 1`, so a console human sees the onset without being
  flooded. (Answering David's question in review: memory metrics exist — prim 104
  heapStats, rung 4 — but overflow counters did not exist anywhere before this fix.
  This is the pull side; the periodic `sys/metrics` record that pushes heap +
  overruns + queue high-water to porta is tuvm#24's scope, and these counters are
  shaped to slot into it. Queue high-water tracking also lands with #24.)

## Tests (`nsl-tests/17_overflow/`, written red-first)

1. `01_stream_flood.ns` — flood one stream past `OEVM_SPACE_QUEUE_DEPTH` (64)
   before draining: the node survives (old code aborts), a second channel's
   already-queued handler still fires, the drained stream saw the first-N events in
   order, and the overrun counter advanced by the exact overflow count.
2. `02_cell_coalesce.ns` — fill the ring with stream events, then N rapid `put:`s
   to a cell: exactly one coalesced poke fires with the last value, `peek:` answers
   the last value, counter advanced.
3. `03_overrun_counter.ns` — per-channel vs `''` total; a quiet channel reads 0;
   total equals the sum.
4. `tuvm/tests/test_space.c` — ring-full post at the C seam: no abort, counter
   incremented, ring intact (depth respected).

## Gates

Suite both paths (floor 190 → 193); `run_c_tests.sh`; portability-32; boot-gc-asan;
MCU footprint re-banked openly (expected: a few dozen bytes text + 132 B BSS — the
32×4 B counters + total), reason stated per tuvm#5 discipline.

## Out of scope, stated

- Declarable per-source policy (no customer).
- The `sys/overrun`/`sys/metrics` record and queue high-water telemetry → tuvm#24.
- The pulse-mailbox overflow (`pulse_post_isr` full → abort/k_oops) — a different
  seam (ISR-side, pre-reactor) deserving its own issue; this fix is the space ring.
