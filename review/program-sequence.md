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
- **B — Immutable message algebra. DONE.** Merged to master all 3 repos (12 tasks landed,
  reviewed, gates green). Standing decisions absorbed: L7 signed-64-bit integers,
  `->` as compile-time rewrite (no builtin→kernel-Object delegation exists — Task 2 finding).
  Close-out synced into `CLAIMS.md`/`review/tests/README.md`.
- **C — Capability-scoped channels + reactor. DONE 2026-07-06.** Merged to master all 3 repos
  (`nsl-tuvm f4ef42c` / `tuvm 6930afc` / `nsl-tests 7e94bfa`). Landed the demo-node
  feed-forwards: capabilities-as-constructor-args, the degenerate (exact-match) tuple space,
  FIFO run-to-completion `Space>>drain`. Close-out synced into `CLAIMS.md`/`review/tests/README.md`.
- **D — Supervisor + let-it-crash + host job. DONE 2026-07-07.** All 10 tasks landed on
  `milestone-d` (crash boundary `vm_protected_fire` unwinding to the one marked reactor
  frame, per `review/lib/exceptions.md` constraint 4; `Supervisor`/`SupervisedJob`
  restart+quarantine; owner-tag identity sweep; host virtual-clock timers; the soak/leak
  test D.5; the demo-node-shaped integration test D.7; carry-forwards nsl-tuvm#5 and
  #9.1/#9.2/#9.3 all closed). `tuvm/docs/MILESTONE_D.md`.

**Phase 1 (host proof) is now COMPLETE — Milestones A through D all landed and green.**
Phase 2 (below) is entered next.

Library landing order rides these milestones — `review/lib/README.md` table is authoritative
(combinators ✓ post-A; messages = B; bytes after B; fsm = C; timers = D).

## Phase 2 — make it real: exceptions + Zephyr (the "second step")

Order chosen so each rung de-risks the next; entry criterion for the phase = Milestone D
green — **met 2026-07-07** (see phase-1 entry above); phase 2 starts now.

1. **Full `on:do:`/`ensure:`** (first-step §8 deferral). Owner: `review/lib/exceptions.md`
   (the 5 pinned constraints; deferring was safe *because* of them). Must compose with D's
   setjmp crash boundary. Retires the td-crash/04 'policy TBD' placeholder.
   **DONE 2026-07-07 (Milestone E).** `vm_on_do`/`vm_ensure` built beside D's `vm_protected_fire`
   (prims 90-93: `raise`/`error:tag:`/`on:do:`/`ensure:`); Record-valued raise; `^`-in-handler
   rejected; C5 no-cross-dispatch pinned; `nsl-tests/12_exceptions/01..14`. Whole-milestone gate
   green both paths; whole-branch review Ready (F1 interned-`#error` fix landed). `tuvm/docs/MILESTONE_E.md`.
2. **Zephyr bring-up** of the backend seam (`backend/<target>/`). Precursors that can land
   during phase 1: FOOTPRINT_CONFIG=mcu true-target RAM line (tuvm#5 — ideally before B Task 12
   banks RAM numbers); OEVM_BUILD=asan|ubsan harness knob. First work order: the **ISR→channel
   pulse primitive** (demo-node finding — the Zephyr seam's shape). On-device memory work rides
   the G3 decision (`2026-07-04-G3-gc-strategy-decision.md`: non-moving mark-sweep + M1–M6
   mitigations); soft watchdog via the debugger safe-point hook (kept in A for this reason).
3. **Wire seam: CBOR + Int64. DONE 2026-07-10.** Owners: `review/lib/cbor.md` (CBOR over custom
   wire format, tags 27/39) and `review/lib/integers.md` (L7: boxed Int64 on 32-bit — decided,
   built with B). G4 says the `.oimg` line-table provision must not be foreclosed meanwhile (it
   wasn't — v8 audit).

   Rung-close: landed as an nsl `Cbor` class (encode/decode tree-walk) in kernel.ns, backed by
   7 C leaf prims (ids 97-103, append-only after `pumpEvents`=96) that do the byte-level,
   non-recursive work — architecture (B), per the frozen wire-seam spec. Encoding is
   deterministic per RFC 8949 §4.2: shortest-form ints/floats, definite lengths only, map keys
   sorted bytewise over the encoded key bytes; decode is total and closed via Milestone-E
   `on:do:`/`ifInvalid:` — every byte sequence yields exactly one message or exactly one of
   `not a value` / `message depth exceeded` / `invalid utf-8` / `integer out of range`. The
   integer contract is the ratified L7 range **±(2^64−1)** (both SMI and LargeInteger, shortest
   form); only `−2^64` and the CBOR bignum tags (2/3) overflow it, both routing to
   `integer out of range`. Both-path (Path A solidified / Path B `.oimg`) verified A==B; tests
   in `nsl-tests/14_cbor/` (01_roundtrip, 02_golden_hex, 03_map_order, 04_reject, 05_int_band,
   06_float_roundtrip). Co-landed a `.oimg` large-SMI capacity fix (Task 1) needed to represent
   the widened integer range.

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
- **Test suites** `review/test-suites-spec.md` + `review/tests/CLAIMS.md`: T-A, T-B, T-C, T-D
  all landed with their milestones (`nsl-tests/07_no_nlr`, `08_messages`, `09_channels`,
  `11_supervision` + `10_fsm/02`); `CLAIMS.md` is current as of Milestone D's close.
- **Target user program**: `review/demo-node.ns` — readme-driven; every phase should shrink the
  gap between it and what actually runs.

## Known stale docs (do not treat as current)

`tuvm/docs/ROADMAP.md` (oevm-era backlog; prime directive still valid, task list superseded),
`fiber_*.md`, `2026-06-29-r7-fibers-design.md` and the derecursify plans (historical record of
the dead pillar), `topics.md`/`optimizations.md` (pre-pivot snapshots). `ECOSYSTEM.md` and
`deployment-model.md` remain valid for the fleet/keeper layers until phase 3 revisits them.
