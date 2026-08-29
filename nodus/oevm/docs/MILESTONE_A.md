# Milestone A — provenance and validation note

*This is a reference note, written once the milestone was green, for future
milestones to cite. It is not a pitch or a walkthrough — see `docs/superpowers/`
plans and `review/` for the day-by-day narrative.*

Milestone A's premise (spec §7): tuvm/nsl-tuvm pivot away from fibers, NLR, and
growable stacks toward a flat run-to-completion engine, proving the pared
conformance subset stays green and the engine shrinks. This note bags the
result: what was deleted, what was deliberately kept (including one late
reversal), the conformance state, the renumbering, the seed provenance, and the
measured footprint delta.

## 1. What was deleted

- **Fibers** — `fiberCreate:`/`fiberSwitch:value:`/`currentFiber` primitives,
  the Suspended/Dead/legacy fiber vtables, the RunnableFiberVtable scheduling
  loop, `struct Fiber`, `current_fiber`, `g_bootstrap_fiber`, the concurrency
  image field, and the Concurrency/Fiber stdlib facade + fiber gates on the
  nsl-tuvm side. The flat loop runs directly on `g_active_stack`; single
  execution context = Stack + current_frame.
- **NLR (non-local return)** — the compiler emit side (`^` no longer compiles
  out of an escaping closure; it is a compile error by design) and the VM
  unwind machinery: the `OP_NLR` handler, `do_unwind`, the four unwind globals
  (`unwind_target_reached`/`_return_value`/`_target_frame` + the symbol),
  `resumeWith:`/`executeCleanup` primitives, and the baked
  `MethodContext>>unwindTo:value:` method.
- **Reify-on-escape (E-series)** — `home_link_is_reified`, the reified-home-link
  decode branch in `home_ctx_frame`, and the RESERVED frame_abi.h wording. This
  was the only reify-family cut (see §2/§3 for what was kept).
- **Growable stacks** — `stack_ensure`/`stack_grow`, the CAP/GROW macros, rebase
  preludes, the straddle guard, and the dead reload blocks. Stacks are now
  fixed-capacity (`INIT_WORDS`); overflow aborts loudly instead of realloc'ing.
- **The v7 .oimg concurrency word** — the retired format-v7 tail word (the
  concurrency capability grant) is gone; format v8 has one fixed header shape.
- **Gravestone opcodes/registry/prim slots** — `OP_SEND_IC_RETIRED` (old 0x03)
  and `OP_NLR_RETIRED` (old 0x05), the reserved fiber vtable rows (6/12/13/14)
  and NLR symbol rows in `builtins.def`, and the retired fiber/NLR prim enum
  slots in dispatch.c — all removed rather than left as reserved gaps.

## 2. What was KEPT (explicitly)

- The flat single-stack interpreter (non-recursive; inherited from the seed's
  pre-Milestone-A derecursify work, not a Milestone-A change, but load-bearing
  for everything above).
- Precise GC.
- The `.oimg` loader, now format v8.
- The capability/confinement seam.
- The mirror/debugger.
- The vestigial access subsystem (oevm#36) — revived rather than deleted
  (`tests/test_access.c`, `tests/test_enclosures.c` were culled as fiber-era
  casualties in an earlier pass, then restored in Task 10a once identified as
  KEEP-subsystem coverage, not fiber residue).
- **Decision 2026-07-04 — the deferred-lift heap Context layer is KEPT**: `struct
  Context`, `MK_CONTEXT`/`LD_CTX`/`ST_CTX` (opcodes 0x15–0x17), and
  `captured_env`. First-class upward-escaping closures are the reactive model's
  foundation (`whenever: chan do: [:e | ...]` captures a temp from a frame that
  has since popped) — dropping the heap Context would collapse the run-to-completion
  replacement for fibers that the milestone's own pitch depends on. Only the
  reify E-series (the *reified-link* branding used to make cross-*stack* home
  links safe, which fibers alone required) was dead and removed; the Context
  mechanism itself is the escape-safe path, not balloon. See
  `review/2026-07-04-context-decision.md` for the full reasoning and the
  cross-check that the stripped engine still measures below the pre-R7 oevm
  baseline on C/H line count.

## 3. CV-4 resolution — `home_generation` / `current_generation_tag`

**Verbatim, controller-verified:** `home_generation` and
`current_generation_tag` are KEPT — written at frame push and at closure
creation. With NLR gone, no read currently guards an access against a stale
home. But the field pairs with the live heap-Context stale-home story (§2), and
the cost is one `u32`; removing it would be re-cheap later, while losing it now
risks a latent dangling-home bug once the Context layer's escape paths are
exercised harder (plan Task 8 CAUTION note). Left in place, unread today,
deliberately.

## 4. The renumber

- Opcodes are dense `0x00`–`0x18`, with `0x0E` = `OP_BREAK` (live, C-runner-only
  — not reachable from the compiler, used by `tests/test_debugger.c`'s hand-composed
  instruction words and similar single-step scaffolding).
- `BUILTIN_REGISTRY_VERSION` = 5.
- Prim ids are compact (no reserved fiber/NLR gaps; platform prims renumbered
  61–71 → 56–66 in the Task 9a compaction).
- `.oimg` format is v8: one fixed header layout, no version-gravestone
  acceptance (v3–v7 parsing dropped; every image is regenerated per-test by the
  harness, so no backward-compat load path was needed).
- No gravestones remain anywhere in the opcode/registry/prim surface — the
  Milestone-A end-state explicitly rejects reserved-slot-for-compat in favor of
  clean renumbering, since nothing outside this repo's own test harness reads
  these formats.

## 5. Conformance state

- 88 PASS / 0 FAIL / 12 SKIP on both Path A and Path B (Model-A/-B link sets).
- C unit-test runner (`tools/run_c_tests.sh`): 18 PASS / 2 SKIP
  (`test_oimg` and `test_solidified` retired pending fixture work — tracked in
  ROADMAP as P2, not a Milestone-A regression).
- 4 integration gates.
- Diff-oracle gate vs the frozen `oevm`/`ns-oevm` pair: 78 shared tests verified
  byte-identical stdout on both paths across both VMs (optional standalone gate,
  not wired into the mandatory `run_tests_oevm.py` gate list — see Task 10b).
- The `07_no_nlr` deletion-conformance suite: the `^`-inside-an-escaping-closure
  compile error is frozen API (`03_blocks/03_nlr_compile_fail.ns` now must
  compile-FAIL for methods and modules both, a semantics change from the frozen
  original documented in the pared-subset note).

## 6. The pared-subset definition

See
`nsl-tuvm/docs/superpowers/plans/pared-conformance-subset.md` for the full list
of excluded tests (fiber/NLR/growable-stack tests, host-deferred networking
tests) and the one semantics-changed test (`03_nlr_compile_fail.ns`).

## 7. Seed provenance

- `tuvm` ← `oevm@aea0af1` (commit `fa301d3`, 2026-07-03).
- `nsl-tuvm` ← `ns-oevm@188426e` (commit `abd9915`, 2026-07-03).
- `nsl-tests` ← `ns-tests@8ad658b`.

**No fully pre-fiber commit exists.** `oevm`'s root commit (`4465e36`) already
contains fibers and NLR ("Phase 4 completed — Fiber Scheduling Architecture");
phases 1–3 predate `git init` and were never committed.

The pre-R7 boundary — `oevm@cee55d4` / `ns-oevm@a327d60` — is the last commit
before the R7 language-level-concurrency effort (ucontext/swapcontext
switching, per-fiber C stacks, growable-stack machinery) landed. It is a
**delta reference only**: useful for scale/shape comparison (the stripped
engine's C/H line count and fiber-mention count both measure below it — see
§2), **never a revert target** — it still carries the simple Phase-4 fibers,
Phase-5 NLR, and a recursive interpreter, none of which match Milestone A's
end-state. See `review/2026-07-04-pre-fiber-baseline.md`.

## 8. Footprint delta summary

Measured with `tools/footprint.sh` (host-proxy build; `size -t` totals over the
engine objects, .oimg/image object excluded; RAM line added this task per
design addendum A2). Full method, per-object breakdown, and attribution
narrative: see
`nsl-tuvm/docs/superpowers/plans/milestone-a-footprint-delta.md`.

| | text | data+bss (RAM, host proxy) |
|---|---:|---:|
| baseline (seed, re-measured at `fa301d3`) | 39148 | 17253508 |
| after (measured at `943d9b5`) | 35227 | 17252832 |
| delta | -3921 bytes | -676 bytes |
| delta % | -10.02% | -0.0039% (-0.33% excluding gc.o's host-only bss arena) |

The seed re-measurement reproduced the banked Task-1 figure of 39148 exactly
(0% deviation), validating the method before banking the delta. The engine
shrank on both axes, as spec §7 requires — the largest single contributors were
the fiber deletion (Tasks 5-6) and the NLR unwind removal (Tasks 2-3); see the
linked note for the full (diffstat-based, labeled-as-estimate) attribution.

Note: `943d9b5` was branch HEAD at measurement time but the branch has since
moved (oracle cleanup + CV-1..CV-6 docs); engine sources (`src/*.c`, `src/*.h`)
are unchanged between `943d9b5` and the current HEAD, so the figures above
still hold — they are simply labeled by the commit actually measured rather
than by "HEAD".

## 9. Cross-validation against the pre-fiber design (CV-1..CV-6)

Controller-run 2026-07-04, after Task 10 (plan §"Cross-validation"). Reference:
the pre-fiber *design record* (`nodus/oevm/docs/archive/dev_process/phase{1,2,3}_walkthrough.md`),
since no pre-fiber commit exists (§7).

- **CV-1 struct inventory — PASS.** All Phase-1 structs present in the stripped
  `oevm.h` (`Object`, `Symbol`, `Binding`, `Vtable`, `MethodContext`, `Closure`);
  `struct Fiber` gone. Notes: `InlineCache` is absent because the de-recursify
  work retired it **pre-seed** (oevm `d6d94c4`) — not a Milestone-A deletion; the
  global method cache is its successor (`test_method_cache` green).
  `struct Context` (`frame_abi.h:77`) is intentionally retained (§2).
  `test_layout` green is the machine check.
- **CV-2 dispatch/send — PASS.** `send()`, method lookup, global method cache,
  `does_not_understand` intact; only fiber-switch/NLR-unwind branches removed.
  `test_method_cache` + `test_oevm` green in the 18/2 runner.
- **CV-3 mirrors/reflection — PASS.** The Phase-3 reflective surface and the
  reified stack walk are intact; the seed→stripped delta in `mirror.c` is one
  truthful comment reword (requestStep: now names the single global execution
  context). `test_debugger` green post-prune.
- **CV-4 lifecycle guard — RESOLVED: KEPT.** See §3.
- **CV-5 VM delta review — PASS.** Every seed→stripped file delta maps to an
  intended Milestone-A change (strip Tasks 2–8n, registry 9a, renumber+v8 9b,
  regenerated image, comment sweep 10a); no collateral edits to KEEP subsystems.
- **CV-6 compiler fiber-delta — PASS.** The Task-4 stdlib strip is the exact
  mirror image of the fiber facade addition in frozen `ns-oevm@88fdf6e`
  (+63 lines, `class Concurrency` + `class Fiber` + factory); zero
  fiber/concurrency residue remains in `lib/kernel.ns`.
