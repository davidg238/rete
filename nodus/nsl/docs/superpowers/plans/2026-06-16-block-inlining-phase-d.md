# Block Inlining Phase D — Loop Inlining Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Inline `whileTrue:`/`whileFalse:` and `to:do:`/`to:by:do:` (literal blocks) into jump-based loops with no per-iteration closure allocation or block-frame setup, matching the existing primitive semantics exactly.

**Architecture:** Extend the Phase A–C inlining machinery in `ns-oevm/tools/ns_compile_oevm.py`. Two new `MethodCompiler` methods (`compile_loop_inline`, `compile_to_do_inline`) lower literal-block loop sends into `OP_JMP_IF_FALSE`/`OP_JMP` skeletons that reuse `compile_inlined_block` for the bodies. `compile_inlined_block` gains a **param-binding** path (the `to:do:` induction variable) and an **opt-in per-iteration temp-nil-init** path (faithful block-activation semantics inside loops). Detection hooks live in `compile_expr`'s `et == "send"` branch, alongside the Phase B/C hooks. Non-literal/unsupported shapes fall back to the existing closure+send path (the primitives in `oevm/src/dispatch.c` stay as the safety net).

**Tech Stack:** Python 3 compiler (`ns-oevm/tools/ns_compile_oevm.py`), C VM (`oevm/`, opcodes in `src/oevm.h`), `.ns` conformance corpus (`ns-tests/`), GCC C99.

## Global Constraints

- **Three sibling repos in play:** compiler = `ns-oevm/`, VM = `oevm/`, tests = `ns-tests/`. All three committed independently. `oevm/src/solidified_image.c` is a **generated artifact** — never hand-edit or commit it; the harness overwrites it per test.
- **No VM changes.** Phase D is compiler-only. The loop primitives in `oevm/src/dispatch.c` (`PRIM_to_do`, `PRIM_to_by_do`, `whileTrue:`/`whileFalse:`) remain as the fallback path and the semantic reference. Do not edit them.
- **Truthiness single-sourced** through `OP_JMP_IF_FALSE` (the VM's `is_truthy`). Never reimplement truthiness in codegen.
- **Jump offset encoding** (already implemented in `finalize_jumps`, verified against `oevm/src/vm.c`): offset = `target - (idx + 1)`. `OP_JMP` = `((off & 0xFFFF) << 8) | OP_JMP`; `OP_JMP_IF_FALSE` = `((off & 0xFFFF) << 16) | ((cond & 0xFF) << 8) | OP_JMP_IF_FALSE`. Use `new_label`/`bind_label`/`emit_jump`/`finalize_jumps` — never hand-encode offsets. `finalize_jumps()` is already called at every frame body-completion site; loop jumps flush there, no extra call needed.
- **No-op proof discipline (carried from Phase A):** any change that must NOT alter existing output must be proven byte-identical with `ns-oevm/tools/snapshot_images.py` + `diff -r`. The if-family and `and:`/`or:` images MUST stay byte-identical through every Phase D task.
- **Commit trailer (every commit):** `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`
- **Baseline at plan start (verified):** conformance `21 / 10 / 1` (`cd ns-oevm && python3 tools/run_tests_oevm.py`). `04_somns_basic/02_block_inlining.ns` FAILS — but on `Array>>do:` ("Message 'do:' not understood"), NOT on `to:do:`. Phase D does **not** close `02`; it lands a new dedicated test `11_loop_inlining.ns` (→ `22/10/1`). Closing `02` needs a separate `Array>>do:` library method (out of scope; see "Out of Scope" below).

## Reference: exact runtime semantics to match (from `oevm/src/dispatch.c`)

- `whileTrue:` / `whileFalse:` → **return `nil` (NULL)**. Loop forever; re-run cond block each iteration; `whileTrue:` runs body while cond truthy, `whileFalse:` while cond falsy. `unwind_target_reached` (a `^` from the body) breaks the loop — handled for free because an inlined `^` compiles to `OP_RET`/`OP_NLR` which exits the frame.
- `to:do:` (`PRIM_to_do`) → **return the receiver** (the start value). Ascending only, `for (i = start; i <= end; i++)`, inclusive of `end`.
- `to:by:do:` (`PRIM_to_by_do`) → **return the receiver**. `step > 0`: `for (i = start; i <= end; i += step)`. `step < 0`: `for (i = start; i >= end; i += step)`. `step == 0`: no iterations.

## Key facts the implementer must not re-derive

- A literal block AST node: `{"type":"block", "params":[...], "temps":[...], "body":[...]}`. Zero-arg ⇒ `not node.get("params")`. One-arg ⇒ `len(node.get("params", [])) == 1`.
- A literal integer AST node: `{"type":"literal", "litKind":"int", "value":N}`. Step test: `a["type"]=="literal" and a.get("litKind")=="int"`; value = `int(a["value"])`.
- `nil` constant = `self.add_constant(None)` then `OP_LD_CONST` (see `compile_expr` `litKind=="nil"`, line ~331).
- SMI constant = `self.add_constant(f"SMI:{n}")` then `OP_LD_CONST`.
- `<=`, `>=`, `<`, `>` are real send primitives (`oevm/src/dispatch.c` `PRIM_le/ge/lt/gt`) returning the `true`/`false` symbols. The loop condition is emitted as a normal 1-arg `OP_SEND` (no fast-path opcode exists for comparison).
- Normal-send register protocol (mirror it exactly): receiver in `rc`, the single arg **contiguous** in `rc+1`, then `OP_SEND nargs=1, selector_cidx, rc`. **The result lands back in `rc`** (the receiver register). See `compile_expr` "Normal send" path, lines ~573–587.
- `+` fast path: `self.emit_ABC(OP_ADD, dst, a, b)` (lines ~520–532). Use this for the induction increment.
- `target_reg` passed into an inline helper is **caller-allocated and sits below** any reg the helper allocates, so it survives the helper's `free_reg` reclaim (Phase B contract).
- `compile_inlined_block(block, target_reg)` today: pushes a child `Scope`, declares the block's **temps** as fresh regs, compiles the body so its value lands in `target_reg`, restores `next_free_reg` + `current_scope`. It does **not** bind params and does **not** nil-init temps. Phase D Task 1 adds both as opt-in.

## Out of Scope (explicitly)

- `Array>>do:` and other collection iteration — a library method, not control-flow inlining. This is the real blocker for `02_block_inlining.ns`; leave that test failing. (Flag for a future phase.)
- `to:by:do:` with a **non-literal** step (loop direction unknown at compile time) — falls back to the primitive.
- Inlining when the loop's receiver/arg is a **variable** holding a block (`blk whileTrue: ...`) — falls back to the primitive.
- `timesRepeat:`, `to:do:` over non-SMI receivers — fallback.

---

## Task 1: Foundation — `param_regs` + `init_temps` on `compile_inlined_block` (pure no-op)

Add two optional parameters to `compile_inlined_block`. Defaults reproduce today's behaviour exactly, so the if-family and `and:`/`or:` images stay byte-identical. Loops (Tasks 2–4) pass them.

**Files:**
- Modify: `ns-oevm/tools/ns_compile_oevm.py` (`compile_inlined_block`, ~lines 160–186)

**Interfaces:**
- Produces: `compile_inlined_block(self, block, target_reg, param_regs=None, init_temps=False)`.
  - `param_regs`: optional list of caller-owned register indices, positionally bound to `block["params"]` in the pushed child scope (the caller keeps them live and controls their values, e.g. a loop induction var). Not allocated here.
  - `init_temps`: when `True`, each declared temp is `OP_LD_CONST`-initialised to `nil` immediately after declaration, giving fresh-per-activation temp semantics when the block body is re-executed in a loop. Default `False` ⇒ no extra instructions ⇒ byte-identical for if/and/or.

- [ ] **Step 1: Snapshot the BEFORE images (no-op baseline)**

```bash
cd ~/workspace/ns-oevm
rm -rf /tmp/img_before && python3 tools/snapshot_images.py /tmp/img_before
```
Expected: `snapshot written to /tmp/img_before`.

- [ ] **Step 2: Add the two parameters**

In `ns-oevm/tools/ns_compile_oevm.py`, change the signature and the temp-declaration loop. Current code:

```python
    def compile_inlined_block(self, block, target_reg):
        ...
        saved_free = self.next_free_reg
        saved_scope = self.current_scope
        self.current_scope = Scope(self, saved_scope)
        for t in block.get("temps", []):
            self.current_scope.declare(t, self.alloc_reg())
        body = block["body"]
```

Replace with:

```python
    def compile_inlined_block(self, block, target_reg, param_regs=None, init_temps=False):
        ...
        saved_free = self.next_free_reg
        saved_scope = self.current_scope
        self.current_scope = Scope(self, saved_scope)
        # Bind block params to caller-owned registers (e.g. a loop induction
        # variable). The caller allocated them below saved_free and keeps them
        # live across iterations; we only name them in the child scope.
        if param_regs is not None:
            for name, reg in zip(block.get("params", []), param_regs):
                self.current_scope.declare(name, reg)
        # Declare block temps as fresh registers. When this body is re-run in a
        # loop, init_temps=True nil-resets them each iteration to match real
        # block-activation semantics (a fresh closure gets nil temps).
        for t in block.get("temps", []):
            reg = self.alloc_reg()
            self.current_scope.declare(t, reg)
            if init_temps:
                self.emit_ABx(OP_LD_CONST, reg, self.add_constant(None))
        body = block["body"]
```

(Leave the docstring's existing lines; just extend behaviour. Keep everything below `body = block["body"]` unchanged.)

- [ ] **Step 3: Snapshot AFTER and prove byte-identical**

```bash
cd ~/workspace/ns-oevm
rm -rf /tmp/img_after && python3 tools/snapshot_images.py /tmp/img_after
diff -r /tmp/img_before /tmp/img_after && echo "NO-OP CONFIRMED"
```
Expected: no diff output, then `NO-OP CONFIRMED`. (No caller passes the new args yet, so every image is identical.)

- [ ] **Step 4: Run conformance — unchanged**

```bash
cd ~/workspace/ns-oevm && python3 tools/run_tests_oevm.py 2>&1 | tail -4
```
Expected: `Passed: 21`, `Failed: 10`, `Skipped: 1`.

- [ ] **Step 5: Commit**

```bash
cd ~/workspace/ns-oevm
git add tools/ns_compile_oevm.py docs/superpowers/plans/2026-06-16-block-inlining-phase-d.md
git commit -m "compiler: add param_regs/init_temps to compile_inlined_block (no-op; loop foundation)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: `to:do:` inlining

Lower `start to: end do: [:i | BODY]` (literal one-param block) to a jump loop with an induction register. Result = receiver (start), matching `PRIM_to_do`.

**Files:**
- Modify: `ns-oevm/tools/ns_compile_oevm.py` (add `compile_to_do_inline` method; add a detection hook in `compile_expr` `et == "send"`)
- Test: `ns-tests/04_somns_basic/11_loop_inlining.ns` (create — incremental; full case list assembled here, expanded in Task 4)

**Interfaces:**
- Produces: `compile_to_do_inline(self, start_expr, end_expr, step_node, block, target_reg)` — `step_node` is `None` for `to:do:` (implicit +1) or a literal-int AST node for `to:by:do:`. Consumed only by the detection hooks.

- [ ] **Step 1: Write the failing test fixture**

Create `ns-tests/04_somns_basic/11_loop_inlining.ns` with the `to:do:` cases (the file is extended in Task 4; assemble it now with these cases):

```
(*
expect:
6
3
1
*)
class LoopInliningTest packageUsing: manifest = () (
public class LoopInlining = () (
	public testToDoSum = (
		| sum |
		sum:: 0.
		1 to: 3 do: [ :i | sum:: sum + i ].
		^ sum
	)
	public testToDoReturnsReceiver = (
		^ 3 to: 5 do: [ :i | i ]
	)
	public testToDoEmptyRange = (
		| count |
		count:: 0.
		5 to: 1 do: [ :i | count:: count + 1 ].
		^ count + 1
	)
)
public main: platform args: args = (
	| t = LoopInlining new. |
	t testToDoSum printString out.
	t testToDoReturnsReceiver printString out.
	t testToDoEmptyRange printString out.
)
) : (
)
```

Expected values: `testToDoSum` = 1+2+3 = **6**; `testToDoReturnsReceiver` returns the receiver **3**; `testToDoEmptyRange`: empty loop (`5 to: 1`), count stays 0, `0 + 1` = **1**.

- [ ] **Step 2: Run it to confirm it PASSES via the primitive (pre-implementation reference)**

```bash
cd ~/workspace/ns-oevm
python3 tools/run_tests_oevm.py 2>&1 | grep '11_loop_inlining'
```
Expected: `... PASS`. (The primitive already implements `to:do:`; this confirms the *expected outputs are correct* before we change codegen. The real assertion that inlining happened is the structural check in Step 5.)

- [ ] **Step 3: Add `compile_to_do_inline`**

Add this method to `MethodCompiler` (place it right after `compile_logic_inline`, ~line 245):

```python
    def compile_to_do_inline(self, start_expr, end_expr, step_node, block, target_reg):
        # `start to: end do: [:i | body]` (and to:by:do: with a literal step).
        # Result value = the receiver (start), matching PRIM_to_do / PRIM_to_by_do.
        # i lives in i_reg (the block's single param); end in end_reg. The
        # condition `i <= end` (or `i >= end` for a negative step) is a normal
        # comparison send; the increment uses the OP_ADD fast path.
        step_val = 1 if step_node is None else int(step_node["value"])
        cmp_sel = ">=" if step_val < 0 else "<="

        self.compile_expr(start_expr, target_reg)        # result = receiver = start
        i_reg = self.alloc_reg()
        self.emit_ABC(OP_LD_REG, i_reg, target_reg, 0)   # i := start
        end_reg = self.alloc_reg()
        self.compile_expr(end_expr, end_reg)

        l_start = self.new_label()
        l_end = self.new_label()
        self.bind_label(l_start)
        # cond: (i <cmp> end) -> result lands in rc
        rc = self.alloc_reg()
        self.emit_ABC(OP_LD_REG, rc, i_reg, 0)
        ec = self.alloc_reg()                            # contiguous arg = rc + 1
        self.emit_ABC(OP_LD_REG, ec, end_reg, 0)
        self.emit_ABC(OP_SEND, 1, self.add_constant(cmp_sel), rc)
        self.emit_jump(OP_JMP_IF_FALSE, rc, l_end)
        self.free_reg()  # ec
        self.free_reg()  # rc
        # body: inline with the param bound to i_reg, temps nil-reset per iter
        body_reg = self.alloc_reg()
        self.compile_inlined_block(block, body_reg, param_regs=[i_reg], init_temps=True)
        self.free_reg()  # body_reg
        # i := i + step
        step_reg = self.alloc_reg()
        self.emit_ABx(OP_LD_CONST, step_reg, self.add_constant(f"SMI:{step_val}"))
        self.emit_ABC(OP_ADD, i_reg, i_reg, step_reg)
        self.free_reg()  # step_reg
        self.emit_jump(OP_JMP, 0, l_start)
        self.bind_label(l_end)
        self.free_reg()  # end_reg
        self.free_reg()  # i_reg
        # target_reg already holds the start value (the receiver) = result
```

- [ ] **Step 4: Add the `to:do:` detection hook in `compile_expr`**

In `compile_expr`, `et == "send"` branch, immediately after the Phase C `and:`/`or:` hook (after the block ending `return` near line 501), insert:

```python
            # Phase D: inline `to:do:` with a literal one-param block. The
            # induction variable is the block's param; the step is +1.
            if (selector == "to:do:"
                    and len(args) == 2
                    and args[1]["type"] == "block"
                    and len(args[1].get("params", [])) == 1):
                self.compile_to_do_inline(receiver, args[0], None, args[1], target_reg)
                return
```

- [ ] **Step 5: Prove it inlined (no `to:do:` send, no closure for the body block)**

```bash
cd ~/workspace/ns-oevm
python3 tools/ns_compile_oevm.py ../ns-tests/04_somns_basic/11_loop_inlining.ns /tmp/loop.c
grep -c 'StaticSymbol_to_do_\|createClosure' /tmp/loop.c
```
Expected: `0` (no `to:do:` selector symbol emitted, no `createClosure:` for the loop body ⇒ it was lowered, not sent).

- [ ] **Step 6: Run conformance — still green, no regressions**

```bash
cd ~/workspace/ns-oevm && python3 tools/run_tests_oevm.py 2>&1 | tail -4
```
Expected: `Passed: 22`, `Failed: 10`, `Skipped: 1` (the new `11_loop_inlining` test is the +1; everything else unchanged). Also confirm `queens` and `sieve` still PASS:
```bash
python3 tools/run_tests_oevm.py 2>&1 | grep -E '01_queens|02_sieve'
```
Expected: both `PASS`.

- [ ] **Step 7: Prove no-op for if/and/or images**

```bash
cd ~/workspace/ns-oevm
rm -rf /tmp/img_after && python3 tools/snapshot_images.py /tmp/img_after
diff /tmp/img_before/04_somns_basic__09_if_inlining.ns.c /tmp/img_after/04_somns_basic__09_if_inlining.ns.c && \
diff /tmp/img_before/04_somns_basic__10_and_or_inlining.ns.c /tmp/img_after/04_somns_basic__10_and_or_inlining.ns.c && echo "IF/AND/OR UNCHANGED"
```
Expected: no diff, then `IF/AND/OR UNCHANGED`. (Other images legitimately change where `to:do:` appears — e.g. `02_block_inlining`, `queens`, `sieve` — that is expected and correct.)

- [ ] **Step 8: Commit (compiler + test, two repos)**

```bash
cd ~/workspace/ns-tests
git add 04_somns_basic/11_loop_inlining.ns
git commit -m "test: add loop-inlining test (to:do: cases)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
cd ~/workspace/ns-oevm
git add tools/ns_compile_oevm.py
git commit -m "compiler: inline to:do: (literal one-param block) into a jump loop

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: `to:by:do:` inlining (literal step)

Reuse `compile_to_do_inline` with a literal-int step node; add the detection hook and test cases for ascending and descending loops.

**Files:**
- Modify: `ns-oevm/tools/ns_compile_oevm.py` (one detection hook in `compile_expr`)
- Test: `ns-tests/04_somns_basic/11_loop_inlining.ns` (append cases)

- [ ] **Step 1: Extend the test fixture with `to:by:do:` cases**

In `ns-tests/04_somns_basic/11_loop_inlining.ns`, add two methods to the `LoopInlining` class (before the closing `)` of the class), and two `out` calls in `main:`, and two expected lines. Add to the class body:

```
	public testToByDoUp = (
		| sum |
		sum:: 0.
		1 to: 10 by: 2 do: [ :i | sum:: sum + i ].
		^ sum
	)
	public testToByDoDown = (
		| sum |
		sum:: 0.
		5 to: 1 by: 0 - 1 do: [ :i | sum:: sum + i ].
		^ sum
	)
```

Add to `main:` (after `testToDoEmptyRange`):

```
	t testToByDoUp printString out.
	t testToByDoDown printString out.
```

Add to the `expect:` block (append after the existing `1`):

```
25
15
```

Values: `testToByDoUp` = 1+3+5+7+9 = **25**; `testToByDoDown` (step −1, 5→1) = 5+4+3+2+1 = **15**. Note the step is written `0 - 1` so it is a literal-int arithmetic — but the **argument node** of `to:by:do:` is a `send` (`0 - 1`), NOT a literal. See Step 2.

> **Decision:** A literal negative integer is not directly expressible as a single literal token in this corpus's grammar (`-1` is unusual; `0 - 1` is a send). To keep the literal-step rule simple and honest, write the descending test as `5 to: 1 by: -1 do: ...` **only if** the parser yields `{"type":"literal","litKind":"int","value":-1}` for `-1`; otherwise keep `by: 0 - 1` and accept that THIS case falls back to the primitive (still correct output 15, just not inlined). Verify in Step 2 and pick the form that the literal-step guard accepts so the descending path is actually exercised.

- [ ] **Step 2: Probe how the parser represents the step, then choose the test form**

```bash
cd ~/workspace/ns-oevm
python3 - <<'PY'
import subprocess, json, tempfile, os, sys
# Compile a tiny probe and inspect: does `-1` parse as a literal int?
src = '''class P packageUsing: m = () (
public class C = () ( public t = ( | s | s:: 0. 5 to: 1 by: -1 do: [:i| s:: s + i]. ^ s ) )
public main: p args: a = ( (C new) t printString out. )
) : ()'''
f = tempfile.NamedTemporaryFile('w', suffix='.ns', delete=False); f.write(src); f.close()
r = subprocess.run([sys.executable, 'tools/ns_compile_oevm.py', f.name, '/tmp/probe.c'],
                   capture_output=True, text=True)
print("rc", r.returncode)
print((r.stderr or r.stdout)[-400:])
PY
```
If it compiles cleanly, `-1` is a usable literal — use `by: -1` in the test and the literal-step guard will inline it. If it errors at parse, keep `by: 0 - 1` in the fixture (that case falls back to the primitive — still output 15) and add a SECOND descending case using whatever literal-negative form the grammar accepts, so the descending inline path is covered. Record which form you used in the commit message.

- [ ] **Step 3: Add the `to:by:do:` detection hook**

In `compile_expr`, `et == "send"`, immediately after the `to:do:` hook from Task 2, insert:

```python
            # Phase D: inline `to:by:do:` with a literal one-param block AND a
            # literal-integer step (so the loop direction is known at compile
            # time). Non-literal steps fall back to the PRIM_to_by_do path.
            if (selector == "to:by:do:"
                    and len(args) == 3
                    and args[2]["type"] == "block"
                    and len(args[2].get("params", [])) == 1
                    and args[1]["type"] == "literal"
                    and args[1].get("litKind") == "int"):
                self.compile_to_do_inline(receiver, args[0], args[1], args[2], target_reg)
                return
```

- [ ] **Step 4: Run conformance**

```bash
cd ~/workspace/ns-oevm && python3 tools/run_tests_oevm.py 2>&1 | grep '11_loop_inlining'
```
Expected: `... PASS`. Full summary still `Passed: 22, Failed: 10, Skipped: 1`.

- [ ] **Step 5: Prove no-op for if/and/or images**

```bash
cd ~/workspace/ns-oevm
rm -rf /tmp/img_after && python3 tools/snapshot_images.py /tmp/img_after
diff /tmp/img_before/04_somns_basic__09_if_inlining.ns.c /tmp/img_after/04_somns_basic__09_if_inlining.ns.c && \
diff /tmp/img_before/04_somns_basic__10_and_or_inlining.ns.c /tmp/img_after/04_somns_basic__10_and_or_inlining.ns.c && echo "IF/AND/OR UNCHANGED"
```
Expected: no diff, then `IF/AND/OR UNCHANGED`.

- [ ] **Step 6: Commit**

```bash
cd ~/workspace/ns-tests
git add 04_somns_basic/11_loop_inlining.ns
git commit -m "test: add to:by:do: ascending/descending loop cases

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
cd ~/workspace/ns-oevm
git add tools/ns_compile_oevm.py
git commit -m "compiler: inline to:by:do: with literal integer step

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: `whileTrue:` / `whileFalse:` inlining

Lower `[cond] whileTrue: [body]` (both literal zero-arg blocks) to a back-edge jump loop. Result = `nil`, matching the primitive.

**Files:**
- Modify: `ns-oevm/tools/ns_compile_oevm.py` (add `compile_loop_inline`; add detection hook)
- Test: `ns-tests/04_somns_basic/11_loop_inlining.ns` (append cases)

**Interfaces:**
- Produces: `compile_loop_inline(self, selector, cond_block, body_block, target_reg)` — `selector` in `("whileTrue:", "whileFalse:")`. Consumed by the detection hook.

- [ ] **Step 1: Extend the test fixture with while cases**

Add to the `LoopInlining` class body:

```
	public testWhileTrueCountUp = (
		| i sum |
		i:: 1. sum:: 0.
		[ i <= 4 ] whileTrue: [ sum:: sum + i. i:: i + 1 ].
		^ sum
	)
	public testWhileFalseCountUp = (
		| i sum |
		i:: 1. sum:: 0.
		[ i > 4 ] whileFalse: [ sum:: sum + i. i:: i + 1 ].
		^ sum
	)
	public testWhileNeverRuns = (
		| n |
		n:: 7.
		[ 1 > 2 ] whileTrue: [ n:: 99 ].
		^ n
	)
	public testWhileEarlyReturn = (
		| i |
		i:: 0.
		[ true ] whileTrue: [ i:: i + 1. i = 3 ifTrue: [ ^ i ] ].
		^ 0
	)
```

Add to `main:`:

```
	t testWhileTrueCountUp printString out.
	t testWhileFalseCountUp printString out.
	t testWhileNeverRuns printString out.
	t testWhileEarlyReturn printString out.
```

Append to `expect:`:

```
10
10
7
3
```

Values: `testWhileTrueCountUp` = 1+2+3+4 = **10**; `testWhileFalseCountUp` (runs while `i > 4` is false, i.e. same 1..4) = **10**; `testWhileNeverRuns` = body never runs, **7**; `testWhileEarlyReturn` = `^ i` fires when `i = 3`, returns **3** (proves an inlined `^` from a `while` body exits the method via `OP_RET`).

> Note `[ true ]` is a literal-bool block here (not a comparison) — confirm `true` as a bare receiver in a block is accepted (the Phase B gotcha was about a bare `true`/`false` *statement* becoming an implicit self-send; inside `[ true ]` it is the block's value expression, which is fine because `whileTrue:` tests its truthiness via `OP_JMP_IF_FALSE`). If the probe in Step 4 shows `[ true ]` misbehaving, replace with `[ 1 < 2 ]`.

- [ ] **Step 2: Add `compile_loop_inline`**

Add to `MethodCompiler`, after `compile_to_do_inline`:

```python
    def compile_loop_inline(self, selector, cond_block, body_block, target_reg):
        # `[cond] whileTrue: [body]` / whileFalse:. Result = nil (matches the
        # primitive). cond_reg and body_reg persist across iterations (allocated
        # once, below the scratch the inlined blocks reuse each pass). Truthiness
        # goes through OP_JMP_IF_FALSE only.
        cond_reg = self.alloc_reg()
        body_reg = self.alloc_reg()          # body value, discarded
        l_start = self.new_label()
        l_end = self.new_label()
        self.bind_label(l_start)
        self.compile_inlined_block(cond_block, cond_reg, init_temps=True)
        if selector == "whileTrue:":
            self.emit_jump(OP_JMP_IF_FALSE, cond_reg, l_end)   # falsy -> exit
            self.compile_inlined_block(body_block, body_reg, init_temps=True)
            self.emit_jump(OP_JMP, 0, l_start)
        else:  # "whileFalse:" — continue while falsy, exit when truthy
            l_body = self.new_label()
            self.emit_jump(OP_JMP_IF_FALSE, cond_reg, l_body)  # falsy -> body
            self.emit_jump(OP_JMP, 0, l_end)                   # truthy -> exit
            self.bind_label(l_body)
            self.compile_inlined_block(body_block, body_reg, init_temps=True)
            self.emit_jump(OP_JMP, 0, l_start)
        self.bind_label(l_end)
        self.emit_ABx(OP_LD_CONST, target_reg, self.add_constant(None))  # nil
        self.free_reg()  # body_reg
        self.free_reg()  # cond_reg
```

- [ ] **Step 3: Add the while detection hook**

In `compile_expr`, `et == "send"`, after the `to:by:do:` hook, insert:

```python
            # Phase D: inline whileTrue:/whileFalse: when both the receiver
            # (condition) and the single argument (body) are literal zero-arg
            # blocks. `blkVar whileTrue: ...` falls back to the primitive.
            if (selector in ("whileTrue:", "whileFalse:")
                    and receiver["type"] == "block" and not receiver.get("params")
                    and len(args) == 1
                    and args[0]["type"] == "block" and not args[0].get("params")):
                self.compile_loop_inline(selector, receiver, args[0], target_reg)
                return
```

- [ ] **Step 4: Probe the `[ true ]` receiver, then run the test**

```bash
cd ~/workspace/ns-oevm
python3 tools/run_tests_oevm.py 2>&1 | grep '11_loop_inlining'
```
Expected: `... PASS`. If it fails on the `testWhileEarlyReturn` row only, swap `[ true ]` → `[ 1 < 2 ]` in the fixture (per the Step 1 note) and re-run.

- [ ] **Step 5: Prove it inlined (no whileTrue:/whileFalse: send symbol)**

```bash
cd ~/workspace/ns-oevm
python3 tools/ns_compile_oevm.py ../ns-tests/04_somns_basic/11_loop_inlining.ns /tmp/loop.c
grep -c 'whileTrue\|whileFalse' /tmp/loop.c
```
Expected: `0`.

- [ ] **Step 6: Conformance + no-op proof**

```bash
cd ~/workspace/ns-oevm && python3 tools/run_tests_oevm.py 2>&1 | tail -4
rm -rf /tmp/img_after && python3 tools/snapshot_images.py /tmp/img_after
diff /tmp/img_before/04_somns_basic__09_if_inlining.ns.c /tmp/img_after/04_somns_basic__09_if_inlining.ns.c && \
diff /tmp/img_before/04_somns_basic__10_and_or_inlining.ns.c /tmp/img_after/04_somns_basic__10_and_or_inlining.ns.c && echo "IF/AND/OR UNCHANGED"
```
Expected: `Passed: 22, Failed: 10, Skipped: 1`; then no diff and `IF/AND/OR UNCHANGED`. Confirm `02_control_flow/02_while_true.ns` still PASS:
```bash
python3 tools/run_tests_oevm.py 2>&1 | grep '02_while_true'
```
Expected: `PASS`.

- [ ] **Step 7: Commit**

```bash
cd ~/workspace/ns-tests
git add 04_somns_basic/11_loop_inlining.ns
git commit -m "test: add whileTrue:/whileFalse: inlining cases (incl. ^ from body)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
cd ~/workspace/ns-oevm
git add tools/ns_compile_oevm.py
git commit -m "compiler: inline whileTrue:/whileFalse: (literal cond+body blocks)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Perf evidence + docs + memory

Quantify the win (the benchmarks are sub-10ms, so measure *instruction-count* reduction, not wall-clock) and record Phase D.

**Files:**
- Modify: `oevm/docs/optimizations.md` (append Phase D log entry)
- Modify: `oevm/docs/topics.md` (extend the `block-inlining` topic with the loop lowering)
- Modify (memory): `~/.claude/projects/-home-david-workspace-oevm/memory/block-inlining-status.md` + `MEMORY.md` pointer

- [ ] **Step 1: Measure instruction-count reduction on a loop-heavy program**

Compile `queens` (uses `to:do:`) before/after is not possible post-merge, so measure on a fixed probe: count emitted instructions for a `to:do:` loop with inlining ON vs the fallback (temporarily disable the `to:do:` hook). Simpler and durable — compare image sizes / send-count:

```bash
cd ~/workspace/ns-oevm
python3 tools/ns_compile_oevm.py ../ns-tests/05_somns_benchmarks/01_queens.ns /tmp/queens.c
echo "to:do: sends remaining:"; grep -c 'StaticSymbol_to_do_' /tmp/queens.c
echo "createClosure sites:"; grep -c 'createClosure' /tmp/queens.c
```
Expected: `to:do:` send count `0` (all inlined); `createClosure` count strictly lower than the Phase C baseline. Record the numbers in the docs entry.

- [ ] **Step 2: Append the optimizations log entry**

Add a dated Phase D section to `oevm/docs/optimizations.md` summarising: what was inlined (`to:do:`/`to:by:do:` literal-step/`whileTrue:`/`whileFalse:`), the lowering shape, the fallback conditions, the no-op proof for if/and/or, conformance `21→22`, and the queens send-count numbers from Step 1.

- [ ] **Step 3: Extend the topics doc**

In `oevm/docs/topics.md`, under the existing `block-inlining` topic, document the loop lowerings (back-edge jump, induction register, `init_temps` per-iteration nil reset, result = receiver for `to:do:` / nil for `while`) and the literal-step direction rule.

- [ ] **Step 4: Commit docs**

```bash
cd ~/workspace/oevm
git add nodus/oevm/docs/optimizations.md nodus/oevm/docs/topics.md
git commit -m "docs: record block-inlining Phase D (loop inlining)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

- [ ] **Step 5: Update memory**

Update `docs/superpowers/plans/block-inlining-status.md` (the single source of
truth for this track): mark Phase D LANDED (to:do:/to:by:do:/whileTrue:/whileFalse:
inlined; `11_loop_inlining.ns` added) and set NEXT to Phase E (the shared
branch-emit helper refactor — Task 6). Record the current conformance count
there. `02_block_inlining` is already green via the landed `Array>>do:` library
method, so it is no longer a Phase E goal.

---

## Task 6: Refactor — extract shared cond/branch skeleton (pure no-op cleanup)

> **LANDED** in commit `3cf2516`. `_emit_cond_jmp_false` exists and is used by
> `compile_if_inline` + `compile_logic_inline`; the loop/`to:do:` sites are
> correctly left unfolded. This is now the **Phase E** completion. Kept below as
> the record of what was done.

Flagged by the Phase C review: `compile_if_inline`, `compile_logic_inline`, `compile_to_do_inline`, and `compile_loop_inline` all share the "alloc a cond reg, compile a condition, `emit_jump(OP_JMP_IF_FALSE, …)`" skeleton. Extract a small helper and prove byte-identical output. **Do this last so functionality is already locked and the diff is provably a no-op.**

**Files:**
- Modify: `ns-oevm/tools/ns_compile_oevm.py`

- [ ] **Step 1: Snapshot BEFORE**

```bash
cd ~/workspace/ns-oevm
rm -rf /tmp/img_before && python3 tools/snapshot_images.py /tmp/img_before
```

- [ ] **Step 2: Extract a `_emit_cond_jmp_false(cond_expr, cond_reg, label)` helper**

Add a helper that compiles a condition into a caller-allocated reg and emits the false-branch jump:

```python
    def _emit_cond_jmp_false(self, cond_expr, cond_reg, label):
        # Compile `cond_expr` into cond_reg and branch to `label` when falsy.
        # Single-sources the condition+JMP_IF_FALSE skeleton shared by the
        # if-family, and:/or:, and the loop lowerings. Truthiness lives solely
        # in OP_JMP_IF_FALSE (the VM's is_truthy).
        self.compile_expr(cond_expr, cond_reg)
        self.emit_jump(OP_JMP_IF_FALSE, cond_reg, label)
```

Then rewrite the two lines `self.compile_expr(receiver, cond_reg)` + `self.emit_jump(OP_JMP_IF_FALSE, cond_reg, l_false)` in **`compile_if_inline`** and **`compile_logic_inline`** as `self._emit_cond_jmp_false(receiver, cond_reg, l_false)`. (Leave the loop helpers as-is for now if their condition isn't a plain expr — `compile_to_do_inline`'s condition is a synthesized comparison send, not a `compile_expr` call, so it does NOT use this helper; do not force it.) Only fold call sites where the body is *exactly* `compile_expr(expr, reg)` then `emit_jump(OP_JMP_IF_FALSE, reg, label)` with nothing between.

- [ ] **Step 3: Prove byte-identical**

```bash
cd ~/workspace/ns-oevm
rm -rf /tmp/img_after && python3 tools/snapshot_images.py /tmp/img_after
diff -r /tmp/img_before /tmp/img_after && echo "NO-OP CONFIRMED"
```
Expected: no diff, then `NO-OP CONFIRMED`.

- [ ] **Step 4: Conformance unchanged**

```bash
cd ~/workspace/ns-oevm && python3 tools/run_tests_oevm.py 2>&1 | tail -4
```
Expected: conformance unchanged from the current baseline (at time of writing
`Passed: 47, Failed: 0, Skipped: 1`). The refactor is a no-op, so whatever the
baseline is before the change must hold after it.

- [ ] **Step 5: Commit**

```bash
cd ~/workspace/ns-oevm
git add tools/ns_compile_oevm.py
git commit -m "compiler: extract _emit_cond_jmp_false helper (no-op; single-source truthiness)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review notes (for the executor)

- **Spec coverage:** `whileTrue:`/`whileFalse:` (Task 4), `to:do:` (Task 2), `to:by:do:` (Task 3) — all four loop forms from the design spec §"Inlining transformation" are covered. The induction-var block-param binding the spec calls out is Task 1's `param_regs`.
- **Type consistency:** `compile_inlined_block(block, target_reg, param_regs=None, init_temps=False)` is defined in Task 1 and called with those exact kwargs in Tasks 2 & 4. `compile_to_do_inline(start, end, step_node, block, target_reg)` defined in Task 2, reused in Task 3. `compile_loop_inline(selector, cond_block, body_block, target_reg)` defined in Task 4.
- **Fallback safety:** every detection hook has explicit literal-shape guards and falls through to the unchanged closure+send path otherwise; the VM primitives remain. A wrong guard degrades to "not inlined" (slower) not "wrong" — verify outputs regardless.
- **Watch items:** (1) the descending `to:by:do:` literal-negative-step form (Task 3 Step 2 probe decides `-1` vs fallback); (2) `[ true ]` as a while condition (Task 4 Step 1 note); (3) per-iteration temp semantics rely on `init_temps=True` — if any loop body temp reads before assignment and the output is wrong, that's the cause.
```
