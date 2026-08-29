> **HISTORICAL — archived 2026-08-29.** Fibers, NLR and growable stacks were DELETED in Milestone A (2026-07-04, `docs/MILESTONE_A.md` §1); the VM is run-to-completion. What this document built and which SURVIVED is the flat non-recursive interpreter and the heap `Context` layer (`MILESTONE_A.md` §2). Read it for how the interpreter got flat, not for what the VM does. Current architecture: `docs/vm.md`.

# De-recursify core — flat non-recursive dispatch (design)

**Status:** DESIGN APPROVED (scope + phasing) 2026-06-30, brainstormed with David.
Supersedes the L2/L3/L4 *ordering* in `fiber_implementation.md §7` (see "Reorder"
below). **All three phases SHIPPED 2026-07-01** — Phase 1 (lowering, 2026-06-30),
Phase 2 (the flip, 2026-07-01), Phase 3 (fibers + demolition, 2026-07-01). See §5.

**Predecessors:** L0 FROZEN (`src/frame_abi.h`), L1 SHIPPED (first-class `Stack`
heap object + relative `prev_frame` + precise GC). This design is the "core
de-recursify" that turns the recursive C `run_interpreter` into one flat loop, so a
fiber switch is just swapping the heap `Stack` — killing the Zephyr hijack's
activation-reuse bug (`ABBBAA`).

---

## 1. Scope (David's call, 2026-06-30)

**IN — core de-recursify only:**
- Flat, non-recursive `run_interpreter` loop (one C activation per fiber).
- `OP_BLOCK_INVOKE` — every block invoke pushes a frame in the loop (no C recursion).
- In-loop unwinder (NLR / `ensure:` / `ifCurtailed:`), replacing the global
  `unwind_target_reached` flag + recursive-return NLR.
- Fibers = swap the first-class `Stack`. **Delete** ucontext, `fiber_trampoline`,
  `g_fiber_registry`, the `#ifdef __ZEPHYR__` hijack (4 blocks), and the **entire
  conservative C-stack scan + all Task 2b apparatus** (precise stack-slot roots
  suffice once dispatch is flat).

**OUT — deferred to a later, separable lift:**
- Heap-closure / temp-vector (`Context`) rework → `home_ctx` **stays an ABSOLUTE
  pointer**. Valid because the core lift keeps stacks **fixed** (no growth), so an
  absolute lexical link never dangles.
- Growable per-fiber stacks.

**Why these are safely deferrable:** the flat loop + in-loop unwinder need only the
DYNAMIC link (`prev_frame`, already relative from L1) and the LEXICAL link
(`home_ctx`); the latter works as an absolute pointer while stacks don't move.
Growth (moving stacks would dangle absolute `home_ctx`) and cross-fiber-escaping
closures are the only things that force the heap-closure rework — neither is in the
core lift. (Caveat to resolve in the plan: a fiber entry block created on the
spawner's stack but run on the fiber's stack reads `home_ctx` across stacks; fine
while the spawner frame is live — confirm the fiber-facade lifetime guarantees this,
else the entry block's captures need boxing. Today's `Concurrency` facade keeps the
spawner frame live, so this holds.)

---

## 2. Grounded findings (what actually recurses)

Control flow is **already lowered**: `ifTrue:`/`ifFalse:`, `and:`/`or:`,
`whileTrue:`/`whileFalse:`/`to:do:`/`to:by:do:` are compiler-inlined to
branches/loops for **literal** blocks (the block-inlining track is COMPLETE —
`ns-oevm/docs/superpowers/plans/block-inlining-status.md`). So §7's "L4 control-flow
lowering" is largely already done.

The only residual C recursion into `run_interpreter`:
1. **Method-send** — `dispatch.c:363` (DNU) and `dispatch.c:456` (`return
   run_interpreter(method, self, argc, args)`).
2. **Block-invoke *fallbacks*** — `PRIM_to_do`/`PRIM_to_by_do` (dispatch.c:738/752),
   `PRIM_ifTrueIfFalse`/`PRIM_ifTrue`/`PRIM_ifFalse` (dispatch.c:948/956/964), and
   `value`/`value:`/`valueWithArguments:` — the DYNAMIC fallback for blocks the
   compiler could not inline (block held in a variable / passed as an arg / invoked
   via `value:` / combinator with a non-literal block receiver).

The existing Zephyr hijack already implements "deliver a callee result into the
caller's register without a C return" (`vm.c:422-445`, the `if (ctx !=
current_frame)` reload). The flat `OP_RETURN` **generalises exactly this** decode.

---

## 3. Key insight — lowering is the prerequisite (reorder §7)

The one hard core is replacing recursive `run_interpreter` with a flat loop, and
that is only *clean* if **no C primitive invokes a block recursively**. So
block/combinator lowering is a **prerequisite of the flip, not a follow-on**. §7's
order (L2 flat dispatch first, L4 lowering later) forces a
flat-methods/recursive-combinators **hybrid**. Reverse it: lower first (on the
stable recursive engine, suite green), then flip once every invoke routes through
`OP_SEND`/`OP_BLOCK_INVOKE`.

---

## 4. End-state architecture

- **One flat `run_interpreter`**, entered once per fiber. Every invoke — method AND
  block — pushes a frame (relative `prev_frame`, save caller `pc` into the caller
  frame's `saved_pc`), sets `current_frame`, `pc = target instructions`, continues.
- **`OP_RETURN`** pops the frame, restores the caller's `pc`/`method`/`regs` via the
  dynamic link, delivers the result into the **caller's dest register** (generalise
  `vm.c:422-445`), continues. The loop bottoms out **only at the fiber's entry
  frame** — detected by `prev_frame == NULL` (the L0 stack-bottom sentinel). For
  main that returns to C; for a spawned fiber that means the fiber is done →
  transition Dead + switch to scheduler.
- **`OP_BLOCK_INVOKE`** — same non-recursive frame push; `home_ctx` taken from the
  closure (absolute, per scope). `value`/`value:`/`valueWithArguments:` compile to
  it. The recursive combinator/block fallback primitives are **deleted** once the
  compiler lowers their remaining (non-literal) cases to a bytecode loop/branch that
  drives `OP_BLOCK_INVOKE`.
- **In-loop unwinder** (`OP_UNWIND`): `^` / `ensure:` / `ifCurtailed:` walk
  `prev_frame` links to the home/target frame, reposition `sp`/`pc`, replay cleanup
  blocks as bytecode. Replaces the `unwind_target_reached`/`unwind_target_frame`
  global-flag protocol (`vm.c:449-462`) and the recursive-return propagation.
- **Fibers** = a `Stack`. `fiberSwitch:value:` swaps
  `g_active_stack`/`vm_stack`/`current_frame` and parks/loads `pc` (the spike's
  `OP_TRANSFER`, ~6 lines). The loop simply continues on the new Stack. DELETE
  ucontext / trampoline / registry / hijack / conservative C-stack scan / per-fiber
  malloc'd C stacks.
- **Debugger hook** (§8 risk): stays a bounded *nested* `run_interpreter` call —
  safe because it fires synchronously in the active fiber, result discarded, and no
  fiber switch crosses it. Revisit only if that invariant changes.

---

## 5. Phasing (each phase internally consistent)

**Phase 1 — finish lowering** ✅ **DONE 2026-06-30** (branch `derecursify-phase1`, both
paths, suite 78/0/6 + all 5 gates). Introduced one `OP_BLOCK_INVOKE` (opcode `0x16`,
operand layout identical to `OP_SEND`; fast-path invokes a `BytecodeClosure` as a
block, else falls back to `send()`); compiled `value`/`value:`/… (fixed-arity
`value`-family selectors) to it; lowered every non-literal combinator fallback
(`ifTrue:`-family, `and:`/`or:`, `to:do:`, `to:by:do:` incl. runtime-direction step,
`whileTrue:`/`whileFalse:`) to bytecode branch/loop driving `OP_BLOCK_INVOKE` via the
shared `emit_block_value` compiler helper. The recursive engine executes
`OP_BLOCK_INVOKE` via a (uniform) recursive `run_interpreter` call for now. **Deleted
the dead recursive combinator primitives** (`PRIM_ifTrue`/`ifFalse`/`ifTrueIfFalse`/
`and`/`or`/`to_do`/`to_by_do`, and the `whileTrue:`/`whileFalse:` `BytecodeClosureVtable`
intercept loop). Both image paths.

*Phase 2 inherits:* (a) the `value`-prefix branch of the `dispatch.c`
`BytecodeClosureVtable` intercept was **kept** — it is the reflective (`perform: #value`)
block-invoke fallback and still recurses into `run_interpreter`; fold it into the flat
loop with the `send()` rework. (b) `valueWithArguments:` was **not** lowered (array
spread; stays an ordinary send). (c) The `OP_BLOCK_INVOKE` case duplicates the `OP_SEND`
post-call tail (the `#ifdef __ZEPHYR__` reload + `unwind_target_reached` check) — unify
under the flat loop. (d) The kept intercept + the DNU/method-send recursion in `send()`
are the only residual C recursion into `run_interpreter`; the combinator path no longer
recurses. *Observable change (see §Dialect):* a reflective `perform:` of a deleted
combinator selector (`#ifTrue:`, `#and:`, `#to:do:`, `#whileTrue:`, …) now `doesNotUnderstand`
where it formerly ran the block (no in-tree caller; ns-tests/ns-oevm/ns-posix grep clean).

**Phase 2 — the flip** (the heavy lift) ✅ **DONE 2026-06-30** (branch `derecursify-phase2`,
both paths, suite 81/0/6 + all 5 gates + `OEVM_GC_STRESS=1` + ASan + `-D__ZEPHYR__`
syntax-clean). Recursive `run_interpreter` → **one flat loop per fiber**. All four send
opcodes (`OP_SEND`/`OP_SEND_IMPLICIT`/`OP_SEND_SUPER`/`OP_BLOCK_INVOKE`) **and** the folded
`value`-prefix intercept resolve via `send_resolve`/`send_super_resolve`: a **bytecode**
target is pushed as a frame (`push_frame`, relative `prev_frame` via
`frame_link_encode`/`decode`) and the loop `continue`s; a **primitive** is called inline.
`OP_RET` pops via the relative dynamic link and delivers the result into the caller's
`regs[src2]` — the uniform bits-24-31 "src2 decode" generalised from the old Zephyr hijack,
**verified correct for `OP_SEND_SUPER` too** (`c == src2`; `saved_pc-1` is exactly the send
instruction, no off-by-one). NLR unwinds **in-loop** (`do_unwind`, loop-local cursor over
`prev_frame`) reusing the kernel `unwindTo:value:`/`resumeWith:`/`executeCleanup` machinery
— **no new opcode** (`OP_UNWIND` avoided; `0x17` still free). `home_ctx` stays **absolute**
(safe on fixed stacks, no growth). Also retired dead `OP_SEND_IC`/`send_ic`/`struct
InlineCache` (enum slot kept as `OP_SEND_IC_RETIRED`, no renumber), and completed the
Phase-1 lowering mandate by lowering the two combinator prims Phase 1 missed —
`n timesRepeat:` (deleted `PRIM_timesRepeat`) and **unary** `[cond] whileTrue`/`whileFalse`
(deleted that intercept branch). `run_interpreter` is **kept re-entrant** only for the three
cold single-shot callers — the debugger hook, the `does_not_understand` bytecode handler, and
`run_method_on` init — plus the fiber-entry trampoline and the C-caller `send()`/`send_super()`
wrappers; **none is on the hot method/block-send path** (which routes exclusively through
`send_resolve`+`push_frame`). Positive proof of O(1) C stack: `sum:50000` runs correctly under
a 512 KB `ulimit -s` (impossible on the old recursive engine).

*Phase 3 inherits:* (a) the `#ifdef __ZEPHYR__` fiber-resume reload hijack + its POSIX
ucontext twin — both **retained**, syntax-checked only (audited Phase-2 Task 6: the four
reload blocks resume an *arbitrary* fiber-switch target frame and handle a never-suspended
fiber `saved_pc==NULL`, so they are **not** subsumed by the flat `OP_RET`'s dynamic-link pop
— structurally distinct, left for Phase 3). (b) The `ensure:`/`ifCurtailed:` cleanup path is
structurally preserved but currently **dead** (`cleanup_cl` only ever assigned `NULL`; no
kernel/compiler user) — Phase 3 / the heap-closure rework should either implement it or delete
the dead path. (c) `valueWithArguments:` still an ordinary send. (d) conservative C-stack scan.

**Phase 3 — fibers + demolition.** ✅ **DONE 2026-07-01** (branch `derecursify-phase3`,
5-task SDD, all tasks reviewed clean, both paths, suite **82/0/6 + all 5 gates** +
`test_fibers` unit + `-D__ZEPHYR__` syntax-clean + `OEVM_GC_STRESS=1`+ASan clean on
`45_fibers.ns`/`54_deep_recursion.ns`/`55_fiber_capture.ns` + structural greps clean).
Swapped `fiberSwitch:value:` to a single-C-stack `Stack`-swap mechanism (one C
activation per switch, no second call stack); **deleted** ucontext, the fiber
trampoline, `g_fiber_registry`, the conservative C-stack scan, and the Task-2b
apparatus outright (`grep -rE 'ucontext|swapcontext|fiber_trampoline|
g_fiber_registry|scan_c_stack|vm_c_stack_base|setjmp' src/` is clean).

Resolved inheritances:
- **(a) reload hijack + POSIX twin — RESOLVED, not merely unified.** Both the
  `#ifdef __ZEPHYR__` reload blocks and their POSIX ucontext twin are **gone**; the
  reload mechanism is now a single universal C-stack activation used by every target.
  Post-demolition there is **zero** `__ZEPHYR__`-conditional fiber-switch code, so
  `-D__ZEPHYR__` compiles the *identical* path POSIX runs (`gcc -D__ZEPHYR__
  -fsyntax-only src/vm.c src/gc.c src/dispatch.c` clean). Consequence: the `ABBBAA`
  interleave bug is now **architecturally impossible**, not just fixed — the hijack
  that caused it no longer exists. Proof: `-D__ZEPHYR__` host build running
  `45_fibers.ns` (the Model-B `.oimg` path, the exact nRF52840 mechanism) prints
  `ABABAB`.
- **(b) `ensure:`/`ifCurtailed:` — narrowed per David's call.** Only the dead
  `cleanup_cl` **field** was deleted (`struct MethodContext` no longer carries it;
  `grep -rn cleanup_cl src/` is clean). `executeCleanup` itself is **kept** — it is
  live NLR infrastructure the kernel `unwindTo:value:` sends on every unwind, and
  remains the seam a future `ensure:`/`ifCurtailed:` implementation hangs off. Full
  implement-or-delete of that seam is deferred to **oevm#28** (tied to oevm#25).
- **(c) `valueWithArguments:`** — unchanged, still an ordinary send, still deferred.
- **(d) conservative C-stack scan — DELETED.** Rooting is now fully precise
  (`scan_vm_stack` + `StackVtable` tracer + `gc_temp_roots` + globals; no
  mark-if-it-looks-like-a-pointer sweep of the C stack). Deletion surfaced two
  pre-existing latent bare-local GC roots the conservative scan had been masking
  (`primitive_vtable_bindings`, `udp` `basicReceive:`); both were handlerized
  (explicit `gc_temp_root_push`/pop) before the scan was removed. This closes the
  oevm#24 conservative-GC-vs-ASan-fake-stack gap in practice.

**home_ctx** stays **ABSOLUTE** (validated cross-stack by `55_fiber_capture.ns`,
clean under `OEVM_GC_STRESS=1`+ASan) — safe because stacks are fixed-size, no growth.

**Deferred (NOT this lift):** growth + heap-closure/temp-vector rework. → picked up by
the **deferred lift** (`docs/design/2026-07-01-derecursify-deferred-lift-*.md`):
Phase 1 (heap temp-vectors / upward closures) **DONE 2026-07-02**; Phase 2 (relativize
`home_ctx`) then Phase 3 (growable stacks + catchable `StackOverflow`) remain.

---

## 6. Validation

Per-phase gate = full suite both paths (`OEVM_PATH=AB`) — Phase 1 delivered 78/0/6 (was
73/0/6 pre-lowering; +5 non-literal-combinator regression tests) — + `fibers-gc-stress` +
`45_fibers` interleave + `portability-32` + `boot-gc-asan` + ASan/`OEVM_GC_STRESS=1`
clean. Perfstex (`ns-zephyr`) as the perf gate on Phase 2 — combinator lowering +
flat dispatch should IMPROVE hot loops (branches/loops beat primitive sends). The
`ABBBAA→ABABAB` host repro (`-D__ZEPHYR__`) is the fiber-correctness proof at Phase 3.

**Phase 3 gate result (2026-07-01, consolidated run):** suite **82/0/6 both paths**,
gates `grant`/`debugger-integration`/`portability-32`/`fibers-gc-stress`/`boot-gc-asan`
all PASS, `test_fibers` C-unit PASS; `-D__ZEPHYR__ -fsyntax-only` clean on
`vm.c`/`gc.c`/`dispatch.c`; `OEVM_GC_STRESS=1` + `-fsanitize=address` clean running
`45_fibers.ns` (prints `ABABAB`), `54_deep_recursion.ns`, and `55_fiber_capture.ns`;
structural greps for `ucontext|swapcontext|fiber_trampoline|g_fiber_registry|
scan_c_stack|vm_c_stack_base|setjmp` and `cleanup_cl` both clean in `src/`.

## 7. Open items for the implementation plan
- Exact `OP_RETURN` result-delivery decode generalised from `vm.c:422-445` (recover
  the caller's dest register from the caller frame's saved `OP_SEND`/`OP_BLOCK_INVOKE`
  operand — the "src2 decode").
- New bytecodes `OP_BLOCK_INVOKE`, `OP_UNWIND`; retire dead `OP_SEND_IC`/`send_ic`.
- Both image paths' agreement on the new bytecode (Path A solidified-C + Path B
  `.oimg` — verify serializer/loader carry the new ops; L0 §7d deferred this to the
  first consumer, which is now Phase 1).
- Confirm the fiber-entry-block `home_ctx`-across-stacks lifetime guarantee (§1
  caveat) holds for the `Concurrency` facade under the flat engine.
