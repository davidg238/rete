# DOCMAP — where every document of the program lives

One place to look. Since the rete consolidation (#79, 2026-08-02) the program spans **two
live trees across two workspace roots**: `~/workspace/rete` — which absorbed `tuvm`,
`nsl-tuvm`, `nsl-tests` and `porta` — and `~/workspaceToit/stic`, the cross-VM benchmark
harness, which stays outside on purpose (design D12). A third, `~/workspaceToit/nodus`, is
the frozen Toit v1 implementation (GitHub: `nodus-v1`), kept for reference rather than
worked on.

This file has two layers: a short hand-curated list of the **documents of record** (below —
edit by hand, keep it short), and a **generated inventory** of everything (bottom — never
edit by hand, rerun `tools/docmap.sh`). In-repo entries in the inventory are repo-relative;
the repo's own absolute path has moved twice and will move again.

For content search across the program:
`rg -i "<term>" ~/workspace/rete ~/workspaceToit/stic`
(add `~/workspaceToit/nodus` to reach the frozen Toit v1.)

> **The inventory's date is the last commit that touched that path**, so every document the
> consolidation moved reads `2026-08-02` regardless of when it was last actually written.
> `git log --follow` does not recover the older date across the subtree imports. Treat a
> `2026-08-02` date as "unknown", not "fresh".

## Documents of record (curated)

**Start here** — `~/workspace/rete/`:

- `README.md` — what rete is, the tree map, and the three gates.
- `CLAUDE.md` — the standing rules, the docs convention, and porta's decision record.

**The roadmap and its ledgers** — `~/workspace/rete/review/`:

- `program-sequence.md` — **THE ladder.** The one document that sequences the whole
  program; read it before claiming direction.
- `tests/CLAIMS.md` — what is proven, by exactly which test.
- `WHY.md` / `WHY-OEOM.md` — rationale; includes the distribution law (Waldo).
- `WHY-AGENTS.md` — the agent-first argument: the operability loop as design test,
  never-silent as design law, what gates autonomous agents, falsifiable criteria.
- `reliability-gaps.md` — G1–G11 cross-cutting ledger.
- `metrics-spec.md` / `test-suites-spec.md` — footprint/latency lines; suite structure.
- `language-and-library-spec.md` — **a PROPOSAL, not ratified truth.**
- `zephyr-driver-exposure.md` — driver exposure tiers + the BME280→Grid-EYE→TFT ladder.
- `demo-node.ns` — the target user program; every phase should shrink the gap to it.
- `slides.html` — the flight-target artifact (≈2026-09); tracks milestone truth.
- `lib/*.md` — per-library owner docs (cbor, integers, exceptions, …).

**Dialect and VM** — `~/workspace/rete/nodus/nsl/docs/` and `~/workspace/rete/docs/`:

- `nodus/nsl/docs/NEWSPEAK_DIALECT.md` / `programming_with_nsl.md` / `compiler.md`.
- `nodus/oevm/docs/MILESTONE_{A,D,E}.md` — milestone close-out records.
- `nodus/tuvm/zephyr/docs/BOARD_PORTS.md` — per-board build/DFU recipes (DK + dongle knobs).

**The wire and the fleet** — `~/workspace/rete/protocol/`, `~/workspace/rete/porta/docs/`
and `~/workspaceToit/nodus/docs/`:

- `protocol/v1/PROTOCOL.md` — porta protocol v1, frozen for Toit nodes.
- `protocol/PROTOCOL-NSL.md` + `PROTOCOL-NSL-RECORDS.md` — the nsl space-sync wire;
  records schemas must be pinned before the bridge rung implements.
- `porta/docs/ARCHITECTURE.md` — the gateway's architecture; **stale in parts** (written
  for v1's command protocol and TFTP transport).
- `~/workspaceToit/nodus/docs/ARCHITECTURE.md` / `NODE-LIFECYCLE.md` — the frozen Toit node
  implementation.

**The benchmark harness** — `~/workspaceToit/stic/`, outside the repo:

- `README.md` — the published ESP32 cross-VM benchmark table (nsl / Toit / MicroPython).

## Conventions

- **Design specs are decision records** → `docs/design/` (and `porta/docs/design/`; moved
  from `docs/superpowers/specs/`, 2026-07-14). **Plans** (execution scaffolding) stay in
  `docs/superpowers/plans/`.
- **Promotion rule:** an issue/rung/milestone is not closed until its durable knowledge
  is promoted into a document of record listed above; the dated design doc stays as the
  historical "why". Every plan's close-out task names which mainline doc absorbed it.
- Cite documents by **absolute path** (`~/workspace/rete/review/…`), never bare `review/`.

<!-- BEGIN GENERATED INVENTORY — do not edit below; rerun tools/docmap.sh -->

_Regenerated 2026-10-02 by `tools/docmap.sh`. Legend: **record** = mainline doc of record · **design** = dated decision record (`docs/design/`) · **plan** = execution scaffolding (`docs/superpowers/plans/`) · **note** = superpowers notes/findings._

### review

- `review/2026-07-03-first-step-design-addendum.md` — nsl-tuvm first-step design — addendum 1 (review amendments) _(record, 2026-08-02)_
- `review/2026-07-04-context-decision.md` — Task 8 design question: keep the deferred-lift heap Context? _(record, 2026-07-04)_
- `review/2026-07-04-pre-fiber-baseline.md` — Pre-fiber baseline commits in oevm / ns-oevm _(record, 2026-08-02)_
- `review/2026-07-21-issue-65-http-load-wedge/README.md` — tuvm#65 — evidence folder (2026-07-21) _(record, 2026-08-02)_
- `review/DOCMAP.md` — DOCMAP — where every document of the program lives _(record, 2026-10-02)_
- `review/ISSUE-dongle-usb-openthread.md` — DRAFT ISSUE (post to davidg238/tuvm — created from a session without gh permission) _(record, 2026-07-12)_
- `review/WHY-AGENTS.md` — Why agents — nodus + porta as an agent-first platform _(record, 2026-08-29)_
- `review/WHY-OEOM.md` — Why OEOM — the kernel that survived the graveyard _(record, 2026-07-12)_
- `review/WHY.md` — Why nsl-tuvm _(record, 2026-08-02)_
- `review/cells-and-streams-v3.html` — Letters and pigeonholes — v3 _(record, 2026-08-07)_
- `review/language-and-library-spec.md` — nsl-tuvm — language sufficiency & library roadmap (proposed) _(record, 2026-08-02)_
- `review/lib/README.md` — review/lib — designed library surface (pre-implementation staging) _(record, 2026-08-02)_
- `review/lib/cbor.md` — L6 — CBOR wire codec (the TISON-analog decision; design frozen, now implemented) _(record, 2026-08-02)_
- `review/lib/errors.md` — L5 — Error convention (adopt before the stdlib grows) _(record, 2026-08-29)_
- `review/lib/exceptions.md` — Exceptions (`on:do:` / `ensure:`) — why NOT detailing it now is safe, and the _(record, 2026-07-07)_
- `review/lib/integers.md` — L7 — Integer semantics across 32/64-bit platforms (VM design) _(record, 2026-08-02)_
- `review/lib/messages.md` — Milestone B — nsl-facing message algebra API (proposed surface) _(record, 2026-07-03)_
- `review/metrics-spec.md` — Node health metrics — the long-term profile (G4/G9 companion) _(record, 2026-07-21)_
- `review/model-capability-findings.md` — Model-capability findings — coordination-model spike series _(record, 2026-08-02)_
- `review/performance_slides.html` — nsl-tuvm — optimize the right thing _(record, 2026-07-22)_
- `review/program-sequence.md` — nsl-tuvm — program sequence (the overarching order of work) _(record, 2026-10-01)_
- `review/reliability-gaps.md` — Reliability gaps — the questions not yet asked (2026-07-03) _(record, 2026-08-02)_
- `review/retained-channels-v2.html` — Names, and what answers to them — v2 _(record, 2026-07-13)_
- `review/retained-channels.html` — Retained channels, and where remoteness lives _(record, 2026-08-02)_
- `review/revisit.md` — REVISIT — things learned in passing that are not yet anyone's work item _(record, 2026-08-02)_
- `review/slides.html` — nsl-tuvm — script Zephyr, don't replace it _(record, 2026-07-12)_
- `review/test-suites-spec.md` — nsl-tests — verification suites for the first step (proposed) _(record, 2026-08-02)_
- `review/tests/CLAIMS.md` — CLAIMS — first-step spec §7 success criteria → proving tests _(record, 2026-09-19)_
- `review/tests/README.md` — review/tests — staged verification suites (T0, T-A…T-D) _(record, 2026-08-29)_
- `review/tests/t0-gates/sanitizers.md` — T0.1 — sanitizer gate (the highest-value single item in the whole test plan) _(record, 2026-08-29)_
- `review/zephyr-driver-exposure.md` — Exposing Zephyr drivers to nsl — a sketch _(record, 2026-07-23)_

### docs

- `docs/README.md` — docs/ — the program's documents _(record, 2026-08-31)_
- `docs/design/2026-06-17-clustered-image-xip-design.md` — Design: Decoupled image + execute-in-place clustered format _(design, 2026-08-29)_
- `docs/design/2026-06-17-global-method-cache-design.md` — Global Method Lookup Cache — Design _(design, 2026-07-14)_
- `docs/design/2026-06-17-unboxed-floats-design.md` — Unboxed Floats (P0) — Design _(design, 2026-08-29)_
- `docs/design/2026-06-18-gc-embedded-memory-design.md` — GC & Embedded Memory Management — Design Scope (GH #3) _(design, 2026-07-14)_
- `docs/design/2026-06-18-nested-class-access-design.md` — Explicit Nested-Class Access + Outer-Sends — Design _(design, 2026-07-14)_
- `docs/design/2026-06-18-w1-gc-trigger-rooting-design.md` — W1 — GC Trigger + Safe-Points / Rooting — Design _(design, 2026-07-14)_
- `docs/design/2026-06-18-w4-fixed-arena-allocator-design.md` — W4 — Fixed-Arena Allocator Foundation — Design _(design, 2026-07-14)_
- `docs/design/2026-06-19-gh4-iterative-mark-design.md` — GH#4 — Iterative Mark (worklist) Design _(design, 2026-08-29)_
- `docs/design/2026-06-19-w2a-float-arena-reclaim-design.md` — W2a — Float-Arena Reclaim (design) _(design, 2026-07-14)_
- `docs/design/2026-06-19-w2a1-float-arena-trigger-design.md` — W2a.1 — Float-Arena GC Trigger (design) _(design, 2026-07-14)_
- `docs/design/2026-07-01-derecursify-deferred-lift-growable-stacks-upward-closures-design.md` — De-recursify deferred lift — growable stacks + upward closures (design) _(design, 2026-08-29)_
- `docs/design/2026-07-02-derecursify-phase2-self-migration-design.md` — De-recursify Phase 2 (redesigned) — `self`-migration + relativize `home_ctx` (design) _(design, 2026-07-14)_
- `docs/design/2026-07-02-derecursify-phase3-growable-stacks-design.md` — De-recursify deferred lift — Phase 3: growable stacks + overflow policy (design) _(design, 2026-07-14)_
- `docs/design/2026-07-03-derecursify-e1-retire-dead-access-walk-design.md` — De-recursify reify-on-escape — E1: retire the dead `home_ctx` walk in `get_top_level_method_closure` (design) _(design, 2026-07-14)_
- `docs/design/2026-07-16-performance-nsl-stic-column-design.md` — performance.nsl — an nsl column for the STIC ESP32 benchmark _(design, 2026-07-16)_
- `docs/design/2026-07-16-zephyr-model-ab-build-knob-design.md` — Zephyr Model A/B build knob + staged .oimg bring-up (design) _(design, 2026-07-16)_
- `docs/design/2026-07-17-collection-performance-assessment.md` — Collection performance: assessment and posture — decision record _(design, 2026-07-17)_
- `docs/design/2026-07-17-fastpath-opcodes-design.md` — Fast-path opcodes: OP_LT/LE/GT/GE + OP_AT/OP_AT_PUT — decision record _(design, 2026-07-17)_
- `docs/design/2026-07-17-ns-to-nsl-extension-migration-design.md` — `.ns` → `.nsl` source-extension migration — design _(design, 2026-07-18)_
- `docs/design/2026-07-17-oeom-native-substitution-hanoi-spike-design.md` — OEOM native-substitution spike (Hanoi) — design _(design, 2026-07-17)_
- `docs/design/2026-07-17-path-b-device-default-design.md` — Path B (.oimg / image-VM) as the device default _(design, 2026-07-17)_
- `docs/design/2026-07-18-coordination-model-spike-series-design.md` — Coordination-model spike series — does the reactor/channel model hold up post-fibers? _(design, 2026-07-18)_
- `docs/design/2026-07-18-esp32-wifi-http-last-mile-design.md` — ESP32/WiFi HTTP last mile (#46) — design _(design, 2026-07-18)_
- `docs/design/2026-07-18-streaming-render-chunked-egress-design.md` — Streaming render — chunked HTTP egress from nsl _(design, 2026-07-19)_
- `docs/design/2026-07-18-webserver-channel-device-spike-design.md` — Spike #1 — the webserver as a channel-native driver _(design, 2026-07-18)_
- `docs/design/2026-07-19-amg8833-thermal-sensor-rung-design.md` — AMG8833 Grid-EYE thermal sensor rung — design _(design, 2026-07-20)_
- `docs/design/2026-07-20-esp32c6-second-target-design.md` — ESP32-C6 as a Second Zephyr Target — Design _(design, 2026-09-19)_
- `docs/design/2026-07-20-esp32c6-thread-ping-rung-design.md` — ESP32-C6 end-to-end Thread ping — Rung Design _(design, 2026-07-20)_
- `docs/design/2026-07-21-job-latency-instrumentation-design.md` — End-to-end job latency instrumentation — Design _(design, 2026-08-06)_
- `docs/design/2026-07-21-performance-deck-design.md` — Performance deck — design _(design, 2026-07-21)_
- `docs/design/2026-07-22-diag-span-capture-design.md` — Bounded span-capture sessions on a `diag/` channel family — Design _(design, 2026-08-02)_
- `docs/design/2026-07-22-node-profiler-slice-design.md` — Node profiler — the porta-lite slice _(design, 2026-08-02)_
- `docs/design/2026-07-22-revenue-and-positioning-strategy.md` — Revenue and positioning — strategy _(design, 2026-07-22)_
- `docs/design/2026-07-23-gpio-output-level-seam-design.md` — The GPIO output + level-read seam _(design, 2026-07-23)_
- `docs/design/2026-07-23-magtag-bell-clock-spec.md` — MagTag bell clock — implementation spec _(design, 2026-07-23)_
- `docs/design/2026-07-23-magtag-buttons-leds-design.md` — MagTag buttons + LEDs, and the sleep-wake seam behind them _(design, 2026-07-23)_
- `docs/design/2026-07-23-magtag-dac-bell-design.md` — MagTag DAC bell — design _(design, 2026-07-23)_
- `docs/design/2026-07-23-magtag-epaper-display-seam-design.md` — The e-paper display seam — nsl paints the MagTag _(design, 2026-07-23)_
- `docs/design/2026-07-24-magtag-deep-sleep-clock-design.md` — MagTag deep-sleep ship's-bell clock — design (tuvm#94 stage 5) _(design, 2026-07-26)_
- `docs/design/2026-07-24-magtag-ntp-catchup-design.md` — MagTag bell clock — stage 4: WiFi + NTP catchup _(design, 2026-07-23)_
- `docs/design/2026-08-01-porta-space-sync-c6-gate-design.md` — porta space-sync — the C6 hardware gate (plan 3 of 3) — Design _(design, 2026-08-02)_
- `docs/design/2026-08-02-rete-repo-consolidation-design.md` — rete — collapsing five repos into one system _(design, 2026-08-02)_
- `docs/design/2026-08-03-c6-shipsbell-always-on-node-design.md` — The C6 ship's bell as an always-on node — design _(design, 2026-08-04)_
- `docs/design/2026-08-03-device-console-collector-design.md` — Device console collector and offline symbolizer — design _(design, 2026-08-05)_
- `docs/design/2026-08-03-t0.3-gate-ownership-decision.md` — T0.3 gate ownership: one script measures, one script decides _(design, 2026-08-03)_
- `docs/design/2026-08-04-metrics-over-porta-design.md` — Node metrics over porta — the run-up, not the dying breath _(design, 2026-08-09)_
- `docs/design/2026-08-05-target-test-harness-design.md` — A diagnostic is a workload, not a test framework — the target harness _(design, 2026-08-22)_
- `docs/design/2026-08-05-timer-token-by-value-design.md` — The timer token rides by value, and the handle goes _(design, 2026-08-05)_
- `docs/design/2026-08-06-method-profile-over-porta-design.md` — Per-method profiling, surfaced over porta _(design, 2026-08-07)_
- `docs/design/2026-08-07-build-asset-archive-design.md` — Build-asset archive — keyed by build id, retained by intent, eventually at porta _(design, 2026-08-24)_
- `docs/design/2026-08-07-code-push-host-slice-design.md` — Code-push, the host-only vertical slice (#25) — design _(design, 2026-08-08)_
- `docs/design/2026-08-07-inclusive-attribution-design.md` — Attributing profile cost to code, not to callees _(design, 2026-08-07)_
- `docs/design/2026-08-10-node-memory-and-liveness-design.md` — Node memory accounting and fleet liveness — design _(design, 2026-08-22)_
- `docs/design/2026-08-12-allocation-attribution-design.md` — Allocation attribution — the profiler's question, asked about bytes _(design, 2026-08-12)_
- `docs/design/2026-08-16-vm-liveness-observer-design.md` — A wedged node never faults — detecting the silence that raises no error _(design, 2026-08-21)_
- `docs/design/2026-08-20-prim-id-on-symbol-design.md` — Primitive classification belongs to the Symbol, not to a cache _(design, 2026-08-21)_
- `docs/design/2026-08-21-liveness-contract-design.md` — The liveness contract — a node declares how long its silence may last _(design, 2026-09-17)_
- `docs/design/2026-08-24-kind-schema-manifest-design.md` — The kind+schema manifest — design (#243, 2026-08-24) _(design, 2026-08-25)_
- `docs/design/2026-08-31-liveness-timers.html` — Every Clock in the Liveness Path _(design, 2026-09-17)_
- `docs/design/2026-09-11-node-identity-hardware-rooted-design.md` — Node identity is the factory link address _(design, 2026-10-02)_
- `docs/design/2026-09-14-liveness-two-cadences-design.md` — Two liveness cadences, both declared — the #273 decision _(design, 2026-09-18)_
- `docs/design/2026-09-14-porta-federation-and-transports-design.md` — porta beyond the LAN: federation, and the transports a public deployment would use _(design, 2026-09-16)_
- `docs/design/2026-10-02-frozen-1.0-surface.md` — The frozen surface 1.0 — what is promised, what is missing, what comes after _(design, 2026-10-02)_
- `docs/design/2026-10-02-security-levels-design.md` — Security levels: who may talk to whom, per node _(design)_
- `docs/development.md` — Development: the loop for a human and for an agent _(record)_
- `docs/next-steps-overview.md` — Next steps — overview _(record, 2026-10-01)_
- `docs/overview.md` — rete — the whole on one page _(record)_
- `docs/results/2026-07-20-c6-heap-ceiling.md` — ESP32-C6 heap-ceiling bisect — Task 3 _(record, 2026-07-20)_
- `docs/results/2026-08-05-c6-checkin-decomposition.md` — Where the 300 ms goes — c6-bench, 2026-08-05 _(record, 2026-08-05)_
- `docs/results/2026-08-05-c6-span-capture-first.md` — First span capture off a real node — c6-bench, 2026-08-05 _(record, 2026-08-05)_
- `docs/results/2026-08-06-method-call-counter-spike.md` — Exact per-method call counts — spike result (#164) _(record, 2026-08-06)_
- `docs/results/2026-08-06-mprof-capacity-under-bridge.md` — Table capacity under a real bridged node (#165 Task 1, design D9) _(record, 2026-08-07)_
- `docs/results/2026-08-06-mprof-window-hardware.md` — A field node profiles on request, and was silent before it was asked _(record, 2026-08-07)_
- `docs/results/2026-08-06-pushbuild-three-way.md` — `pushBuild` is three spans — and it is one thing done twice, not three things _(record, 2026-08-06)_
- `docs/results/2026-08-09-cbor-encode-shape.md` — `Cbor>>encode:` — what shape is the cost? (#163) _(record, 2026-08-09)_
- `docs/results/2026-08-09-checkin-after-the-codec-fix.md` — The check-in after the codec fix — `pushBuild` is still the top span, and it is no longer the story _(record, 2026-08-09)_
- `docs/results/2026-08-11-host-arena-floor.md` — Host arena floor — the smallest `OEVM_HEAP_SIZE` that boots, and why 131072 did not _(record, 2026-08-11)_
- `docs/results/2026-08-12-allocation-attribution-c6.md` — Allocation attribution on the C6 — the boot window, the steady-state window, and an incident _(record, 2026-08-12)_
- `docs/results/2026-08-12-allocation-attribution-host.md` — Allocation attribution — the host measurement _(record, 2026-08-12)_
- `docs/results/2026-08-12-profile-window-cost-c6.md` — What an open attribution window costs on a C6 — +1.9% of local CPU, +0.13% of elapsed _(record, 2026-08-12)_
- `docs/results/2026-08-29-thread-echo-soak.md` — A 48-day Thread echo soak nobody was reading — 720,878 round trips, and one unattributable day _(record, 2026-08-30)_
- `docs/results/2026-10-01-rp2350-first-numbers.md` — RP2350 first numbers — #302 Task 5 _(record, 2026-10-01)_
- `docs/spikes/2026-07-17-oeom-native-hanoi/README.md` — Spike archive: OEOM native-substitution (Hanoi AOT ceiling) — 2026-07-17 _(record, 2026-07-17)_
- `docs/spikes/2026-08-11-rete199-refused-node/README.md` — rete#199 — a node whose frames the gateway refuses, reproduced off hardware _(record, 2026-08-11)_
- `docs/spikes/2026-08-27-liveness-model/README.md` — The liveness contract, model-checked (2026-08-27) _(record, 2026-08-28)_
- `docs/status/2026-08-12-where-we-are.html` — rete &mdash; where we are _(record, 2026-08-12)_
- `docs/status/README.md` — docs/status — the orientation deck _(record, 2026-08-13)_
- `docs/status/dev-status.html` — rete dev status _(record, 2026-10-02)_
- `docs/status/focus-history.md` — `## Current focus` — history _(record, 2026-09-30)_
- `docs/superpowers/notes/2026-06-18-w1-rooting-audit.md` — W1 Rooting Audit — Runtime `gc_allocate` / `float_alloc` Sites _(note, 2026-07-03)_
- `docs/superpowers/plans/2026-06-17-decoupled-xip-image.md` — Decoupled Image + Execute-In-Place Format — Implementation Plan _(plan, 2026-08-29)_
- `docs/superpowers/plans/2026-06-17-global-method-cache.md` — Global Method Lookup Cache Implementation Plan _(plan, 2026-07-03)_
- `docs/superpowers/plans/2026-06-18-w1-gc-trigger-rooting.md` — W1 — GC Trigger + Safe-Points / Rooting Implementation Plan _(plan, 2026-08-29)_
- `docs/superpowers/plans/2026-06-18-w4-fixed-arena-allocator.md` — W4 Fixed-Arena Allocator Implementation Plan _(plan, 2026-08-29)_
- `docs/superpowers/plans/2026-06-19-gh4-iterative-mark.md` — GH#4 — Iterative Mark Implementation Plan _(plan, 2026-08-29)_
- `docs/superpowers/plans/2026-06-19-w2a-float-arena-reclaim.md` — W2a — Float-Arena Reclaim Implementation Plan _(plan, 2026-08-29)_
- `docs/superpowers/plans/2026-06-19-w2a1-float-arena-trigger.md` — W2a.1 — Float-Arena GC Trigger Implementation Plan _(plan, 2026-08-29)_
- `docs/superpowers/plans/2026-07-16-perf-trials-pushframe-opat.md` — Perf trials: push_frame + OP_AT/OP_AT_PUT — working journal _(plan, 2026-07-17)_
- `docs/superpowers/plans/2026-07-16-zephyr-model-ab-build-knob.md` — Zephyr Model A/B Build Knob Implementation Plan _(plan, 2026-07-16)_
- `docs/superpowers/plans/2026-07-17-hanoi-ab-results.md` — Hanoi A/B — ESP32 results (native PrimitiveClosure vs bytecode interpreter) _(plan, 2026-07-17)_
- `docs/superpowers/plans/2026-07-17-ns-to-nsl-extension-migration.md` — `.ns` → `.nsl` Source-Extension Migration — Implementation Plan _(plan, 2026-07-18)_
- `docs/superpowers/plans/2026-07-17-oeom-native-hanoi-spike.md` — OEOM Native-Substitution Hanoi Spike — Implementation Plan _(plan, 2026-07-17)_
- `docs/superpowers/plans/2026-07-18-esp32-wifi-http-last-mile.md` — ESP32/WiFi HTTP Last Mile (#46) Implementation Plan _(plan, 2026-07-18)_
- `docs/superpowers/plans/2026-07-18-streaming-render.md` — Streaming Render — Chunked HTTP Egress Implementation Plan _(plan, 2026-07-18)_
- `docs/superpowers/plans/2026-07-18-webserver-channel-device-spike.md` — Webserver Channel-Device Spike (#1) Implementation Plan _(plan, 2026-07-18)_
- `docs/superpowers/plans/2026-07-19-amg8833-thermal-sensor-rung.md` — AMG8833 Grid-EYE Thermal Sensor Rung — Implementation Plan _(plan, 2026-07-20)_
- `docs/superpowers/plans/2026-07-20-esp32c6-second-target.md` — ESP32-C6 Second Zephyr Target — Implementation Plan _(plan, 2026-07-20)_
- `docs/superpowers/plans/2026-07-20-esp32c6-thread-ping.md` — ESP32-C6 end-to-end Thread ping — Implementation Plan _(plan, 2026-07-20)_
- `docs/superpowers/plans/2026-07-21-job-latency-instrumentation.md` — Job Latency Instrumentation Implementation Plan _(plan, 2026-08-02)_
- `docs/superpowers/plans/2026-07-22-diag-span-capture.md` — Bounded span-capture sessions on a `diag/` channel family — Implementation Plan _(plan, 2026-08-02)_
- `docs/superpowers/plans/2026-07-22-node-profiler-slice.md` — Node Profiler — porta-lite slice — Implementation Plan _(plan, 2026-08-02)_
- `docs/superpowers/plans/2026-07-22-performance-deck-build.md` — Performance deck (#70) — build plan _(plan, 2026-07-22)_
- `docs/superpowers/plans/2026-07-23-gpio-output-level-seam.md` — GPIO output + level-read seam — Implementation Plan _(plan, 2026-07-23)_
- `docs/superpowers/plans/2026-07-23-magtag-epaper-display-seam.md` — MagTag e-paper display seam — Implementation Plan _(plan, 2026-07-23)_
- `docs/superpowers/plans/2026-07-24-magtag-deep-sleep-clock.md` — MagTag Deep-Sleep Ship's-Bell Clock Implementation Plan _(plan, 2026-07-24)_
- `docs/superpowers/plans/2026-07-24-magtag-ntp-catchup.md` — MagTag NTP Catchup (stage 4) Implementation Plan _(plan, 2026-07-23)_
- `docs/superpowers/plans/2026-08-01-porta-space-sync-c6-gate.md` — porta space-sync — the C6 hardware gate (plan 3 of 3) — Implementation Plan _(plan, 2026-08-02)_
- `docs/superpowers/plans/2026-08-02-porta-space-sync-node.md` — porta space-sync — the node side (plan 2 of 3) — Implementation Plan _(plan, 2026-08-02)_
- `docs/superpowers/plans/2026-08-02-rete-repo-consolidation.md` — rete Repo Consolidation — Implementation Plan _(plan, 2026-08-02)_
- `docs/superpowers/plans/2026-08-03-device-console-collector.md` — Device Console Collector and Offline Symbolizer — Implementation Plan _(plan, 2026-08-03)_
- `docs/superpowers/plans/2026-08-04-sys-metrics-over-porta.md` — sys/metrics over porta — v1 Implementation Plan _(plan, 2026-08-04)_
- `docs/superpowers/plans/2026-08-06-method-profile-over-porta.md` — Per-method profiling over porta — Implementation Plan _(plan, 2026-08-06)_
- `docs/superpowers/plans/2026-08-07-code-push-host-slice.md` — Code-Push Host Slice (#25) Implementation Plan _(plan, 2026-08-07)_
- `docs/superpowers/plans/2026-08-10-alloc-seam-and-cheap-gauge.md` — Allocation seam and the `sys/cheap` gauge — implementation plan _(plan, 2026-08-10)_
- `docs/superpowers/plans/2026-08-10-inline-symbol-bytes.md` — Inline Symbol bytes — implementation plan (rete#190 stage 2, design §4 / sub-project B) _(plan, 2026-08-10)_
- `docs/superpowers/plans/2026-08-11-short-wire-labels.md` — Short wire labels — implementation plan (rete#197) _(plan, 2026-08-11)_
- `docs/superpowers/plans/2026-08-12-allocation-attribution-stage1.md` — Allocation Attribution — Stage 1 Implementation Plan _(plan, 2026-08-12)_
- `docs/superpowers/plans/2026-08-19-vm-liveness-observer.md` — VM Liveness Observer Implementation Plan (#215) _(plan, 2026-08-19)_
- `docs/superpowers/plans/2026-08-21-liveness-contract.md` — The liveness contract — implementation plan (#231) _(plan, 2026-08-22)_
- `docs/superpowers/plans/2026-08-25-kind-schema-manifest.md` — Kind+Schema Manifest (#243) Implementation Plan _(plan, 2026-08-25)_
- `docs/superpowers/plans/2026-08-26-porta-refuse-board-session.md` — #232 Board Session Implementation Plan — porta-refuse scenario, #114 must-deliver, two riders _(plan, 2026-08-26)_
- `docs/superpowers/plans/2026-09-17-liveness-two-cadences.md` — Two liveness cadences (#273 D1 + D2) Implementation Plan _(plan, 2026-09-18)_
- `docs/superpowers/plans/2026-10-02-node-id-factory-address.md` — Node id = factory link address (#118/#242) Implementation Plan _(plan, 2026-10-02)_
- `docs/tutorials/01-host.md` — Tutorial 1 — the VM on your host: build it, run the suite, write ten lines of nsl _(record, 2026-08-29)_
- `docs/tutorials/02-board.md` — Tutorial 2 — a board: flash a node, watch it join a mesh, run the gates _(record, 2026-10-02)_
- `docs/tutorials/03-porta.md` — Tutorial 3 — porta on your host: a gateway, a node, and the diff between them _(record, 2026-10-02)_

### nodus/oevm/docs

- `nodus/oevm/docs/MILESTONE_A.md` — Milestone A — provenance and validation note _(record, 2026-08-29)_
- `nodus/oevm/docs/MILESTONE_D.md` — Milestone D — provenance and validation note _(record, 2026-08-29)_
- `nodus/oevm/docs/MILESTONE_E.md` — Milestone E — provenance and validation note _(record, 2026-08-29)_
- `nodus/oevm/docs/README.md` — nodus/oevm/docs — the VM's documents _(record, 2026-08-29)_
- `nodus/oevm/docs/archive/ECOSYSTEM.md` — Ecosystem map — the language/VM/fleet constellation _(record, 2026-08-29)_
- `nodus/oevm/docs/archive/ROADMAP.md` — OEVM Roadmap _(record, 2026-08-29)_
- `nodus/oevm/docs/archive/concept.md` — OEVM Concept & Genesis: From LLM-Driven Titelang to Custom Micro-Kernel VM _(record, 2026-08-29)_
- `nodus/oevm/docs/archive/dev_process/phase1_walkthrough.md` — Walkthrough - Core C Structures for OEVM _(record, 2026-08-29)_
- `nodus/oevm/docs/archive/dev_process/phase2_walkthrough.md` — Walkthrough - Phase 2: Instruction Set Expansion & XIP Solidification _(record, 2026-08-29)_
- `nodus/oevm/docs/archive/dev_process/phase3_walkthrough.md` — Walkthrough - Phase 3: Introspection & Newspeak Mirrors _(record, 2026-08-29)_
- `nodus/oevm/docs/archive/dev_process/phase4_walkthrough.md` — Walkthrough - Phase 4: Fiber Scheduling Architecture _(record, 2026-08-29)_
- `nodus/oevm/docs/archive/dev_process/phase5_walkthrough.md` — Walkthrough: Phase 5 - Stack Reification & Bytecode-Driven Non-Local Returns _(record, 2026-08-29)_
- `nodus/oevm/docs/archive/dev_process/phase6_walkthrough.md` — Walkthrough - Phase 6: Nested Classes & Lexical Enclosures _(record, 2026-08-29)_
- `nodus/oevm/docs/archive/fiber_derecursify_core_design.md` — De-recursify core — flat non-recursive dispatch (design) _(record, 2026-08-29)_
- `nodus/oevm/docs/archive/fiber_implementation.md` — Fiber implementation — de-recursifying the oevm interpreter (the Toit model) _(record, 2026-08-29)_
- `nodus/oevm/docs/archive/fiber_l0_frame_abi.md` — L0 — Frame / bytecode ABI contract (the de-recursify foundation) _(record, 2026-08-29)_
- `nodus/oevm/docs/archive/fiber_l1_implementation_plan.md` — L1 — First-class Stack + relative dynamic links + precise GC: Implementation Plan _(record, 2026-08-29)_
- `nodus/oevm/docs/archive/fiber_l1_stack_context_design.md` — L1 — Stack object & lexical environment (Context / indirect temp-vectors) _(record, 2026-08-29)_
- `nodus/oevm/docs/archive/implementation_plan.md` — Implementation Plan: Phases 5 - 8 Roadmap & Phase 5 Specification _(record, 2026-08-29)_
- `nodus/oevm/docs/archive/spec.md` — Architectural Specification: Open, Lazy-Reified Virtual Machine _(record, 2026-08-29)_
- `nodus/oevm/docs/deployment-model.md` — Deployment model: resident VM + OTA'd image (Model B) _(record, 2026-08-29)_
- `nodus/oevm/docs/op_send_implicit.md` — OP_SEND_IMPLICIT: Lexical Implicit-Receiver Message Sends _(record, 2026-08-29)_
- `nodus/oevm/docs/optimizations.md` — OEVM interpreter optimization log _(record, 2026-08-29)_
- `nodus/oevm/docs/topics.md` — Design topics _(record, 2026-08-29)_
- `nodus/oevm/docs/vm.md` — oevm — what the VM is, as of 2026-08-29 _(record, 2026-08-29)_

### protocol

- `protocol/PROTOCOL-NSL-RECORDS.md` — Porta ↔ nsl — the record schemas _(record, 2026-10-02)_
- `protocol/PROTOCOL-NSL.md` — Porta ↔ nsl node protocol — the space-sync model _(record, 2026-09-17)_
- `protocol/archive/PROTOCOL-NSL-draft-2026-07-11.md` — Porta ↔ nsl node protocol — the space-sync model (genesis draft) _(record)_
- `protocol/archive/PROTOCOL.md` — Porta wire protocol _(record)_
- `protocol/tools/.pytest_cache/README.md` — pytest cache directory # _(record)_

### nodus/nsl/docs

- `nodus/nsl/docs/NEWSPEAK_DIALECT.md` — ns-oevm — a Newspeak dialect (intentional differences from full Newspeak) _(record, 2026-08-26)_
- `nodus/nsl/docs/README.md` — nodus/nsl/docs — the dialect's documents _(record, 2026-08-29)_
- `nodus/nsl/docs/compiler.md` — The nsl compiler _(record, 2026-10-01)_
- `nodus/nsl/docs/design/2026-06-16-block-inlining-design.md` — Block inlining via a scope/frame model _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-06-19-next-phase-stdlib-and-debug.md` — ns-oevm — next phase: standard library + debug story _(design, 2026-08-29)_
- `nodus/nsl/docs/design/2026-06-20-ns-oevm-stdlib-design.md` — ns-oevm — standard library: the capability-pure primitive/library line _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-06-20-ns-oevm-stdlib-step3a-kernel-object-array.md` — ns-oevm std-lib step 3a — kernel infrastructure + Object + Array _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-06-20-ns-oevm-stdlib-step3b-kernel-module-collections.md` — ns-oevm std-lib step 3b — Kernel module foundation + Collection + OrderedCollection _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-06-21-ns-oevm-stdlib-step3c1-string-booleans.md` — ns-oevm std-lib step 3c-1 — String + the universal value protocol (hash / = / printString), incl. booleans _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-06-21-ns-oevm-stdlib-step3c2-dictionary-set.md` — ns-oevm std-lib step 3c-2 — Dictionary + Set (HashedCollection) _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-06-22-ns-oevm-stdlib-step3c3-streams-design.md` — ns-oevm std-lib step 3c-3a — Streams (ReadStream + WriteStream) _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-06-22-ns-oevm-stdlib-step3c3b-serial-design.md` — ns-oevm std-lib step 3c-3b — Serial (the first native capability tier) _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-06-23-ns-oevm-in-image-debugger-design.md` — In-image debugger + mirror grant (POSIX, agent-drivable) — design _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-06-23-ns-oevm-mirror-confinement-design.md` — ns-oevm Mirror tier — confining ambient reflection (the privileged-reflection capability) _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-06-23-ns-oevm-stratify-execution-off-reflection-design.md` — ns-oevm — Stratify base-level execution off reflection _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-06-25-ns-posix-keeper-r1-design.md` — Design: R1 — `ns-posix` node skeleton (nodus-style keeper on the resident oevm VM) _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-06-25-ns-posix-keeper-r2-telemetry-design.md` — R2 — ns-posix keeper telemetry forwarding (design) _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-06-25-ns-posix-keeper-r3-remote-debug-design.md` — R3 — remote-debug ns-posix through porta (design) _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-06-26-gpio-capability-deferred-blueprint.md` — GPIO capability — deferred blueprint (do on Zephyr, not host-mock) _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-06-26-ns-oevm-udp-networking-capability-design.md` — R4 — UDP networking capability (design) _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-06-26-r6-ns-zephyr-net-capability-design.md` — R6 — ns-zephyr net capability on hardware (design) _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-06-29-r7-fibers-design.md` — R7 — Fibers: language-level cooperative concurrency (design) _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-07-03-first-step-design-addendum.md` — nsl-tuvm first-step design — addendum 1 (review amendments) _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-07-03-nsl-tuvm-first-step-design.md` — nsl-tuvm — reactive, capability-secure, no-NLR node runtime (first-step design) _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-07-04-G3-gc-strategy-decision.md` — G3 — GC strategy: non-moving mark-sweep, ratified _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-07-04-L7-integer-semantics-decision.md` — L7 — Integer semantics across 32/64-bit platforms _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-07-04-milestone-b-message-algebra-design.md` — Milestone B — the message algebra (design) _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-07-06-milestone-c-channels-reactor-design.md` — Milestone C — capability-scoped channels + reactor (design) _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-07-07-milestone-d-supervisor-crash-boundary-design.md` — Milestone D — supervisor + let-it-crash + host job (design) _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-07-07-milestone-e-exceptions-design.md` — Milestone E — full `on:do:` / `ensure:` (design) _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-07-08-zephyr-pulse-primitive-design.md` — Zephyr rung-2, sub-project 1 — boot-the-VM + ISR→channel pulse primitive _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-07-09-rung3-cbor-int64-wire-seam-design.md` — Rung 3 — CBOR + Int64 wire seam (design) _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-07-10-on-target-measurement-design.md` — On-Target Measurement — design _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-07-10-rung5-udp-transport-l1-design.md` — Rung 5 — UDP transport (Layer 1: net capability) — native_sim proof — design _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-07-10-rung6-udp-bridge-l2-design.md` — Rung 6 — Layer 2: the reactive channel↔UDP bridge (CBOR) — design _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-07-11-rung7-openthread-l3-design.md` — Rung 7 — Layer 3: NetBridge over real OpenThread (nRF52840-DK, MTD child) — design _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-07-11-rung8-ontarget-timer-design.md` — Rung 8 — On-target timer seam + the self-gating Thread node — Design _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-07-13-rung9-cells-design.md` — Rung 9 — Cells: a name that has a value _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-07-14-tuvm20-overflow-policy-design.md` — tuvm#20 — G2 overflow policy: the ring never kills the node (design) _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-07-14-tuvm23-crash-timer-sweep-design.md` — tuvm#23 — the crash sweep must cancel timers, not just reactions (design) _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-07-14-tuvm24-sys-metrics-design.md` — tuvm#24 — the sys/metrics story: heap + handles + queue telemetry as a cell _(design, 2026-08-02)_
- `nodus/nsl/docs/design/2026-07-15-tuvm21-take-nil-vs-empty-design.md` — tuvm#21 — a posted `nil` must be distinguishable from an empty stream at `take:` _(design, 2026-08-02)_
- `nodus/nsl/docs/programming_with_nsl.md` — Programming with nsl _(record, 2026-08-24)_
- `nodus/nsl/docs/superpowers/findings/2026-07-04-oimg-large-smi-constant-corruption.md` — FINDING (2026-07-04): oimg backend corrupts large SMI integer constants _(note, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-06-16-block-inlining-phase-a.md` — Block Inlining — Phase A (Foundation) Implementation Plan _(plan, 2026-08-29)_
- `nodus/nsl/docs/superpowers/plans/2026-06-16-block-inlining-phase-b.md` — Block Inlining — Phase B (if-family inlining) Implementation Plan _(plan, 2026-08-29)_
- `nodus/nsl/docs/superpowers/plans/2026-06-16-block-inlining-phase-c.md` — Block Inlining — Phase C (`and:` / `or:` short-circuit inlining) Implementation Plan _(plan, 2026-08-29)_
- `nodus/nsl/docs/superpowers/plans/2026-06-16-block-inlining-phase-d.md` — Block Inlining Phase D — Loop Inlining Implementation Plan _(plan, 2026-08-29)_
- `nodus/nsl/docs/superpowers/plans/2026-06-19-source-map-emission.md` — Source-Map Emission Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-06-20-stdlib-step1-indexable-platform.md` — Std-lib Step 1: Real Indexable Representation + `platform` Capability Object — Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-06-20-stdlib-step2-compiler-platform-wiring.md` — Std-lib Step 2 — Compiler Platform Wiring Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-06-20-stdlib-step3a-kernel-object-array.md` — Std-lib Step 3a — Kernel Infrastructure + Object + Array Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-06-21-stdlib-step3b-kernel-module-collections.md` — Std-lib Step 3b — Kernel Module + Collection + OrderedCollection (Option C) Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-06-21-stdlib-step3c1-string-booleans.md` — std-lib step 3c-1 — String + universal value protocol (hash/=/printString) Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-06-21-stdlib-step3c2-dictionary-set.md` — Std-lib step 3c-2 — Dictionary + Set Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-06-22-stdlib-step3c3a-streams.md` — Std-lib step 3c-3a — Streams (ReadStream + WriteStream) Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-06-22-stdlib-step3c3b-serial.md` — Std-lib step 3c-3b — Serial (first native capability tier) Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-06-23-ns-oevm-mirror-confinement.md` — Mirror Confinement Implementation Plan _(plan, 2026-08-29)_
- `nodus/nsl/docs/superpowers/plans/2026-06-23-stratify-execution-off-reflection.md` — Stratify Base-Level Execution Off Reflection — Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-06-24-ns-oevm-in-image-debugger-plan2.md` — In-Image Debugger (Plan 2) Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-06-24-ns-oevm-mirror-grant-supervisor.md` — Mirror grant → Supervisor boot citizen (Plan 1 of 3) Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-06-25-ns-posix-keeper-r1.md` — R1 — `ns-posix` keeper on the resident oevm VM — Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-06-25-ns-posix-keeper-r3-remote-debug.md` — R3 — Remote-debug ns-posix through porta — Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-06-25-ns-zephyr-capability-node-roadmap.md` — Newspeak node — phase roadmap (desktop-first: ns-posix → ns-zephyr) _(plan, 2026-08-29)_
- `nodus/nsl/docs/superpowers/plans/2026-06-26-r6-ns-zephyr-net-capability.md` — R6 — ns-zephyr net capability on hardware Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-06-26-udp-networking-capability.md` — UDP Networking Capability Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-06-29-r7-fibers.md` — R7 Fibers Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-06-29-r7-task2b-fiber-gc-integration.md` — R7 Task 2b — fiber GC integration + Zephyr re-fence (post-review fixes) _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-06-30-derecursify-phase1-finish-lowering.md` — De-recursify Phase 1 — Finish Lowering (OP_BLOCK_INVOKE) Implementation Plan _(plan, 2026-08-29)_
- `nodus/nsl/docs/superpowers/plans/2026-06-30-derecursify-phase2-the-flip.md` — De-recursify Phase 2 — The Flip (flat non-recursive dispatch) Implementation Plan _(plan, 2026-08-29)_
- `nodus/nsl/docs/superpowers/plans/2026-07-01-derecursify-deferred-phase1-temp-vectors.md` — De-recursify Deferred Lift — Phase 1: Indirect Temp-Vectors — Implementation Plan _(plan, 2026-08-29)_
- `nodus/nsl/docs/superpowers/plans/2026-07-01-derecursify-phase3-fibers-demolition.md` — De-recursify Phase 3 — Fibers + Demolition Implementation Plan _(plan, 2026-08-29)_
- `nodus/nsl/docs/superpowers/plans/2026-07-02-derecursify-deferred-lift-phase2-relativize-home-ctx.md` — De-recursify Deferred Lift — Phase 2: Relativize `home_ctx` — Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-07-02-derecursify-deferred-lift-phase3-growable-stacks.md` — De-recursify deferred lift — Phase 3: growable stacks Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-07-02-derecursify-phase2-self-migration-and-flip.md` — De-recursify Phase 2 (redesigned) — `self`-migration + relativize `home_ctx` — Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-07-03-derecursify-reify-e1-retire-dead-access-walk.md` — Reify-on-escape E1 — Retire the dead `home_ctx` access-walk Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-07-03-nsl-tuvm-milestone-a-seed-delete.md` — Milestone A — Seed + Delete (the verified-simplification milestone) Implementation Plan _(plan, 2026-08-29)_
- `nodus/nsl/docs/superpowers/plans/2026-07-04-milestone-b-message-algebra.md` — Milestone B — Message Algebra Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-07-06-milestone-c-channels-reactor.md` — Milestone C — Capability-Scoped Channels + Reactor Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-07-07-milestone-d-supervisor-crash-boundary.md` — Milestone D — Supervisor + Crash Boundary Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-07-07-milestone-e-exceptions.md` — Milestone E — full `on:do:` / `ensure:` Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-07-08-zephyr-pulse-primitive.md` — Zephyr Pulse-Primitive Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-07-09-rung3-cbor-int64-wire-seam.md` — Rung 3 — CBOR + Int64 Wire Seam Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-07-10-on-target-measurement.md` — On-Target Measurement Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-07-10-rung5-udp-transport-l1.md` — Rung 5 — UDP transport (Layer 1) native_sim proof Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-07-10-rung6-udp-bridge-l2.md` — Rung 6 — Layer 2: reactive channel↔UDP bridge (CBOR) — Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-07-11-rung7-openthread-l3.md` — Rung 7 — Layer 3: NetBridge over real OpenThread (MTD child) — Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-07-11-rung8-ontarget-timer.md` — Rung 8 — On-target timer seam + self-gating Thread node — Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-07-13-rung9-cells.md` — Rung 9 — Cells: a name that has a value — Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-07-14-tuvm24-sys-metrics.md` — tuvm#24 sys/metrics Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/2026-07-15-tuvm21-take-nil-vs-empty.md` — tuvm#21 — Distinguish posted `nil` from empty stream at `take:` — Implementation Plan _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/block-inlining-status.md` — Block Inlining — Track Status _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/milestone-a-footprint-delta.md` — Milestone A footprint delta (Task 10c) _(plan, 2026-08-02)_
- `nodus/nsl/docs/superpowers/plans/pared-conformance-subset.md` — Pared conformance subset (Milestone A) _(plan, 2026-08-02)_

### nodus/oevm/zephyr/docs

- `nodus/oevm/zephyr/docs/BOARD_PORTS.md` — oevm on Zephyr — board ports _(record, 2026-10-02)_
- `nodus/oevm/zephyr/docs/ZEPHYR_LESSONS.md` — Zephyr / NCS + OpenThread + USB — hard-won lessons (nRF52840) _(record, 2026-08-24)_
- `nodus/oevm/zephyr/docs/results.md` — tuvm on-device results — ESP32-WROOM-32 (Xtensa) _(record, 2026-08-29)_
- `nodus/oevm/zephyr/docs/stic-README-snapshot-2026-07-17.md` — STIC Smalltalk benchmark _(record, 2026-08-24)_

### porta/docs

- `porta/docs/ARCHITECTURE.md` — porta system architecture _(record, 2026-09-17)_
- `porta/docs/CLI.md` — porta CLI reference _(record, 2026-08-29)_
- `porta/docs/DEVSDK.md` — porta dev-SDK (`devsdk/`) — the northbound contract _(record, 2026-08-29)_
- `porta/docs/GETTING-STARTED.md` — Getting started with porta _(record, 2026-10-02)_
- `porta/docs/README.md` — porta/docs — the gateway's documents _(record, 2026-08-29)_
- `porta/docs/archive/plans/2026-05-23-gateway-b1-tftp-free-core.md` — Gateway B1 — TFTP-free core Implementation Plan _(record, 2026-08-29)_
- `porta/docs/archive/plans/2026-05-23-gateway-b2-wire-device-hardware.md` — Gateway B2 — wire + device + hardware Implementation Plan _(record, 2026-08-29)_
- `porta/docs/archive/plans/2026-05-23-tftp-engine-transport-handler-split.md` — TFTP engine / transport / handler split — Implementation Plan _(record, 2026-08-29)_
- `porta/docs/archive/plans/2026-05-24-config-self-heal.md` — Config Self-Heal Implementation Plan _(record, 2026-08-29)_
- `porta/docs/archive/plans/2026-05-24-d5-observed-config-echo.md` — D5 — Observed-Config Echo Implementation Plan _(record, 2026-08-29)_
- `porta/docs/archive/plans/2026-05-24-m2-2-down-path-config.md` — M2.2 Down-Path (Config / Setpoints) Implementation Plan _(record, 2026-08-29)_
- `porta/docs/archive/plans/2026-05-24-m2-telemetry.md` — M2 Telemetry (up-path) Implementation Plan _(record, 2026-08-29)_
- `porta/docs/archive/plans/2026-05-26-porta-nodus-split.md` — porta → porta/nodus Split Implementation Plan _(record, 2026-08-29)_
- `porta/docs/archive/plans/2026-05-27-go-mainline-restructure.md` — Go-Mainline Restructure (sub-project A) — Implementation Plan _(record, 2026-08-29)_
- `porta/docs/archive/plans/2026-05-27-porta-go-config-plane.md` — porta Go Core — B2 Config Plane Implementation Plan _(record, 2026-08-29)_
- `porta/docs/archive/plans/2026-05-27-porta-go-core-data-plane.md` — porta Go core data plane (B1) Implementation Plan _(record, 2026-08-29)_
- `porta/docs/archive/plans/2026-05-28-porta-b4a-multi-listener.md` — porta Go Core — B4a Multi-Listener Serve Implementation Plan _(record, 2026-08-29)_
- `porta/docs/archive/plans/2026-05-28-porta-b4b-mcp-read-surface.md` — porta B4b — MCP read surface Implementation Plan _(record, 2026-08-29)_
- `porta/docs/archive/plans/2026-05-28-porta-b4c-htmx-operator-ui.md` — porta B4c — htmx Operator UI Implementation Plan _(record, 2026-08-29)_
- `porta/docs/archive/plans/2026-05-28-porta-go-telemetry-plane.md` — porta Go Core — B3 Telemetry Plane Implementation Plan _(record, 2026-08-29)_
- `porta/docs/archive/plans/2026-05-31-command-lifecycle-ui.md` — Command Lifecycle Badges + Operator-UI Reorg — Implementation Plan _(record, 2026-08-29)_
- `porta/docs/archive/plans/2026-06-02-cli-api-client-s2.md` — CLI-as-API-client (S2) Implementation Plan _(record, 2026-08-29)_
- `porta/docs/archive/plans/2026-06-02-control-plane-api-s1.md` — Control-plane HTTP API (S1) Implementation Plan _(record, 2026-08-29)_
- `porta/docs/archive/plans/2026-06-02-monitor-over-api-s5.md` — S5 — `porta monitor` over the control-plane API — Implementation Plan _(record, 2026-08-29)_
- `porta/docs/archive/plans/2026-06-02-porta-run-over-api-s3.md` — `porta run` over the API (S3) — Implementation Plan _(record, 2026-08-29)_
- `porta/docs/archive/plans/2026-06-02-tools-toit-phase1.md` — `/tools/toit` Phase 1 — `porta run` + node identity (Implementation Plan) _(record, 2026-08-29)_
- `porta/docs/archive/plans/2026-06-02-web-node-identity-s4.md` — Web node identity (chip/SDK) visibility (S4) — Implementation Plan _(record, 2026-08-29)_
- `porta/docs/archive/plans/2026-06-03-panic-decode-s6.md` — Panic Decode (S6) — porta side — Implementation Plan _(record, 2026-08-29)_
- `porta/docs/archive/plans/2026-06-03-porta-devsdk-c1.md` — porta devsdk carve (C1) Implementation Plan _(record, 2026-08-29)_
- `porta/docs/archive/plans/2026-06-04-reset-reason.md` — Reset Reason Implementation Plan _(record, 2026-08-29)_
- `porta/docs/archive/specs/2026-05-23-porta-toit-gateway-design.md` — Design: Porta Toit gateway (command-queue control plane + sqlite) _(record, 2026-08-29)_
- `porta/docs/archive/specs/2026-05-23-tftp-engine-transport-handler-split-design.md` — Spec A: TFTP engine / transport / handler split _(record, 2026-08-29)_
- `porta/docs/archive/specs/2026-05-24-config-self-heal-design.md` — Config Self-Heal Design _(record, 2026-08-29)_
- `porta/docs/archive/specs/2026-05-24-d5-observed-config-echo-design.md` — D5 — Observed-Config Echo Design _(record, 2026-08-29)_
- `porta/docs/archive/specs/2026-05-24-m2-telemetry-design.md` — Design: Porta M2 — telemetry (`data_log`) + `gateway monitor` _(record, 2026-08-29)_
- `porta/docs/archive/specs/2026-05-24-security-transport-evolution-design.md` — Design note: Security posture & transport evolution _(record, 2026-08-29)_
- `porta/docs/archive/specs/2026-05-26-porta-nodus-split-design.md` — Splitting porta into `porta` (gateway) + `nodus` (node) — design _(record, 2026-08-29)_
- `porta/docs/archive/specs/2026-05-27-porta-go-config-plane-design.md` — porta Go core — B2 config plane (design) _(record, 2026-08-29)_
- `porta/docs/archive/specs/2026-05-27-porta-go-core-data-plane-design.md` — porta Go core data plane (sub-project B1) — design _(record, 2026-08-29)_
- `porta/docs/archive/specs/2026-05-27-porta-go-mainline-restructure-design.md` — porta → Go-mainline restructure (sub-project A) _(record, 2026-08-29)_
- `porta/docs/archive/specs/2026-05-28-porta-b4a-multi-listener-design.md` — porta Go core — B4a multi-listener serve (design) _(record, 2026-08-29)_
- `porta/docs/archive/specs/2026-05-28-porta-b4b-mcp-read-surface-design.md` — porta B4b — MCP read surface: design _(record, 2026-08-29)_
- `porta/docs/archive/specs/2026-05-28-porta-b4c-htmx-operator-ui-design.md` — porta B4c — htmx operator UI: design _(record, 2026-08-29)_
- `porta/docs/archive/specs/2026-05-28-porta-go-telemetry-plane-design.md` — porta Go core — B3 telemetry plane (design) _(record, 2026-08-29)_
- `porta/docs/archive/specs/2026-05-31-command-lifecycle-ui-design.md` — Command lifecycle badges + operator-UI reorg — design _(record, 2026-08-29)_
- `porta/docs/archive/specs/2026-06-02-cli-api-client-s2-design.md` — CLI becomes an API client (S2) — writes over HTTP _(record, 2026-08-29)_
- `porta/docs/archive/specs/2026-06-02-control-plane-api-design.md` — Control-plane HTTP API (S1) — server-as-workhorse, CLI-as-client _(record, 2026-08-29)_
- `porta/docs/archive/specs/2026-06-02-monitor-over-api-s5-design.md` — S5 — `porta monitor` over the control-plane API — design _(record, 2026-08-29)_
- `porta/docs/archive/specs/2026-06-02-porta-run-over-api-s3-design.md` — S3 — `porta run` over the control-plane API (Design) _(record, 2026-08-29)_
- `porta/docs/archive/specs/2026-06-02-tools-toit-design.md` — `/tools/toit` — trainer-wheels toolchain for porta nodes _(record, 2026-08-29)_
- `porta/docs/archive/specs/2026-06-02-web-node-identity-s4-design.md` — S4 — Web node identity (chip/SDK) visibility (Design) _(record, 2026-08-29)_
- `porta/docs/archive/specs/2026-06-03-panic-decode-s6-design.md` — S6 — Client-side panic decode (design) _(record, 2026-08-29)_
- `porta/docs/archive/specs/2026-06-03-porta-devsdk-nodus-flash-design.md` — porta dev-SDK + `nodus flash` toolchain — design _(record, 2026-08-29)_
- `porta/docs/archive/specs/2026-06-04-reset-reason-design.md` — Neutral `reset` reason in the report, surfaced by porta _(record, 2026-08-29)_
- `porta/docs/demo/porta-show-and-tell-script.md` — porta — show-and-tell narration script _(record, 2026-08-02)_
- `porta/docs/demo/show-and-tell-template.md` — Show-and-tell walkthrough template _(record, 2026-08-02)_
- `porta/docs/design/2026-06-06-web-telemetry-console-panels-design.md` — Web telemetry: per-node console panels + telemetry node filter — Design _(design, 2026-08-02)_
- `porta/docs/design/2026-06-07-panic-decode-link-design.md` — Panic decode link (porta → nodus) — design _(design, 2026-08-02)_
- `porta/docs/design/2026-06-26-profile-verb-design.md` — `profile` verb — remote target-execution profiling — design _(design, 2026-08-02)_
- `porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md` — porta → channels: the space-sync rewrite (v1 deleted) _(design, 2026-08-26)_
- `porta/docs/design/2026-08-11-gatekeeper-and-the-introspectable-space-design.md` — Gatekeeper, and the introspectable space — design note (2026-08-11) _(design, 2026-08-29)_
- `porta/docs/design/2026-08-15-boot-anchored-cell-reads-design.md` — Boot-anchored cell reads — `porta get` can no longer blend two boots _(design, 2026-08-15)_
- `porta/docs/design/2026-08-22-gatekeeper-first-cut-design.md` — Gatekeeper — the first cut (design, 2026-08-22) _(design, 2026-08-29)_
- `porta/docs/design/2026-08-26-space-sync-durability-trio-design.md` — The space-sync durability trio: the replay guard, the RAM cursor, and what must-deliver means _(design, 2026-08-26)_
- `porta/docs/superpowers/plans/2026-06-06-set-forward-verb.md` — set-forward verb + enriched telemetry entries (porta side) Implementation Plan _(plan, 2026-08-02)_
- `porta/docs/superpowers/plans/2026-06-06-web-telemetry-console-panels.md` — Web telemetry console panels + node filter Implementation Plan _(plan, 2026-08-02)_
- `porta/docs/superpowers/plans/2026-06-07-panic-decode-link.md` — Panic Decode Link Implementation Plan _(plan, 2026-08-02)_
- `porta/docs/superpowers/plans/2026-06-26-profile-verb.md` — `profile` verb — remote target-execution profiling — Implementation Plan _(plan, 2026-08-02)_
- `porta/docs/superpowers/plans/2026-08-01-porta-space-sync-core.md` — porta space-sync core — Implementation Plan (plan 1 of 3) _(plan, 2026-08-02)_
- `porta/docs/superpowers/plans/2026-08-18-gw-liveness-silence-sweep.md` — `gw/**` and the silence sweep — implementation plan (#217) _(plan, 2026-08-18)_
- `porta/docs/superpowers/plans/2026-08-26-gatekeeper-query-core.md` — Gatekeeper query core (#198) — implementation plan _(plan, 2026-08-29)_
- `porta/docs/superpowers/plans/2026-08-26-space-sync-durability-trio.md` — Space-Sync Durability Trio Implementation Plan _(plan, 2026-08-26)_

### ~/workspaceToit/nodus/docs

- `~/workspaceToit/nodus/docs/ARCHITECTURE.md` — nodus architecture _(record, 2026-06-12)_
- `~/workspaceToit/nodus/docs/CLI.md` — `nodus` CLI reference _(record, 2026-06-12)_
- `~/workspaceToit/nodus/docs/CONTRIBUTING.md` — Contributing / development workflow _(record, 2026-06-09)_
- `~/workspaceToit/nodus/docs/GETTING-STARTED.md` — Getting started — end to end _(record, 2026-06-12)_
- `~/workspaceToit/nodus/docs/MCP-VERIFICATION.md` — Verifying `nodus mcp` works _(record, 2026-06-07)_
- `~/workspaceToit/nodus/docs/NODE-LIFECYCLE.md` — Node Lifecycle & Power Modes _(record, 2026-06-12)_
- `~/workspaceToit/nodus/docs/PANIC-REPORTING.md` — Panic reporting (node → gateway) _(record, 2026-06-03)_
- `~/workspaceToit/nodus/docs/PROVISIONING.md` — Provisioning & flashing _(record, 2026-06-09)_
- `~/workspaceToit/nodus/docs/RELIABILITY.md` — Reliability & fault model _(record, 2026-06-12)_
- `~/workspaceToit/nodus/docs/WRITING-PAYLOADS.md` — Writing a node payload _(record, 2026-06-09)_
- `~/workspaceToit/nodus/docs/demo/nodus-show-and-tell-script.md` — nodus — show-and-tell narration script _(record, 2026-06-07)_
- `~/workspaceToit/nodus/docs/demo/nodus-topology-slide.md` — nodus ⇄ porta — topology _(record, 2026-06-07)_
- `~/workspaceToit/nodus/docs/demo/show-and-tell-template.md` — Show-and-tell walkthrough template _(record, 2026-06-07)_
- `~/workspaceToit/nodus/docs/design/2026-06-12-timeout-classification-forward-echo-design.md` — Design: gateway-timeout classification (#16) + forward-gate echo (#15) _(design, 2026-07-14)_
- `~/workspaceToit/nodus/docs/design/2026-06-28-profile-node-side-design.md` — Design: profile verb — node side (nodus) _(design, 2026-07-14)_
- `~/workspaceToit/nodus/docs/node-lifecycle-2.md` — Node Lifecycle — Two Modes Side by Side _(record, 2026-06-09)_
- `~/workspaceToit/nodus/docs/plans/2026-05-21-toit-tftp-loader-smoke-test.md` — Toit-over-TFTP Loader Smoke Test — Implementation Plan _(record, 2026-05-26)_
- `~/workspaceToit/nodus/docs/plans/2026-05-22-porta-no-jaguar-supervisor.md` — Porta No-Jaguar Supervisor Implementation Plan _(record, 2026-05-26)_
- `~/workspaceToit/nodus/docs/plans/2026-05-26-vin-run-once-lifecycle.md` — Vin run-once lifecycle Implementation Plan _(record, 2026-05-26)_
- `~/workspaceToit/nodus/docs/plans/2026-05-27-always-on-vin.md` — Always-on vin Implementation Plan _(record, 2026-05-27)_
- `~/workspaceToit/nodus/docs/plans/2026-06-02-report-chip-sdk-identity.md` — Emit `chip` + `sdk` firmware identity in the report (porta /tools/toit companion) _(record, 2026-06-02)_
- `~/workspaceToit/nodus/docs/plans/2026-06-03-gateway-config.md` — Gateway Config (provisioned gateway address) Implementation Plan _(record, 2026-06-03)_
- `~/workspaceToit/nodus/docs/plans/2026-06-03-nodus-dev-tool-c2c3.md` — nodus dev tool (C2 `run`/`monitor`/`panic` + C3 `flash`) — Implementation Plan _(record, 2026-06-05)_
- `~/workspaceToit/nodus/docs/plans/2026-06-03-nodus-dev-tool.md` — nodus dev tool (`nodus run` / `nodus flash`) — cross-repo handoff plan _(record, 2026-06-03)_
- `~/workspaceToit/nodus/docs/plans/2026-06-03-panic-capture.md` — nodus Panic Capture Implementation Plan _(record, 2026-06-03)_
- `~/workspaceToit/nodus/docs/plans/2026-06-04-nodus-mcp-control-plane.md` — nodus mcp control plane (v1) Implementation Plan _(record, 2026-06-04)_
- `~/workspaceToit/nodus/docs/plans/2026-06-04-report-effective-checkin-interval.md` — Emit `health.report_interval` — effective check-in cadence (porta #14 companion) _(record, 2026-06-05)_
- `~/workspaceToit/nodus/docs/plans/2026-06-06-northbound-channels-a-service-hub.md` — Northbound channels (a) — supervisor service hub + buffers — Implementation Plan _(record, 2026-06-06)_
- `~/workspaceToit/nodus/docs/plans/2026-06-06-northbound-channels-b-forwarding.md` — Northbound channels (b) — forwarding policy Implementation Plan _(record, 2026-06-07)_
- `~/workspaceToit/nodus/docs/plans/2026-06-06-northbound-channels-c-door-example.md` — Northbound channels (c) — door example Implementation Plan _(record, 2026-06-07)_
- `~/workspaceToit/nodus/docs/plans/2026-06-07-nodus-decode-handler-impl.md` — `nodus decode` URL-scheme handler — Implementation Plan _(record, 2026-06-07)_
- `~/workspaceToit/nodus/docs/plans/2026-06-07-nodus-decode-handler.md` — nodus `nodus://decode` URL-scheme handler (`nodus decode`) _(record, 2026-06-07)_
- `~/workspaceToit/nodus/docs/secrets.md` — secrets — local provisioning values (DO NOT COMMIT) _(record)_
- `~/workspaceToit/nodus/docs/specs/2026-05-21-toit-tftp-loader-design.md` — Design: Toit-over-TFTP loader smoke test _(record, 2026-05-26)_
- `~/workspaceToit/nodus/docs/specs/2026-05-22-porta-no-jaguar-supervisor-design.md` — Design: Porta no-jaguar supervisor + Artemis-pod-compatible node runtime _(record, 2026-05-26)_
- `~/workspaceToit/nodus/docs/specs/2026-05-24-node-lifecycle-reliability-design.md` — Design note: Node lifecycle & reliability (power modes + watchdog) _(record, 2026-05-26)_
- `~/workspaceToit/nodus/docs/specs/2026-05-27-always-on-vin-design.md` — Design note: Always-on vin (run-loop power mode + WatchdogService) _(record, 2026-05-27)_
- `~/workspaceToit/nodus/docs/specs/2026-06-03-gateway-config-design.md` — nodus — provisioned gateway address (design) _(record, 2026-06-03)_
- `~/workspaceToit/nodus/docs/specs/2026-06-03-panic-capture-design.md` — nodus — panic capture & forward (design) _(record, 2026-06-03)_
- `~/workspaceToit/nodus/docs/specs/2026-06-04-nodus-mcp-control-plane-design.md` — nodus mcp — control plane (v1) design _(record, 2026-06-05)_
- `~/workspaceToit/nodus/docs/specs/2026-06-06-northbound-channels-a-service-hub-design.md` — Northbound channels (a) — supervisor service hub + buffers _(record, 2026-06-06)_
- `~/workspaceToit/nodus/docs/specs/2026-06-06-northbound-channels-b-forwarding-design.md` — Northbound channels (b) — forwarding flags, cadences & porta wire _(record, 2026-06-06)_
- `~/workspaceToit/nodus/docs/specs/2026-06-06-northbound-channels-c-door-example-design.md` — Northbound channels (c) — the door example (deep-sleep sensor) _(record, 2026-06-06)_
- `~/workspaceToit/nodus/docs/specs/2026-06-07-clock-example-design.md` — clock example — loop-forever digital clock (design) _(record, 2026-06-07)_
- `~/workspaceToit/nodus/docs/specs/2026-06-07-nodus-decode-handler-design.md` — `nodus decode` — `nodus://decode` URL-scheme handler (design) _(record, 2026-06-07)_
- `~/workspaceToit/nodus/docs/specs/2026-06-08-node-config-ownership-design.md` — Node config ownership + `set-mode` — design _(record, 2026-06-08)_
- `~/workspaceToit/nodus/docs/specs/2026-06-09-writeable-loop-sleep-design.md` — Writeable always-on `loop_sleep_s` — design _(record, 2026-06-09)_
- `~/workspaceToit/nodus/docs/specs/porta-coordination-door-timestamp.md` — porta coordination — metric `ts` on `data?id=` entries (northbound-channels c) _(record, 2026-06-07)_
- `~/workspaceToit/nodus/docs/specs/porta-coordination-forward-echo.md` — porta coordination — forward-gate echo in `node_config` + `health.poll_timeouts` (#15 #16) _(record, 2026-06-12)_
- `~/workspaceToit/nodus/docs/specs/porta-coordination-loop-sleep.md` — porta coordination — settable always-on `loop_sleep_s` on `set-mode` _(record, 2026-06-12)_
- `~/workspaceToit/nodus/docs/specs/porta-coordination-profile-timeout.md` — porta coordination — `duration_s` on `profile_session` + derived lazy-timeout status _(record, 2026-07-14)_
- `~/workspaceToit/nodus/docs/specs/porta-coordination-set-forward.md` — porta coordination — `set-forward` verb + enriched `data?id=` entries (northbound-channels b) _(record, 2026-06-07)_
- `~/workspaceToit/nodus/docs/superpowers/plans/2026-06-08-node-config-ownership.md` — Node Config Ownership + `set-mode` Implementation Plan _(plan, 2026-06-08)_
- `~/workspaceToit/nodus/docs/superpowers/plans/2026-06-09-writeable-loop-sleep.md` — Writeable always-on `loop_sleep_s` — Implementation Plan _(plan, 2026-06-09)_
- `~/workspaceToit/nodus/docs/superpowers/plans/2026-06-12-timeout-classification-forward-echo.md` — Gateway-Timeout Classification (#16) + Forward-Gate Echo (#15) Implementation Plan _(plan, 2026-07-14)_
- `~/workspaceToit/nodus/docs/superpowers/plans/2026-06-28-profile-node-side.md` — Profile verb — node side (nodus) Implementation Plan _(plan, 2026-07-14)_

### ~/workspaceToit/stic

- `~/workspaceToit/stic/README.md` — STIC Smalltalk benchmark _(record, 2026-07-17)_
