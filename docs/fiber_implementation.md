# Fiber implementation — de-recursifying the oevm interpreter (the Toit model)

**Status:** DECIDED — Option C (Toit-style non-recursive interpreter). Clean, bottom-up
rewrite; tear out rather than convert. No backward-compat / "keep the suite green"
constraint during the rewrite — correctness of each layer's contract is the gate.
**Date:** 2026-06-30.
**Scope:** the oevm VM execution core (`oevm/src/vm.c`, `src/dispatch.c`, `src/gc.c`,
bytecode/ABI) + the Newspeak compiler's control-flow lowering (ns-oevm frontend).
**Origin:** R7 fibers shipped desktop-first (POSIX=ucontext). The Zephyr fiber path
(the "hijack") is architecturally broken for multi-fiber scheduling; this doc records
why, the options weighed, the Toit template, the oevm audit, and the chosen direction.

---

## 1. Problem statement

R7 added cooperative fibers (`.ns` `Concurrency`/`Fiber` facade over universal prims
`currentFiber` / `fiberCreate:` / `fiberSwitch:value:`). Two C-level switch mechanisms
coexist in `vm.c`, guarded by `#ifndef __ZEPHYR__` / `#ifdef __ZEPHYR__`:

- **POSIX = ucontext:** each fiber gets its own malloc'd C stack + `ucontext_t`;
  `swapcontext` switches. Works (suite 73/0, `ABABAB`).
- **Zephyr = hijack:** ONE shared C stack; `primitive_fiber_switch` swaps the NS-stack
  globals and returns; after every OP_SEND/IC/SUPER/IMPLICIT, `if (ctx != current_frame)`
  rebuilds pc/method/regs and resumes the target fiber in the same C loop.

This session proved on real Zephyr (native_sim Model B) that the hijack **compiles,
links, and runs**, single-worker fibers round-trip correctly (`1 2 3`), but **multi-fiber
scheduling is wrong**: `45_fibers` expects `ABABAB`, deterministically produces `ABBBAA`.
Same result on a fast host repro (`gcc -D__ZEPHYR__ … oevm_image_main.c`).

## 2. Root cause of the hijack bug

Confirmed by instrumentation (a cross-stack `prev_frame`-link detector that fired exactly
once, at the divergence): **the hijack reuses one C `run_interpreter` activation for a
different fiber than the one that entered it.** Each `run_interpreter` caches
`saved_stack_ptr` + `prev_ctx` as C locals bound to the *entering* fiber. When that
activation later unwinds via `exit_frame`, it restores the **wrong** fiber's
`vm_stack_ptr`/`current_frame`. The result: `vm_stack`, `vm_stack_ptr`, and
`current_frame` end up belonging to three different fibers, and the next frame is
bump-allocated on fiber A's NS stack with `prev_frame` pointing into fiber B's NS stack —
scrambled scheduling. Single-worker ping-pong stays balanced by luck; two worker stacks
at different offsets expose it. POSIX is immune because ucontext gives each fiber a real
separate C stack (no activation reuse).

**This is an architectural flaw in the hijack, not a one-line fix.** The hijack tries to
run multiple fibers on a single C stack while the interpreter still uses C recursion per
send; those two facts are fundamentally incompatible.

## 3. Options weighed

| | Mechanism | Targets | Reuses Task 2b GC | Risk | Fit |
|---|---|---|---|---|---|
| **A** | Zephyr `k_thread` per fiber (coop) + sema handoff | one mechanism native_sim+DK | partial (thread-stack scan rework) | low–med | correct but heaviest; OS thread per fiber |
| **B** | per-fiber C stack + hand-rolled context switch (ucontext host / x86 native_sim / ARM DK) | 3 switch backends | **fully** | moderate | lightweight; faithful to current engine; papers over recursion |
| **C** | **non-recursive interpreter (Toit model); switch = swap heap Stack** | **one mechanism, all targets** | **deletes it (precise roots instead)** | high (touches the engine + compiler) | **the proven foundation; fibers as cheap as Toit tasks** |

**Decision: C.** Rationale: concurrency substrate is foundational (per the ns-oevm
strategic framing — "foundational axis paid up front because it can't be retrofitted"),
and Toit proves C is the destination and de-risks it. There is no time pressure and
nothing that must keep working; the goal is to get it right and clean. B is a tactical
patch that keeps three switch backends + conservative GC and leaves the C recursion in
place; A turns fibers into OS threads. Both work *around* the recursive interpreter; C
removes the recursion the interpreter never fundamentally needed.

The POSIX consequence is explicit and intended: **POSIX runs the same de-recursified
interpreter** and sheds `make/swapcontext`, the per-fiber 256KB C stacks, and *all* of
Task 2b's conservative C-stack GC machinery (the `g_fiber_registry` walk, `c_stack_sp`
parking, register-file scan, `no_sanitize_address` reads). One switch mechanism on every
target; the GC gets simpler, not more complex.

---

## 4. Toit as the template (the elegant solution)

Source studied: `~/workspaceToit/toit/src`. Toit's bytecode interpreter keeps the language
call stack as a **heap `Stack` object** and stays exactly **one C frame deep**
(`Interpreter::run`, `interpreter_run.cc:291`). Everything below is the template oevm
will follow.

### 4.1 Stack & frames as data
- `class Stack : public HeapObject` (`objects.h:912`): `length`, `top` (sp), `try_top`,
  guard zone. Grows downward.
- A frame is **two words**: saved return `bcp` + a `frame_marker` sentinel
  (`FRAME_SIZE == 2`, `interpreter.h:52`). The marker is a pointer into the immutable
  bytecode region, so GC/stack-walkers can recognise frame boundaries.
- A `Task` is just `{Stack*, id}` (`objects.h:1620`).

### 4.2 INVOKE / RETURN — goto, not C call/return
- `CALL_METHOD` (`interpreter_run.cc:216`): `PUSH(return_bcp); PUSH(frame_marker);
  bcp = target.entry(); DISPATCH(0)`. A `goto`, never a C call. `INVOKE_STATIC/VIRTUAL/
  BLOCK` all end here; tail calls move args down and reuse the return address.
- `RETURN` (`interpreter_run.cc:1183`): pop temps, pop marker, restore caller `bcp`, drop
  args, **`PUSH(result)`**, `DISPATCH(0)`. The result is delivered by leaving it on the
  heap stack and resuming the caller's `bcp` — **no C value handed back**.

### 4.3 Blocks — base-relative
A block is a single Smi: a *base-relative* offset to a slot on the current stack
(`from_block`/`to_block`, `interpreter.h:226-230`, `BLOCK_SALT`). Outer vars via
`LOAD_OUTER`/`STORE_OUTER`. Because it's base-relative, **it survives stack relocation**.

### 4.4 NLR / branch / exceptions / finally — one unwinder
`NON_LOCAL_RETURN` (`interpreter_run.cc:1233`), `NON_LOCAL_BRANCH` (`:1247`), `THROW`
(`:1173`), and internal errors all push a small descriptor and `goto UNWIND_IMPLEMENTATION`
(`:1281`). The descriptor's low bits encode the reason (`-1` fall-through, `-2` exception,
even=NLR w/ arity, odd=branch w/ bci). `UNWIND_IMPLEMENTATION` walks the `try_sp_` chain:
if a more-nested `finally` exists (`target_sp > try_sp_`), it stashes the descriptor in the
`LINK` record, repositions to the try-block call site, and **runs the `finally` as ordinary
bytecode** (which ends in an `UNWIND` bytecode that re-enters the unwinder); otherwise it
lands directly (`sp = target_sp`, pop marker, restore `bcp`, drop arity, push result),
`DISPATCH(0)`. `try`/`finally` is a 4-word `LINK` record chained via `try_sp_`
(`:1264`/`:1277`), mirrored to the heap object as `try_top()`.
**Nothing longjmps; nothing returns out of the C loop. Unwinding == repositioning
`sp`/`bcp` + replaying `finally` blocks as bytecode.**

### 4.5 The hard rule: ZERO C re-entry into the interpreter, ever
A C++ primitive may **never** synchronously call Toit and use the result in C. This is the
property that keeps `run()` one frame deep. Two patterns replace it:
- **Restartable intrinsic bytecodes** — e.g. `INTRINSIC_HASH_DO` (`:1512`): iteration
  state lives on the stack, the block is called via `CALL_METHOD(block, 0)` whose **return
  address is the same bytecode**, so `RETURN` lands back on it and the C helper resumes.
- **Marker-result primitives** — `task_transfer` (`primitive_core.cc:2034`): the whole
  cooperative switch is `store_stack(); set_task(to); load_stack(); return mark_as_error(to)`
  — three `Stack` field swaps + one shared `RETURN` against the swapped-in stack. `bcp` is
  not reloaded; the resumed task continues at its own saved location. `prepare_task`
  pre-builds a fresh task's stack as a faked call to `task_transfer_` so it resumes through
  the identical tail. **No C recursion, no return to the scheduler.**

Finalizers/callbacks are queued and run as scheduled bytecode work — never by re-entering
the interpreter from C.

### 4.6 Growth / relocation — cheap because links are relative
Overflow is a watermark compare on every call (`CHECK_STACK_OVERFLOW`, `:166`;
`watermark_` doubles as the preemption flag). Growth = allocate a bigger `Stack`,
`copy_to` (`objects_runtime.cc:132`) = `memcpy` + adjust two scalar offsets (`top`,
`try_top`). Safe **only** because every intra-stack reference is base-relative (blocks,
unwind targets, the try chain) and saved return addresses point into the immutable
program, not the stack. GC can even shrink stacks (`roots_do`, `objects.cc:262`).

### 4.7 GC precision
`Stack::roots_do` treats every used slot as a root, skipping only words pointing into the
bytecode region (saved `bcp`s + frame markers). Fully precise w.r.t. liveness with no
stack maps — possible *only* because frames are heap data. A C-recursive VM cannot do this.

### 4.8 Preemption
Same machinery: set `watermark_ = PREEMPTION_MARKER`, catch at the call-site check, push
`bcp`+marker, `store_stack`, return `PREEMPTED`. A preempted task parks identically to a
cooperatively-yielded one (continuation = heap Stack + saved `bcp`).

---

## 5. oevm de-recursify audit (current engine)

Full call-site inventory done; key findings:

- **`send()` (`dispatch.c:611`) is the single recursion funnel.** `run_interpreter` is
  referenced only in `vm.c`, `dispatch.c`, and the two test drivers.
- **The frame chain is already reified.** `MethodContext` frames live on `vm_stack` with
  `prev_frame`/`saved_pc`/`closure`/`home_ctx`/`regs`; the GC already walks them via
  `prev_frame` with zero C recursion (`gc.c:508-600`). "Language stack is data" is already
  half-true.
- **The result-delivery primitive already exists.** The Zephyr hijack (`vm.c:392-398`)
  already decodes the caller's result register `src2` from the saved instruction and writes
  the result without a C return — exactly what de-recursified RETURN needs. De-recursifying
  the return path is largely *generalising the hijack to all returns on all targets*.
- **NLR is already partly data-driven.** Target-finding + `ensure:` cleanup run by walking
  `prev_frame` inside the `unwindTo:value:` *bytecode* (Layer 1). Only the C pop-up (Layer
  2: the chained `if (unwind_target_reached) goto exit_frame`) rides the C return; it
  becomes an explicit "pop until `current_frame == unwind_target_frame`" loop.
- **The genuine obstacles: 16 call sites / 13 selectors** — the control-flow combinators
  that call `run_interpreter` *and then keep doing C work* (loop counters, conditions):
  `to:do:` `to:by:do:` `timesRepeat:` `whileTrue:` `whileFalse:` (+0-arg) `ifTrue:ifFalse:`
  `ifTrue:` `ifFalse:` `and:` `or:` `value*` (in `dispatch.c`). Plus `does_not_understand`
  (`dispatch.c:347`), bound-class `run_method_on` (`:440`), and 3 debugger-hook sites
  (`vm.c:267/300/864`).
- `OP_SEND_IC` / `send_ic` is **dead code** (compiler never emits it; an in-stream IC would
  SIGSEGV the read-only Model B blob). Ignore for the port.

## 6. Conclusions that shape the rewrite

1. **The "hybrid" (leave combinators as C primitives) is NOT viable for fibers.** Toit's
   zero-re-entry rule is *necessary*, not stylistic. Proof from our own code: the scheduler
   `run` is `[readyQueue isEmpty] whileFalse: [ … fiberSwitch … ]` — it switches fibers
   **from inside the `whileFalse:` C loop**. Any combinator a fiber can switch across, kept
   as a C loop, re-creates the activation-reuse hazard (`ABBBAA`). User code routinely
   yields inside loops. Therefore **the control-flow combinators must be lowered to
   bytecode** (branches + a bytecode loop over block-`value`), exactly as Toit's compiler
   does. There is no shortcut.
2. **Frame links will be BASE-RELATIVE offsets, adopted now (not deferred).** A first pass
   leaned toward keeping oevm's absolute `prev_frame` pointers (the per-fiber NS stacks are
   fixed-size today, so absolute links "work"). That is the expedient answer, not the right
   one. We adopt Toit's base-relative offsets at the ABI layer (L0) for the reasons in §6a.
3. **POSIX unifies and simplifies** (see §3) — one switch mechanism, precise GC, ucontext +
   Task 2b conservative scanning deleted.

## 6a. Why base-relative frame links (the right call, not the expedient one)

The choice is how every cross-frame reference on a fiber's stack is stored: the dynamic link
(`prev_frame`), block/outer-variable references, and unwind targets.

- **Absolute pointers** (oevm today): a link is `oop` = `&stack[i]`. Cheap to dereference, but
  the stack array can never move — so each fiber must pre-allocate its **maximum** stack up
  front (`VM_STACK_MAX_WORDS` per fiber today). That is exactly the wart that makes "many
  cheap fibers" expensive on a 256 KB node: every fiber, however shallow, reserves the worst
  case.
- **Base-relative offsets** (Toit): a link is a small integer offset from the stack base
  (`from_block`/`to_block`, `interpreter.h:226-230`). One extra add/sub per access, and in
  return it makes the stack **growable, relocatable, and shrinkable** — `copy_to` is a
  `memcpy` + two scalar fixups (`top`, `try_top`) because **nothing on the stack points at the
  stack** (saved return addresses point into the immutable program; everything else is an
  offset). GC can even compact a stack in place (`objects.cc:262`).

Decision: **base-relative**. Rationale, weighted by "get it right, it's foundational, no time
pressure":
1. **Growable per-fiber stacks are most of the point of Toit-class fibers.** A fiber can start
   tiny (tens of words) and grow on demand instead of reserving the max. On a RAM-bound node
   this is the difference between "a handful of fibers" and "many." Relative links are the
   single precondition that makes growth a `memcpy`.
2. **The frame ABI is the one thing you cannot retrofit cheaply.** Absolute-vs-relative is an
   L0 decision baked into the frame layout, the compiler's block representation, the
   serializer, and both image paths. Choosing absolute now and wanting growth later means
   re-opening the ABI across all of them — precisely the foundational rework the strategic
   framing says to pay for up front.
3. **It is the unwinder's natural currency too.** Toit's one unwinder (§4.4) addresses targets
   as base-relative blocks; adopting relative links at L0 means NLR/branch/exception targets
   fall out of the same representation instead of needing a parallel pointer scheme.

Cost accepted: an offset→address conversion on frame-link and outer-variable access (Toit pays
it without regret), and the compiler must emit blocks as base-relative offsets. The spike
(§10) validates exactly this property — frames addressed by relative index survive a stack
realloc untouched.

## 6b. Closure semantics — DECIDED: full upward (Toit-class) closures

The relative-link decision (§6a) settles the *dynamic* link (`prev_frame`), which is always
same-stack. The *lexical* link (`home_ctx` — what a block follows to read an outer variable)
is a separate, deeper fork, because a closure's home can live on a **different fiber's stack**
or **outlive its home frame**. A second spike (`docs/spikes/home_link_spike.c`, green) pinned
down the real behaviour; the full contract is `docs/fiber_l0_frame_abi.md` §6. Summary:

**Spike findings (the framing):**
- A **bare relative home offset is unsafe cross-fiber** — it resolves against whatever stack
  is running, but a closure's home lives on its *creator's* stack. The spike's closure,
  invoked on fiber B, read B's slot (`999`) instead of its home value (`42`). And cross-fiber
  home is the **fiber main path** (the entry block is created on the spawner's stack and run on
  the new fiber's stack), not a corner case.
- oevm reifies a context **in place** (vtable patch; the frame stays on the stack and dies with
  it) and **guards escape-past-pop with `gen_tag`** (a dangling-return *error*). So there is no
  live "heap context" today — keeping outer state alive past the home is new machinery.

**The two options (pros/cons):**

| | Option A — downward-only (status quo) | Option B — full upward (Toit-class) |
|---|---|---|
| Block lifetime | valid only while home frame is live | may outlive home frame; outer state survives |
| `home_ctx` encoding | `{stack-id, relative offset}` + generation check | **relative SMI (on-stack) \| reified `Context` heap oop** |
| Escape-past-pop | stays an error (`gen_tag`) | valid (reify-on-escape boxes the live vars) |
| Cross-fiber | safe (resolve against home stack id) | safe (escape reifies *before* the handoff) |
| Cost | smaller commit; preserves current semantics | heap box at escape + reify machinery + GC of `Context`s |
| Capability | no callbacks/continuations that outlive a method | callbacks, stored handlers, continuations across time |

**Decision: Option B (full upward closures). 2026-06-30, David.** Rationale: closures are
**foundational** — pay the hard work up front; the runtime capability compounds as the language
grows. This is the strategic-framing "foundational axis paid up front" call applied
deliberately (B *could* technically be retrofitted, but doing it now keeps the ABI single-pass).

Choosing B **resurrects the clean `IS_SMI` discriminator** for `home_ctx` — relative offset for
an on-stack home, a reified `Context` heap object for an escaped one — which is Toit's exact
model. No `{stack-id}` word is needed: the invariant "a non-reified relative home is only ever
followed on its own fiber" holds *because escape reifies first*.

### 6b.1 Performance characterization of B
B charges **only at escape, never at block use** — so the cost splits cleanly:
- **Downward blocks (`do:`/`whileTrue:`/`ifTrue:`/iteration) — ~zero B-cost.** They never
  escape, so they stay on-stack relative links, identical to Option A. And the de-recursify
  rewrite makes this path *faster* (combinators lowered to bytecode branches/loops beat
  primitive sends — Perfstex is the gate; expect improvement, §7 L6).
- **Lexical reads** add one `IS_SMI` tag-check per hop (depth usually 1–2; most reads are
  own-frame registers). Negligible — Toit pays exactly this.
- **Escape path** — a one-time heap box of the live outer vars per escape, proportional to
  *escape frequency*, not invocation frequency; plus GC tracing of the resulting `Context`s.
- **Fibers — net win.** One reification per `spawn` (the entry block escapes once) is trivial
  against spawn cost, and it *replaces* the ucontext machinery: the **256 KB C stack per fiber**
  plus all of Task 2b's conservative C-stack scanning. B is net-positive on RAM/complexity here.

Honest headline: **runtime cost of B over A ≈ 0 on the hot path; a bounded one-time heap cost
only where closures actually escape; the rewrite overall is neutral-to-faster; for fibers it
sheds 256 KB/fiber.** The real cost of B is *engineering* (reification correctness, `Context`
GC, NLR over reified homes) — the "hard work up front", not a runtime tax.

### 6b.2 Why the product vision requires B
The guiding concept: **NS as a powerful, clean scripting language over Zephyr sub-systems —
Thread, I2C, SPI, …** Scripting those is inherently event-driven and callback-shaped:

```
i2c    onData:   [:bytes | sensor update: bytes].   "block stored in the driver"
thread onPacket: [:pkt   | router dispatch: pkt].   "invoked later, on the driver's fiber"
spi    after: 5 do: [ ledToggle ].                  "deferred; outlives this method"
```

Every one of those is an **upward escape**: a handler block stored by a subsystem and invoked
*after* the registering method returned, often *on another fiber*. Under Option A those are
dangling-return errors; under **B they just work**. So upward closures are the substrate for
the whole register-a-handler / call-me-back model that makes NS feel like a real scripting
language over Zephyr rather than a toy. The vision is what *justifies* paying for B now.

---

## 7. Direction: clean, bottom-up, tear-out rewrite (Toit-templated)

Not an incremental "convert + keep green" migration. Build the new execution core from the
lowest layer up; each layer's **contract** is nailed and made robust before the layer above
leans on it; the old mechanisms (ucontext, hijack, C-recursion per send, C-loop combinators,
Task 2b conservative scanning) are **torn out**, not preserved behind compatibility shims.
The legacy suite is the **acceptance oracle** for the composed engine, not a continuous-green
constraint during the rewrite. Do the work on an isolated branch/worktree.

### Layer-order refinement (2026-06-30, found at L1 planning)
Detailed planning of L1 surfaced two ordering facts that refine the layer boundaries below:
- **`home_ctx` (the lexical link) cannot relativize until temp-vectors are live**, because it is
  read cross-fiber (R7 entry-block `OP_LD_LEXICAL` home-walk). So the lexical/`Context`/NLR rework
  bundles with the **compiler** captured-variable emission (L4), not L1.
- **Stack growth needs two preconditions** — flat dispatch (no C-cached frame pointers) *and*
  heap lexical links (no interior `home_ctx` pointers a Stack-move would dangle). So **growth
  moves to the final layer**, after L4.

Refined order: **L0 (frozen) → L1 first-class `Stack` + relative *dynamic* link + precise GC
(plan: `fiber_l1_implementation_plan.md`) → L2 flat dispatch (NO growth) → L3 unwinder → L4
temp-vectors + compiler captured-vars + NLR-over-Stack-identity (replaces absolute `home_ctx`)
→ L5 fibers (swap `Stack`, delete ucontext/conservative C-scan) → L6 enable growth + cleanup +
validate.** Q1=A's spirit is intact (relative dynamic links live in L1; growth deferred — now to
L6, where both preconditions hold). The L0/L1 layer descriptions below predate this and read
slightly broader for L1; the plan + `fiber_l1_stack_context_design.md §7` are authoritative.

### Layered architecture (bottom-up build + robustness order)

**L0 — Bytecode / frame ABI (the contract).** Define the new frame layout (frame-marker
sentinel, saved pc, result-on-stack protocol), the block representation, the unwind
descriptor format, and any new/removed bytecodes (block-invoke, inline branches, loop
back-edges, unwind). Both the compiler and the VM depend on this, so it is fixed first.
**Frame links are base-relative offsets, enabling growable per-fiber stacks (§6a).**

**L1 — Stack object & frame primitives.** The per-fiber NS stack as the sole call stack:
push/pop frame, deliver result on stack, walk frames, precise `roots_do` over stack slots.
This is Toit's `Stack` + frame marker + `from/to_block`. GC roots become precise stack-slot
scanning here; the conservative C-stack apparatus is removed at this layer.

**L2 — Non-recursive dispatch loop.** INVOKE (incl. block-invoke) pushes a frame and
continues; RETURN pops and continues, delivering the result on the stack (generalise the
hijack's `src2` decode). `run_interpreter` stays one C frame deep; `send()` no longer
recurses for bytecode methods. Primitives remain leaf C calls (no re-entry).

**L3 — Unwinding.** One in-loop unwinder (Toit's `UNWIND_IMPLEMENTATION`): NLR, `ensure:`/
unwind-protect, and (future) exceptions repositioning `sp`/`pc`, replaying cleanup blocks as
bytecode. Replaces the split bytecode-Layer-1 + C-Layer-2 design with a single mechanism.

**L4 — Control-flow lowering (compiler + VM bytecodes).** `ifTrue:`/`ifFalse:`/`and:`/`or:`
→ inline conditional branches; `whileTrue:`/`whileFalse:`/`to:do:`/`timesRepeat:` → bytecode
loops over block-invoke; `value*` → block-invoke (already core in L2). Eliminates the 16
re-entrant combinator sites entirely — the precondition for correct fibers. Touches the
ns-oevm frontend, the bytecode, and both image paths (Path A solidified C, Path B `.oimg`).

**L5 — Fibers.** A fiber = an NS Stack; `fiberSwitch:value:` = swap
`vm_stack`/`vm_stack_ptr`/`current_frame` (Toit's `store/set/load`). Delete ucontext,
`fiber_trampoline`, `g_fiber_registry`, and the `#ifdef __ZEPHYR__` hijack — all subsumed.
`fiberCreate:` pre-builds a fresh fiber's stack as a faked entry call (Toit's `prepare_task`).

**L6 — Cleanup & validation.** Remove dead paths (`OP_SEND_IC`/`send_ic`, the two switch
backends, Task 2b conservative scanning). Re-validate the whole: conformance suite both
image paths, C unit tests, fibers-gc-stress under `OEVM_GC_STRESS=1`, debugger integration,
Perfstex perf (the combinator lowering is where any per-loop cost would show — expect it to
*improve*, since branches/loops beat primitive sends).

### Validation philosophy
Each layer is validated against its own contract as the layer above is built (a layer that
can't yet run whole programs is exercised by targeted harnesses). The full legacy suite is
the acceptance gate once L2+ compose into a runnable engine — used to prove behavioural
equivalence, not to keep the old engine alive mid-rewrite.

---

## 8. Open questions / risks
- **Closure escape: RESOLVED → Option B (§6b).** Was the deepest open fork; now decided as full
  upward closures via reify-on-escape (`home_ctx` = relative SMI on-stack | reified `Context`
  heap oop). The new risk this introduces is *reification correctness* — see the new L0/L1/L3/L5
  obligations in `fiber_l0_frame_abi.md` §6.1 (escape trigger, the SMI-stop-sentinel rework at
  `vm.c:507`/`:1156`, GC of `Context`s, NLR over a reified home).
- **Debugger hook:** runs synchronously, result discarded, guarded by `in_debugger_hook`.
  It can remain a bounded nested interpreter call *only if* a fiber can never switch across
  it; simplest to lower it into the same non-recursive model. Decide during L3/L4.
- **Both image paths:** L0/L4 ABI changes ripple through the compiler, serializer, and both
  Path A and Path B builders. Sequence so the ABI is frozen before either consumer changes.
- **Unwind termination:** the explicit pop-until-target loop must land exactly on the target
  frame (off-by-one = leak or over-pop). Highest-care item in L3.
- **Perf:** lowering combinators to bytecode should be neutral-to-positive; confirm against
  Perfstex / mandelbrot.

## 10. Validation spike — NEXT SESSION (deferred, designed here)

A standalone throwaway C VM (target: `oevm/docs/spikes/fiber_spike.c`, dir already created)
that validates the **L0 frame ABI end-to-end on a toy before any teardown of the real
engine**. It depends on nothing in oevm. Goal: prove the three properties the rewrite rests
on, especially the two the hijack got wrong.

**What it must demonstrate:**
1. **Non-recursive CALL/RET** — the interpreter is one flat C loop; a bytecode call pushes a
   frame on the heap stack and `goto`s the entry; return pops and delivers the result *on the
   stack*. No C recursion per call.
2. **Base-relative links survive stack growth (the §6a property).** A recursive bytecode
   `sum(n)=n+sum(n-1)` run deep enough (e.g. n=2000) to force several `realloc` grows *while
   many frames are live*; assert `sum(2000)==2001000` and report grow count > 0. Because frame
   back-links and locals are addressed by **index** (offset from base), not pointer, the
   realloc that moves `data` needs **zero** fixups — that is the whole point.
3. **2-fiber stack-swap interleave (the bug the hijack broke).** Scheduler task + 2 worker
   tasks, all bytecode, each a separate heap stack. Workers loop `print(tag); yield` 3× via a
   lowered loop (counter local + `JZ`/`JMP` back-edge — i.e. `whileTrue:` as bytecode, *not* a
   C primitive). A `TRANSFER` bytecode is the entire switch: pop value, store current task's
   (sp,fb,pc), set current=target, load target's regs, push value. Assert output is
   **`ABABAB`** (the correct interleave). The hijack produced `ABBBAA`; the spike must not.

**Design notes (so it can be built directly):**
- Stack slot = `intptr_t`. Stack grows upward; `push` checks `sp>=len` → `grow()` (realloc 2×,
  update `len`, pointer moves, **indices unchanged**). No GC in the spike (out of scope; GC
  precision is asserted by the real L1, not here).
- Frame (pushed by `CALL target argc nlocals`): copy `argc` args to a C buffer; push
  `saved_fb` (index), push `saved_pc`; `fb = sp`; push args back as locals 0..argc-1; push
  zeros for extra locals. `LOAD_LOCAL i`/`STORE_LOCAL i` = `data[fb+i]`. `RET`: `result=pop`;
  `sp=fb`; `pc=data[fb-1]`; `fb=data[fb-2]`; `sp=fb-2`; `push(result)`. **`saved_fb` is an
  index ⇒ relative ⇒ realloc-safe.**
- ISA (minimal): `PUSH imm`, `POP`, `ADD`, `SUB`, `OR`, `AND`, `PRINT_CHAR`, `PRINT_INT`,
  `LOAD_LOCAL i`, `STORE_LOCAL i`, `JMP pc`, `JZ pc`, `CALL t argc nlocals`, `RET`,
  `TRANSFER task`, `HALT`.
- Task = `{data,len,sp,fb,pc}`; top-level task body uses `fb=0` with N pre-zeroed locals at the
  bottom (no CALL frame needed for the root). The recursion demo is a single task using
  CALL/RET; the fiber demo is 3 tasks using TRANSFER + lowered loops.
- Worker body (tag `T`): `counter=3`; `wloop: LOAD counter; JZ wdone; PUSH T; PRINT_CHAR;
  counter-=1; PUSH 0(yield); TRANSFER sched; JMP wloop; wdone: PUSH 1(done); TRANSFER sched;
  JMP wdone`. Scheduler alternates `TRANSFER A`/`TRANSFER B`, OR-ing the returned marker into
  per-worker done flags, halting when both done → yields `A B A B A B`.
- Output: explicit `PASS`/`FAIL` per demo + the grow count for demo 2.

This spike is the gate before starting L0/L1 on a real branch — if the relative-link ABI or
the stack-swap interleave is wrong here, fix it on the toy, not in the torn-open engine.

## 11. References
- **L0 ABI contract:** `docs/fiber_l0_frame_abi.md` (the frozen frame/link encoding; §6 the
  closure-semantics decision in full).
- **Spikes:** `docs/spikes/fiber_spike.c` (§10 — dynamic link / flat dispatch / 2-fiber swap,
  green); `docs/spikes/home_link_spike.c` (§6b — lexical-link cross-fiber & escape hazards,
  green).
- Toit VM: `~/workspaceToit/toit/src` — `interpreter_run.cc` (loop + handlers, esp.
  `:1097-1389`), `interpreter.cc` (`load/store_stack`, `handle_stack_overflow`,
  `prepare_task`), `interpreter.h`, `objects.h:912-1066` (`Stack`), `objects.cc:262-333`
  (`roots_do`/`frames_do`), `objects_runtime.cc:132-175` (`copy_to`/`transfer_*`),
  `primitive_core.cc:2012-2048` (`task_new`/`task_transfer`), `scheduler.cc:634-665`.
- oevm: `src/vm.c` (`run_interpreter` 164, `exit_frame` 876, hijack 381-405/444-465/
  743-764/797-816, OP_NLR 492-543, fiber prims 1003-1152), `src/dispatch.c` (`send` 611,
  combinators §5), `src/gc.c` (frame walk 508-600, unwind roots 810).
- R7 memory: `r7-fibers-cooperative-concurrency` (the hijack history, GC Task 2b, native_sim
  proof + `ABBBAA` root cause).
