# DOCMAP — where every document of the program lives

One place to look. The program spans **five doc homes across two workspace roots**
(`~/workspace/{tuvm,nsl-tuvm,nsl-tests,porta}` and `~/workspaceToit/nodus`). This file has
two layers: a short hand-curated list of the **documents of record** (below — edit by hand,
keep it short), and a **generated inventory** of everything (bottom — never edit by hand,
rerun `tuvm/tools/docmap.sh`).

For content search across the program:
`rg -i "<term>" ~/workspace/{tuvm,nsl-tuvm,nsl-tests,porta} ~/workspaceToit/nodus`

## Documents of record (curated)

**The roadmap and its ledgers** — `~/workspace/tuvm/review/`:

- `program-sequence.md` — **THE ladder.** The one document that sequences the whole
  program; read it before claiming direction.
- `tests/CLAIMS.md` — what is proven, by exactly which test.
- `WHY.md` / `WHY-OEOM.md` — rationale; includes the distribution law (Waldo).
- `reliability-gaps.md` — G1–G11 cross-cutting ledger.
- `metrics-spec.md` / `test-suites-spec.md` — footprint/latency lines; suite structure.
- `language-and-library-spec.md` — **a PROPOSAL, not ratified truth.**
- `zephyr-driver-exposure.md` — driver exposure tiers + the BME280→Grid-EYE→TFT ladder.
- `demo-node.ns` — the target user program; every phase should shrink the gap to it.
- `slides.html` — the flight-target artifact (≈2026-09); tracks milestone truth.
- `lib/*.md` — per-library owner docs (cbor, integers, exceptions, …).

**Dialect and VM** — `~/workspace/nsl-tuvm/docs/` and `~/workspace/tuvm/docs/`:

- `nsl-tuvm/docs/NEWSPEAK_DIALECT.md` / `programming_with_nsl.md` / `compiler.md`.
- `tuvm/docs/MILESTONE_{A,D,E}.md` — milestone close-out records.
- `tuvm/zephyr/docs/BOARD_PORTS.md` — per-board build/DFU recipes (DK + dongle knobs).

**The wire and the fleet** — `~/workspace/porta/docs/` and `~/workspaceToit/nodus/docs/`:

- `porta/docs/PROTOCOL.md` — porta protocol v1, frozen for Toit nodes.
- `porta/docs/PROTOCOL-NSL.md` + `PROTOCOL-NSL-RECORDS.md` — the nsl space-sync wire;
  records schemas must be pinned before the bridge rung implements.
- `nodus/docs/ARCHITECTURE.md` / `NODE-LIFECYCLE.md` — the Toit node implementation.

## Conventions

- **Design specs are decision records** → `docs/design/` in each repo (moved from
  `docs/superpowers/specs/`, 2026-07-14). **Plans** (execution scaffolding) stay in
  `docs/superpowers/plans/`.
- **Promotion rule:** an issue/rung/milestone is not closed until its durable knowledge
  is promoted into a document of record listed above; the dated design doc stays as the
  historical "why". Every plan's close-out task names which mainline doc absorbed it.
- Cite documents by **absolute path** (`~/workspace/tuvm/review/…`), never bare `review/`.

<!-- BEGIN GENERATED INVENTORY — do not edit below; rerun tools/docmap.sh -->

_Regenerated 2026-07-14 by `tools/docmap.sh`. Legend: **record** = mainline doc of record · **design** = dated decision record (`docs/design/`) · **plan** = execution scaffolding (`docs/superpowers/plans/`) · **note** = superpowers notes/findings._

### ~/workspace/tuvm/review

- `~/workspace/tuvm/review/2026-07-03-first-step-design-addendum.md` — nsl-tuvm first-step design — addendum 1 (review amendments) _(record, 2026-07-04)_
- `~/workspace/tuvm/review/2026-07-04-context-decision.md` — Task 8 design question: keep the deferred-lift heap Context? _(record, 2026-07-04)_
- `~/workspace/tuvm/review/2026-07-04-pre-fiber-baseline.md` — Pre-fiber baseline commits in oevm / ns-oevm _(record, 2026-07-04)_
- `~/workspace/tuvm/review/DOCMAP.md` — DOCMAP — where every document of the program lives _(record)_
- `~/workspace/tuvm/review/ISSUE-dongle-usb-openthread.md` — DRAFT ISSUE (post to davidg238/tuvm — created from a session without gh permission) _(record, 2026-07-12)_
- `~/workspace/tuvm/review/WHY-OEOM.md` — Why OEOM — the kernel that survived the graveyard _(record, 2026-07-12)_
- `~/workspace/tuvm/review/WHY.md` — Why nsl-tuvm _(record, 2026-07-12)_
- `~/workspace/tuvm/review/cells-and-streams-v3.html` — Letters and pigeonholes — v3 _(record, 2026-07-13)_
- `~/workspace/tuvm/review/language-and-library-spec.md` — nsl-tuvm — language sufficiency & library roadmap (proposed) _(record, 2026-07-12)_
- `~/workspace/tuvm/review/lib/README.md` — review/lib — designed library surface (pre-implementation staging) _(record, 2026-07-12)_
- `~/workspace/tuvm/review/lib/cbor.md` — L6 — CBOR wire codec (the TISON-analog decision; design frozen, now implemented) _(record, 2026-07-10)_
- `~/workspace/tuvm/review/lib/errors.md` — L5 — Error convention (adopt before the stdlib grows) _(record, 2026-07-10)_
- `~/workspace/tuvm/review/lib/exceptions.md` — Exceptions (`on:do:` / `ensure:`) — why NOT detailing it now is safe, and the _(record, 2026-07-07)_
- `~/workspace/tuvm/review/lib/integers.md` — L7 — Integer semantics across 32/64-bit platforms (VM design) _(record, 2026-07-04)_
- `~/workspace/tuvm/review/lib/messages.md` — Milestone B — nsl-facing message algebra API (proposed surface) _(record, 2026-07-03)_
- `~/workspace/tuvm/review/metrics-spec.md` — Node health metrics — the long-term profile (G4/G9 companion) _(record, 2026-07-07)_
- `~/workspace/tuvm/review/program-sequence.md` — nsl-tuvm — program sequence (the overarching order of work) _(record, 2026-07-13)_
- `~/workspace/tuvm/review/reliability-gaps.md` — Reliability gaps — the questions not yet asked (2026-07-03) _(record, 2026-07-07)_
- `~/workspace/tuvm/review/retained-channels-v2.html` — Names, and what answers to them — v2 _(record, 2026-07-13)_
- `~/workspace/tuvm/review/retained-channels.html` — Retained channels, and where remoteness lives _(record, 2026-07-12)_
- `~/workspace/tuvm/review/slides.html` — nsl-tuvm — script Zephyr, don't replace it _(record, 2026-07-12)_
- `~/workspace/tuvm/review/test-suites-spec.md` — nsl-tests — verification suites for the first step (proposed) _(record, 2026-07-03)_
- `~/workspace/tuvm/review/tests/CLAIMS.md` — CLAIMS — first-step spec §7 success criteria → proving tests _(record, 2026-07-13)_
- `~/workspace/tuvm/review/tests/README.md` — review/tests — staged verification suites (T0, T-A…T-D) _(record, 2026-07-07)_
- `~/workspace/tuvm/review/tests/t0-gates/sanitizers.md` — T0.1 — sanitizer gate (the highest-value single item in the whole test plan) _(record, 2026-07-03)_
- `~/workspace/tuvm/review/zephyr-driver-exposure.md` — Exposing Zephyr drivers to nsl — a sketch _(record, 2026-07-12)_

### ~/workspace/tuvm/docs

- `~/workspace/tuvm/docs/ECOSYSTEM.md` — Ecosystem map — the language/VM/fleet constellation _(record, 2026-07-03)_
- `~/workspace/tuvm/docs/MILESTONE_A.md` — Milestone A — provenance and validation note _(record, 2026-07-04)_
- `~/workspace/tuvm/docs/MILESTONE_D.md` — Milestone D — provenance and validation note _(record, 2026-07-07)_
- `~/workspace/tuvm/docs/MILESTONE_E.md` — Milestone E — provenance and validation note _(record, 2026-07-07)_
- `~/workspace/tuvm/docs/ROADMAP.md` — OEVM Roadmap _(record, 2026-07-03)_
- `~/workspace/tuvm/docs/concept.md` — OEVM Concept & Genesis: From LLM-Driven Titelang to Custom Micro-Kernel VM _(record, 2026-07-03)_
- `~/workspace/tuvm/docs/deployment-model.md` — Deployment model: resident VM + OTA'd image (Model B) _(record, 2026-07-03)_
- `~/workspace/tuvm/docs/design/2026-06-17-clustered-image-xip-design.md` — Design: Decoupled image + execute-in-place clustered format _(design)_
- `~/workspace/tuvm/docs/design/2026-06-17-global-method-cache-design.md` — Global Method Lookup Cache — Design _(design)_
- `~/workspace/tuvm/docs/design/2026-06-17-unboxed-floats-design.md` — Unboxed Floats (P0) — Design _(design)_
- `~/workspace/tuvm/docs/design/2026-06-18-gc-embedded-memory-design.md` — GC & Embedded Memory Management — Design Scope (GH #3) _(design)_
- `~/workspace/tuvm/docs/design/2026-06-18-nested-class-access-design.md` — Explicit Nested-Class Access + Outer-Sends — Design _(design)_
- `~/workspace/tuvm/docs/design/2026-06-18-w1-gc-trigger-rooting-design.md` — W1 — GC Trigger + Safe-Points / Rooting — Design _(design)_
- `~/workspace/tuvm/docs/design/2026-06-18-w4-fixed-arena-allocator-design.md` — W4 — Fixed-Arena Allocator Foundation — Design _(design)_
- `~/workspace/tuvm/docs/design/2026-06-19-gh4-iterative-mark-design.md` — GH#4 — Iterative Mark (worklist) Design _(design)_
- `~/workspace/tuvm/docs/design/2026-06-19-w2a-float-arena-reclaim-design.md` — W2a — Float-Arena Reclaim (design) _(design)_
- `~/workspace/tuvm/docs/design/2026-06-19-w2a1-float-arena-trigger-design.md` — W2a.1 — Float-Arena GC Trigger (design) _(design)_
- `~/workspace/tuvm/docs/design/2026-07-01-derecursify-deferred-lift-growable-stacks-upward-closures-design.md` — De-recursify deferred lift — growable stacks + upward closures (design) _(design)_
- `~/workspace/tuvm/docs/design/2026-07-02-derecursify-phase2-self-migration-design.md` — De-recursify Phase 2 (redesigned) — `self`-migration + relativize `home_ctx` (design) _(design)_
- `~/workspace/tuvm/docs/design/2026-07-02-derecursify-phase3-growable-stacks-design.md` — De-recursify deferred lift — Phase 3: growable stacks + overflow policy (design) _(design)_
- `~/workspace/tuvm/docs/design/2026-07-03-derecursify-e1-retire-dead-access-walk-design.md` — De-recursify reify-on-escape — E1: retire the dead `home_ctx` walk in `get_top_level_method_closure` (design) _(design)_
- `~/workspace/tuvm/docs/dev_process/phase1_walkthrough.md` — Walkthrough - Core C Structures for OEVM _(record, 2026-07-03)_
- `~/workspace/tuvm/docs/dev_process/phase2_walkthrough.md` — Walkthrough - Phase 2: Instruction Set Expansion & XIP Solidification _(record, 2026-07-03)_
- `~/workspace/tuvm/docs/dev_process/phase3_walkthrough.md` — Walkthrough - Phase 3: Introspection & Newspeak Mirrors _(record, 2026-07-03)_
- `~/workspace/tuvm/docs/dev_process/phase4_walkthrough.md` — Walkthrough - Phase 4: Fiber Scheduling Architecture _(record, 2026-07-03)_
- `~/workspace/tuvm/docs/dev_process/phase5_walkthrough.md` — Walkthrough: Phase 5 - Stack Reification & Bytecode-Driven Non-Local Returns _(record, 2026-07-03)_
- `~/workspace/tuvm/docs/dev_process/phase6_walkthrough.md` — Walkthrough - Phase 6: Nested Classes & Lexical Enclosures _(record, 2026-07-03)_
- `~/workspace/tuvm/docs/fiber_derecursify_core_design.md` — De-recursify core — flat non-recursive dispatch (design) _(record, 2026-07-03)_
- `~/workspace/tuvm/docs/fiber_implementation.md` — Fiber implementation — de-recursifying the oevm interpreter (the Toit model) _(record, 2026-07-03)_
- `~/workspace/tuvm/docs/fiber_l0_frame_abi.md` — L0 — Frame / bytecode ABI contract (the de-recursify foundation) _(record, 2026-07-03)_
- `~/workspace/tuvm/docs/fiber_l1_implementation_plan.md` — L1 — First-class Stack + relative dynamic links + precise GC: Implementation Plan _(record, 2026-07-03)_
- `~/workspace/tuvm/docs/fiber_l1_stack_context_design.md` — L1 — Stack object & lexical environment (Context / indirect temp-vectors) _(record, 2026-07-03)_
- `~/workspace/tuvm/docs/implementation_plan.md` — Implementation Plan: Phases 5 - 8 Roadmap & Phase 5 Specification _(record, 2026-07-03)_
- `~/workspace/tuvm/docs/op_send_implicit.md` — OP_SEND_IMPLICIT: Lexical Implicit-Receiver Message Sends _(record, 2026-07-03)_
- `~/workspace/tuvm/docs/optimizations.md` — OEVM interpreter optimization log _(record, 2026-07-03)_
- `~/workspace/tuvm/docs/spec.md` — Architectural Specification: Open, Lazy-Reified Virtual Machine _(record, 2026-07-03)_
- `~/workspace/tuvm/docs/superpowers/notes/2026-06-18-w1-rooting-audit.md` — W1 Rooting Audit — Runtime `gc_allocate` / `float_alloc` Sites _(note, 2026-07-03)_
- `~/workspace/tuvm/docs/superpowers/plans/2026-06-17-decoupled-xip-image.md` — Decoupled Image + Execute-In-Place Format — Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/tuvm/docs/superpowers/plans/2026-06-17-global-method-cache.md` — Global Method Lookup Cache Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/tuvm/docs/superpowers/plans/2026-06-18-w1-gc-trigger-rooting.md` — W1 — GC Trigger + Safe-Points / Rooting Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/tuvm/docs/superpowers/plans/2026-06-18-w4-fixed-arena-allocator.md` — W4 Fixed-Arena Allocator Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/tuvm/docs/superpowers/plans/2026-06-19-gh4-iterative-mark.md` — GH#4 — Iterative Mark Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/tuvm/docs/superpowers/plans/2026-06-19-w2a-float-arena-reclaim.md` — W2a — Float-Arena Reclaim Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/tuvm/docs/superpowers/plans/2026-06-19-w2a1-float-arena-trigger.md` — W2a.1 — Float-Arena GC Trigger Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/tuvm/docs/topics.md` — Design topics _(record, 2026-07-03)_

### ~/workspace/nsl-tuvm/docs

- `~/workspace/nsl-tuvm/docs/NEWSPEAK_DIALECT.md` — ns-oevm — a Newspeak dialect (intentional differences from full Newspeak) _(record, 2026-07-11)_
- `~/workspace/nsl-tuvm/docs/compiler.md` — ns-oevm compiler _(record, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/design/2026-06-16-block-inlining-design.md` — Block inlining via a scope/frame model _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-06-19-next-phase-stdlib-and-debug.md` — ns-oevm — next phase: standard library + debug story _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-06-20-ns-oevm-stdlib-design.md` — ns-oevm — standard library: the capability-pure primitive/library line _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-06-20-ns-oevm-stdlib-step3a-kernel-object-array.md` — ns-oevm std-lib step 3a — kernel infrastructure + Object + Array _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-06-20-ns-oevm-stdlib-step3b-kernel-module-collections.md` — ns-oevm std-lib step 3b — Kernel module foundation + Collection + OrderedCollection _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-06-21-ns-oevm-stdlib-step3c1-string-booleans.md` — ns-oevm std-lib step 3c-1 — String + the universal value protocol (hash / = / printString), incl. booleans _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-06-21-ns-oevm-stdlib-step3c2-dictionary-set.md` — ns-oevm std-lib step 3c-2 — Dictionary + Set (HashedCollection) _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-06-22-ns-oevm-stdlib-step3c3-streams-design.md` — ns-oevm std-lib step 3c-3a — Streams (ReadStream + WriteStream) _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-06-22-ns-oevm-stdlib-step3c3b-serial-design.md` — ns-oevm std-lib step 3c-3b — Serial (the first native capability tier) _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-06-23-ns-oevm-in-image-debugger-design.md` — In-image debugger + mirror grant (POSIX, agent-drivable) — design _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-06-23-ns-oevm-mirror-confinement-design.md` — ns-oevm Mirror tier — confining ambient reflection (the privileged-reflection capability) _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-06-23-ns-oevm-stratify-execution-off-reflection-design.md` — ns-oevm — Stratify base-level execution off reflection _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-06-25-ns-posix-keeper-r1-design.md` — Design: R1 — `ns-posix` node skeleton (nodus-style keeper on the resident oevm VM) _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-06-25-ns-posix-keeper-r2-telemetry-design.md` — R2 — ns-posix keeper telemetry forwarding (design) _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-06-25-ns-posix-keeper-r3-remote-debug-design.md` — R3 — remote-debug ns-posix through porta (design) _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-06-26-gpio-capability-deferred-blueprint.md` — GPIO capability — deferred blueprint (do on Zephyr, not host-mock) _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-06-26-ns-oevm-udp-networking-capability-design.md` — R4 — UDP networking capability (design) _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-06-26-r6-ns-zephyr-net-capability-design.md` — R6 — ns-zephyr net capability on hardware (design) _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-06-29-r7-fibers-design.md` — R7 — Fibers: language-level cooperative concurrency (design) _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-07-03-first-step-design-addendum.md` — nsl-tuvm first-step design — addendum 1 (review amendments) _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-07-03-nsl-tuvm-first-step-design.md` — nsl-tuvm — reactive, capability-secure, no-NLR node runtime (first-step design) _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-07-04-G3-gc-strategy-decision.md` — G3 — GC strategy: non-moving mark-sweep, ratified _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-07-04-L7-integer-semantics-decision.md` — L7 — Integer semantics across 32/64-bit platforms _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-07-04-milestone-b-message-algebra-design.md` — Milestone B — the message algebra (design) _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-07-06-milestone-c-channels-reactor-design.md` — Milestone C — capability-scoped channels + reactor (design) _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-07-07-milestone-d-supervisor-crash-boundary-design.md` — Milestone D — supervisor + let-it-crash + host job (design) _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-07-07-milestone-e-exceptions-design.md` — Milestone E — full `on:do:` / `ensure:` (design) _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-07-08-zephyr-pulse-primitive-design.md` — Zephyr rung-2, sub-project 1 — boot-the-VM + ISR→channel pulse primitive _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-07-09-rung3-cbor-int64-wire-seam-design.md` — Rung 3 — CBOR + Int64 wire seam (design) _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-07-10-on-target-measurement-design.md` — On-Target Measurement — design _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-07-10-rung5-udp-transport-l1-design.md` — Rung 5 — UDP transport (Layer 1: net capability) — native_sim proof — design _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-07-10-rung6-udp-bridge-l2-design.md` — Rung 6 — Layer 2: the reactive channel↔UDP bridge (CBOR) — design _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-07-11-rung7-openthread-l3-design.md` — Rung 7 — Layer 3: NetBridge over real OpenThread (nRF52840-DK, MTD child) — design _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-07-11-rung8-ontarget-timer-design.md` — Rung 8 — On-target timer seam + the self-gating Thread node — Design _(design)_
- `~/workspace/nsl-tuvm/docs/design/2026-07-13-rung9-cells-design.md` — Rung 9 — Cells: a name that has a value _(design)_
- `~/workspace/nsl-tuvm/docs/programming_with_nsl.md` — Programming with nsl _(record, 2026-07-07)_
- `~/workspace/nsl-tuvm/docs/superpowers/findings/2026-07-04-oimg-large-smi-constant-corruption.md` — FINDING (2026-07-04): oimg backend corrupts large SMI integer constants _(note, 2026-07-04)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-06-16-block-inlining-phase-a.md` — Block Inlining — Phase A (Foundation) Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-06-16-block-inlining-phase-b.md` — Block Inlining — Phase B (if-family inlining) Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-06-16-block-inlining-phase-c.md` — Block Inlining — Phase C (`and:` / `or:` short-circuit inlining) Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-06-16-block-inlining-phase-d.md` — Block Inlining Phase D — Loop Inlining Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-06-19-source-map-emission.md` — Source-Map Emission Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-06-20-stdlib-step1-indexable-platform.md` — Std-lib Step 1: Real Indexable Representation + `platform` Capability Object — Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-06-20-stdlib-step2-compiler-platform-wiring.md` — Std-lib Step 2 — Compiler Platform Wiring Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-06-20-stdlib-step3a-kernel-object-array.md` — Std-lib Step 3a — Kernel Infrastructure + Object + Array Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-06-21-stdlib-step3b-kernel-module-collections.md` — Std-lib Step 3b — Kernel Module + Collection + OrderedCollection (Option C) Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-06-21-stdlib-step3c1-string-booleans.md` — std-lib step 3c-1 — String + universal value protocol (hash/=/printString) Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-06-21-stdlib-step3c2-dictionary-set.md` — Std-lib step 3c-2 — Dictionary + Set Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-06-22-stdlib-step3c3a-streams.md` — Std-lib step 3c-3a — Streams (ReadStream + WriteStream) Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-06-22-stdlib-step3c3b-serial.md` — Std-lib step 3c-3b — Serial (first native capability tier) Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-06-23-ns-oevm-mirror-confinement.md` — Mirror Confinement Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-06-23-stratify-execution-off-reflection.md` — Stratify Base-Level Execution Off Reflection — Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-06-24-ns-oevm-in-image-debugger-plan2.md` — In-Image Debugger (Plan 2) Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-06-24-ns-oevm-mirror-grant-supervisor.md` — Mirror grant → Supervisor boot citizen (Plan 1 of 3) Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-06-25-ns-posix-keeper-r1.md` — R1 — `ns-posix` keeper on the resident oevm VM — Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-06-25-ns-posix-keeper-r3-remote-debug.md` — R3 — Remote-debug ns-posix through porta — Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-06-25-ns-zephyr-capability-node-roadmap.md` — Newspeak node — phase roadmap (desktop-first: ns-posix → ns-zephyr) _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-06-26-r6-ns-zephyr-net-capability.md` — R6 — ns-zephyr net capability on hardware Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-06-26-udp-networking-capability.md` — UDP Networking Capability Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-06-29-r7-fibers.md` — R7 Fibers Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-06-29-r7-task2b-fiber-gc-integration.md` — R7 Task 2b — fiber GC integration + Zephyr re-fence (post-review fixes) _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-06-30-derecursify-phase1-finish-lowering.md` — De-recursify Phase 1 — Finish Lowering (OP_BLOCK_INVOKE) Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-06-30-derecursify-phase2-the-flip.md` — De-recursify Phase 2 — The Flip (flat non-recursive dispatch) Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-07-01-derecursify-deferred-phase1-temp-vectors.md` — De-recursify Deferred Lift — Phase 1: Indirect Temp-Vectors — Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-07-01-derecursify-phase3-fibers-demolition.md` — De-recursify Phase 3 — Fibers + Demolition Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-07-02-derecursify-deferred-lift-phase2-relativize-home-ctx.md` — De-recursify Deferred Lift — Phase 2: Relativize `home_ctx` — Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-07-02-derecursify-deferred-lift-phase3-growable-stacks.md` — De-recursify deferred lift — Phase 3: growable stacks Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-07-02-derecursify-phase2-self-migration-and-flip.md` — De-recursify Phase 2 (redesigned) — `self`-migration + relativize `home_ctx` — Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-07-03-derecursify-reify-e1-retire-dead-access-walk.md` — Reify-on-escape E1 — Retire the dead `home_ctx` access-walk Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-07-03-nsl-tuvm-milestone-a-seed-delete.md` — Milestone A — Seed + Delete (the verified-simplification milestone) Implementation Plan _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-07-04-milestone-b-message-algebra.md` — Milestone B — Message Algebra Implementation Plan _(plan, 2026-07-05)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-07-06-milestone-c-channels-reactor.md` — Milestone C — Capability-Scoped Channels + Reactor Implementation Plan _(plan, 2026-07-06)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-07-07-milestone-d-supervisor-crash-boundary.md` — Milestone D — Supervisor + Crash Boundary Implementation Plan _(plan, 2026-07-07)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-07-07-milestone-e-exceptions.md` — Milestone E — full `on:do:` / `ensure:` Implementation Plan _(plan, 2026-07-07)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-07-08-zephyr-pulse-primitive.md` — Zephyr Pulse-Primitive Implementation Plan _(plan, 2026-07-08)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-07-09-rung3-cbor-int64-wire-seam.md` — Rung 3 — CBOR + Int64 Wire Seam Implementation Plan _(plan, 2026-07-09)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-07-10-on-target-measurement.md` — On-Target Measurement Implementation Plan _(plan, 2026-07-10)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-07-10-rung5-udp-transport-l1.md` — Rung 5 — UDP transport (Layer 1) native_sim proof Implementation Plan _(plan, 2026-07-10)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-07-10-rung6-udp-bridge-l2.md` — Rung 6 — Layer 2: reactive channel↔UDP bridge (CBOR) — Implementation Plan _(plan, 2026-07-10)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-07-11-rung7-openthread-l3.md` — Rung 7 — Layer 3: NetBridge over real OpenThread (MTD child) — Implementation Plan _(plan, 2026-07-11)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-07-11-rung8-ontarget-timer.md` — Rung 8 — On-target timer seam + self-gating Thread node — Implementation Plan _(plan, 2026-07-11)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/2026-07-13-rung9-cells.md` — Rung 9 — Cells: a name that has a value — Implementation Plan _(plan, 2026-07-13)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/block-inlining-status.md` — Block Inlining — Track Status _(plan, 2026-07-03)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/milestone-a-footprint-delta.md` — Milestone A footprint delta (Task 10c) _(plan, 2026-07-04)_
- `~/workspace/nsl-tuvm/docs/superpowers/plans/pared-conformance-subset.md` — Pared conformance subset (Milestone A) _(plan, 2026-07-03)_

### ~/workspace/porta/docs

- `~/workspace/porta/docs/ARCHITECTURE.md` — Porta system architecture _(record, 2026-06-10)_
- `~/workspace/porta/docs/CLI.md` — porta CLI reference _(record, 2026-06-08)_
- `~/workspace/porta/docs/DEVSDK.md` — porta dev-SDK (`devsdk/`) — the northbound contract _(record, 2026-06-07)_
- `~/workspace/porta/docs/GETTING-STARTED.md` — Getting started with porta _(record, 2026-06-26)_
- `~/workspace/porta/docs/PROTOCOL-NSL-RECORDS.md` — Porta ↔ nsl — the record schemas _(record, 2026-07-13)_
- `~/workspace/porta/docs/PROTOCOL-NSL.md` — Porta ↔ nsl node protocol — the space-sync model _(record, 2026-07-13)_
- `~/workspace/porta/docs/PROTOCOL.md` — Porta wire protocol _(record, 2026-07-12)_
- `~/workspace/porta/docs/demo/porta-show-and-tell-script.md` — porta — show-and-tell narration script _(record, 2026-06-07)_
- `~/workspace/porta/docs/demo/show-and-tell-template.md` — Show-and-tell walkthrough template _(record, 2026-06-07)_
- `~/workspace/porta/docs/design/2026-06-06-web-telemetry-console-panels-design.md` — Web telemetry: per-node console panels + telemetry node filter — Design _(design)_
- `~/workspace/porta/docs/design/2026-06-07-panic-decode-link-design.md` — Panic decode link (porta → nodus) — design _(design)_
- `~/workspace/porta/docs/design/2026-06-26-profile-verb-design.md` — `profile` verb — remote target-execution profiling — design _(design)_
- `~/workspace/porta/docs/plans/2026-05-23-gateway-b1-tftp-free-core.md` — Gateway B1 — TFTP-free core Implementation Plan _(record, 2026-05-23)_
- `~/workspace/porta/docs/plans/2026-05-23-gateway-b2-wire-device-hardware.md` — Gateway B2 — wire + device + hardware Implementation Plan _(record, 2026-05-23)_
- `~/workspace/porta/docs/plans/2026-05-23-tftp-engine-transport-handler-split.md` — TFTP engine / transport / handler split — Implementation Plan _(record, 2026-05-23)_
- `~/workspace/porta/docs/plans/2026-05-24-config-self-heal.md` — Config Self-Heal Implementation Plan _(record, 2026-05-25)_
- `~/workspace/porta/docs/plans/2026-05-24-d5-observed-config-echo.md` — D5 — Observed-Config Echo Implementation Plan _(record, 2026-05-24)_
- `~/workspace/porta/docs/plans/2026-05-24-m2-2-down-path-config.md` — M2.2 Down-Path (Config / Setpoints) Implementation Plan _(record, 2026-05-24)_
- `~/workspace/porta/docs/plans/2026-05-24-m2-telemetry.md` — M2 Telemetry (up-path) Implementation Plan _(record, 2026-05-24)_
- `~/workspace/porta/docs/plans/2026-05-26-porta-nodus-split.md` — porta → porta/nodus Split Implementation Plan _(record, 2026-05-26)_
- `~/workspace/porta/docs/plans/2026-05-27-go-mainline-restructure.md` — Go-Mainline Restructure (sub-project A) — Implementation Plan _(record, 2026-05-27)_
- `~/workspace/porta/docs/plans/2026-05-27-porta-go-config-plane.md` — porta Go Core — B2 Config Plane Implementation Plan _(record, 2026-05-27)_
- `~/workspace/porta/docs/plans/2026-05-27-porta-go-core-data-plane.md` — porta Go core data plane (B1) Implementation Plan _(record, 2026-05-27)_
- `~/workspace/porta/docs/plans/2026-05-28-porta-b4a-multi-listener.md` — porta Go Core — B4a Multi-Listener Serve Implementation Plan _(record, 2026-05-28)_
- `~/workspace/porta/docs/plans/2026-05-28-porta-b4b-mcp-read-surface.md` — porta B4b — MCP read surface Implementation Plan _(record, 2026-05-28)_
- `~/workspace/porta/docs/plans/2026-05-28-porta-b4c-htmx-operator-ui.md` — porta B4c — htmx Operator UI Implementation Plan _(record, 2026-05-28)_
- `~/workspace/porta/docs/plans/2026-05-28-porta-go-telemetry-plane.md` — porta Go Core — B3 Telemetry Plane Implementation Plan _(record, 2026-05-28)_
- `~/workspace/porta/docs/plans/2026-05-31-command-lifecycle-ui.md` — Command Lifecycle Badges + Operator-UI Reorg — Implementation Plan _(record, 2026-05-31)_
- `~/workspace/porta/docs/plans/2026-06-02-cli-api-client-s2.md` — CLI-as-API-client (S2) Implementation Plan _(record, 2026-06-02)_
- `~/workspace/porta/docs/plans/2026-06-02-control-plane-api-s1.md` — Control-plane HTTP API (S1) Implementation Plan _(record, 2026-06-02)_
- `~/workspace/porta/docs/plans/2026-06-02-monitor-over-api-s5.md` — S5 — `porta monitor` over the control-plane API — Implementation Plan _(record, 2026-06-02)_
- `~/workspace/porta/docs/plans/2026-06-02-porta-run-over-api-s3.md` — `porta run` over the API (S3) — Implementation Plan _(record, 2026-06-02)_
- `~/workspace/porta/docs/plans/2026-06-02-tools-toit-phase1.md` — `/tools/toit` Phase 1 — `porta run` + node identity (Implementation Plan) _(record, 2026-06-02)_
- `~/workspace/porta/docs/plans/2026-06-02-web-node-identity-s4.md` — Web node identity (chip/SDK) visibility (S4) — Implementation Plan _(record, 2026-06-02)_
- `~/workspace/porta/docs/plans/2026-06-03-panic-decode-s6.md` — Panic Decode (S6) — porta side — Implementation Plan _(record, 2026-06-03)_
- `~/workspace/porta/docs/plans/2026-06-03-porta-devsdk-c1.md` — porta devsdk carve (C1) Implementation Plan _(record, 2026-06-03)_
- `~/workspace/porta/docs/plans/2026-06-04-reset-reason.md` — Reset Reason Implementation Plan _(record, 2026-06-04)_
- `~/workspace/porta/docs/specs/2026-05-23-porta-toit-gateway-design.md` — Design: Porta Toit gateway (command-queue control plane + sqlite) _(record, 2026-05-23)_
- `~/workspace/porta/docs/specs/2026-05-23-tftp-engine-transport-handler-split-design.md` — Spec A: TFTP engine / transport / handler split _(record, 2026-05-23)_
- `~/workspace/porta/docs/specs/2026-05-24-config-self-heal-design.md` — Config Self-Heal Design _(record, 2026-05-25)_
- `~/workspace/porta/docs/specs/2026-05-24-d5-observed-config-echo-design.md` — D5 — Observed-Config Echo Design _(record, 2026-05-24)_
- `~/workspace/porta/docs/specs/2026-05-24-m2-telemetry-design.md` — Design: Porta M2 — telemetry (`data_log`) + `gateway monitor` _(record, 2026-05-24)_
- `~/workspace/porta/docs/specs/2026-05-24-security-transport-evolution-design.md` — Design note: Security posture & transport evolution _(record, 2026-05-24)_
- `~/workspace/porta/docs/specs/2026-05-26-porta-nodus-split-design.md` — Splitting porta into `porta` (gateway) + `nodus` (node) — design _(record, 2026-05-26)_
- `~/workspace/porta/docs/specs/2026-05-27-porta-go-config-plane-design.md` — porta Go core — B2 config plane (design) _(record, 2026-05-27)_
- `~/workspace/porta/docs/specs/2026-05-27-porta-go-core-data-plane-design.md` — porta Go core data plane (sub-project B1) — design _(record, 2026-05-27)_
- `~/workspace/porta/docs/specs/2026-05-27-porta-go-mainline-restructure-design.md` — porta → Go-mainline restructure (sub-project A) _(record, 2026-05-27)_
- `~/workspace/porta/docs/specs/2026-05-28-porta-b4a-multi-listener-design.md` — porta Go core — B4a multi-listener serve (design) _(record, 2026-05-28)_
- `~/workspace/porta/docs/specs/2026-05-28-porta-b4b-mcp-read-surface-design.md` — porta B4b — MCP read surface: design _(record, 2026-05-28)_
- `~/workspace/porta/docs/specs/2026-05-28-porta-b4c-htmx-operator-ui-design.md` — porta B4c — htmx operator UI: design _(record, 2026-05-28)_
- `~/workspace/porta/docs/specs/2026-05-28-porta-go-telemetry-plane-design.md` — porta Go core — B3 telemetry plane (design) _(record, 2026-05-28)_
- `~/workspace/porta/docs/specs/2026-05-31-command-lifecycle-ui-design.md` — Command lifecycle badges + operator-UI reorg — design _(record, 2026-05-31)_
- `~/workspace/porta/docs/specs/2026-06-02-cli-api-client-s2-design.md` — CLI becomes an API client (S2) — writes over HTTP _(record, 2026-06-02)_
- `~/workspace/porta/docs/specs/2026-06-02-control-plane-api-design.md` — Control-plane HTTP API (S1) — server-as-workhorse, CLI-as-client _(record, 2026-06-02)_
- `~/workspace/porta/docs/specs/2026-06-02-monitor-over-api-s5-design.md` — S5 — `porta monitor` over the control-plane API — design _(record, 2026-06-02)_
- `~/workspace/porta/docs/specs/2026-06-02-porta-run-over-api-s3-design.md` — S3 — `porta run` over the control-plane API (Design) _(record, 2026-06-02)_
- `~/workspace/porta/docs/specs/2026-06-02-tools-toit-design.md` — `/tools/toit` — trainer-wheels toolchain for porta nodes _(record, 2026-06-02)_
- `~/workspace/porta/docs/specs/2026-06-02-web-node-identity-s4-design.md` — S4 — Web node identity (chip/SDK) visibility (Design) _(record, 2026-06-02)_
- `~/workspace/porta/docs/specs/2026-06-03-panic-decode-s6-design.md` — S6 — Client-side panic decode (design) _(record, 2026-06-03)_
- `~/workspace/porta/docs/specs/2026-06-03-porta-devsdk-nodus-flash-design.md` — porta dev-SDK + `nodus flash` toolchain — design _(record, 2026-06-03)_
- `~/workspace/porta/docs/specs/2026-06-04-reset-reason-design.md` — Neutral `reset` reason in the report, surfaced by porta _(record, 2026-06-04)_
- `~/workspace/porta/docs/superpowers/plans/2026-06-06-set-forward-verb.md` — set-forward verb + enriched telemetry entries (porta side) Implementation Plan _(plan, 2026-06-06)_
- `~/workspace/porta/docs/superpowers/plans/2026-06-06-web-telemetry-console-panels.md` — Web telemetry console panels + node filter Implementation Plan _(plan, 2026-06-06)_
- `~/workspace/porta/docs/superpowers/plans/2026-06-07-panic-decode-link.md` — Panic Decode Link Implementation Plan _(plan, 2026-06-07)_
- `~/workspace/porta/docs/superpowers/plans/2026-06-26-profile-verb.md` — `profile` verb — remote target-execution profiling — Implementation Plan _(plan, 2026-06-26)_

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
- `~/workspaceToit/nodus/docs/design/2026-06-12-timeout-classification-forward-echo-design.md` — Design: gateway-timeout classification (#16) + forward-gate echo (#15) _(design)_
- `~/workspaceToit/nodus/docs/design/2026-06-28-profile-node-side-design.md` — Design: profile verb — node side (nodus) _(design)_
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
- `~/workspaceToit/nodus/docs/specs/porta-coordination-profile-timeout.md` — porta coordination — `duration_s` on `profile_session` + derived lazy-timeout status _(record, 2026-06-28)_
- `~/workspaceToit/nodus/docs/specs/porta-coordination-set-forward.md` — porta coordination — `set-forward` verb + enriched `data?id=` entries (northbound-channels b) _(record, 2026-06-07)_
- `~/workspaceToit/nodus/docs/superpowers/plans/2026-06-08-node-config-ownership.md` — Node Config Ownership + `set-mode` Implementation Plan _(plan, 2026-06-08)_
- `~/workspaceToit/nodus/docs/superpowers/plans/2026-06-09-writeable-loop-sleep.md` — Writeable always-on `loop_sleep_s` — Implementation Plan _(plan, 2026-06-09)_
- `~/workspaceToit/nodus/docs/superpowers/plans/2026-06-12-timeout-classification-forward-echo.md` — Gateway-Timeout Classification (#16) + Forward-Gate Echo (#15) Implementation Plan _(plan, 2026-06-12)_
- `~/workspaceToit/nodus/docs/superpowers/plans/2026-06-28-profile-node-side.md` — Profile verb — node side (nodus) Implementation Plan _(plan, 2026-06-28)_
