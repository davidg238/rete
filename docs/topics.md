# Design topics

Durable rationale behind the OEVM/ns-oevm performance and compiler work — the
"why" that gets lost from commit messages and code. Other docs link here by
anchor (e.g. `topics.md#scope-vs-frame`) instead of repeating the reasoning.

Cross-references: `docs/optimizations.md` (perf log),
`ns-oevm/docs/superpowers/specs/` (feature specs), GitHub `oevm#1`.

---

## reflective-frame-access

**Capturing outer variables via `home`/`regAt:` message sends is the core
inefficiency.** A non-inlined block reads an enclosing variable by sending
`home` (once per lexical frame) then `regAt:` — full `send()` dispatches into
`primitive_closure_home` / `primitive_method_context_reg_at`. `self` inside a
block is fetched the same way. Profiling `fib(32)` attributed ~17.6M `home` +
~17.6M `regAt:` sends here — the dominant interpreter cost after dispatch was
optimized.

Two independent fixes attack it:
- **[block-inlining](#block-inlining)** removes the sends for *control-flow*
  blocks entirely (they stop being frames).
- **`oevm#1`** replaces the representation for the *remaining* non-inlined
  blocks: a direct outer-frame pointer + indexed access instead of reflective
  sends. Deferred until after inlining, built on the [scope/frame](#scope-vs-frame)
  model.

---

## scope-vs-frame

**A lexical scope (for name resolution and shadowing) is not the same thing as a
frame (a runtime activation).** The original compiler conflated them: every block
was its own `MethodCompiler` (a frame), and a variable from an enclosing scope
cost one `home`/`regAt:` frame-hop per lexical level.

That assumption — "every block is a frame" — is exactly what inlining breaks:
- An **inlined** block (e.g. an `ifTrue:` body) introduces a new *scope* (so
  `[ |a| … ]` can shadow) but **no new frame**: its code runs in the enclosing
  method's frame, so its variable accesses are ordinary register reads.
- A **non-inlined** block introduces *both* a scope and a frame.

Once access cost is computed from *frame* distance (not *scope* nesting), the
hard cases stop being special: a `^` inside an inlined `ifTrue:` is a plain
`OP_RET` from the method; a non-inlined block nested inside an inlined `ifTrue:`
still has the method as its home frame, because inlining inserted no frame.

---

## why-standalone-scope-class

**Variable resolution belongs in one small object with a clean interface,
separate from the frame's register/slot/class machinery.** A `Scope` holds
`name → register` bindings, a link to its parent scope, and the frame that owns
its registers; `resolve(name)` returns `(reg, frame_depth)` by walking parents
and counting frame boundaries crossed.

Why a class rather than an ad-hoc scope stack inside `MethodCompiler`:
- **Clarity** — one place answers "where does this name live, and how far in
  frames is it?"; the frame keeps doing register allocation and slot/class
  lookup.
- **Easier to optimize later** — it is the natural home for the `oevm#1` closure
  representation (a captured `Scope` can own an environment), for per-scope
  register reuse, and for lexical-capture analysis. These are localized changes
  to `Scope` instead of surgery across resolution sites.
- **Testability** — resolution can be reasoned about and checked independently of
  codegen.

---

## block-inlining

**Control-flow blocks are not real activations, so they should not compile to
closures.** `ifTrue:ifFalse:`, `and:`/`or:`, `whileTrue:`, `to:do:` etc. with
*literal* block arguments lower to conditional jumps with their bodies compiled
directly into the enclosing frame — no closure allocation, no `value` send, no
[reflective frame access](#reflective-frame-access). Non-literal cases (e.g.
`someBlockVar value`) keep the existing closure path.

This is simultaneously a **correctness** feature (the failing
`02_block_inlining` conformance test needs it) and the **performance** ceiling
remover. fib getting fast is a *consequence* of the correct model, not a
benchmark-specific hack — which is the bar this project holds itself to.

### Phases A–C (if-family, and:/or:)

See `docs/optimizations.md` Phase A–C entries for the incremental rollout:
Phase A was a behaviour-preserving no-op (Scope model + jump helpers); Phase B
inlined the four `if` variants; Phase C added `and:`/`or:` short-circuit
inlining. All share the same `compile_inlined_block` primitive: compile a block
body under a child `Scope` on the enclosing frame, so every variable access is a
plain register read.

### Phase D — loop lowerings

Phase D adds four loop forms. All compile to back-edge jump loops in the
enclosing frame. Two capabilities were added to `compile_inlined_block`:

**`param_regs`** — binds a block parameter to a caller-allocated register (e.g.
the induction variable for `to:do:`). The parameter is visible inside the block
body as an ordinary register; no closure capture is needed.

**`init_temps`** — emits `OP_LD_CONST nil` for each block-local temp at the head of
every iteration, faithfully reproducing fresh-activation semantics without
allocating a closure.

#### `to:do:` lowering

```
  <receiver expr>   → reg_start
  <limit expr>      → reg_limit
  reg_induction ← reg_start
loop_top:
  init_temps (body block locals)
  <body block, induction visible as param>
  reg_induction ← reg_induction + 1
  JMP_IF_FALSE (reg_induction <= reg_limit), loop_top
  result ← reg_start   (receiver, per Newspeak semantics)
```

The `to:do:` selector is never emitted; zero sends remain.

#### `to:by:do:` lowering

Same structure as `to:do:`, but the step integer is a literal known at compile
time. The comparison direction is fixed at compile time: `<=` when step > 0,
`>=` when step < 0. Two cases fall back to the `PRIM_to_by_do` primitive path
(unchanged behaviour): a non-literal step (direction unknown at compile time),
and a literal `by: 0` step — the primitive does zero iterations and returns the
receiver, whereas an inlined `i := i + 0` loop would never terminate.

#### `whileTrue:` / `whileFalse:` lowering

```
loop_top:
  <condition block body>       → reg_cond
  JMP_IF_FALSE reg_cond, loop_exit   (negated for whileFalse:)
  init_temps (body block locals)
  <body block body>
  JMP loop_top
loop_exit:
  result ← nil                 (per Newspeak semantics)
```

Both condition and body must be literal zero-arg blocks; otherwise the
closure+send path is used. Body block locals are nil-reset on every iteration
via `init_temps`.

### Slot-layout fix (superInit / inheritance)

Loop inlining changed register and heap allocation patterns and exposed a latent
slot-layout bug: a child class inheriting more parent slots than its own declared
params ended up with a too-short layout, causing inherited method reads to go out
of bounds. The compiler fix (`ns-oevm/tools/ns_compile_oevm.py`) has two parts:

- `collect_slots` merges parent slots before child slots to produce a layout
  wide enough for all inherited fields.
- `compile_initializer` inlines a single-level parent `new:` call rather than
  emitting a `superInit` send.

**Limitations (by design, documented in compiler):** parent must appear before
child in source order (silent skip otherwise); single-level inheritance only —
grandparent chains are not walked. See `docs/optimizations.md` Phase D for the
symptom that revealed the bug and the conformance evidence.

---

## calling-convention-invariants

Two register/layout invariants the codegen must preserve. Both were violated by
latent bugs that only surfaced under the SOMns benchmarks (see
`docs/optimizations.md` "Conformance correctness fixes").

**Send arguments occupy *contiguous* registers immediately above the receiver.**
`OP_SEND`/`OP_SEND_IMPLICIT` and the class-init send read `argc` operands from
`rcvr+1, rcvr+2, …`; the VM does not get an arg list, only the base register and
count. So the arg-evaluation loop must place arg *i* in exactly `rcvr+1+i`. The
trap: an argument is itself an expression that allocates scratch registers — a
nested send, a block/closure, an inlined `to:do:` — and leaves `next_free_reg`
above its result. If the next arg is then allocated with a bare `alloc_reg()`,
it lands in a *gap* and the VM reads stale scratch (e.g. a nested send's leftover
`self`) as the missing argument. **Rule: after compiling each argument, reset
`next_free_reg = areg + 1`.** The arg's result sits in `areg`, below the
reclaimed scratch, so reclaiming is always safe. This must hold in all three arg
loops (normal send, implicit-receiver send, class-initializer send).

**A nested instance's `outer` (slot 0) is the instance of its lexically
enclosing CLASS — not whatever `self` happens to be at the `new` site.** When
`Foo new` instantiates a class nested in container `K`, slot 0 must point at the
live `K` instance. That instance is reached by walking `self`'s outer chain
`hops` times, where `hops` (from `resolve_class_ref`) is how many lexical levels
separate the current class from `K`. `hops == 0` (instantiating a class defined
in the current class) means `self` *is* the `K` instance — store it directly.
`hops > 0` happens when instantiating a *sibling/cousin* class that shares an
ancestor container: storing `self` there makes outer-slot access (reading an
enclosing class's slots) read the wrong object. The naive "always store self"
codegen is the `hops == 0` special case; the general rule walks the chain. This
is why outer-class constants resolve correctly only when the outer pointer is set
from the enclosing instance, consistent with the [scope/frame](#scope-vs-frame)
model and the depth-aware reads in `compile_expr`'s `varRef`/`assign` slot paths.

---

## no-op-first-rollout

**Land a structural refactor as a behavior-preserving no-op before any
optimization rides on it.** Phase A introduces the [Scope](#why-standalone-scope-class)
model and jump infrastructure but inlines nothing; with every block still a
frame, resolved frame-depths match the old code exactly.

Exit criterion is *mechanical*, not "looks fine": regenerate
`src/solidified_image.c` for every test input before and after the refactor and
require a **byte-identical diff**. This guarantees we never debug "new model" and
"new optimization" at the same time, and keeps technical debt out of the
foundation.

---

## jmp-if-false-truthiness

**Booleans in this VM are the symbols `true`/`false`, so conditional jumps must
use the same truthiness as message dispatch.** `OP_JMP_IF_FALSE` originally
jumped only when `cond == TAG_SMI(0) || cond == NULL` — it treated the `false`
*symbol* as true. Harmless only because nothing emitted the opcode yet; inlined
conditions expose it. The fix routes it through the one `is_truthy` used by
`send()`, so there is a single definition of truth shared by dispatch and
control flow.

---

## selector-dispatch-caching

**Selectors are interned (one stable `Symbol` pointer per name), so primitive
interception can be a cached integer-id lookup instead of a `strcmp` gauntlet.**
`send()` formerly ran up to 47 `strcmp`s on every message before vtable lookup.
A pointer-keyed cache classifies each symbol once into a primitive id
(`PRIM_NONE` for the common non-primitive case); dispatch then skips the gauntlet
or jumps through a `switch` jump table. See `docs/optimizations.md` Opt #1/#2 for
measurements.
