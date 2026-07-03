# De-recursify deferred lift — growable stacks + upward closures (design)

**Status:** DESIGN APPROVED (scope + phasing + closure model) 2026-07-01,
brainstormed with David.

**Predecessors:** the de-recursify **core rewrite is COMPLETE** (Phase 1 lowering,
Phase 2 the flip, Phase 3 fibers+demolition — all shipped 2026-07-01; see
`docs/fiber_derecursify_core_design.md`). That core explicitly deferred two coupled
capabilities, marked "Deferred (NOT this lift): growth + heap-closure/temp-vector
rework." **This is that deferred lift.**

**One-line:** make per-fiber stacks grow on demand (footprint + no-`abort()`
robustness) and make captured variables live in heap temp-vectors (`struct Context`)
so closures are fully upward / Toit-class and correct across stack growth and
fibers.

---

## 1. Motivation (David's call, 2026-07-01)

Three real forcing functions, all selected:

1. **Embedded footprint.** Today every fiber pre-allocates a fixed
   `VM_STACK_MAX_WORDS` stack (host default 16384 words = 64 KB on 32-bit;
   `-D`-lowered for embedded). On the nRF52840 (~256 KB RAM, heap ~32 KB in
   Perfstex builds), `N` fibers × a fixed max is the wrong shape. Growth lets each
   fiber **start small and grow on demand** — the real motivator for a many-fiber
   node.
2. **Correctness / completeness.** Cross-fiber-escaping closures and
   upward-escaping closures (a block that outlives its home method) are a known gap;
   today's `home_ctx` is an absolute pointer into the parent stack frame. Make the
   closure model **fully upward (Toit-class)**.
3. **Deep-recursion robustness.** Stack overflow currently calls `abort()`
   (`vm.c:173`), unacceptable for a long-running node. Grow, and at a hard cap raise
   a **catchable `StackOverflow`** rather than killing the process.

## 2. Grounded findings (current engine)

- **Dynamic link (`prev_frame`) is ALREADY relative** (base-relative SMI,
  `frame_abi.h`). `frame_abi.h` states relative offsets "survive a stack realloc
  with ZERO fixups: only `stack_base` changes; every stored offset stays valid." So
  the dynamic link is already growth-ready.
- **Lexical link (`home_ctx`) is still ABSOLUTE** (`vm.c:182` `ctx->home_ctx =
  cl->home_ctx ? cl->home_ctx : (oop)ctx; /* ABSOLUTE (fixed stacks) */`). The
  discriminated encoding `{NULL | relative-SMI | reified-Context}` and `struct
  Context` are **RESERVED in `frame_abi.h` (Option B / §6.1) but NOT built** — no
  `ContextVtable`, no reify, no GC tracing of `Context`.
- **Captured-variable access** (`OP_LD_LEXICAL`/`OP_ST_LEXICAL`, `vm.c:402-418`)
  walks the **absolute** `home_ctx` chain `depth` times (`primitive_closure_home`)
  and reads/writes the home frame's `regs[]` **directly on the stack**. This is the
  classic "closure points into the parent stack frame" model — exactly what breaks
  under growth (realloc moves the frame), cross-fiber invocation (a fiber-local
  offset is meaningless against another fiber's base), and upward escape (home frame
  popped → regs gone/reused).
- **Growth and escape decouple** more than the core design implied: *growth* needs
  only that `home_ctx` also become relative (offsets survive realloc);
  *reify-to-heap-`Context`* is forced only by *escape* (cross-fiber / outlive-frame).
- **Fixed stacks:** `VM_STACK_MAX_WORDS 16384` (`vm.c:33`), one `Stack` per fiber,
  `abort()` on overflow.
- **Fiber-entry block** is the one cross-fiber escape that exists today; it works by
  accident because the spawner frame stays physically live so its absolute
  `home_ctx` pointer stays valid (`fiber_derecursify_core_design.md` §1 caveat).

## 3. Approach (decided)

**Closure/capture model = Approach A — indirect temp-vectors** (the ratified L1
endpoint: "closures = full UPWARD/Toit-class via indirect TEMP-VECTORS"; the target
`struct Context` in `frame_abi.h` was reserved for exactly this). The compiler does
capture analysis: locals captured by a nested block are hoisted into a heap
temp-vector (`Context`) at frame entry; *all* accesses to them — from the home
method **and** nested blocks — go **indirect** through the vector; non-captured
locals stay in fast on-stack `regs[]`. Nested blocks chain via `Context->outer`.

Rejected alternative — **B, lazy reify-on-escape** (keep captured vars on-stack,
box only when a closure escapes at runtime): faster common case, but fragile escape
detection (write barriers / pop-time scans) and subtle mutation-aliasing; higher
risk. Block-inlining (core Phase 1/2) already removed the common control-flow
closures, so the closures that remain tend to escape — A's indirection cost is
unavoidable for them anyway, and A is correct-by-construction.

**Overflow policy = catchable `StackOverflow`** (grow to a `-D` cap, then raise a
signalable error the image can handle — kill just that fiber, log, etc), unwinding
through the existing NLR / `unwindTo:value:` machinery.

**Growth mechanism = `realloc`-style copy-to-larger** (not segmented/chained),
because `frame_abi.h` was explicitly designed for it (relative links survive with
zero fixups).

**Sizing = host-first.** Get the mechanism correct and `-D`-tunable now (generous
host defaults); defer embedded-specific tuning to a later footprint pass on silicon.

## 4. Phasing (three internally-consistent phases, each green-gated)

**Dependency-forced order (corrected 2026-07-01 during plan-writing — see the
finding below): temp-vectors → relativize → grow.** This inverts the intuitive
"grow first" order because of a hard dependency chain.

**The finding that forces this order.** Both existing fiber tests require **mutable
*shared* capture across fibers + the spawner**, not just reads: in `45_fibers.ns`
fibers A and B *both* mutate `log` (`log:: log , 'A'`) across yields and main reads
it; in `55_fiber_capture.ns` fiber B mutates `shared:: 99` from a third Stack and
fiber A must observe `99` on resume. Today this works only because `home_ctx` is an
**absolute** pointer, so every block and the spawner resolve to the *same physical
stack slot*. A **relative** `home_ctx` is decoded against the *current* fiber's
`vm_stack`, so a closure created on main's Stack but run on fiber A's Stack decodes
its offset against the wrong base. The frozen L0 ABI stores a bare offset with **no
stack identity** (betting cross-fiber → reify-to-heap-`Context`), and a one-way reify
(block reads a copy) **breaks mutable sharing** — preserving it requires the spawner
*also* to indirect through the same heap `Context`, which *is* the temp-vector
mechanism. Therefore relativizing `home_ctx` cannot be behavior-equivalent until
temp-vectors exist, and growable stacks (which need relative links — absolute
`home_ctx` dangles on `realloc`) cannot precede relativization. Consequence: **the
footprint/growth win lands last, not first.** (Segmented/chained stacks *could*
decouple growth from relativization — existing frames never move — but we rejected
that model; §3.)

### Phase 1 — Indirect temp-vectors (the compiler capture rework) — ✅ DONE 2026-07-02

**Status: DONE** (pending final whole-branch review). Gate green, **re-verified
solo/serial** (the earlier Path-A flakiness was the documented concurrency race in
`run_tests_oevm.py`, not a regression — a solo run is clean on BOTH paths): full
suite **84 pass / 0 fail / 6 skip** Path A (solidified-C) **and** Path B (`.oimg`);
all 5 gates (`grant`, `debugger-integration`, `portability-32`, `fibers-gc-stress`,
`boot-gc-asan`) PASS; `45_fibers`=`ABABAB`, `55_fiber_capture` correct,
`56_upward_closure` and `57_loop_var_capture` pass under `OEVM_GC_STRESS=1`+ASan;
`-D__ZEPHYR__ -fsyntax-only` clean; C-units `test_context`/`test_ctx_ops` green.
No `.oimg` `format_version` bump (Contexts + `Closure.captured_env` are runtime-only;
`oimg_serialize.py` + registry `VERSION=4` unchanged). Behavior-equivalent — **no
dialect row** (the only new observable is upward-escaping closures now working, a
correctness fix exercised by `56_upward_closure`, not a dialect change).

**Perf (Task 5, oevm appliance, branch vs master, median of 3):** the hot **numeric**
loop `mandelbrot` (3.0s, the one benchmark well above the ~10ms noise floor) is
**−5.6% — NOT regressed** (temp-vectors don't touch non-capturing loops). The
**closure-heavy** `richards` (64ms) is **+10.8%** — the real, bounded cost of the
"every real block allocates a (0-slot pass-through) Context for depth bookkeeping"
rule. All sub-10ms benchmarks (queens, sieve, deltablue, nbody, limit tests) are
noise-floor (`/usr/bin/time`=0.00). Gate criterion (no hot integer/loop regression)
**met**. Future optimization filed as **oevm#33**: skip the pass-through Context for a
block that neither captures, creates a capturing closure, nor reads an enclosing
capture (deferred — it breaks the simple `OP_LD_CTX depth == frame_depth` invariant).

**Deferred limitation (documented, David-approved):** an **inlined**-block-managed
captured local — a `to:do:`/`to:by:do:` induction variable, or an inlined
`ifTrue:`/`whileTrue:` block param/temp — captured by a nested **real** block stays
**register + `home_ctx`-homed** (excluded from Context-homing: `real_captures =
[n for n in captured_locals if n in local_vars]`), i.e. *exactly* pre-Phase-1
behavior. It is therefore NOT upward/cross-fiber-safe (same limitation as before this
phase); fixing it is deferred to a later phase. (A naïve Context-homing of these
crashed — the inline loop codegen writes the raw register, never the Context slot, so
reads saw nil; the exclusion restores correctness. Regression: `57_loop_var_capture`.)

**Landed as:** Tasks 1–2 inert runtime (oevm `b13ab6b`, `7947354`; ns-oevm `20f2d89`);
Task 3 capture analysis + determinism fix (ns-oevm `9e55532`, `fe152f7`); Task 4 the
flip — compiler codegen (`ns_compile_oevm.py`: per-frame `ctx_reg = num_arguments+1`
reserved uniformly; a finalize post-pass rewrites captured-local accesses to
`OP_LD_CTX`/`OP_ST_CTX` and prepends the `OP_MK_CONTEXT` prologue — jumps already
relative so the prepend is fixup-free; `self` and inlined-loop captures excluded, stay
on `home_ctx`) + VM `OP_CLOSURE` capturing the creating frame's Context (oevm
`f65f494`) + loop-var fix (ns-oevm `2ce28c8`, ns-tests `a32b2d8`, `9b37ca0`); Task 5
benchmark-harness repair (ns-tests `f8dd7aa`). **Necessary GC fix (`f65f494`):** moving
captures into heap Contexts adds GC points where the cooperative scheduler is
momentarily unreachable via registers, so a suspended fiber (holding the scheduler's
live frames) could be swept and its slot reused for a Context (use-after-free); fixed
by rooting the **bootstrap fiber** (fiber 0) — its stack transitively reaches the
scheduler's heap-held ready-queue → every live fiber — leak-free (dropped/completed
fibers stay collectable) and NOT a revival of the demolished `g_fiber_registry`.

---

Move captured variables off the stack into heap temp-vectors (`struct Context`) so
they have a single fiber-independent home. **Behavior-equivalent** (same observable
output, including the cross-fiber tests — which become correct *by construction*
rather than by absolute-pointer accident); stacks still fixed, `home_ctx` still
absolute for the *stack-frame* (NLR) role.

- **Build the `Context` runtime**: allocator (`context_alloc`-style), `ContextVtable`,
  precise GC tracing (trace `outer` + `slots[0..num_slots]`) — all wired like
  `StackVtable` (a **private runtime vtable**, out of the frozen `builtins.def`,
  `.oimg` format untouched; Model-A wiring via the `ns-oevm/tools/solidify.py`
  generator).
- **Compiler capture analysis**: identify locals captured by any nested block;
  allocate a per-frame temp-vector (`Context`) at frame entry holding exactly those;
  emit **indirect** load/store for them from *both* the home method and nested blocks;
  non-captured locals stay in fast on-stack `regs[]`. Nested blocks reach enclosing
  temp-vectors by chaining `Context->outer`; a closure references its enclosing
  `Context` directly (a heap pointer, fiber-independent) for *variable access*.
- **Separate the two roles of `home_ctx`**: variable access now goes through the
  heap `Context` (above); the `home_ctx` *stack-frame* link is retained **only** for
  the NLR `^`-return target and stays absolute this phase (relativized in Phase 2).

**Result:** captured variables are never on the moving/popping/fiber-local stack →
cross-fiber capture correct by construction; the substrate for upward escape and
growth is in place. This subsumes the previously-planned "reify the fiber-entry
block" — no separate reify needed.

**Gate:** full suite both paths + `fibers-gc-stress` + `45_fibers` interleave +
`portability-32` + `boot-gc-asan` + ASan/`OEVM_GC_STRESS=1` + `-D__ZEPHYR__`
syntax-clean. Same observable behavior. Perfstex as the perf gate (indirection lands
only on captured locals; hot path unaffected). New test: upward-closure-outlives-home
(a method returns a block that reads/mutates a captured local; invoking it after
return works — now correct because captures outlive the frame).

### Phase 2 — Relativize `home_ctx` (the NLR link)

> **✅ 2026-07-02 — DONE via the self-migration redesign** (`2026-07-02-derecursify-phase2-self-migration-design.md`;
> Part 1 self-migration oevm `05e0aab`/ns-oevm `d9b1cd2`+`e77f980`, Part 2 flip oevm
> `f269606`; 84/0/6 both paths + 5 gates, reviewed clean). The two-attempt history and
> the corrected premise are below.
>
> **⚠️ 2026-07-02 — FIRST ATTEMPT BLOCKED; PREMISE CORRECTED (David: revert + re-plan).**
> This section's claim "with variable access already off the stack (Phase 1), the
> remaining `home_ctx` role is the NLR `^`-return target — inherently same-fiber" is
> **FALSE.** Phase 1 **deliberately kept `self` on the absolute `home_ctx` link**
> (`ns_compile_oevm.py` `resolve_name`: `self` is EXCLUDED from Context-homing), and
> **any nested block reads its enclosing `self`/slots by walking `home_ctx`**
> (`get_self_reg` → `OP_LD_LEXICAL`). A **fiber block** that touches an enclosing slot
> (e.g. `concurrency` in `55_fiber_capture`) does this **cross-fiber**; relativizing
> `home_ctx` decodes the offset against the wrong fiber's stack base → garbage frame
> (`cl==NULL`) → `primitive_method_context_reg_at` SIGSEGV. Proven by bisection
> (`55_fiber_capture` + `fibers-gc-stress`). (`shared` is fine — it *is* Context-homed.)
> **The real prerequisite for relativizing = migrate enclosing-`self` reads off
> `home_ctx` into the fiber-independent heap `Context`** (finishing a Phase-1 deferral).
> The behavior-preserving decode chokepoint (`home_ctx_frame`, oevm `6e988af`) landed
> and is KEPT; the flip itself was reverted. Re-planning the `self`-migration + flip as
> a redesigned Phase 2 (see the newer plan doc). The bullets below describe only the
> *flip half* and remain valid for after `self`-migration.

With variable access already off the stack (Phase 1) **and `self`-migration done (the
new prerequisite)**, the remaining `home_ctx` role is the NLR `^`-return target —
inherently same-fiber. Turn it from an absolute pointer into the discriminated
`{NULL | relative-SMI | reified-Context}` encoding so it survives stack growth.
**Behavior-equivalent** (stacks still fixed).

- `make_closure`/`OP_CLOSURE` (`dispatch.c:610`) and the frame-setup sites
  (`vm.c:182/276/1174`) encode `home_ctx` via `home_link_encode_relative` against
  the current fiber's `vm_stack`.
- The NLR walk (`vm.c:567`) and `get_top_level_method_closure` (`vm.c:1237`) flip the
  legacy "`IS_SMI` == stop" convention to "`IS_SMI` == relative, follow it" (as
  `frame_abi.h` §43-51 documents); `primitive_closure_home` (`vm.c:1104`) decodes the
  relative-SMI case via the `frame_abi.h` helpers. The local-return check
  `(oop)ctx == target_home` decodes both sides to pointers before comparing.
- GC marking of `frame->home_ctx` (`gc.c:515/573`) no-ops safely on a relative SMI
  (`mark` ignores SMIs) and traces a reified `Context` normally.

**Gate:** full suite both paths + all the above + `-D__ZEPHYR__` syntax-clean. No new
observable behavior.

### Phase 3 — Growable stacks + catchable `StackOverflow`

- `push_frame`/`push_frame_reg` (`vm.c:172/261`) replace `abort()` on overflow with
  `stack_grow`: allocate a larger `Stack`, copy `data[]` (relative
  `prev_frame`/`home_ctx` offsets survive the copy untouched), update
  `fiber->stack_obj`, `g_active_stack`, `vm_stack`, and recompute `current_frame`;
  the caller reloads `ctx` (push already returns the new frame).
- Initial size / growth step / hard cap all `-D`-tunable (generous host defaults;
  embedded tuning deferred).
- At the hard cap, raise a **catchable `StackOverflow`** through the NLR /
  `unwindTo:value:` machinery rather than `abort()`.
  - **Dependency (scope risk):** catching it needs minimal signalable-error infra.
    Scope the smallest thing that works — a kernel-level signal that unwinds to a
    handler frame. If it needs more than a small seam, split it to its own mini-phase
    rather than bloating Phase 3.

**Gate:** as Phase 2, plus new tests: deep-recursion-that-grows (a `sum:`/factorial
depth that forces ≥1 realloc, result correct); `StackOverflow`-caught-and-handled (a
runaway hits the cap, the image catches and recovers, node survives).

## 5. Cross-cutting

- **GC:** `Context` traced precisely by a vtable tracer (like `StackVtable`);
  `home_generation` validates NLR across reify. Rooting stays fully precise — the
  conservative C-stack scan deleted in core Phase 3 does **not** return.
- **Both image paths:** `Context`/`ContextVtable` are private runtime vtables (frozen
  `builtins.def` untouched, `.oimg` format version unchanged); Model-A wiring via the
  `solidify.py` generator, exactly as `StackVtable` was done in L1. Verify the
  serializer/loader need no change (no new `.oimg`-visible structure).
- **`valueWithArguments:`** stays an ordinary send (unchanged; still deferred).
- **`-D__ZEPHYR__`** compiles the identical path POSIX runs (core Phase 3 removed all
  `__ZEPHYR__`-conditional fiber code); each phase stays syntax-clean under it.

## 6. Validation

Per-phase gate = full suite both paths (`OEVM_PATH=AB`) + `fibers-gc-stress` +
`45_fibers` interleave + `portability-32` + `boot-gc-asan` + ASan /
`OEVM_GC_STRESS=1` clean + `-D__ZEPHYR__ -fsyntax-only` clean. Plus the per-phase new
tests listed above. Perfstex (`ns-zephyr`) as the perf gate on Phase 1 (the
temp-vector indirection is the change with a hot-path perf profile).

## 7. Open items for the implementation plan

- Exact `Context` layout use vs. the reserved `frame_abi.h` `struct Context`
  (`home_generation`, `num_slots`, `outer`, `slots[]`) — confirm it needs no field
  changes, or amend the frozen L0 reservation deliberately if it does.
- Compiler capture-analysis site and the new indirect load/store codegen (Phase 1) —
  where the `.ns`→bytecode compiler lives and how temp-vector allocation is emitted;
  how a closure references its enclosing `Context` for variable access vs. its
  `home_ctx` stack-frame link for NLR.
- `stack_grow` allocation path (Phase 3): does the GC allocator support a
  copy-to-larger, and the exact set of globals/locals to reload after it (`vm_stack`,
  `g_active_stack`, `current_frame`, any C-local frame pointers live across a growing
  `push`).
- Minimal signalable-error mechanism for `StackOverflow` (Phase 3 dependency) — the
  smallest kernel seam that unwinds to an image handler; decide implement-in-Phase-3
  vs. split.
- Interaction of a reified-`Context` home with NLR validity (`home_generation`) and
  the `cannotReturn` error path (Phase 2).
