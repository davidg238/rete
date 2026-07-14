# De-recursify Phase 2 (redesigned) — `self`-migration + relativize `home_ctx` (design)

**Status:** ✅ DONE + REVIEWED CLEAN 2026-07-02. Both parts shipped on branch
`derecursify-deferred-phase2` (not yet merged/pushed — awaiting David). Part 1
(self-migration, Approach B): oevm `05e0aab` + ns-oevm `d9b1cd2` (get_self_reg /
implicit path) and ns-oevm `e77f980` (explicit varRef-self path — found via §1's
kernel-scheduler crash). Part 2 (the flip): oevm `f269606`. Per-task reviews clean
(spec ✅ + quality; Part 2 reviewer verified the fib_base encode/decode symmetry,
reified-guard dormancy, decode-side consistency, both-paths validity). Suite **84/0/6
BOTH paths + all 5 gates incl. fibers-gc-stress** (the twice-crashed cases 45_fibers /
55_fiber_capture now green) + `-D__ZEPHYR__` syntax-clean + grep proof (4 relative
encode sites, 0 absolute assigns, reified-guard never fired). No dialect row, no
`.oimg` format bump. Deferred: inlined-loop-var cross-fiber captures (§5).

**Status (design approval):** APPROVED (approach + phasing) 2026-07-02, brainstormed with David.

**Supersedes:** the original Phase-2 section of
`2026-07-01-derecursify-deferred-lift-growable-stacks-upward-closures-design.md`
and its plan `ns-oevm/docs/superpowers/plans/2026-07-02-derecursify-deferred-lift-phase2-relativize-home-ctx.md`
(both assumed `home_ctx` was vestigial cross-fiber; that premise was falsified — see §1).

---

## 1. Why the first Phase-2 attempt was blocked (the falsified premise)

The original plan assumed that after deferred-lift Phase 1, captured-variable access
lived entirely in heap `Context` temp-vectors, leaving `home_ctx`'s only role the
same-fiber NLR `^`-return target — so relativizing it would be behavior-equivalent.

**That premise is false.** Phase 1 *deliberately* kept `self` on the absolute
`home_ctx` link (`ns_compile_oevm.py` `resolve_name`: `self` is EXCLUDED from
Context-homing; only ordinary args/temps move to the temp-vector). Any nested block
reads its **enclosing `self`/slots** by walking `home_ctx`:
`get_self_reg` emits `OP_LD_LEXICAL base, outer_depth, 0` — walk the `home_ctx` chain
`outer_depth` frames to the method, read register 0 (the receiver). `get_self_reg` is
pervasive: every implicit-receiver send, every slot access, and every explicit `self`
inside a block routes through it.

A **fiber block** that touches an enclosing slot (e.g. `concurrency` in
`55_fiber_capture.ns`) does this **cross-fiber**: the block runs on the fiber's own
Stack, but its `home_ctx` refers to the spawner's Stack. Under the absolute encoding
this works (the pointer is a global address into the spawner's still-live Stack).
Relativized, `home_ctx_frame` decodes the offset against the *current* fiber's
`vm_stack` base → a garbage frame with `closure == NULL` →
`primitive_method_context_reg_at` dereferences `closure_entry(NULL)` → SIGSEGV.

Proven by bisection: with the relativization edits stashed, `55_fiber_capture` prints
`before10/mutated/after99/done`; with them applied it SIGSEGVs (identical backtrace for
`55_fiber_capture` and the `fibers-gc-stress` gate). (`shared` itself is fine — it *is*
Context-homed; only the enclosing-`self` read rides `home_ctx`.)

**Conclusion:** `home_ctx` is NOT vestigial cross-fiber — it is the live path by which
any nested block reads its enclosing `self`. The genuine prerequisite for relativizing
`home_ctx` is to make enclosing-`self` reads **fiber-independent** first.

## 2. Grounded findings (current engine)

- **Sole `BytecodeClosure` creator:** `make_closure` (`dispatch.c:634`), called from
  `OP_CLOSURE` (`vm.c:440`) and `PRIM_createClosure` (`dispatch.c:774`). One place to
  set a new closure field.
- **`struct Closure`** (`oevm.h:138`) already carries a runtime-only `captured_env`
  (the enclosing `Context`, deferred-lift Phase 1) — Closures are never `.oimg`-
  serialized, so adding another runtime-only field is free of format concern.
- **`get_self_reg`** (`ns_compile_oevm.py:736`): for a block scope walks `home_ctx`
  (`OP_LD_LEXICAL … , 0`); for a method scope returns register 0 directly.
- **A block's `self` is a constant** — it is exactly the receiver of its home method.
  A method and all its lexically-nested blocks share one receiver. So `self` can be
  snapshotted once, per closure, at creation time.
- **Free opcode:** `0x1A` (0x16–0x19 are `OP_BLOCK_INVOKE`/`OP_MK_CONTEXT`/`OP_LD_CTX`/
  `OP_ST_CTX`).
- **The decode chokepoint** `home_ctx_frame` (oevm `6e988af`, already merged on the
  branch) routes every `home_ctx` read through one function; the flip flips its non-SMI
  branch to a reified-Context fail-fast. Kept and reused.
- **Remaining `home_ctx` register-reads after `self`-migration:** only the inlined-loop
  induction-var captures (excluded from Context-homing in Phase 1). No test reads one
  cross-fiber; deferred (§5).

## 3. Approach (decided) — B: `self` on the `Closure`

`self` becomes a per-closure snapshot, computed O(1) at creation and read
fiber-independently — chosen over Approach A (`self` as a heap-`Context` slot) because
it is localized to `make_closure` + `get_self_reg` and does **not** disturb the Phase-1
capture analysis (which Phase 1 found fragile: Context-homing inlined-loop vars
crashed).

**Mechanism:**
1. `struct Closure` gains `oop captured_self` (runtime-only, mirrors `captured_env`).
2. New opcode **`OP_LD_SELF = 0x1A`** (`dest`): VM handler
   `ctx->regs[dest] = ((struct Closure *)ctx->closure)->captured_self;`. Compiled on
   both paths (dispatch switch + solidified generator); one bytecode word.
3. `make_closure` sets `captured_self` at creation (same-fiber, `home_ctx` still valid
   whatever its encoding — the discriminator here is NULL-vs-non-NULL, encoding-
   independent):
   ```c
   struct Closure *ccl = current_frame ? (struct Closure *)current_frame->closure : NULL;
   oop self_val;
   if (ccl == NULL)                 self_val = NULL;                  /* no frame (boot) */
   else if (ccl->home_ctx == NULL)  self_val = current_frame->regs[0];/* method frame: receiver */
   else                             self_val = ccl->captured_self;    /* block frame: propagate */
   ((struct Closure *)cl)->captured_self = self_val;
   ```
   Inductive correctness: the outermost block is created in a method frame (reads
   `regs[0]`); inner blocks propagate. Method closures (from the image, `home_ctx==NULL`,
   never `make_closure`-created) never need it — `get_self_reg` returns reg 0 for methods.
4. Compiler — **there are TWO `self`-access emit paths, and BOTH must emit `OP_LD_SELF`**
   (this was found the hard way: a first cut migrated only path (a) and the kernel
   scheduler's fiber entry block still crashed under the flip via path (b)):
   - **(a) implicit receiver / slot base — `get_self_reg`:** for a block scope emit
     `OP_LD_SELF dest` (was `OP_LD_LEXICAL dest, outer_depth, 0`); method scope
     unchanged. Covers implicit self-sends and slot/ivar reads (`current` etc.).
   - **(b) explicit `self` value/receiver — `resolve_name` → `("lexical", 0, depth)`:**
     when a `varRef` named `self` resolves as a cross-frame lexical (e.g. `self complete`,
     or `self` passed as an argument), the `varRef` "lexical" emit must special-case
     `name == "self"` and emit `OP_LD_SELF dest` instead of `OP_LD_LEXICAL dest, depth, 0`
     (and skip the `_ctx_patch`, since `self` is not Context-homed). This is the path the
     kernel `Concurrency` entry block `[ current block value. self complete ]` uses for
     `self complete`. Other cross-frame lexicals (inlined-loop-var captures) stay
     `OP_LD_LEXICAL` — the documented deferred limitation (§5).

   Verified: recompiling `45_fibers` with BOTH paths migrated turns the entry block's last
   `OP_LD_LEXICAL` (`0x00010512`) into `OP_LD_SELF` (`0x0000051A`), leaving the block with
   ZERO `home_ctx`-walking reads; the full suite then passes 84/0/6 both paths incl.
   `fibers-gc-stress` with the flip applied.

**Behavior-equivalent:** every `self` read returns the same receiver, just sourced from
the closure instead of the parent stack frame. No dialect row (no new observable).
No `.oimg` format bump (`captured_self` runtime-only; new opcode rides the instruction
encoding, image-visible only as a bytecode the loader already passes through).

## 4. Phasing (two internally-consistent, green-gated parts)

The Task-1 decode chokepoint (oevm `6e988af`) already landed on the branch and is kept.

### Part 1 — `self`-migration (Approach B)
Add `captured_self` + `OP_LD_SELF` + `make_closure` snapshot + compiler `get_self_reg`
change. **Independently landable and behavior-equivalent.**

**Gate:** full suite BOTH paths (`OEVM_PATH=AB`) = 84/0/6, all 5 gates
(grant/debugger-integration/portability-32/fibers-gc-stress/boot-gc-asan) PASS;
`-D__ZEPHYR__ -fsyntax-only` clean. Structural check: `get_self_reg` no longer emits
`OP_LD_LEXICAL` for blocks (self reads route through `OP_LD_SELF`); `OP_LD_LEXICAL`
remains only for inlined-loop-var captures.

### Part 2 — the flip (relativize `home_ctx`)
The exact edits reverted from the first attempt: the 4 `home_ctx` encode sites
(`push_frame`, `run_interpreter` setup, `primitive_fiber_create` — against the *new
fiber's* base —, `make_closure`) → `home_link_encode_relative`; the `home_ctx_frame`
non-SMI branch → reified-Context fail-fast `abort()`; the two `gc.c` `mark(home_ctx)`
confirming comments; truthful "now relative" comments. Now safe: after Part 1,
`home_ctx` carries no cross-fiber `self` read; its remaining reads (NLR `^`,
super-resolution via `get_top_level_method_closure`, `primitive_closure_home` feeding
the residual inlined-loop `OP_LD_LEXICAL`) are same-fiber in the whole suite.

**Gate:** full suite BOTH paths = 84/0/6 incl. `55_fiber_capture`
(`before10/mutated/after99`) and `fibers-gc-stress` PASS; all 5 gates;
`-D__ZEPHYR__` syntax-clean. Structural grep proof: all 4 encode sites use
`home_link_encode_relative`; no `home_ctx = (oop)ctx / (oop)current_frame` remains; the
reified-guard `abort()` never fires during the suite (proof every live home is NULL or
relative). The frame is now growth-ready (frozen `frame_abi.h` dynamic + lexical links
both relative).

## 5. Deferred (documented limitation)

Inlined-loop induction vars (`to:do:`/`to:by:do:`/inlined `whileTrue:` block temps)
captured by a nested **real** block stay register + `home_ctx`-homed (Phase 1's
excluded set, `real_captures = [n for n in captured_locals if n in local_vars]` minus
inline-managed ones). After Part 2 they are read via the relative `home_ctx`. A block
that reads such a var **cross-fiber** would decode against the wrong base — but instead
of the pre-relativization "works by accident" (absolute pointer into a still-live
spawner stack), it now **fail-fasts** (garbage-frame guard / crash). No existing test
exercises this; it is already a documented deferred limitation of the closure model.
Relativization changes its *failure mode* (silent-maybe-correct → fail-fast), which is
an improvement, and is documented here and in the deferred-lift memory. The full fix
(migrate these off `home_ctx` too, or the general reify-on-escape) belongs with the
growth/escape phase.

**The three deferred cross-fiber `home_ctx` readers** (all unexercised by the suite, all
fail-fast-not-corrupt, all to be fixed together in the growth/escape phase) — named here
so a later phase covers all three, not two (final-review catch):
1. **inlined-loop-var captures** — `OP_LD_LEXICAL`/`OP_ST_LEXICAL` for a loop var read by a
   nested block (above).
2. **non-local return `^`** from a block whose home is on another fiber's stack (the NLR walk
   decodes `home_ctx`).
3. **private-method access check** — `check_access_allowed` → `find_defining_vtable` →
   `get_top_level_method_closure(current_frame->closure)` walks the block closure's home chain
   when a private method is sent on `self` from inside a fiber-entry block. Not exercised (the
   fiber-entry frame's `regs[0]` is the closure, not `self`, so `receiver != caller_self`
   short-circuits before the walk).

## 6. Cross-cutting

- **Both image paths:** `captured_self` is a runtime-only Closure field; `OP_LD_SELF`
  is a bytecode both the dispatch switch and the `solidify.py` generator must handle.
  Verify the serializer/loader need no change (no new `.oimg`-visible structure; the
  opcode is already carried in method bytecode the loader passes through verbatim).
- **GC:** `captured_self` is a heap oop (the receiver) and must be traced by the
  Closure tracer — Closures are traced by the generic word-walk `trace_object`, which
  already covers `captured_env`; confirm the added field is within the traced size
  (it is: `sizeof(struct Closure)` grows by one word and `trace_object` uses the header
  size). No special tracer needed.
- **`home_generation` / NLR validity:** unchanged. `self`-migration does not touch the
  `^`-return path; the flip keeps `home_generation` as-is.
- **`-D__ZEPHYR__`:** compiles the identical path POSIX runs (core Phase 3 removed all
  `__ZEPHYR__`-conditional fiber code); each part stays syntax-clean under it.

## 7. Validation

Per-part gate = full suite both paths (`OEVM_PATH=AB python3 tools/run_tests_oevm.py`
from `ns-oevm`) + all 5 gates + `-D__ZEPHYR__ -fsyntax-only` clean (from `oevm`).
Baseline to preserve: **84 pass / 0 fail / 6 skip both paths.** Part 2 additionally
runs the structural grep proof and depends on `55_fiber_capture` + `fibers-gc-stress`
(the exact tests the first attempt crashed) passing.

## 8. Open items for the implementation plan

- Exact `OP_LD_SELF` wiring: dispatch-switch case in `vm.c`, the `solidify.py`
  generator's opcode table (Path A), and the compiler constant + `get_self_reg` emit.
- Confirm `make_closure`'s `current_frame->closure` is always a valid Closure at every
  call site (OP_CLOSURE and PRIM_createClosure) — and that reading `regs[0]` of a method
  frame yields the receiver at those sites.
- Confirm the Closure GC trace covers `captured_self` (size-based word-walk) with a
  targeted check under `OEVM_GC_STRESS=1`.
- Part 2 reuses the reverted edits verbatim; re-derive them from the first plan's Task 2
  (they were captured in the task-2 report before revert).
