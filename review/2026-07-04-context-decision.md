# Task 8 design question: keep the deferred-lift heap Context?

*2026-07-04 — review-agent answer to the question the milestone-a controller routed
here before pausing Task 8. Recommendation, pending David's sign-off.*

## The question (as recorded in the ledger)

> Does Milestone A keep first-class upward-escaping closures — which REQUIRE the
> deferred-lift heap Context — or does the run-to-completion model drop them
> (then the heap Context + reify can both go)?

## Recommendation: KEEP upward-escaping closures. Keep the heap Context (B). Delete only the reify E-series (A).

This matches the controller's own re-escalation evidence and, decisively, the
review kit's already-pinned language surface:

1. **The reactive model IS stored closures.** The run-to-completion replacement for
   fibers is "register a handler, return, let events drive it." Every staged artifact
   does exactly this: `fsm.ns:19` and `demo-node.ns:124` register
   `whenever: chan do: [:e | machine handle: e]` — a closure that captures `machine`
   (a temp of the setup frame) and runs long after that frame pops. With a
   relative-SMI `home_ctx` into a popped frame, that's a dangling access. Drop
   upward-escaping closures and the pitch's own alternative-to-fibers collapses.

2. **The spec already legalizes escape.** WHY.md bans fibers, NLR, and growable
   stacks — the heap Context is none of these. The staged deletion test
   `ta-deletion/01_nlr_escape_compile_error.ns` makes only `^` *inside* an escaping
   closure a compile error; the escape itself is legal by construction. If escape
   were dropped, that test (and the L1 combinator style rules built around
   "blocks, no `^`") would be incoherent.

3. **The mechanism is the escape-SAFE path, not balloon.** The compiler already
   splits real escaping captures (heap Context, `OP_LD_CTX`/`ST_CTX`) from
   non-escaping inlined-loop vars (`home_ctx`/lexical). 56_upward_closure is
   retained-and-passing proof. The 54k `LD_CTX` executions in the AB run are the
   ordinary cost of closures being used heavily, not fiber residue; if that ever
   shows up in footprint/perf, the lever is compiler capture-analysis, not deletion.

4. **Deep-sleep argument doesn't apply.** WHY.md's "a suspended fiber's stack cannot
   survive deep sleep" indicts *stacks held across suspension*. A heap Context hung
   off a live closure is ordinary heap data — it persists or is rebuilt exactly like
   every other object; no special problem.

5. **The reify E-series (A) is genuinely dead** — `home_link_is_reified` has no live
   caller, the reified branch already aborts, frame_abi.h documents it as RESERVED.
   Delete it; that removes the last piece of the fiber-era plan (reify-on-escape was
   only needed because fibers made cross-stack home links possible).

## Consequent milestone unblocking

- **Task 8 (narrowed):** delete reify E-series (A) only — `home_link_is_reified`,
  the reified-home-LINK branch in `home_ctx_frame`, the RESERVED frame_abi.h wording.
  Keep struct Context, ContextVtable, OP_MK_CONTEXT/LD_CTX/ST_CTX, captured_env,
  the GC Context trace, and `home_generation` (entangled, keep-when-in-doubt stands).
- **Task 9b:** opcode-enum renumber runs once, with Context opcodes 0x17–0x19 keeping
  live slots; only `OP_SEND_IC_RETIRED`/`OP_NLR_RETIRED` (+ nothing new from Task 8)
  compact away.
- **Task 10:** unchanged, plus the review-tree fold-ins already listed in the ledger
  (ta-deletion tests + `expect-compile-error:` harness extension, RAM-line banking).
- **G3/G4 check:** keeping the heap Context forecloses nothing — it's a plain heap
  object under either a moving or non-moving GC, and touches no .oimg format field.

## Baseline cross-check (see 2026-07-04-pre-fiber-baseline.md)

The stripped tuvm at 4e24ba2 measures BELOW the pre-R7 oevm baseline while keeping
the flat interpreter:

| Measure (src/) | oevm@cee55d4 (pre-R7) | oevm@aea0af1 (tip) | tuvm@4e24ba2 (stripped) |
|---|---|---|---|
| C/H lines | 10,049 | 10,652 | **9,844** |
| ucontext refs | 0 | yes (R7) | **0** |
| `fiber` mentions in vm.c | 38 (real fibers) | 40+ | **22 — comments only** |

Every remaining `fiber` string in tuvm src/ is prose (stale comments queued for the
Task 10 sweep, plus intentional v7-image-compat notes in oimg_loader.c). No fiber
machinery survives. The VM is now *simpler than it ever was in oevm's history*:
cee55d4 still carried real fibers + NLR + a recursive interpreter.
