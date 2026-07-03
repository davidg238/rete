# De-recursify Phase 1 — Finish Lowering (OP_BLOCK_INVOKE) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Introduce a single `OP_BLOCK_INVOKE` bytecode, route every block invocation (explicit `value`/`value:`/… and the non-literal combinator fallbacks) through it, and delete the recursive block-invoking combinator primitives — while the C engine STAYS recursive and the suite stays green on both image paths.

**Architecture:** This is Phase 1 of the "core de-recursify" design (`oevm/docs/fiber_derecursify_core_design.md`, approved 2026-06-30). The design's key insight: the flat-dispatch flip (Phase 2) is only *clean* if no C primitive invokes a block by recursively re-entering `run_interpreter`. So Phase 1 makes every block invoke route through one uniform bytecode (`OP_BLOCK_INVOKE`) — executed for now by a still-recursive `run_interpreter` call — and lowers the non-literal `ifTrue:`/`and:`/`to:do:`/`whileTrue:` fallbacks (which today hit recursive C primitives) to bytecode that drives `OP_BLOCK_INVOKE`. The dead C primitives are then deleted. No hybrid: after Phase 1, every block invoke is one mechanism.

**Tech Stack:** C99 (oevm VM: `src/vm.c`, `src/oevm.h`, `src/dispatch.c`), Python 3 (compiler `ns-oevm/tools/ns_compile_oevm.py`; image generators `tools/solidify.py` Path A, `tools/oimg_serialize.py` Path B), Newspeak `.ns` (ns-tests suite).

## Global Constraints

- **Repos & cwd:** VM edits in `/home/david/workspace/oevm` (`src/`). Compiler/generator/plan edits in `/home/david/workspace/ns-oevm`. Run the suite from the `ns-oevm` cwd.
- **Both image paths, every task:** `cd /home/david/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py` must pass with **no new failures** vs. the pre-Phase-1 baseline (currently **73/0/6**). Path A = solidified-C image; Path B = `.oimg` image-VM. Both carry raw bytecode unchanged, so a new opcode only needs: (a) compiler emits it, (b) `enum Opcode` in `oevm.h` defines it, (c) `vm.c` handles it. There is NO separate serializer change for a new opcode number.
- **Opcode numbering:** the next free opcode is `0x16` (`OP_LD_SENDER = 0x15` is the current max). Use `OP_BLOCK_INVOKE = 0x16`. The number MUST be identical in `oevm/src/oevm.h` (`enum Opcode`) and `ns-oevm/tools/ns_compile_oevm.py` (the `OP_*` constants block, lines 10–29).
- **Engine stays recursive in Phase 1.** Do NOT touch the flat-loop flip, the Zephyr hijack, ucontext, `unwind_target_*`, or fibers here — those are Phase 2/3.
- **Behavior-equivalence is the oracle.** Every lowering must match the semantics of the primitive it replaces (result value AND side effects). The relevant equivalences are quoted inline per task.
- **Commit after each task** with the trailer:
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`
- **Do NOT push.** Push only on David's explicit say-so (house rule).
- **Baseline first:** before Task 1, capture the baseline counts (see Task 0) so "no new failures" is checkable.

---

### Task 0: Capture baseline + scope grep

**Files:** none (read-only measurement).

- [ ] **Step 1: Record the current green baseline (both paths)**

Run: `cd /home/david/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | tail -5`
Expected: a pass/fail/skip summary (baseline is 73/0/6). Write the exact numbers into your working notes — every later task compares against them.

- [ ] **Step 2: Confirm the block-invoking primitives that Phase 1 will delete still exist**

Run: `cd /home/david/workspace/oevm && grep -nE 'case PRIM_(to_do|to_by_do|ifTrue|ifFalse|ifTrueIfFalse):' src/dispatch.c`
Expected: five `case` lines. These are the deletion targets (Tasks 2–4). If the set differs, reconcile with the design before proceeding.

- [ ] **Step 3: Survey reflective combinator sends (deletion-safety check)**

Run: `cd /home/david/workspace/ns-oevm && grep -rnE "perform:.*#(ifTrue|ifFalse|to:do|to:by:do|and|or|whileTrue|whileFalse)" ../ns-tests 2>/dev/null; echo "exit=$?"`
Expected: no matches (`exit=1`). If any suite code sends these selectors reflectively via `perform:`, note it — deleting the C primitive would turn that into a `doesNotUnderstand`, and that case must instead be handled by an image-level method. (No matches ⇒ deletion is safe.)

---

### Task 1: `OP_BLOCK_INVOKE` opcode + recursive-engine handler + route `value`-family sends to it

**Files:**
- Modify: `/home/david/workspace/oevm/src/oevm.h` (the `enum Opcode` block — add `OP_BLOCK_INVOKE = 0x16`)
- Modify: `/home/david/workspace/oevm/src/vm.c` (add `case OP_BLOCK_INVOKE:` to the `switch (op)` in `run_interpreter`)
- Modify: `/home/david/workspace/ns-oevm/tools/ns_compile_oevm.py` (opcode constant + route `value`-family sends)
- Test: `/home/david/workspace/ns-tests/<new>_block_invoke.ns` (a new suite file; match the existing suite naming/numbering — inspect `../ns-tests` for the next free number and the file conventions)

**Interfaces:**
- Produces: `OP_BLOCK_INVOKE = 0x16` — operand layout **identical to `OP_SEND`**: `a` (bits 8–15) = argc, `b` (bits 16–23) = selector constant index, `c` (bits 24–31) = receiver register (also the result destination). Later tasks emit it via `self.emit_ABC(OP_BLOCK_INVOKE, argc, cidx, base)`.
- Semantics: if `regs[c]` is a `BytecodeClosureVtable` object → invoke it as a block (recursive `run_interpreter` for now); otherwise → fall back to a normal `send(receiver, selector, argc, args)`. The fallback is essential: `value` (0-colon) is also `Association>>value` etc., so a non-closure receiver must dispatch normally.

- [ ] **Step 1: Write the failing test**

Create `../ns-tests/<n>_block_invoke.ns` (pick `<n>` = next free suite number). It must exercise a **non-literal** block invoked via `value:` (the case that today routes through the `dispatch.c` `BytecodeClosureVtable` intercept, and after this task routes through `OP_BLOCK_INVOKE`). Model the structure on an existing single-`main` suite file. Content of the driver method:

```
main
    | adder result |
    adder := [:x | x + 1].
    result := adder value: 41.
    ^ result printString
```

Expected program output: `42`. Register the file in the suite's manifest the same way peer test files are registered (inspect a sibling `.ns` file and `tools/run_tests_oevm.py` for how expected output is declared).

- [ ] **Step 2: Run the suite to confirm the new test PASSES on the current build (guards against a vacuous test)**

Run: `cd /home/david/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | grep -iE 'block_invoke|PASS|FAIL' | tail -5`
Expected: the new test PASSES *today* (the value: send currently works via the intercept). This confirms the test is wired correctly before we change the compile path. (We are re-routing an already-working case through a new opcode; the behavioral guard is "still passes after re-routing", plus the both-paths gate.)

- [ ] **Step 3: Add the opcode to the VM enum**

In `/home/david/workspace/oevm/src/oevm.h`, in `enum Opcode`, add the new member after `OP_LD_SENDER = 0x15,`:

```c
    OP_LD_SENDER  = 0x15,   /* dest, src1=ctx-reg : load prev_frame (sender) of the context in R(src1) */
    OP_BLOCK_INVOKE = 0x16, /* a=argc, b=selector-const, c=receiver reg (== OP_SEND layout).
                               Fast-path: invoke a BytecodeClosure as a block; else fall back to send(). */
```

- [ ] **Step 4: Add the opcode constant to the compiler**

In `/home/david/workspace/ns-oevm/tools/ns_compile_oevm.py`, in the opcode-constants block (currently ending `OP_CLOSURE = 0x14` around line 29), add:

```python
OP_LD_SENDER  = 0x15
OP_BLOCK_INVOKE = 0x16
```

(Add `OP_LD_SENDER` too if the compiler side doesn't already define it; keep both in numeric sync with `oevm.h`.)

- [ ] **Step 5: Add the VM handler**

In `/home/david/workspace/oevm/src/vm.c`, inside the `switch (op)` in `run_interpreter`, add a `case OP_BLOCK_INVOKE:` immediately after the existing `case OP_SEND:` block. It is a copy of the `OP_SEND` body with ONE change — the call site. Copy the `OP_SEND` case verbatim (including its `#ifdef __ZEPHYR__` reload block and its `if (unwind_target_reached)` check and the trailing `ctx->regs[src2] = result;`), and replace only the `oop result = send(target, selector, send_argc, send_args);` line with:

```c
            case OP_BLOCK_INVOKE: {
                // Layout identical to OP_SEND. Fast-path: a BytecodeClosure is
                // invoked as a block (still recursive in Phase 1); any other
                // receiver falls back to a normal send (e.g. Association>>value).
                oop selector = cm_const_at(method, src1);
                oop target = ctx->regs[src2];
                int send_argc = dest;
                oop *send_args = (send_argc > 0) ? &ctx->regs[src2 + 1] : NULL;

                ctx->saved_pc = pc;
                oop result;
                if (target != NULL && obj_vtable(target) == BytecodeClosureVtable) {
                    result = run_interpreter(target, target, send_argc, send_args);
                } else {
                    result = send(target, selector, send_argc, send_args);
                }

                /* --- BEGIN: copy verbatim from the OP_SEND case tail --- */
                /* #ifdef __ZEPHYR__ ... reload-current_frame hijack block ... #endif */
                /* if (unwind_target_reached) { ... goto exit_frame ... }        */
                /* --- END copy ---                                               */

                ctx->regs[src2] = result;
                break;
            }
```

The two commented regions MUST be filled by pasting the corresponding lines from the adjacent `OP_SEND` case (the `#ifdef __ZEPHYR__` reload block and the `unwind_target_reached` propagation check) unchanged — they are receiver-agnostic and apply identically.

- [ ] **Step 6: Route compiled `value`-family sends to `OP_BLOCK_INVOKE`**

In `/home/david/workspace/ns-oevm/tools/ns_compile_oevm.py`, in the normal message-send path (the block that ends around line 686–689 with `self.emit_ABC(OP_SEND, len(args), cidx, rcvr_reg)`), detect the block-value selector family and emit `OP_BLOCK_INVOKE` instead. Add a small predicate helper on `MethodCompiler`:

```python
    @staticmethod
    def _is_block_value_selector(selector):
        # "value", "value:", "value:value:", ... — the fixed-arity block-invoke
        # protocol. Matches "value" followed by zero or more ":" and nothing else.
        # (valueWithArguments: deliberately does NOT match — it spreads an array
        #  and stays an ordinary send for now.)
        if not selector.startswith("value"):
            return False
        rest = selector[len("value"):]
        return rest == "" or all(ch == ":" for ch in rest)
```

Then at the send emit site, choose the opcode:

```python
                    cidx = self.add_constant(selector)
                    send_op = OP_BLOCK_INVOKE if self._is_block_value_selector(selector) else OP_SEND
                    self.emit_ABC(send_op, len(args), cidx, rcvr_reg)
```

(Locate the exact emit line by the quoted text `self.emit_ABC(OP_SEND, len(args), cidx, rcvr_reg)` in the normal-send path — around line 686 — NOT the `OP_SEND_SUPER`/`OP_SEND_IMPLICIT` sites, which stay unchanged.)

- [ ] **Step 7: Rebuild both image paths and run the suite**

Run: `cd /home/david/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | tail -5`
Expected: same pass/fail/skip counts as the Task 0 baseline (73/0/6), including the new `block_invoke` test PASSING on BOTH paths. If Path A and Path B disagree, the opcode number is out of sync between `oevm.h` and `ns_compile_oevm.py` — fix and re-run.

- [ ] **Step 8: Commit**

```bash
cd /home/david/workspace/oevm && git add src/oevm.h src/vm.c && \
  git commit -m $'feat(derecursify): OP_BLOCK_INVOKE opcode + recursive-engine handler (Phase 1)\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>'
cd /home/david/workspace/ns-oevm && git add tools/ns_compile_oevm.py && \
  git commit -m $'feat(derecursify): compile value-family sends to OP_BLOCK_INVOKE (Phase 1)\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>'
cd /home/david/workspace/ns-tests && git add -A && \
  git commit -m $'test(derecursify): non-literal block value: via OP_BLOCK_INVOKE\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>'
```

---

### Task 2: Lower non-literal if-family + `and:`/`or:` to `OP_BLOCK_INVOKE`; delete `PRIM_ifTrue`/`PRIM_ifFalse`/`PRIM_ifTrueIfFalse`

**Files:**
- Modify: `/home/david/workspace/ns-oevm/tools/ns_compile_oevm.py` (new `emit_block_value` helper; generalize `compile_if_inline` + `compile_logic_inline`; broaden the guards at lines 692–706)
- Modify: `/home/david/workspace/oevm/src/dispatch.c` (delete `PRIM_ifTrue`, `PRIM_ifFalse`, `PRIM_ifTrueIfFalse` cases)
- Test: `/home/david/workspace/ns-tests/<new>_ifTrue_nonliteral.ns`

**Interfaces:**
- Consumes: `OP_BLOCK_INVOKE` (Task 1).
- Produces: `MethodCompiler.emit_block_value(self, block_arg, target_reg, param_regs=None, init_temps=False)` — puts the VALUE of `block_arg` into `target_reg`. If `block_arg` is a literal block of matching arity, inlines its body (existing `compile_inlined_block` fast path, unchanged). Otherwise evaluates `block_arg` to a closure and emits `OP_BLOCK_INVOKE` (with `param_regs` passed as `value:`-arguments). Used by Tasks 2–4.

**Background — the equivalence to preserve** (from `oevm/src/dispatch.c`):
`ifTrue:` truthy → block value, falsy → the `false` symbol. `ifFalse:` is the mirror. `and:` truthy → block value, falsy → `false` symbol; `or:` truthy → `true` symbol, falsy → block value. The existing literal-block lowerings `compile_if_inline` (line 237) and `compile_logic_inline` (line 268) already encode this; the non-literal path must produce the same values via `OP_BLOCK_INVOKE`.

- [ ] **Step 1: Write the failing test**

Create `../ns-tests/<n>_ifTrue_nonliteral.ns` with a driver that puts the arm block in a variable (non-literal), forcing the fallback path:

```
main
    | flag arm result |
    flag := 1 < 2.
    arm := ['taken'].
    result := flag ifTrue: arm.
    ^ result
```

Expected output: `taken`. Register it in the suite manifest.

- [ ] **Step 2: Run the suite to confirm the new test currently PASSES (via PRIM_ifTrue)**

Run: `cd /home/david/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | grep -iE 'ifTrue_nonliteral|FAIL' | tail -5`
Expected: the new test PASSES today (it routes through `PRIM_ifTrue`, which recursively invokes the block). This is the behavior we must preserve after re-routing through bytecode + `OP_BLOCK_INVOKE`.

- [ ] **Step 3: Add the `emit_block_value` helper**

In `ns_compile_oevm.py`, add this method to `MethodCompiler` (place it next to `compile_inlined_block`, ~line 227). It mirrors the register discipline of the normal-send emit (quoted lines 682–689):

```python
    def emit_block_value(self, block_arg, target_reg, param_regs=None, init_temps=False):
        # Put the VALUE of a combinator's block argument into target_reg.
        #  - literal block of matching arity -> inline its body (fast path).
        #  - any other expression (block held in a var/slot/param) -> evaluate to
        #    a closure and OP_BLOCK_INVOKE it; param_regs become value: arguments.
        want_params = len(param_regs) if param_regs else 0
        if (block_arg["type"] == "block"
                and len(block_arg.get("params", [])) == want_params):
            self.compile_inlined_block(block_arg, target_reg,
                                       param_regs=param_regs, init_temps=init_temps)
            return
        saved_free = self.next_free_reg
        base = self.alloc_reg()                       # receiver = the closure
        self.compile_expr(block_arg, base)
        self.next_free_reg = base + 1
        argc = 0
        if param_regs:
            for pr in param_regs:
                areg = self.alloc_reg()               # contiguous value: args
                self.emit_ABC(OP_LD_REG, areg, pr, 0)
                self.next_free_reg = areg + 1
                argc += 1
        cidx = self.add_constant("value" + ":" * argc)
        self.emit_ABC(OP_BLOCK_INVOKE, argc, cidx, base)
        self.emit_ABC(OP_LD_REG, target_reg, base, 0)  # result lands in `base`
        self.next_free_reg = saved_free
```

- [ ] **Step 4: Route `compile_if_inline` and `compile_logic_inline` through `emit_block_value`**

In `compile_if_inline` (line 237), replace each `self.compile_inlined_block(true_block, target_reg)` / `self.compile_inlined_block(false_block, target_reg)` call with `self.emit_block_value(true_block, target_reg)` / `self.emit_block_value(false_block, target_reg)`. (The `None`-arm branches that emit the `false` symbol stay unchanged.)

In `compile_logic_inline` (line 268), replace the two `self.compile_inlined_block(block, target_reg)` calls with `self.emit_block_value(block, target_reg)`.

- [ ] **Step 5: Broaden the guards so non-literal args reach the lowerings**

At lines 695–706, drop the "all args are literal zero-arg blocks" restriction so the if-family and `and:`/`or:` sends are lowered whether or not the arms are literal blocks. Replace:

```python
            if (selector in ("ifTrue:", "ifFalse:", "ifTrue:ifFalse:", "ifFalse:ifTrue:")
                    and all(a["type"] == "block" and not a.get("params") for a in args)):
                self.compile_if_inline(selector, receiver, args, target_reg)
                return

            if (selector in ("and:", "or:")
                    and len(args) == 1 and args[0]["type"] == "block"
                    and not args[0].get("params")):
                self.compile_logic_inline(selector, receiver, args[0], target_reg)
                return
```

with:

```python
            # if-family and and:/or: are ALWAYS lowered to branch + OP_BLOCK_INVOKE.
            # Literal zero-arg block arms inline (fast path); any other arm is
            # invoked via OP_BLOCK_INVOKE inside emit_block_value. The recursive
            # PRIM_ifTrue/PRIM_ifFalse/PRIM_ifTrueIfFalse/PRIM_and/PRIM_or paths
            # are retired (Phase 1 de-recursify).
            if selector in ("ifTrue:", "ifFalse:", "ifTrue:ifFalse:", "ifFalse:ifTrue:"):
                self.compile_if_inline(selector, receiver, args, target_reg)
                return

            if selector in ("and:", "or:") and len(args) == 1:
                self.compile_logic_inline(selector, receiver, args[0], target_reg)
                return
```

- [ ] **Step 6: Delete the recursive if-family primitives**

In `/home/david/workspace/oevm/src/dispatch.c`, delete the `case PRIM_ifTrueIfFalse:`, `case PRIM_ifTrue:`, and `case PRIM_ifFalse:` blocks (each currently recursively calls `run_interpreter` on an arm). Leave `PRIM_and`/`PRIM_or` handling as-is IF present (Task 0 found no `PRIM_and`/`PRIM_or` — non-literal `and:`/`or:` is now lowered by the compiler, so no C case is needed). If the switch requires every `PRIM_*` enum value to have a case (e.g. a `-Wswitch` build), route the deleted selectors to the default `does_not_understand` path rather than leaving a dangling label.

- [ ] **Step 7: Rebuild + run the suite**

Run: `cd /home/david/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | tail -5`
Expected: baseline counts (73/0/6) plus the two new tests, no new failures, both paths. If a previously-green test now fails, it exercised a non-literal if-family case whose lowered value differs from the primitive — diff the expected vs. actual and reconcile the lowering to the equivalence in the Background note.

- [ ] **Step 8: Commit**

```bash
cd /home/david/workspace/oevm && git add src/dispatch.c && \
  git commit -m $'refactor(derecursify): delete recursive PRIM_ifTrue/ifFalse/ifTrueIfFalse (Phase 1)\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>'
cd /home/david/workspace/ns-oevm && git add tools/ns_compile_oevm.py && \
  git commit -m $'feat(derecursify): lower non-literal if-family + and:/or: via OP_BLOCK_INVOKE\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>'
cd /home/david/workspace/ns-tests && git add -A && \
  git commit -m $'test(derecursify): non-literal ifTrue: arm\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>'
```

---

### Task 3: Lower non-literal `to:do:` / `to:by:do:` to `OP_BLOCK_INVOKE`; delete `PRIM_to_do`/`PRIM_to_by_do`

**Files:**
- Modify: `/home/david/workspace/ns-oevm/tools/ns_compile_oevm.py` (generalize `compile_to_do_inline` for a non-literal body block; add a runtime-direction path for a non-literal step; broaden the guards at lines 708–732)
- Modify: `/home/david/workspace/oevm/src/dispatch.c` (delete `PRIM_to_do`, `PRIM_to_by_do`)
- Test: `/home/david/workspace/ns-tests/<new>_todo_nonliteral.ns`

**Interfaces:**
- Consumes: `OP_BLOCK_INVOKE` (Task 1), `emit_block_value` (Task 2).

**Background — equivalence** (`dispatch.c` `PRIM_to_do`/`PRIM_to_by_do`): `start to: end do: aBlock` invokes `aBlock value: i` for `i = start, start+1, …` while `i <= end`; result value = the receiver (`start`). `to:by:do:` uses `step`; when `step > 0` the guard is `i <= end`, when `step < 0` it is `i >= end`, and `step = 0` does zero iterations and returns the receiver. `compile_to_do_inline` (line 294) already encodes this for a literal block + compile-time-known step direction.

- [ ] **Step 1: Write the failing test**

Create `../ns-tests/<n>_todo_nonliteral.ns` — the body block is held in a variable (non-literal), forcing the fallback:

```
main
    | body sum |
    sum := 0.
    body := [:i | sum := sum + i].
    1 to: 4 do: body.
    ^ sum printString
```

Expected output: `10`. Register it.

- [ ] **Step 2: Run the suite to confirm it currently PASSES (via PRIM_to_do)**

Run: `cd /home/david/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | grep -iE 'todo_nonliteral|FAIL' | tail -5`
Expected: PASSES today (routes through `PRIM_to_do`, which recursively invokes `body`).

- [ ] **Step 3: Generalize `compile_to_do_inline` for a non-literal body block**

In `compile_to_do_inline` (line 294), the body is currently emitted by
`self.compile_inlined_block(block, body_reg, param_regs=[i_reg], init_temps=True)` (line 323).
Replace that single call with the arity-agnostic helper so a non-literal body is invoked as `body value: i`:

```python
        # body: literal block inlines with param bound to i_reg; a non-literal
        # block is OP_BLOCK_INVOKE'd as `body value: i`.
        body_reg = self.alloc_reg()
        self.emit_block_value(block, body_reg, param_regs=[i_reg], init_temps=True)
        self.free_reg()  # body_reg
```

`compile_to_do_inline` already reads `step_node` for the literal-step case; keep that path unchanged. For a compile-time-known step (literal or `None`/+1) with a non-literal body, this generalization is sufficient.

- [ ] **Step 4: Handle a non-literal STEP (runtime direction)**

`compile_to_do_inline` derives `cmp_sel` from the literal step sign, which is unavailable when `by:` is a non-literal expression. Add a sibling `compile_to_by_do_runtime(self, start_expr, end_expr, step_expr, block, target_reg)` that resolves direction at runtime by branching once on the sign of the step, then running one of two counted loops (each identical except for the comparison selector and reusing `emit_block_value` for the body). Concretely: evaluate `start`→`target_reg` (the result), `i := start`, evaluate `end`→`end_reg`, evaluate `step`→`step_reg`; emit `step_reg < 0` (a `<` send) → `OP_JMP_IF_FALSE l_pos`; at `l_neg` run the `i >= end` loop; `OP_JMP l_done`; at `l_pos` run the `i <= end` loop; `l_done`. Factor the per-direction loop body into a local closure/helper that takes the comparison selector so the two loops share code. Each loop's body uses `self.emit_block_value(block, body_reg, param_regs=[i_reg], init_temps=True)` and the increment `i := i + step_reg` via `OP_ADD`.

- [ ] **Step 5: Broaden the `to:do:` / `to:by:do:` guards**

At lines 708–732, extend the guards so all `to:do:` and `to:by:do:` sends are lowered rather than falling through to the primitive:

```python
            # to:do: — always lowered. Literal one-param body inlines; any other
            # body is OP_BLOCK_INVOKE'd as `body value: i`.
            if selector == "to:do:" and len(args) == 2:
                self.compile_to_do_inline(receiver, args[0], None, args[1], target_reg)
                return

            # to:by:do: — literal nonzero-int step keeps the compile-time-direction
            # fast path; a non-literal (or non-int) step uses the runtime-direction
            # lowering. `by: 0` still yields zero iterations (guarded below).
            if selector == "to:by:do:" and len(args) == 3:
                step_arg = args[1]
                if (step_arg["type"] == "literal"
                        and step_arg.get("litKind") == "int"
                        and int(step_arg["value"]) != 0):
                    self.compile_to_do_inline(receiver, args[0], step_arg, args[2], target_reg)
                else:
                    self.compile_to_by_do_runtime(receiver, args[0], step_arg, args[2], target_reg)
                return
```

Note: `compile_to_do_inline`'s existing literal-step guard already rejects `by: 0` (line 730). The runtime path must reproduce the `step = 0` ⇒ zero-iterations-returns-receiver semantics (a runtime `step = 0` check that skips both loops), matching `PRIM_to_by_do`.

- [ ] **Step 6: Delete the recursive `to:do:`/`to:by:do:` primitives**

In `/home/david/workspace/oevm/src/dispatch.c`, delete `case PRIM_to_do:` and `case PRIM_to_by_do:` (both recursively `run_interpreter` the block per iteration). As in Task 2 Step 6, if the build requires the enum value to have a case, route it to `does_not_understand`.

- [ ] **Step 7: Rebuild + run the suite**

Run: `cd /home/david/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | tail -5`
Expected: baseline + new tests, no new failures, both paths. Pay attention to any suite test using `to:by:do:` with a negative or non-literal step — those exercise the new runtime-direction path.

- [ ] **Step 8: Commit** (three repos, as in Task 2 Step 8, with messages naming `to:do:`/`to:by:do:` lowering + `PRIM_to_do`/`PRIM_to_by_do` deletion + the `todo_nonliteral` test).

---

### Task 4: Lower non-literal `whileTrue:` / `whileFalse:`; retire the `whileTrue:`/`whileFalse:` closure intercept

**Files:**
- Modify: `/home/david/workspace/ns-oevm/tools/ns_compile_oevm.py` (generalize `compile_loop_inline`; broaden the guard at lines 734–742)
- Modify: `/home/david/workspace/oevm/src/dispatch.c` (delete the `whileTrue:`/`whileFalse:` branch of the `BytecodeClosureVtable` intercept)
- Test: `/home/david/workspace/ns-tests/<new>_while_nonliteral.ns`

**Interfaces:**
- Consumes: `OP_BLOCK_INVOKE`, `emit_block_value`.

**Background — equivalence** (`dispatch.c` intercept): `[cond] whileTrue: [body]` loops while `cond value` is truthy, invoking `body value` each pass; result = nil. `whileFalse:` continues while `cond value` is falsy. `compile_loop_inline` (line 336) already encodes this for literal receiver + body blocks.

- [ ] **Step 1: Write the failing test**

Create `../ns-tests/<n>_while_nonliteral.ns` with the condition and body blocks held in variables (non-literal receiver AND arg — the case that today hits the closure intercept, not the inline path):

```
main
    | n cond body |
    n := 0.
    cond := [n < 3].
    body := [n := n + 1].
    cond whileTrue: body.
    ^ n printString
```

Expected output: `3`. Register it.

- [ ] **Step 2: Run the suite to confirm it currently PASSES (via the intercept)**

Run: `cd /home/david/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | grep -iE 'while_nonliteral|FAIL' | tail -5`
Expected: PASSES today (routes through the `whileTrue:` branch of the `dispatch.c` `BytecodeClosureVtable` intercept, which recursively invokes both blocks).

- [ ] **Step 3: Generalize `compile_loop_inline` for non-literal cond/body**

In `compile_loop_inline` (line 336), replace the two body/cond emitters `self.compile_inlined_block(cond_block, cond_reg, init_temps=True)` and `self.compile_inlined_block(body_block, body_reg, init_temps=True)` with `self.emit_block_value(cond_block, cond_reg, init_temps=True)` and `self.emit_block_value(body_block, body_reg, init_temps=True)`. (The label/jump skeleton and the `nil` result stay unchanged.) `emit_block_value` inlines literal zero-arg blocks (fast path) and `OP_BLOCK_INVOKE`s non-literal ones — both are zero-arg here (`param_regs=None`).

- [ ] **Step 4: Broaden the `whileTrue:`/`whileFalse:` guard**

At lines 737–742, drop the "receiver and arg are literal zero-arg blocks" restriction so all `whileTrue:`/`whileFalse:` sends are lowered:

```python
            # whileTrue:/whileFalse: — always lowered to a bytecode loop. Literal
            # zero-arg cond/body blocks inline; non-literal ones are OP_BLOCK_INVOKE'd.
            if selector in ("whileTrue:", "whileFalse:") and len(args) == 1:
                self.compile_loop_inline(selector, receiver, args[0], target_reg)
                return
```

Caution: `compile_loop_inline` treats `receiver` as the condition block and `args[0]` as the body. Confirm the guard only fires for the binary `whileTrue:`/`whileFalse:` message shape (it does — `len(args) == 1`).

- [ ] **Step 5: Retire the intercept's `whileTrue:`/`whileFalse:` branch**

In `/home/david/workspace/oevm/src/dispatch.c`, inside the `if (receiver_vtable == BytecodeClosureVtable)` block, delete the `strcmp(sname, "whileTrue:") == 0` loop and its `whileFalse:` twin (they recursively `run_interpreter` the cond/body blocks). **Keep** the `value`-prefix branch (`strncmp(sname, "value", 5) == 0`) — it remains the reflective/`perform:`-send fallback for block invocation and is folded into the flat loop in Phase 2 (see "Deferred to Phase 2" below).

- [ ] **Step 6: Rebuild + run the suite**

Run: `cd /home/david/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | tail -5`
Expected: baseline + all four new tests, no new failures, both paths.

- [ ] **Step 7: Commit** (three repos, messages naming `whileTrue:`/`whileFalse:` lowering + intercept retirement + the `while_nonliteral` test).

---

### Task 5: Full regression gate, dialect/doc update, and Phase-2 handoff note

**Files:**
- Modify: `/home/david/workspace/oevm/docs/fiber_derecursify_core_design.md` (mark Phase 1 DONE; record what Phase 2 inherits)
- Modify: `/home/david/workspace/ns-oevm/docs/NEWSPEAK_DIALECT.md` (if a new dialect row is warranted — see Step 4)

- [ ] **Step 1: Full suite, both paths**

Run: `cd /home/david/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | tail -8`
Expected: baseline pass count + the 4 new Phase-1 tests, 0 failures, both paths.

- [ ] **Step 2: Fibers + GC + portability regression gates**

Run the same gates used by the L1 work (they are the standing regression set for interpreter-core changes):

```bash
cd /home/david/workspace/oevm
# fibers interleave + GC-stress + 32-bit portability + ASan/GC_STRESS.
# Use the exact recipes from oevm's test harness / Makefile — locate them with:
grep -rniE 'fibers-gc-stress|portability_32|GC_STRESS|asan' Makefile* tests/ tools/ 2>/dev/null | head
```

Expected: locate and run `test_portability_32.sh`, the fibers-gc-stress build, and an `OEVM_GC_STRESS=1` / ASan run; all green. (These caught real bugs in L1 — do not skip. If a recipe is unclear, consult `docs/fiber_implementation.md` and the L1 commits `b0f111e..f808d50` for the exact invocations.)

- [ ] **Step 3: Confirm the recursive primitives are gone**

Run: `cd /home/david/workspace/oevm && grep -nE 'PRIM_(to_do|to_by_do|ifTrue|ifFalse|ifTrueIfFalse)\b' src/dispatch.c; echo "exit=$?"`
Expected: no `case` definitions remain (matches only in an enum declaration, if any, are acceptable; there must be no `case PRIM_...:` bodies that call `run_interpreter`). Also confirm the `whileTrue:`/`whileFalse:` intercept branch is gone: `grep -nE 'whileTrue:|whileFalse:' src/dispatch.c` should show no `strcmp(...)` loop.

- [ ] **Step 4: Update dialect ledger if warranted**

Phase 1 is an internal lowering with NO source-visible semantic change (behavior-equivalent by construction), so it likely does NOT need a new `NEWSPEAK_DIALECT.md` row. Confirm no observable behavior changed (all tests green with unchanged expected outputs). If nothing user-visible changed, add NO dialect row; instead note the internal change in the design doc (Step 5). If you discovered an unavoidable observable difference (e.g. a reflective `perform: #ifTrue:` now DNUs), that IS a dialect change — add a row and escalate to David first (per the "escalate design reversals" rule).

- [ ] **Step 5: Mark Phase 1 done + record the Phase-2 inheritance**

In `oevm/docs/fiber_derecursify_core_design.md`, update the Phase 1 bullet in §5 to DONE and add a short "Phase 1 delivered / Phase 2 inherits" note capturing:
- `OP_BLOCK_INVOKE = 0x16` exists; every compiled block invoke (explicit `value*` and all combinator fallbacks) routes through it or through inlined bytecode.
- Deleted: `PRIM_to_do`, `PRIM_to_by_do`, `PRIM_ifTrue`, `PRIM_ifFalse`, `PRIM_ifTrueIfFalse`, and the `whileTrue:`/`whileFalse:` closure intercept.
- **Deferred to Phase 2** (NOT block-invoking C primitives, but part of the send()/flat-loop rework): the `value`-prefix branch of the `dispatch.c` `BytecodeClosureVtable` intercept (reflective `perform: #value`), and `valueWithArguments:`. When Phase 2 removes `send()`'s recursion, both convert to the flat mechanism together.

- [ ] **Step 6: Commit**

```bash
cd /home/david/workspace/oevm && git add docs/fiber_derecursify_core_design.md && \
  git commit -m $'docs(derecursify): Phase 1 (finish lowering) complete; Phase 2 handoff\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>'
# plus ns-oevm docs/NEWSPEAK_DIALECT.md only if Step 4 added a row.
```

- [ ] **Step 7: Request code review**

Use `superpowers:requesting-code-review` on the full Phase-1 diff across the three repos before considering merge. Phase 1 is a prerequisite for the high-risk Phase 2 flip — it must be clean.

---

## Self-Review notes (author)

- **Spec coverage:** design §5 Phase 1 requires (a) one `OP_BLOCK_INVOKE` [Task 1], (b) route every block invoke through it [Task 1 for `value*`; Tasks 2–4 for combinator fallbacks], (c) lower non-literal `to:do:`/`ifTrue:`/… to bytecode [Tasks 2–4], (d) delete the dead recursive combinator primitives [Tasks 2–4], (e) both image paths [Global Constraints + every task's `OEVM_PATH=AB` gate], (f) no hybrid [after Task 4 every block invoke is one mechanism]. §7 open item "both paths agree on the new bytecode" → Task 1 Step 7. §7 open item "OP_RETURN src2 decode" and "OP_UNWIND" belong to Phase 2, not here.
- **Deliberate scope calls (flag at handoff):**
  1. `valueWithArguments:` stays an ordinary send in Phase 1 (it spreads an array; not a fixed-arity `OP_BLOCK_INVOKE`). Deferred to Phase 2.
  2. The `value`-prefix `dispatch.c` intercept is KEPT as the reflective fallback and folded in Phase 2 with the `send()` rework — deleting it now would strand `perform: #value`. This is consistent with the design (Phase 1's mandated deletions are the combinator *primitives*, which ARE all deleted).
  3. `to:by:do:` with a non-literal step is lowered via a runtime-direction branch (Task 3 Step 4) rather than an image method, honoring the design's "lower to a bytecode loop" wording.
- **Type/name consistency:** `emit_block_value(block_arg, target_reg, param_regs=None, init_temps=False)` is defined in Task 2 and consumed unchanged in Tasks 3–4. `OP_BLOCK_INVOKE = 0x16` is identical in `oevm.h` and `ns_compile_oevm.py`.
