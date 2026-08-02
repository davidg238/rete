# De-recursify Phase 2 — The Flip (flat non-recursive dispatch) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **⚠️ APPROVAL GATE:** This plan rewrites the interpreter core (`run_interpreter`) — the hottest, most invariant-laden function in the VM. It is Phase 2 ("the flip") of the de-recursify design (`oevm/docs/fiber_derecursify_core_design.md`). **Do not begin execution until David has reviewed the "Findings & decisions requiring sign-off" section below.** Two items were discovered during grounding that change scope, and the flip's decomposition strategy is a genuine fork.

**Goal:** Replace the recursive C `run_interpreter` (where a method/block invoke re-enters `run_interpreter` via a nested C call) with a single flat non-recursive loop where every invoke PUSHES a frame in the loop and `OP_RETURN` pops it — so a fiber is one C activation and a fiber switch is just swapping the heap `Stack`.

**Architecture:** `OP_SEND`/`OP_SEND_IMPLICIT`/`OP_SEND_SUPER`/`OP_BLOCK_INVOKE`, on resolving a *bytecode* target, push a new `MethodContext` frame (relative `prev_frame`, caller `pc` saved into the caller frame's `saved_pc`) and `continue` the loop instead of calling `run_interpreter`. `OP_RETURN` pops the frame via the dynamic link, restores the caller's `pc`/`method`/`regs`, and delivers the result into the caller's destination register (the "src2 decode" generalised from today's Zephyr hijack). The loop bottoms out only at the fiber's entry frame (`prev_frame == NULL`). NLR / `ensure:` / `ifCurtailed:` become an in-loop unwinder walking the `prev_frame` chain, replacing the recursive-return + global-`unwind_target_reached` protocol. `home_ctx` stays an ABSOLUTE pointer (valid because stacks are FIXED — no growth this phase). Primitives still run as inline C calls (they never pushed interpreter frames). The recursive `run_interpreter` signature is KEPT as a bounded, re-entrant entry point for the debugger hook and the (single-shot) `does_not_understand`/init helpers — the design permits this because those fire synchronously and never cross a fiber switch.

**Tech Stack:** C99 (oevm VM: `src/vm.c`, `src/dispatch.c`, `src/oevm.h`, `src/frame_abi.h`), Python 3 compiler (`ns-oevm/tools/ns_compile_oevm.py`), image generators (`tools/solidify.py` Path A, `tools/oimg_serialize.py` Path B), Newspeak `.ns` (ns-tests).

## Global Constraints

- **Repos & cwd:** VM edits in `/home/david/workspace/oevm/src`. Compiler/generator/plan edits in `/home/david/workspace/ns-oevm`. Suite runs from the `ns-oevm` cwd.
- **Behavior-equivalence is the oracle.** The flip changes the *mechanism*, not observable behavior. The gate at EVERY task: `cd /home/david/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py` passes with **no new failures** vs. the Phase-1 baseline (**78/0/6 both paths**) AND all 5 gates PASS (grant, debugger-integration, portability-32, fibers-gc-stress, boot-gc-asan). The suite already exercises NLR, `ensure:`/`ifCurtailed:`, deep recursion, and fiber interleaving — it is the behavior oracle for the flip.
- **Extra flip gates (run at every flip task):** `OEVM_GC_STRESS=1` clean + ASan clean (the flip changes frame lifetime); `vm.c` must stay `-D__ZEPHYR__` syntax-clean (`gcc -fsyntax-only -D__ZEPHYR__ -Isrc src/vm.c`).
- **`home_ctx` stays ABSOLUTE.** Do NOT relativize `home_ctx` in this phase (that is the deferred L4 heap-closure/temp-vector lift). It is safe as an absolute pointer because stacks are FIXED (no growth in Phase 2).
- **No growth.** Do NOT add stack growth. `VM_STACK_MAX_WORDS` overflow stays a hard `abort()` as today.
- **Both image paths, every task.** New opcodes (`OP_UNWIND` if introduced) pass through Path A (solidify.py) + Path B (oimg_serialize.py) as raw bytecode; a new opcode number must be identical in `oevm/src/oevm.h` (`enum Opcode`) and `ns-oevm/tools/ns_compile_oevm.py`. Next free opcode after Phase 1 is `0x17`.
- **Commit per task per repo** with the trailer `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. **Do NOT push** unless David says so.
- **Locate every edit site by QUOTED CODE, not line numbers** (they drift heavily in `vm.c`).

---

## Findings & decisions requiring sign-off (READ FIRST)

**F1 — Two recursive block-invoke primitives survived Phase 1 (scope gap).** Phase 1 deleted the if-family/`and:`/`or:`/`to:do:`/`to:by:do:`/`whileTrue:` primitives, but its plan did not enumerate:
- `PRIM_timesRepeat` (`n timesRepeat: [block]`, `dispatch.c` `case PRIM_timesRepeat:`) — a C loop calling `run_interpreter(block, block, 0, NULL)` per iteration. Not compiler-lowered.
- The **unary** `whileTrue`/`whileFalse` intercept (`dispatch.c`, `strcmp(sname, "whileTrue") == 0 || strcmp(sname, "whileFalse") == 0`) — `[cond] whileTrue` (no colon, no body), a C loop calling `run_interpreter` on the receiver-block per iteration.

A *looping* C primitive **cannot** work in the flat model (it can't re-push the block each iteration once control leaves its C frame), so both MUST be lowered to bytecode before the flip. **Decision (recommended):** treat these as Phase 2 pre-tasks (Tasks 1–2) — the same lowering mechanism Phase 1 used (`emit_block_value` + delete the primitive). This retroactively completes Phase 1's "no recursive block-invoke primitive" mandate.

**F2 — `OP_SEND_IC` / `send_ic` / `struct InlineCache` is dead code.** `OP_SEND_IC` is never emitted by the compiler (writing an in-stream IC would SIGSEGV on the read-only Model B blob — documented in `dispatch.c`). Retire it (Task 3) to shrink the flip's surface (the Zephyr reload block's IC-skip logic disappears with it).

**F3 — The flip is semi-atomic; TDD granularity is limited.** You cannot half-rewrite `run_interpreter` and keep the suite green. The flip (Tasks 4–6) is therefore fewer, larger tasks gated by the *comprehensive existing suite* as the behavior-equivalence oracle, rather than many small red-green cycles. **Decision fork for David:** (A) **big-bang-with-suite-gate** (recommended) — rewrite the loop in one focused task, lean on the full suite + NLR/ensure/fiber/GC-stress gates; or (B) **parallel-flagged** — build the flat loop behind a compile flag alongside the recursive one, migrate, then delete the old. (B) is lower-risk-per-step but ~2× the work and carries its own divergence risk. This plan is written for **(A)**.

**F4 — `run_interpreter` is KEPT (re-entrant) for three single-shot callers.** After the flip, `run_interpreter` is still called (non-recursively in the hot path, but re-entrantly) by: the debugger hook (design-sanctioned — fires synchronously, result discarded, no fiber switch crosses it), `does_not_understand` when the handler is bytecode, and `run_method_on` (nested-class init). These are single-shot, bounded, and never loop — acceptable to keep as bounded nested calls. The hot method/block-send path does NOT recurse. **Decision (recommended):** keep them; revisit only if a fiber switch is ever shown to cross one.

---

## File structure

- `oevm/src/vm.c` — the flip lives here: the flat loop, frame push helper, `OP_RETURN` result-delivery, in-loop unwinder, retired IC handler. Largest change.
- `oevm/src/dispatch.c` — `send()`/`send_super()` restructured to a "resolve to (method, is_primitive)" split; `PRIM_timesRepeat` + unary while intercept deleted; `send_ic` deleted.
- `oevm/src/oevm.h` — retire `OP_SEND_IC` enum + `struct InlineCache`; add `OP_UNWIND` if the unwinder needs a bytecode (see Task 5).
- `ns-oevm/tools/ns_compile_oevm.py` — `timesRepeat:` lowering + unary `whileTrue`/`whileFalse` lowering; drop any `OP_SEND_IC` constant.
- `ns-tests/04_somns_basic/*.ns` — new regression tests for the lowered `timesRepeat:` / unary while; the flip adds NO new tests (existing suite is the oracle) but MAY add targeted deep-recursion / NLR-depth stress tests (Task 6).

---

### Task 1: Lower `timesRepeat:`; delete `PRIM_timesRepeat`

**Files:**
- Modify: `ns-oevm/tools/ns_compile_oevm.py` (add a `timesRepeat:` guard + a counting-loop lowering, reusing `emit_block_value`)
- Modify: `oevm/src/dispatch.c` (delete `case PRIM_timesRepeat:`)
- Test: `ns-tests/04_somns_basic/52_timesrepeat_nonliteral.ns`

**Interfaces:**
- Consumes: `emit_block_value(self, block_arg, target_reg, param_regs=None, init_temps=False)` and `OP_BLOCK_INVOKE` (both from Phase 1).

**Background — equivalence** (`dispatch.c` `PRIM_timesRepeat`): `n timesRepeat: aBlock` evaluates the 0-arg `aBlock` exactly `n` times (for `n ≤ 0`, zero times); result value = the receiver `n`. Break on `unwind_target_reached` (NLR out of the block).

- [ ] **Step 1: Write the failing test**

Create `ns-tests/04_somns_basic/52_timesrepeat_nonliteral.ns` — a non-literal body block (forces the lowered `OP_BLOCK_INVOKE` path):

```
(*
`n timesRepeat: aBlock` with a non-literal (temp-held) 0-arg block, forcing the
compiler's lowered counting loop driving OP_BLOCK_INVOKE (not PRIM_timesRepeat).
4 timesRepeat: [count := count + 1] -> 4.

expect:
4
*)
class TimesRepeatTest packageUsing: manifest = () (
public main: platform args: args = (
    | body count |
    count:: 0.
    body:: [count:: count + 1].
    4 timesRepeat: body.
    count printString out.
)
) : (
)
```

- [ ] **Step 2: Run the suite to confirm the new test PASSES today (via PRIM_timesRepeat, non-vacuous guard)**

Run: `cd /home/david/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | grep -iE 'timesrepeat|FAIL' | tail -5`
Expected: PASSES today (routes through `PRIM_timesRepeat`, which recursively invokes `body`).

- [ ] **Step 3: Add the `timesRepeat:` lowering to the compiler**

In `ns_compile_oevm.py`, add a `MethodCompiler` method modelled on `compile_to_do_inline` but with a fixed 0-based counter and a 0-arg body invoked via `emit_block_value`:

```python
    def compile_times_repeat(self, receiver, block, target_reg):
        # `n timesRepeat: aBlock` — evaluate the 0-arg block `n` times; result = n
        # (the receiver). Matches PRIM_timesRepeat. Loop counter i: 0..<n.
        self.compile_expr(receiver, target_reg)        # result = receiver = n
        n_reg = self.alloc_reg()
        self.emit_ABC(OP_LD_REG, n_reg, target_reg, 0) # n := receiver
        i_reg = self.alloc_reg()
        self.emit_ABx(OP_LD_CONST, i_reg, self.add_constant("SMI:0"))  # i := 0
        l_start = self.new_label()
        l_end = self.new_label()
        self.bind_label(l_start)
        # cond: (i < n) -> rc
        rc = self.alloc_reg()
        self.emit_ABC(OP_LD_REG, rc, i_reg, 0)
        ec = self.alloc_reg()                          # contiguous arg = rc + 1
        self.emit_ABC(OP_LD_REG, ec, n_reg, 0)
        self.emit_ABC(OP_SEND, 1, self.add_constant("<"), rc)
        self.emit_jump(OP_JMP_IF_FALSE, rc, l_end)
        self.free_reg()  # ec
        self.free_reg()  # rc
        # body: 0-arg block (literal inlines; non-literal OP_BLOCK_INVOKE'd)
        body_reg = self.alloc_reg()
        self.emit_block_value(block, body_reg, init_temps=True)
        self.free_reg()  # body_reg
        # i := i + 1
        step_reg = self.alloc_reg()
        self.emit_ABx(OP_LD_CONST, step_reg, self.add_constant("SMI:1"))
        self.emit_ABC(OP_ADD, i_reg, i_reg, step_reg)
        self.free_reg()  # step_reg
        self.emit_jump(OP_JMP, 0, l_start)
        self.bind_label(l_end)
        self.free_reg()  # i_reg
        self.free_reg()  # n_reg
        # target_reg already holds n (the receiver) = result
```

- [ ] **Step 4: Wire the guard**

In the combinator-guard region (near the `to:do:` guard added in Phase 1), add — BEFORE the normal-send fallthrough:

```python
            # timesRepeat: — always lowered to a counting loop driving OP_BLOCK_INVOKE.
            if selector == "timesRepeat:" and len(args) == 1:
                self.compile_times_repeat(receiver, args[0], target_reg)
                return
```

- [ ] **Step 5: Delete `PRIM_timesRepeat`**

In `oevm/src/dispatch.c`, delete the `case PRIM_timesRepeat:` block (the C loop calling `run_interpreter(block, block, 0, NULL)`), mirroring the Phase-1 deletion style (remove the case, leave an explanatory comment; the selector falls through to `does_not_understand`). Confirm no reflective `perform: #timesRepeat:` in the suite: `grep -rn "perform:.*#timesRepeat" ../ns-tests` (expect none).

- [ ] **Step 6: Rebuild + suite**

Run: `cd /home/david/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | tail -6`
Expected: 79/0/6 both paths (baseline 78 + new test), all 5 gates PASS. Watch for any existing suite test using `timesRepeat:` — it now takes the lowered path.

- [ ] **Step 7: Commit** (oevm dispatch.c; ns-oevm compiler; ns-tests test — three commits, trailer each; messages naming `timesRepeat:` lowering + `PRIM_timesRepeat` deletion).

---

### Task 2: Lower unary `whileTrue`/`whileFalse`; retire that intercept branch

**Files:**
- Modify: `ns-oevm/tools/ns_compile_oevm.py` (lower unary `whileTrue`/`whileFalse` — a receiver-block looped with an empty body)
- Modify: `oevm/src/dispatch.c` (delete the `strcmp(sname, "whileTrue")==0 || strcmp(sname, "whileFalse")==0` loop in the `BytecodeClosureVtable` intercept; KEEP the `value`-prefix branch)
- Test: `ns-tests/04_somns_basic/53_while_unary.ns`

**Interfaces:** Consumes `emit_block_value`, `compile_loop_inline` (Phase 1).

**Background — equivalence** (`dispatch.c` intercept): `[cond] whileTrue` (unary — no body) loops evaluating `cond value` until it is falsy; result = nil. `whileFalse` loops until `cond value` is truthy. (Rare idiom; the loop body's effect must be a side effect of the condition block.)

- [ ] **Step 1: Verify usage** — `grep -rn "whileTrue\b\|whileFalse\b" ../ns-tests | grep -v "whileTrue:\|whileFalse:"` — determine whether any suite `.ns` uses the unary form. If NONE (likely), the new test below is the sole exerciser and the change is purely defensive; still lower it (the intercept must go for the flip).

- [ ] **Step 2: Write the failing test**

Create `ns-tests/04_somns_basic/53_while_unary.ns` — a unary `whileTrue` whose condition block has the loop's side effect and eventually returns false:

```
(*
Unary `[cond] whileTrue` (no body) — the condition block carries the side effect
and eventually goes false. Lowered to a bytecode loop (no PRIM/intercept).
n starts 0, cond increments n and yields (n < 3); loop ends at n = 3.

expect:
3
*)
class WhileUnaryTest packageUsing: manifest = () (
public main: platform args: args = (
    | n cond |
    n:: 0.
    cond:: [n:: n + 1. n < 3].
    cond whileTrue.
    n printString out.
)
) : (
)
```

- [ ] **Step 3: Run the suite to confirm it PASSES today (via the intercept)**

Run: `cd /home/david/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | grep -iE 'while_unary|FAIL' | tail -5`
Expected: PASSES today (unary-while intercept loops the block).

- [ ] **Step 4: Lower unary while in the compiler**

Add a guard + lowering. The unary form is a receiver-block looped with an empty body; reuse `compile_loop_inline` by synthesising an empty-body block, OR add a dedicated helper. Concretely, add a helper that emits the loop with only the condition (no body invoke):

```python
    def compile_while_unary(self, selector, cond_block, target_reg):
        # `[cond] whileTrue` / `[cond] whileFalse` — no body. Loop evaluating the
        # condition block until it goes falsy (whileTrue) / truthy (whileFalse).
        # Result = nil. Condition invoked via emit_block_value (literal inlines,
        # non-literal OP_BLOCK_INVOKE'd).
        cond_reg = self.alloc_reg()
        l_start = self.new_label()
        l_end = self.new_label()
        self.bind_label(l_start)
        self.emit_block_value(cond_block, cond_reg, init_temps=True)
        if selector == "whileTrue":
            self.emit_jump(OP_JMP_IF_FALSE, cond_reg, l_end)   # falsy -> exit
            self.emit_jump(OP_JMP, 0, l_start)
        else:  # "whileFalse"
            l_again = self.new_label()
            self.emit_jump(OP_JMP_IF_FALSE, cond_reg, l_again) # falsy -> loop
            self.emit_jump(OP_JMP, 0, l_end)                   # truthy -> exit
            self.bind_label(l_again)
            self.emit_jump(OP_JMP, 0, l_start)
        self.bind_label(l_end)
        self.emit_ABx(OP_LD_CONST, target_reg, self.add_constant(None))  # nil
        self.free_reg()  # cond_reg
```

Guard (note: unary selectors are `whileTrue`/`whileFalse` with `len(args) == 0`):

```python
            if selector in ("whileTrue", "whileFalse") and len(args) == 0:
                self.compile_while_unary(selector, receiver, target_reg)
                return
```

- [ ] **Step 5: Delete the intercept's unary-while branch**

In `oevm/src/dispatch.c`, inside `if (receiver_vtable == BytecodeClosureVtable)`, delete the `strcmp(sname, "whileTrue") == 0 || strcmp(sname, "whileFalse") == 0` loop. **KEEP** the `strncmp(sname, "value", 5) == 0` branch (still the reflective block-invoke fallback until Task 4 folds it).

- [ ] **Step 6: Rebuild + suite**

Run: `cd /home/david/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | tail -6`
Expected: 80/0/6 both paths (78 + Task 1 + Task 2 tests), all 5 gates PASS.

- [ ] **Step 7: Commit** (oevm + ns-oevm + ns-tests; trailer each).

---

### Task 3: Retire dead `OP_SEND_IC` / `send_ic` / `struct InlineCache`

**Files:**
- Modify: `oevm/src/vm.c` (delete `case OP_SEND_IC:` handler)
- Modify: `oevm/src/dispatch.c` (delete `send_ic`)
- Modify: `oevm/src/oevm.h` (remove `OP_SEND_IC` from `enum Opcode`? — see caution; remove `struct InlineCache`)
- Modify: `ns-oevm/tools/ns_compile_oevm.py` (remove the `OP_SEND_IC` constant if present)

**Background:** `OP_SEND_IC` is never emitted (documented dead code in `dispatch.c`). Removing it also removes the `sizeof(struct InlineCache)` skip-back logic the Zephyr reload block references — simplifying the flip.

- [ ] **Step 1: Confirm it is dead** — `grep -rn "OP_SEND_IC\|send_ic\|InlineCache" oevm/src ns-oevm/tools`. Expected: definitions + the vm.c handler + the Zephyr reload `sizeof(struct InlineCache)` references, but NO compiler emission of `OP_SEND_IC`.

- [ ] **Step 2: Delete `case OP_SEND_IC:` in `vm.c`** (the block that reads `struct InlineCache *ic = (struct InlineCache *)pc; pc += ...; send_ic(...)`).

- [ ] **Step 3: Delete `send_ic` in `dispatch.c`** and its forward declaration.

- [ ] **Step 4: Handle the Zephyr reload `sizeof(struct InlineCache)` references.** The reload block backs up over an inline-cache word only for `OP_SEND_IC`. With `OP_SEND_IC` gone, that branch is unreachable — but `struct InlineCache` may still be referenced there. Simplify: since the surviving reload opcodes (`OP_SEND`/`OP_SEND_IMPLICIT`/`OP_BLOCK_INVOKE`) are all IC-less, the entire `if (op != ... ) { inst_ptr = ... sizeof(struct InlineCache) ... }` skip-back can be DELETED (all reload sites read `src2` directly). Do this at all 4 `#ifdef __ZEPHYR__` reload sites. **CAUTION:** this touches the Zephyr hijack, which Task 6 deletes entirely — coordinate; if Task 6 immediately follows, you may leave the skip-back and let Task 6 remove it. Prefer deleting here for a clean Task 3.

- [ ] **Step 5: Remove `struct InlineCache` from `oevm.h`** once no references remain. For `OP_SEND_IC` in `enum Opcode`: you MAY keep the enum value as a reserved/retired marker (a comment `/* 0x03 retired: OP_SEND_IC (dead IC path) */`) to avoid renumbering `OP_RET=4`… — **do NOT renumber existing opcodes** (that would break every `.oimg` and solidified image). Just remove the handler and mark the enum value retired.

- [ ] **Step 6: Rebuild + suite + syntax-check under Zephyr**

Run: `cd /home/david/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | tail -6` (expect 80/0/6 both paths) and `cd /home/david/workspace/oevm && gcc -fsyntax-only -D__ZEPHYR__ -Isrc src/vm.c && echo OK`.

- [ ] **Step 7: Commit** (oevm; trailer).

---

### Task 4: Split `send()` into resolve + invoke; make bytecode-send push a frame in the loop

**Files:**
- Modify: `oevm/src/dispatch.c` (add a `resolve_send(...)` that returns the target method + a primitive/bytecode flag WITHOUT invoking; keep `send()` as the re-entrant single-shot wrapper for `does_not_understand`/init/debugger)
- Modify: `oevm/src/vm.c` (`OP_SEND` and its siblings call `resolve_send`; on a bytecode target, PUSH a frame + `continue` instead of `run_interpreter`; on a primitive, call the C fn inline as today)

**Interfaces:**
- Produces: `oop resolve_send(oop receiver, oop selector, int argc, oop *args, oop *out_method, int *out_is_primitive)` — performs method-cache lookup + access check; sets `*out_method` to the resolved closure and `*out_is_primitive` to 1 for a `PrimitiveClosureVtable` target, 0 for a `BytecodeClosureVtable`. Returns a sentinel (e.g. a special "handled" oop or a status) when the send was fully handled inline (DNU dispatched, or an intercepted `value`-family block invoke that itself resolves to a bytecode closure — which must ALSO become a frame push, see Step 4). This signature is the crux; refine it in Step 1 against the real `send()` body.

**Architecture note:** Today `OP_SEND` does `result = send(...); regs[src2] = result;`. After this task, `OP_SEND` for a *bytecode* target must NOT get a completed `result` — it must push the callee frame and let the loop run it, with `OP_RETURN` later delivering the result into `regs[src2]`. So the caller frame records, in its `saved_pc`, enough to recover `src2` on return (today's hijack already decodes `src2` from the instruction at `saved_pc - 1`; Task 5 formalises this as the return decode). **This task and Task 5 are tightly coupled — a bytecode send that pushes a frame is only correct once `OP_RETURN` knows how to deliver the result back. Implement Tasks 4+5 together and gate on the full suite.**

- [ ] **Step 1: Extract `resolve_send` from `send()`** — factor the lookup/cache/access-check/DNU portion of `send()` (everything up to the `if (obj_vtable(cl)==PrimitiveClosureVtable) {...} else {return run_interpreter(...);}` tail) into `resolve_send`, returning the method + primitive flag. Keep `send()` calling `resolve_send` then doing the old primitive-call / `run_interpreter` tail, so `send()`'s behavior is UNCHANGED at this step (pure refactor). Gate: suite 80/0/6 both paths.

- [ ] **Step 2: Commit the refactor** (behavior-identical; a clean checkpoint before the loop change).

- [ ] **Step 3–N (the loop change):** In `vm.c`, change `OP_SEND` (then `OP_SEND_IMPLICIT`, `OP_SEND_SUPER`, `OP_BLOCK_INVOKE`) so that after `resolve_send` (or the implicit/super lookup): a **primitive** target calls the C fn inline → `regs[src2] = result` (as today); a **bytecode** target PUSHES a frame (factor the vm.c frame-push prologue — currently the top of `run_interpreter` — into a static helper `push_frame(closure, receiver, argc, args)` that sets `prev_frame`/`home_ctx`/regs/`current_frame` and advances `vm_stack_ptr`) and `continue`s the loop with `method`/`pc`/`pc_end`/`ctx` reloaded for the callee. The caller's `saved_pc = pc` is already set before the send. See Task 5 for the matching `OP_RETURN`.

  Because this is the semi-atomic core (F3), implement Steps 3–N as one cohesive change across the four send opcodes + the frame-push helper + Task 5's return, then gate on the full suite + GC-stress + ASan + Zephyr-syntax. Detailed sub-steps and the exact `push_frame` body are specified in Task 5 (they are one unit).

- [ ] **Gate + Commit:** deferred to the end of Task 5 (Tasks 4-loop and 5 land together).

---

### Task 5: Flat `OP_RETURN` (pop + src2-deliver) + in-loop unwinder; bottom-out at entry frame

**Files:**
- Modify: `oevm/src/vm.c` (the `run_interpreter` loop body: `push_frame` helper, `OP_RET` rewrite, `OP_NLR`/unwind rewrite, `exit_frame` semantics, entry-frame bottom-out)
- Modify: `oevm/src/oevm.h` (add `OP_UNWIND = 0x17` ONLY if the unwinder is implemented as a bytecode; the recommended design does the unwind in C without a new opcode — see Step 4)

**Interfaces:**
- Consumes: `push_frame(...)` and the send-opcode changes from Task 4.

**Design — the flat loop end-state** (implements design doc §4):

1. **`push_frame` helper** (extract from `run_interpreter`'s current prologue, verbatim semantics):

```c
/* Push a new bytecode-method/block frame onto the active fiber stack and make it
   current. Mirrors the old run_interpreter prologue exactly (relative prev_frame,
   absolute home_ctx, gen tag, regs[0]=receiver, regs[1..]=args). Returns the new
   frame; caller reloads method/pc/pc_end from it and `continue`s the loop. */
static struct MethodContext *push_frame(oop closure, oop receiver, int argc, oop *args) {
    struct Closure *cl = (struct Closure *)closure;
    struct CompiledMethod *m = (struct CompiledMethod *)closure_entry(cl);
    struct MethodContext *prev_ctx = current_frame;
    size_t frame_words = sizeof(struct MethodContext) / sizeof(oop) + m->num_registers;
    if (vm_stack_ptr + frame_words > VM_STACK_MAX_WORDS) { fprintf(stderr, "VM Error: Stack Overflow\n"); abort(); }
    struct MethodContext *ctx = (struct MethodContext *)&vm_stack[vm_stack_ptr];
    vm_stack_ptr += frame_words;
    ctx->vtable = NULL;
    ctx->prev_frame = frame_link_encode(vm_stack, prev_ctx);
    ctx->saved_pc = NULL;
    ctx->closure = closure;
    ctx->home_ctx = cl->home_ctx ? cl->home_ctx : (oop)ctx;   /* ABSOLUTE (fixed stacks) */
    ctx->gen_tag.tag32 = ++current_generation_tag;
    ctx->cleanup_cl = NULL;
    ctx->regs[0] = receiver;
    for (int i = 0; i < argc; i++) if (i + 1 < m->num_registers) ctx->regs[i + 1] = args[i];
    for (int i = argc + 1; i < m->num_registers; i++) ctx->regs[i] = NULL;
    current_frame = ctx;
    return ctx;
}
```

2. **`OP_RETURN` (rewrite `OP_RET`)** — pop the frame, deliver the result into the CALLER's destination register, resume the caller's `pc`. The destination register is recovered exactly as today's Zephyr hijack does it: decode the caller's suspended instruction (at `caller->saved_pc - 1`) and read its `src2` (bits 24-31), since `OP_SEND`/`OP_SEND_IMPLICIT`/`OP_SEND_SUPER`/`OP_BLOCK_INVOKE` all write their result to `regs[src2]` (for super, `regs[c]` where c==src2). Bottom-out when `prev_frame == NULL`:

```c
case OP_RET: {
    oop rv = (dest < method->num_registers) ? ctx->regs[dest] : NULL;
    struct MethodContext *caller = frame_link_decode(vm_stack, ctx->prev_frame);
    vm_stack_ptr = (size_t)((oop *)ctx - vm_stack);     /* pop: reclaim this frame */
    if (caller == NULL) {                                /* fiber entry frame */
        current_frame = NULL;
        return_val = rv;
        goto exit_frame;                                 /* to C caller of run_interpreter (main / trampoline / debugger / DNU) */
    }
    current_frame = caller;
    ctx = caller;
    struct Closure *ccl = (struct Closure *)caller->closure;
    method = (struct CompiledMethod *)closure_entry(ccl);
    pc = caller->saved_pc;
    pc_end = cm_instructions(method) + method->instruction_count;
    uint32_t suspended = *(caller->saved_pc - 1);        /* the send that called us */
    uint8_t dst = (suspended >> 24) & 0xFF;              /* src2 of the send == result reg */
    caller->regs[dst] = rv;
    break;                                               /* continue caller's loop */
}
```

3. **In-loop unwinder (NLR / `ensure:` / `ifCurtailed:`)** — replace the recursive-return + `unwind_target_reached`-global protocol. `OP_NLR` (and the `ensure:`/`ifCurtailed:` cleanup path) walk `prev_frame` from `current_frame` toward the home frame, running each frame's `cleanup_cl` (via the existing `Symbol_value` send — which itself pushes a frame and must complete before continuing the unwind), popping frames, until reaching the target home frame; then deliver the NLR value into the home frame's caller register and resume. **Recommended: keep the existing `unwindTo:value:` kernel method + `resumeWith:`/`executeCleanup` primitives, but drive them from the flat loop** — i.e. the `unwind_target_reached` flag becomes a loop-local unwind state that the `OP_RET`/send-return path honors by continuing to pop until `current_frame == unwind_target_frame`, rather than relying on nested C returns. This reuses the tested cleanup machinery while removing the C recursion. Detail this against the quoted `OP_NLR` + the send-return `unwind_target_reached` checks; the flat loop's single return path replaces the per-send-site checks.

4. **Do NOT add `OP_UNWIND` unless needed.** The recommended design reuses `OP_NLR` + the kernel `unwindTo:value:` + a loop-local unwind cursor, so no new opcode is required. Only introduce `OP_UNWIND = 0x17` (registering it in `oevm.h` + the compiler, both image paths) if driving the unwind purely in C proves to need a dedicated bytecode. Prefer no new opcode.

5. **Debugger hook + single-shot callers** — leave `run_interpreter(debugger_hook, ...)` (both hook sites + the post-step hook) as bounded nested calls. Leave `does_not_understand`'s bytecode-handler `run_interpreter` and `run_method_on`'s init `run_interpreter` as bounded nested calls (single-shot, never loop). The flat loop is the hot path; these are cold, bounded re-entries (F4).

- [ ] **Step 1: Implement `push_frame` + the four send-opcode frame-push (Task 4 Steps 3–N) + `OP_RET` rewrite together.** This is the atomic core. Build it as one change.

- [ ] **Step 2: Rewrite the unwind path** (OP_NLR + send-return unwind checks → loop-local unwind cursor honored by the single `OP_RET`/return path). Keep `unwindTo:value:`/`resumeWith:`/`executeCleanup` semantics.

- [ ] **Step 3: Full behavior-equivalence gate.** Run: `cd /home/david/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | tail -6` — expect 80/0/6 both paths + all 5 gates PASS. Then `OEVM_GC_STRESS=1` run + ASan run clean, and `gcc -fsyntax-only -D__ZEPHYR__ -Isrc src/vm.c`. The suite's NLR/`ensure:`/`ifCurtailed:`/deep-recursion/`45_fibers` tests are the oracle — every one must pass.

- [ ] **Step 4: Add a deep-recursion stress test** — a `.ns` test recursing ~50,000 deep (which the OLD recursive engine would stack-overflow the C stack on, but the flat engine handles within `VM_STACK_MAX_WORDS`). This is the positive proof the flip actually removed C recursion. Confirm it passes on the flat engine (and document that it would blow the C stack on the old one). If it exceeds `VM_STACK_MAX_WORDS`, size it to just under the limit.

- [ ] **Step 5: Commit** (oevm; trailer; message describing the flat loop + `OP_RET` src2-deliver + in-loop unwinder).

---

### Task 6: Simplify the fiber-switch return path for the flat loop (no demolition yet)

**Files:** Modify `oevm/src/vm.c`.

**Background:** With the flat loop, a fiber switch that returns into the middle of a method is handled by the loop's normal return/continue mechanics, not the per-send `#ifdef __ZEPHYR__` reload hijack. Phase 3 DELETES ucontext/hijack/registry entirely; this task only ensures the flat loop's fiber-switch return is correct and removes any now-dead reload code that the flat `OP_RET` subsumes. **Do NOT delete ucontext/swapcontext or the conservative C-stack scan here** — that is Phase 3 (it requires the swap-Stack `fiberSwitch:value:` rework).

- [ ] **Step 1:** Audit the four `#ifdef __ZEPHYR__` reload blocks. In the flat loop, a Zephyr fiber switch inside `primitive_fiber_switch` returns to the send site; the flat return machinery must deliver correctly. Determine (with the suite's `45_fibers` + fibers-gc-stress as oracle, built once with `-D__ZEPHYR__` semantics where feasible) whether the reload blocks are still needed or subsumed. Remove what the flat `OP_RET` makes dead; leave what Phase 3 will remove.
- [ ] **Step 2: Gate** — suite 80+/0/6 both paths + fibers-gc-stress + ASan + Zephyr-syntax-clean.
- [ ] **Step 3: Commit.**

---

### Task 7: Validation, docs, and final review

- [ ] **Step 1: Full regression** — suite both paths + all 5 gates + `OEVM_GC_STRESS=1` + ASan + `-D__ZEPHYR__` syntax. Add a Perfstex comparison note if available (the flip + Phase-1 lowering should not regress hot loops; the design predicts improvement).
- [ ] **Step 2: Confirm no C recursion remains on the hot send path** — `grep -n run_interpreter oevm/src/vm.c oevm/src/dispatch.c` and verify every remaining caller is one of the F4 sanctioned bounded/cold sites (debugger hook, `does_not_understand` bytecode handler, `run_method_on` init) or the top-level entry (`main`/fiber trampoline). NO caller on the `OP_SEND`/`OP_BLOCK_INVOKE` path.
- [ ] **Step 3: Update the design doc** — mark Phase 2 DONE in `oevm/docs/fiber_derecursify_core_design.md` §5; record what Phase 3 inherits (ucontext/hijack/registry/conservative-scan still present, to be demolished; swap-Stack `fiberSwitch:value:` is the Phase 3 mechanism).
- [ ] **Step 4: Dialect** — the flip is behavior-equivalent (no observable change), so NO new dialect row is expected. Confirm via green suite with unchanged expected outputs; if an unavoidable observable difference appears, escalate to David (per the standing rule) before recording.
- [ ] **Step 5: Final whole-branch review (opus)** — the flip is the highest-risk change in the whole rewrite; run a broad adversarial review focused on: `OP_RET` src2-decode correctness for all four send opcodes (esp. `OP_SEND_SUPER`'s `regs[c]` vs `src2`), unwind correctness across `ensure:`/`ifCurtailed:`/nested-NLR, `home_ctx`-absolute safety on fixed stacks, GC precision over the flat frames, and fiber-switch return correctness.

---

## Self-Review notes (author)

- **Coverage vs design §4/§7:** flat loop + `push_frame` (Task 5); `OP_RETURN` src2-deliver generalised from the hijack (Task 5 §2 — resolves §7 item 1); `OP_BLOCK_INVOKE` already exists (Phase 1); `OP_UNWIND` deliberately AVOIDED (Task 5 §4 — resolves §7 item 2 by reusing `unwindTo:value:`); both-paths opcode agreement (only if `OP_UNWIND` is added — §7 item 3); fiber-entry-block `home_ctx`-across-stacks lifetime (§7 item 4) — the `Concurrency` facade keeps the spawner frame live, and `home_ctx` stays absolute on fixed stacks, so it holds; confirm in Task 5 review. Retire `OP_SEND_IC` (Task 3). Delete ucontext/hijack/conservative-scan = Phase 3, NOT here.
- **New scope discovered (F1/F2), flagged for David:** `timesRepeat:` + unary `whileTrue`/`whileFalse` lowering (Tasks 1–2) and `OP_SEND_IC` retirement (Task 3) — pre-flip cleanups that complete Phase 1's mandate and shrink the flip surface.
- **Atomicity honesty (F3):** Tasks 4-loop + 5 land together (the send-frame-push is only correct with the matching `OP_RET`); gated by the comprehensive existing suite as the behavior oracle, plus a new deep-recursion positive-proof test.
- **Type/name consistency:** `push_frame(closure, receiver, argc, args)`, `resolve_send(receiver, selector, argc, args, *out_method, *out_is_primitive)`, `frame_link_encode`/`frame_link_decode` (existing), `emit_block_value` (Phase 1) used in Tasks 1–2.
