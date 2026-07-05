# nsl-tuvm — program sequence (the overarching order of work)

**Status:** drafted 2026-07-04 (mid-Milestone-B). This is the ONE document that sequences
the whole program. It is an *index with ordering*: every line points at the document that
owns the detail — decisions live in `nsl-tuvm/docs/superpowers/specs/`, execution state in
the SDD ledger, open items in GitHub issues on `davidg238/{tuvm,nsl-tuvm}`. Update this file
at milestone boundaries, not per task.

**North star** (unchanged from the 2026-06-25 node roadmap): a modern embedded scripting
**node** — object-capability security, OTA/in-image debug, resident VM under a nodus-style
keeper on the porta fleet plane — nRF52840-class (~256 KB) + OpenThread. What the 2026-07-03
pivot changed is the concurrency pillar: **no fibers, no NLR** — a reactive, run-to-completion
model (channels + reactor + supervisor) replaces them. The Zephyr gate remains **memory, not
speed** (old `tuvm/docs/ROADMAP.md` prime directive; still true).

---

## Phase 0 — heritage: what the pivot kept (done, pre-2026-07-03)

The pre-pivot ladder (`nsl-tuvm/docs/superpowers/plans/2026-06-25-ns-zephyr-capability-node-roadmap.md`)
ran R0–R7 on the fiber-era VM. Its **infrastructure rungs survive the pivot** (language-neutral
or VM-shape-independent); its **concurrency rungs are dead**:

| Rung | Status post-pivot |
|---|---|
| R0 `.oimg` Model-B loader (primary path) | KEPT — carried into tuvm, format now v8 |
| R1 ns-posix keeper + real porta end-to-end | KEPT as asset — re-validation needed (phase 3) |
| R2 telemetry / R3 porta-debug bridge | designs exist (posix-keeper R2/R3 specs) — re-validate in phase 3 |
| R4 GPIO capability blueprint | KEPT as design (`2026-06-26-gpio-capability-deferred-blueprint.md`) — phase 3 |
| R5 net/Thread capability (r6 design) | re-validate against no-fiber VM — phase 3 |
| R6 ns-zephyr port | superseded in detail, right idea — phase 2 |
| R7 fibers + de-recursify | **DEAD as a feature** — the flat interpreter it built is kept; fibers/NLR deleted by Milestone A |

Frozen oracles: `~/workspace/oevm`, `ns-oevm`, `ns-tests` (never touched); pre-fiber shape
reference = oevm `cee55d4` / ns-oevm `a327d60` (`review/2026-07-04-pre-fiber-baseline.md`).

## Phase 1 — the first step: prove the model on the host (Milestones A–D)

Owner doc: `nsl-tuvm/docs/superpowers/specs/2026-07-03-nsl-tuvm-first-step-design.md` §4
(+ `review/2026-07-03-first-step-design-addendum.md` A1–A3). Bar: end-to-end on the desktop
with the balloon gone.

- **A — Seed + delete. DONE 2026-07-04.** Merged to master all 3 repos; 88/0/12 both paths;
  text −10.02% banked; deletion machine-proven (07_no_nlr + diff oracle). `tuvm/docs/MILESTONE_A.md`.
- **B — Immutable message algebra. IN FLIGHT.** Spec+plan 2026-07-04 (12 tasks; Tasks 1–4 done,
  reviewed, gates green as of this draft). Standing decisions absorbed: L7 signed-64-bit integers,
  `->` as compile-time rewrite (no builtin→kernel-Object delegation exists — Task 2 finding).
- **C — Capability-scoped channels + reactor.** Not specced yet. MUST absorb the demo-node
  feed-forwards: **capabilities-as-constructor-args** (not messages) needs a spec line; the
  degenerate (exact-match) tuple space only.
- **D — Supervisor + let-it-crash + host job.** Not specced yet. MUST-DO BEFORE SPEC: the
  **supervisor/job restart contract** (demo-node finding); crash boundary = "unwind to marked
  frame" per `review/lib/exceptions.md` constraint 4, compatible with the future on:do: model.

Library landing order rides these milestones — `review/lib/README.md` table is authoritative
(combinators ✓ post-A; messages = B; bytes after B; fsm = C; timers = D).

## Phase 2 — make it real: exceptions + Zephyr (the "second step")

Order chosen so each rung de-risks the next; entry criterion for the phase = Milestone D green.

1. **Full `on:do:`/`ensure:`** (first-step §8 deferral). Owner: `review/lib/exceptions.md`
   (the 5 pinned constraints; deferring was safe *because* of them). Must compose with D's
   setjmp crash boundary. Retires the td-crash/04 'policy TBD' placeholder.
2. **Zephyr bring-up** of the backend seam (`backend/<target>/`). Precursors that can land
   during phase 1: FOOTPRINT_CONFIG=mcu true-target RAM line (tuvm#5 — ideally before B Task 12
   banks RAM numbers); OEVM_BUILD=asan|ubsan harness knob. First work order: the **ISR→channel
   pulse primitive** (demo-node finding — the Zephyr seam's shape). On-device memory work rides
   the G3 decision (`2026-07-04-G3-gc-strategy-decision.md`: non-moving mark-sweep + M1–M6
   mitigations); soft watchdog via the debugger safe-point hook (kept in A for this reason).
3. **Wire seam: CBOR + Int64.** Owners: `review/lib/cbor.md` (CBOR over custom wire format,
   tags 27/39) and `review/lib/integers.md` (L7: boxed Int64 on 32-bit — decided, built with B).
   G4 says the `.oimg` line-table provision must not be foreclosed meanwhile (it wasn't — v8 audit).

## Phase 3 — rejoin the node ladder (keeper, fleet, capabilities)

Entry: phase 2 rungs 1–2 green on hardware. These re-validate proven pre-pivot assets against
the no-fiber reactive VM rather than building from scratch:

1. **ns-posix keeper + porta rejoin** (R1/R2 assets; keeper core was VM-neutral by design).
2. **Remote debug over porta** (R3 design; debugger survived Milestone A untouched).
3. **Capability breadth**: GPIO (R4 blueprint) → i2c → **OpenThread/UDP** (r6 design re-validated;
   UDP prims exist in tuvm today, fiber-free entry points need review). Capabilities are granted
   as **constructor args** per the Milestone-C rule.

## Phase 4 — generalize (only when real workloads justify)

First-step §6 guarantees these are additive on the same VM shape: wildcard/associative matching,
persistent state-tuples (TTL/lifecycle), large-buffer ownership-transfer (zero-copy), multiple
event sources, power/deep-sleep. No owner docs yet — write them when a workload forces the issue.

---

## Cross-cutting ledgers (constraints, not phases)

- **Reliability gaps** `review/reliability-gaps.md` (G1–G11): G3 RESOLVED (GC decision doc);
  G4 protected through B (v8 audit clean); the rest are watch-items assigned above where scheduled.
- **Metrics** `review/metrics-spec.md`: every phase banks its footprint/latency lines; the RAM
  line becomes target-config (mcu) at phase 2 rung 2 (tuvm#5).
- **Test suites** `review/test-suites-spec.md` + `review/tests/CLAIMS.md`: T-A landed (07_no_nlr);
  T-B lands with Milestone B Task 11; suites C–D with their milestones.
- **Target user program**: `review/demo-node.ns` — readme-driven; every phase should shrink the
  gap between it and what actually runs.

## Known stale docs (do not treat as current)

`tuvm/docs/ROADMAP.md` (oevm-era backlog; prime directive still valid, task list superseded),
`fiber_*.md`, `2026-06-29-r7-fibers-design.md` and the derecursify plans (historical record of
the dead pillar), `topics.md`/`optimizations.md` (pre-pivot snapshots). `ECOSYSTEM.md` and
`deployment-model.md` remain valid for the fleet/keeper layers until phase 3 revisits them.
