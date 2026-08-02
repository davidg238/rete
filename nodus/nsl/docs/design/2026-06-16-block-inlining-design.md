# Block inlining via a scope/frame model

**Status:** complete — Phases A–E all landed (see `../plans/block-inlining-status.md`)
**Date:** 2026-06-16
**Repos:** `ns-oevm` (compiler — the bulk) + `oevm` (one small VM truthiness fix)
**Acceptance test:** `ns-tests/04_somns_basic/02_block_inlining.ns` (currently failing)
**Regression gate:** full ns-oevm conformance suite, baseline 19 pass / 10 fail / 1 skip; must never regress a passing test.
**Perf scoreboard:** `fib(32)` and the queens/sieve benchmarks (see `oevm/docs/optimizations.md`).

**Design rationale (topics):** the "why" behind this design is captured as
referenceable topics in `oevm/docs/topics.md` —
[`reflective-frame-access`](../../../../oevm/docs/topics.md#reflective-frame-access),
[`scope-vs-frame`](../../../../oevm/docs/topics.md#scope-vs-frame),
[`why-standalone-scope-class`](../../../../oevm/docs/topics.md#why-standalone-scope-class),
[`block-inlining`](../../../../oevm/docs/topics.md#block-inlining),
[`no-op-first-rollout`](../../../../oevm/docs/topics.md#no-op-first-rollout),
[`jmp-if-false-truthiness`](../../../../oevm/docs/topics.md#jmp-if-false-truthiness).

## Motivation

Profiling `fib(32)` after the dispatch optimizations showed ~70M message sends for
~3.5M calls, dominated by ~17.6M `home` + ~17.6M `regAt:` reflective sends. These
come from the compiler emitting control-flow blocks (`ifTrue:ifFalse:`) as real
closures: reading an enclosing variable from inside a block costs a `home` send
plus a `regAt:` send per lexical level.

The structural fix is **block inlining**: control-flow blocks are not real
activations, so their bodies should compile directly into the enclosing method's
frame, accessing variables as ordinary registers — no closure, no `home`/`regAt:`.
This is also the missing feature behind the failing `02_block_inlining`
conformance test, so the work turns a red test green *and* removes the perf
ceiling. fib getting fast is a consequence of the correct model, not the goal.

## Core idea: separate lexical scope from frame

Today `MethodCompiler` conflates the two — every block is its own
`MethodCompiler` (a frame), and `resolve_name` charges one `home`/`regAt:`
frame-hop per `outer` link. Inlining breaks the "every block is a frame"
assumption, which is the whole point.

New invariants:

- **`MethodCompiler` = a frame.** Methods and *non-inlined* blocks own one.
  `outer` links *frames only*.
- **Lexical scopes nest within a frame.** An *inlined* block introduces a new
  lexical scope but **no new frame**: its body compiles into the enclosing
  frame's instruction stream and register space.

### The `Scope` class (standalone)

A `Scope` owns variable bindings and knows which frame holds its registers.
Variable resolution lives here, decoupled from the frame's register-allocation
and slot/class machinery.

```python
class Scope:
    def __init__(self, frame, parent):
        self.frame = frame        # owning MethodCompiler (where regs live)
        self.parent = parent      # enclosing Scope, or None
        self.vars = {}            # name -> register index within self.frame

    def declare(self, name, reg):
        self.vars[name] = reg

    def resolve(self, name):
        """Return (reg, frame_depth) or None. frame_depth counts real frame
        boundaries crossed from this scope to the defining scope."""
        scope = self
        cur_frame = self.frame
        depth = 0
        while scope is not None:
            if scope.frame is not cur_frame:
                depth += 1
                cur_frame = scope.frame
            if name in scope.vars:
                return (scope.vars[name], depth)
            scope = scope.parent
        return None
```

- A **method** or **non-inlined block** creates a *root* scope bound to its own
  `MethodCompiler`.
- An **inlined block** pushes a *child* scope bound to the **same** frame as its
  parent scope. Crossing it costs **zero** frame hops.
- Shadowing falls out: inner declarations live in the inner scope and are found
  first.

`MethodCompiler` holds the innermost active `Scope` (`current_scope`) and
delegates `resolve_name`:

1. `current_scope.resolve(name)`:
   - `(reg, 0)` → `("local", reg)`
   - `(reg, d>0)` → `("lexical", reg, d)`
2. else slot / nested_class / implicit_send — unchanged (frame-level concerns
   stay on `MethodCompiler`: `self`, `get_self_reg`, class slots).

Because inlined scopes add zero frame hops, the `home`/`regAt:` counts and
`get_self_reg` automatically reflect *real* lexical distance.

## Why the hard cases fall out for free

- **`^` inside an inlined `ifTrue:`** → compiled into the method frame, so
  `compile_statement` emits `OP_RET` (the frame is a method, name has no
  `_block`). Returns from the home method directly — correct and simpler than NLR.
- **A non-inlined block inside an inlined `ifTrue:`** → still its own
  `MethodCompiler` with a `_block` name → `OP_NLR`; its `outer` points at the
  method frame because inlining inserted no frame. Correct home target.
- **Local shadowing** (`[ |a| … ]`) → a pushed scope with a fresh register;
  outer `a` reappears when the scope pops.

## Inlining transformation

In `compile_expr` for `et == "send"`, when the selector is a control-flow
selector **and the relevant arguments are literal blocks** (the receiver too,
for `whileTrue:`/`whileFalse:`), emit jumps and inline the block bodies.
Otherwise fall back to the existing closure+send path (e.g. `someBlockVar value`,
or `bool ifTrue: nonLiteralExpr`).

Inlining a literal block = push a child `Scope` on the current frame, declare its
params/temps as fresh registers, compile its statements into the current
instruction stream, then pop the scope (reclaiming registers, except a result
that must survive).

Lowerings (result register = `target_reg`):

- `cond ifTrue: [A]` → load the `false` symbol into target; `cond→c`;
  `JMP_IF_FALSE c → Lend`; inline A→target; `Lend:`. The not-taken value is the
  `false` symbol, matching the existing `ifTrue:` primitive
  (`dispatch.c` returns `false_symbol`); confirm against the suite in Phase B.
- `cond ifTrue: [A] ifFalse: [B]` → `JMP_IF_FALSE c → Lelse`; A→target;
  `JMP Lend`; `Lelse:` B→target; `Lend:`.
- `ifFalse:`, `ifFalse:ifTrue:` symmetric.
- `cond and: [B]` → short-circuit: if `cond` false, target=false; else inline B.
  `or:` symmetric.
- `[C] whileTrue: [B]` → `Lstart:` inline C→c; `JMP_IF_FALSE c → Lend`; inline B;
  `JMP Lstart`; `Lend:`. `whileFalse:` symmetric.
- `n to: m do: [:i | B]` → loop with induction variable `i` declared in the
  block's scope; `to:by:do:` adds a step.

## Supporting infrastructure

### Labels / backpatching (`MethodCompiler`)

No jump emission exists today (`emit_jump` is referenced but undefined; the jump
opcodes are unused by codegen). Add:

- `new_label()` → an id.
- `emit_jump(op, cond_reg, label)` → emit a placeholder instruction, record its
  index and target label.
- `bind_label(label)` → mark the current instruction index for the label.
- A final patch pass fills signed-16 offsets.

Encodings (from the VM):
- `OP_JMP`: signed-16 offset in bits 8–23 (`(int16_t)(inst >> 8)`).
- `OP_JMP_IF_FALSE`: cond reg in bits 8–15, signed-16 offset in bits 16–31
  (`(int16_t)(inst >> 16)`).
- The VM does `pc++` before decoding, so an offset is relative to the
  instruction **following** the jump.

### VM fix (`oevm/src/vm.c`) — `OP_JMP_IF_FALSE` truthiness

Currently it jumps only when `cond == TAG_SMI(0) || cond == NULL`. But this
language represents booleans as the **symbols** `true`/`false`, so the `false`
symbol is wrongly treated as true. Fix it to jump when `!is_truthy(cond)`, using
the same `is_truthy` that `send()` uses (expose it via a header so there is one
truthiness definition). This is a latent-bug fix required for inlined conditions.

No other VM changes: `OP_JMP`, `OP_RET`, `OP_ADD`/`OP_SUB`, comparisons already
exist.

## Incremental rollout

Each phase keeps the gate ≥19 (and never regresses a passing test), measures
fib + benchmarks, commits, and logs results in `oevm/docs/optimizations.md`.

- **Phase A — Foundation (pure no-op).** Introduce `Scope`; route variable
  resolution through it; add label/backpatch helpers; apply the VM truthiness
  fix. **No inlining.** With every block still a frame (root scope per frame),
  resolved `frame_depth`s match today exactly.
  **Exit criterion (mechanical):** for *every* `.ns` test input, regenerate
  `src/solidified_image.c` before and after the refactor and require a
  **byte-identical diff** (capture the baseline images first, then re-run and
  `diff`). 19/29 unchanged. This proves the no-op claim rather than asserting it;
  see `oevm/docs/topics.md#no-op-first-rollout`. (The VM truthiness fix changes
  runtime behavior of a previously-unemitted opcode only, so it does not affect
  generated bytecode and is compatible with the byte-identical requirement.)
- **Phase B — if-family inlining** (`ifTrue:`/`ifFalse:`/`ifTrue:ifFalse:`/
  `ifFalse:ifTrue:`). Expect the fib win and `02_block_inlining` progress.
  **LANDED.**
- **Phase C — `and:`/`or:`** short-circuit inlining. **LANDED.**
- **Phase D — loops** (`whileTrue:`/`whileFalse:`/`to:do:`/`to:by:do:`). Expect
  queens/sieve wins. **LANDED.**
- **Phase E — refactor** (extract the shared cond/branch-emit skeleton now
  duplicated across Phases B–D; pure no-op cleanup). **LANDED** (`3cf2516`).
  The goal this phase was originally scoped around (`02_block_inlining`) already
  passes — it needed `Array>>do:`, a library method since landed, not loop
  inlining.

## Testing

- **Acceptance:** run `02_block_inlining` each phase to watch it converge.
- **Regression:** full ns-oevm suite; must stay ≥19 with no passing test
  regressing (diff against a saved baseline list).
- **Perf:** `fib(32)` plus queens/sieve via `oevm/bench.sh`.

## Out of scope (tracked separately)

No new closure representation here — the `home`/`regAt:` indirection for
*non-inlined* blocks stays as-is. Replacing it with a direct outer-frame pointer
+ indexed access is **oevm#1**, best done after this lands, on top of the
scope/frame model.
