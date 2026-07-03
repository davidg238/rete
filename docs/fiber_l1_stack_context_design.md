# L1 — Stack object & lexical environment (Context / indirect temp-vectors)

**Status:** DESIGN, approved 2026-06-30 (brainstorm). Second layer of the Option-C
de-recursify rewrite (`fiber_implementation.md` §7; builds on the L0 contract
`fiber_l0_frame_abi.md`). **Branch:** `derecursify-fibers` (oevm).
**Scope:** the per-fiber call stack as a first-class heap object, and the
closure/lexical-variable model — implemented as compiler-emitted **indirect
temp-vectors** (the tested Smalltalk solution), which *supersede* the
reify-on-escape sketch L0 carried.

---

## 1. Decisions this design rests on (with rationale)

| | Decision | Why |
|---|---|---|
| Q1 | **Relative links go live in L1 on fixed-size stacks; growth + flat dispatch are L2.** | Growth can't activate while dispatch recurses (C locals cache absolute frame addresses; a realloc mid-call dangles them). Making links relative on a *non-moving* base resolves identically to today → contract goes live, suite stays green as a free equivalence check, and L2's risk is decoupled. |
| Q2 | **`Stack` is a first-class heap object** (main thread becomes "fiber 0"). | One uniform GC mechanism; stack-as-value opens continuations/snapshot/serialization later (ties to remote-debug + `.oimg`); gives NLR a *fiber identity* (the Stack oop) to detect cross-fiber `^`; awkward to retrofit, so paid up front. Doesn't disturb L0 (offsets are offsets). |
| Q4 | **Captured variables live in compiler-emitted indirect temp-vectors** (shared heap cells), not reified-on-escape copies. | Correct-by-construction shared mutable bindings (a copy would diverge a block's view of an outer var from the home method's). The battle-tested Smalltalk model. The L0-reserved `Context` *is* the temp-vector. |

**Correction folded in (was misstated during L0):** full upward closures enable a block
to **read/write captured variables after its home method returns** (temp-vectors are heap,
they survive) — but a **non-local return (`^`) after the home returned remains an error**
(`BlockCannotReturn`). That error is correct full-Smalltalk semantics; we keep it, guarded by
generation + Stack identity. Upward *data* ≠ upward *control*.

---

## 2. Components

### 2.1 `Stack` — first-class per-fiber call stack (heap object)
A GC-managed object whose payload holds the `MethodContext` frames (bump-allocated within it,
exactly as frames live on `vm_stack` today). Shape:

```
struct Stack {
    struct Object base;     /* vtable = StackVtable; ordinary heap object */
    uint32_t capacity;      /* slots in `data` */
    uint32_t sp;            /* bump cursor (top), word index */
    uint32_t current_off;   /* offset of the live frame (current_frame) within data */
    oop      data[];        /* the frame region: MethodContext headers + regs */
};
```

- The **main thread becomes fiber 0**: at VM init allocate the main `Stack` early (before the
  first GC — see §6 bootstrap) and retire the static `boot_stack`. `vm_stack`/`vm_stack_ptr`/
  `current_frame` become views onto the active `Stack` (`data` / `sp` / `data+current_off`).
- **Switching fibers = swap the active `Stack`** (Toit's store/load): save sp/current_off into
  the outgoing `Stack`, point the active register at the incoming one. (Activation happens in
  L5; L1 only makes the representation a `Stack` object.)
- **Fixed capacity in L1** (no realloc yet). Overflow still aborts as today; L2 turns the abort
  into grow-by-reallocating-a-bigger-`Stack` (safe once dispatch is flat).

### 2.2 `Context` — indirect temp-vector (heap object)
The shared cell for captured variables (supersedes the L0 reify-on-escape `Context`):

```
struct Context {
    struct Object base;     /* vtable = ContextVtable; ordinary heap object */
    oop      outer;         /* enclosing scope's Context | NULL */
    uint32_t num_slots;     /* number of captured variables held here */
    oop      slots[];       /* the captured variables (the shared bindings) */
};
```

One `Context` per *activation that has captured variables*. Both the home method **and** any
block that closes over those variables reference the same `Context` → mutation is shared by
construction. It is heap-allocated, so it outlives the stack frame when a block escapes.

### 2.3 `Closure` — amended
Today: `{ base, home_ctx, home_generation, entry_point }`. The single `home_ctx` (which served
double duty) splits:

```
struct Closure {
    struct Object base;
    oop      env;             /* innermost enclosing Context (lexical env) | NULL */
    oop      home_stack;      /* Stack oop of the home METHOD frame (NLR fiber identity) */
    uint32_t home_offset;     /* relative offset of the home method frame in home_stack */
    uint32_t home_generation; /* gen of the home method frame (BlockCannotReturn if stale) */
    void    *entry_point;
};
```

- `env` drives lexical variable access (§3).
- `home_stack`/`home_offset`/`home_generation` drive NLR (§4).

### 2.4 `MethodContext` frame — links relative, lexical field repurposed
Header offsets unchanged (32/64 parity + reification trick). Field *meanings* under L0/L1:
- `prev_frame` (off 1): **relative SMI** dynamic link (`frame_abi.h`).
- `saved_pc` (off 2): absolute, into immutable code (unchanged).
- `home_ctx` (off 3): **repurposed → `context`**, this frame's own `Context` (its temp-vector),
  or NULL if the activation has no captured variables.
- `closure` (off 4): the running `BytecodeClosure` (unchanged); reaches `env`/`home_*`.
- `gen_tag`, `cleanup_cl`, `regs[]`: unchanged. **Plain (non-captured) locals stay in `regs[]`**
  (fast path); only captured locals live in the `Context`.

---

## 3. Lexical variable access (the temp-vector chain)

- **Plain local** → `regs[i]` (unchanged fast path; `OP_LD_REG`/`OP_ST_REG`).
- **A captured local of the *current* activation** → `frame.context->slots[i]` (the method's own
  indirect-temp access).
- **An outer captured variable** (`OP_LD_LEXICAL depth,idx` / `OP_ST_LEXICAL`) → walk
  `closure.env` then `Context.outer` a total of `depth` times, then index `slots[idx]`.

**`depth` counts temp-vector hops, not source nesting** — scopes with no captured variables have
no `Context` and do not appear in the chain. The compiler (L4) maps each variable reference to
its `(hops, slot)` and emits the right `depth`. The L1 runtime contract is just "walk `outer`
`depth` times, index `slots[idx]`." All links are heap → cross-fiber- and pop-safe automatically;
the L0 relative-vs-reified discriminator is **not** needed for the lexical link.

**Chain construction.** When a frame's `Context` is created, set `context->outer = closure.env`
(link to the enclosing environment). When a closure is created (`OP_CLOSURE`):
`new.env = current_frame.context ? current_frame.context : current_frame.closure.env`
(prepend this activation's vector if it has one, else pass the enclosing env through).

**Context allocation timing (L1): eager on entry** of a method/block whose `CompiledMethod`
declares captured variables (a new field, e.g. `num_captured`); captured parameters are copied
into it at entry. *Lazy allocation on first `OP_CLOSURE`* is a later optimization (deferred —
eager is correct and simpler; avoids the "write before first block" subtlety).

---

## 4. Non-local return (`^`) and cross-fiber safety

`^expr` in a block returns from the **home method**. Resolution:
1. From the running `closure`, read `home_stack` / `home_offset` / `home_generation`.
2. **Cross-fiber check:** if `home_stack != <active Stack>` the block is running on a different
   fiber than its home → `BlockCannotReturn`. (This is the check first-class `Stack` makes
   possible — the Stack oop is the fiber identity. A bare relative offset could not detect it.)
3. **Liveness check:** resolve the home frame at `home_stack.data + home_offset`; if its
   `gen_tag` ≠ `home_generation`, the home method already returned → `BlockCannotReturn`.
4. Otherwise unwind to that frame and deliver the result (the L3 unwinder; L1 only fixes the
   representation + the two checks).

`home_*` is stamped at closure creation: in a method frame, the home is that frame; in a block
frame, the new closure inherits `home_*` from the enclosing `closure` (so it always names the
method). Generation + Stack identity together make a stale or cross-fiber `^` a clean error
rather than corruption.

---

## 5. Compiler capture model (contract pinned now; implementation in L4)

L1 is runtime, but Option 2 couples it to a frontend contract, fixed here:
- The compiler partitions each scope's locals into **plain** (frame `regs[]`) and **captured**
  (referenced by an inner block → emitted into that scope's `Context`).
- `CompiledMethod` gains `num_captured` (and the slot mapping) so the VM can size/allocate the
  `Context` and route captured-local access through it.
- Variable references compile to `(hops, slot)` against the temp-vector chain; `OP_LD_LEXICAL`/
  `OP_ST_LEXICAL` carry `depth = hops`.
- Both image paths (Path A solidified C, Path B `.oimg`) must serialize `num_captured`/mapping
  — this lands with the L4 compiler work, the first consumer to change either path.

The L4 plan implements this; L1 implements the runtime side against the contract and is
exercised by a synthetic harness (§7) until L4 emits real captured-variable bytecode.

---

## 6. Garbage collection

Two precise trace functions replace the bespoke stack scans:
- **`Stack` trace:** walk frames from `current_off` via the relative `prev_frame`
  (`frame_link_decode`); for each frame mark `vtable`, `context` (its `Context`), `cleanup_cl`,
  and its live `regs[]` (count by gap to the next frame — the existing precise logic at
  `gc.c:565`, moved into the `Stack` tracer). Do **not** mark `prev_frame`/`saved_pc` (an SMI
  offset / a code pointer, not heap roots).
- **`Context` trace:** mark `outer` and all `slots[]`.

The conservative **C-stack** apparatus (ucontext per-fiber C stacks + `g_fiber_registry` +
`scan_c_range`) **stays in L1** — it is only removable when fibers stop needing separate C
stacks, i.e. when dispatch goes flat (L2) and ucontext is deleted (L5). (`fiber_implementation.md`
§7 L1's "remove conservative scan" is sequenced here to L2/L5; L1 cannot remove it while
dispatch still recurses.)

**Bootstrap (the one fiddly bit):** the GC scans roots through the active `Stack`, but the
`Stack` is itself GC-allocated. Allocate the main `Stack` at VM init *before any allocation that
can trigger GC*, and make the active-`Stack` register a GC root so the collector can always find
it. (Toit allocates the initial task/stack early for the same reason.)

---

## 7. L1 scope, boundary, and acceptance

**In L1:**
- `Stack`/`StackVtable`/allocator + main-thread-as-fiber-0 (retire `boot_stack`); fixed capacity.
- `Context`/`ContextVtable`/allocator (the temp-vector); amended `Closure`.
- Frame setup + all frame walks (GC, `OP_LD_SENDER`, NLR target search) switched to the
  `frame_abi.h` relative-link helpers; the legacy `IS_SMI(home_ctx)`-means-stop walks
  (`vm.c:507`, `:1156`) reworked to the new `context`/`closure.env` model.
- Lexical access (§3) and NLR checks (§4) against the new representation.
- The two precise GC tracers (§6).

**NOT in L1 (explicit):** stack *growth* (L2), the non-recursive dispatch loop (L2), deleting
ucontext / conservative C-stack scan (L2/L5), combinator lowering + the real compiler
captured-variable emission (L4), the unified unwinder (L3 — L1 only adds the NLR *checks* and
representation).

**Acceptance:**
1. **Suite stays green, both image paths** — relative links on fixed (non-moving) stacks are
   behavior-equivalent to today's absolute links; the first-class `Stack` and `Context` are
   transparent to existing programs. Green here is the equivalence oracle for the representation
   change. *(Per the rewrite ethos this is a welcome check, not a hard continuous-green
   constraint — but L1 is specifically designed to be equivalent, so it should hold.)*
2. **`tests/test_frame_abi.c`** (the L0 proof) stays green.
3. **New synthetic harness** `tests/test_context.c`: build a `Context`, share a captured var
   between a "method" frame and a "block" (both mutate it → both observe the change), run GC,
   assert the `Context` and its slots survive and stay shared (proves temp-vector semantics +
   `Context` tracing before L4 emits real captured-variable bytecode). Green plain + ASan.
4. **`fibers-gc-stress` gate** still passes (the existing fiber GC regression).

---

## 8. Performance characterization (corrected, honest)

- **Plain locals: unchanged** (frame `regs[]`, fast).
- **Captured variables cost one `Context` allocation per activation that has them** — the
  classic Smalltalk indirect-temp cost, **not** zero. Eager-on-entry in L1 (lazy is a later
  optimization).
- **Control flow pays nothing extra after L4:** `do:`/`whileTrue:`/`ifTrue:`/`to:do:` get
  *lowered to bytecode* (L4) and create **no closures** → no temp-vectors. So the `Context` cost
  lands only on **user-written blocks that actually capture** — exactly where the upward-closure
  capability is being used.
- **Relative-link access:** one offset→address add on frame-link/`OP_LD_SENDER`/GC-walk
  (negligible; these are not the hottest ops).
- **Lexical access:** `depth` `outer` hops (depth normally 1–2) + one index.
- Net for typical code is small; **Perfstex is the gate (L6)**, and the de-recursify whole
  (flat dispatch + lowered combinators) is expected neutral-to-faster.

---

## 9. L0 amendment required (folded in during implementation)

L0 (`fiber_l0_frame_abi.md`, on-branch/unpushed) described reify-on-escape; Option 2 supersedes
it. The implementation will:
- Replace `struct Context` in `src/frame_abi.h` with the temp-vector shape (§2.2) and drop the
  `home_*` fields from `Context` (they move to `Closure`, §2.3).
- Note in `fiber_l0_frame_abi.md §6.1` that the reify-on-escape obligations are superseded by the
  temp-vector model (this design); keep the `frame_link_*` (dynamic link) helpers as-is — those
  are unaffected and already proven.
- Update `fiber_implementation.md §6b/§6b.1` (the perf story → the corrected §8 above; the
  upward-data-vs-upward-control correction).

The `frame_abi.h` dynamic-link encoding and `tests/test_frame_abi.c` remain valid as frozen.

---

## 9a. Reflection / mirror interaction (confinement preserved; meta-level completed)

Reifying the call stack and lexical environment as heap objects *completes* the reflective
object model — but must not perturb the capability-confined mirror story.

- **What it unlocks (future, not L1 scope):** `Stack`/`Context`/`Closure` are now genuine
  objects, so future confined mirror facets — `FiberMirror`/`ContextMirror`/`ClosureMirror` —
  can reflect over *real objects* instead of the debugger's bespoke C frame-peeking prims. A
  suspended fiber is a `Stack` value → directly serves snapshot/marshal for remote-debug (R3)
  and any later continuation work. This realises the Newspeak "reified activations" ideal.
- **Confinement INVARIANT (an L1 implementation constraint):** these objects stay **below the
  waterline** — VM-internal, reachable only through the *granted* mirror capability, never via
  ordinary message sends. **No public selector may return a `Stack`/`Context`/`Closure`**, and
  any reflective operation on them remains `doesNotUnderstand` unless the mirror grants it
  (the existing stratify-execution "reflection off by default" model extends unchanged). L1
  adds the objects; it must **not** add a public accessor that leaks them. This opens no new
  reflection hole — it gives the *existing* confined mirror richer objects to govern.

## 10. Risks / open items
- **Bootstrap ordering** (§6): main `Stack` must exist before first GC; get the init sequence
  right (highest-care item).
- **Compiler/runtime contract drift:** `num_captured` + the `(hops, slot)` mapping must agree
  between L1 (runtime) and L4 (emitter); the synthetic harness (§7.3) pins the runtime side now.
- **Equivalence under fixed stacks:** confirm relative-link resolution is bit-equivalent to the
  absolute path on a non-moving base (it is, by construction; the suite is the check).
- **`OP_LD_SENDER` / debugger** walks that read `prev_frame` directly must all route through
  `frame_link_decode` — inventory them (audit `prev_frame` readers) so none is missed.

## 11. References
- `docs/fiber_implementation.md` (Option-C decision, layered plan, §6b closure semantics).
- `docs/fiber_l0_frame_abi.md` + `src/frame_abi.h` + `tests/test_frame_abi.c` (the frozen L0
  dynamic-link encoding; `Context` shape amended by §9).
- Toit: `objects.h:912` (`Stack`), `objects.cc:262` (`roots_do`), `interpreter.cc`
  (`store/load_stack`, `prepare_task`).
- Smalltalk indirect temp-vectors: the classic shared-binding solution this design adopts.
