# Block Inlining — Phase A (Foundation) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the scope/frame foundation (standalone `Scope` class, jump/backpatch helpers, VM truthiness fix) as a behaviour-preserving no-op, proven by a byte-identical bytecode diff and the unchanged 19/29 conformance gate.

**Architecture:** Introduce a standalone `Scope` (lexical bindings + frame link) and route variable resolution through it, keeping every block a frame so resolved frame-depths are unchanged. Add label/backpatch jump helpers (unused this phase) and fix `OP_JMP_IF_FALSE` to use `is_truthy` (an opcode no codegen emits yet). No inlining.

**Tech Stack:** Python compiler (`ns-oevm/tools/ns_compile_oevm.py`), C VM (`oevm/src/{vm.c,dispatch.c,oevm.h}`), conformance harness (`ns-oevm/tools/run_tests_oevm.py`).

**Design refs:** `oevm/docs/topics.md#scope-vs-frame`, `#why-standalone-scope-class`, `#no-op-first-rollout`, `#jmp-if-false-truthiness`; spec `ns-oevm/docs/design/2026-06-16-block-inlining-design.md`.

---

## File Structure

- `ns-oevm/tools/snapshot_images.py` — **new**: compiles every `.ns` test to a per-test C image into a snapshot dir; the no-op proof harness.
- `ns-oevm/tools/ns_compile_oevm.py` — **modify**: add `Scope` class; build a root scope per `MethodCompiler`; route `resolve_name` local/lexical through `Scope`; add `new_label`/`bind_label`/`emit_jump`/`finalize_jumps`.
- `oevm/src/dispatch.c` — **modify**: expose `is_truthy` (drop `static inline`).
- `oevm/src/oevm.h` — **modify**: declare `int is_truthy(oop obj);`.
- `oevm/src/vm.c` — **modify**: `OP_JMP_IF_FALSE` uses `is_truthy`.

---

## Task 1: No-op proof harness

**Files:**
- Create: `ns-oevm/tools/snapshot_images.py`

- [ ] **Step 1: Write the snapshot script**

```python
#!/usr/bin/env python3
"""Compile every ns-tests/**/*.ns to a per-test solidified C image under <outdir>.
Used to prove a refactor is a no-op: snapshot before, snapshot after, `diff -r`.
Compile failures are recorded as a stable marker so expect-compile-fail tests
stay comparable."""
import sys, subprocess, pathlib

HERE = pathlib.Path(__file__).resolve().parent
COMPILER = HERE / "ns_compile_oevm.py"
TESTS = HERE.parent.parent / "ns-tests"

def main():
    outdir = pathlib.Path(sys.argv[1])
    outdir.mkdir(parents=True, exist_ok=True)
    for ns in sorted(TESTS.glob("**/*.ns")):
        rel = ns.relative_to(TESTS)
        out = outdir / (str(rel).replace("/", "__") + ".c")
        r = subprocess.run(
            [sys.executable, str(COMPILER), str(ns), str(out)],
            capture_output=True, text=True,
        )
        if r.returncode != 0:
            out.write_text("COMPILE_FAIL\n")
    print(f"snapshot written to {outdir}")

if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Capture the pre-change baseline snapshot**

Run:
```bash
cd /home/david/workspace/ns-oevm
python3 tools/snapshot_images.py /tmp/img_before
```
Expected: `snapshot written to /tmp/img_before`, directory holds one `.c` per test.

- [ ] **Step 3: Commit the harness**

```bash
cd /home/david/workspace/ns-oevm
git add tools/snapshot_images.py
git commit -m "test: add solidified-image snapshot harness for no-op proofs"
```

---

## Task 2: VM — `OP_JMP_IF_FALSE` uses `is_truthy`

**Files:**
- Modify: `oevm/src/dispatch.c:31`
- Modify: `oevm/src/oevm.h`
- Modify: `oevm/src/vm.c:417-425`

- [ ] **Step 1: Expose `is_truthy` in dispatch.c**

Change the definition at `oevm/src/dispatch.c:31` from:
```c
static inline int is_truthy(oop obj) {
```
to:
```c
int is_truthy(oop obj) {
```

- [ ] **Step 2: Declare it in the header**

In `oevm/src/oevm.h`, near the other `extern oop send(...)` / dispatch prototypes, add:
```c
int is_truthy(oop obj);
```

- [ ] **Step 3: Use it in OP_JMP_IF_FALSE**

Replace the `OP_JMP_IF_FALSE` case body in `oevm/src/vm.c` (currently jumps on `cond == TAG_SMI(0) || cond == NULL`) with:
```c
            case OP_JMP_IF_FALSE: {
                oop cond = ctx->regs[dest];
                if (!is_truthy(cond)) {
                    int16_t offset = (int16_t)(inst >> 16);
                    pc += offset;
                }
                break;
            }
```

- [ ] **Step 4: Build the VM**

Run:
```bash
cd /home/david/workspace/oevm
gcc -Wall -Wextra -std=c99 -c src/dispatch.c -o /tmp/d.o && gcc -Wall -Wextra -std=c99 -c src/vm.c -o /tmp/v.o && echo BUILD_OK
```
Expected: `BUILD_OK` with no warnings.

- [ ] **Step 5: Run the conformance gate**

Run:
```bash
cd /home/david/workspace/ns-oevm
python3 tools/run_tests_oevm.py 2>&1 | tail -4
```
Expected: `Passed: 19`, `Failed: 10`, `Skipped: 1` (unchanged — the opcode is not emitted by codegen yet).

- [ ] **Step 6: Commit (oevm repo)**

```bash
cd /home/david/workspace/oevm
git add src/dispatch.c src/oevm.h src/vm.c
git commit -m "oevm: OP_JMP_IF_FALSE uses is_truthy (fixes false-symbol truthiness)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Add the `Scope` class

**Files:**
- Modify: `ns-oevm/tools/ns_compile_oevm.py` (add class above `MethodCompiler`)

- [ ] **Step 1: Insert the `Scope` class**

Immediately above the `MethodCompiler` class definition (the `class` whose
`__init__` is at line ~26), add:
```python
class Scope:
    """A lexical scope: name -> register bindings, plus the frame that owns
    those registers. Resolution walks enclosing scopes and counts how many real
    frame boundaries are crossed. See oevm/docs/topics.md#why-standalone-scope-class."""

    def __init__(self, frame, parent):
        self.frame = frame        # owning MethodCompiler (where regs live)
        self.parent = parent      # enclosing Scope, or None
        self.vars = {}            # name -> register index within self.frame

    def declare(self, name, reg):
        self.vars[name] = reg

    def resolve(self, name):
        """Return (reg, frame_depth) or None. frame_depth counts real frame
        boundaries crossed from this scope to the defining scope (0 = same frame)."""
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

- [ ] **Step 2: Syntax-check the module imports cleanly**

Run:
```bash
cd /home/david/workspace/ns-oevm
python3 -c "import importlib.util,sys; spec=importlib.util.spec_from_file_location('c','tools/ns_compile_oevm.py'); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m); print('IMPORT_OK')"
```
Expected: `IMPORT_OK` (module parses and the `Scope` class loads).

- [ ] **Step 3: Commit**

```bash
cd /home/david/workspace/ns-oevm
git add tools/ns_compile_oevm.py
git commit -m "compiler: add standalone Scope class (unused)"
```

---

## Task 4: Build a root scope per frame and route resolution through it

**Files:**
- Modify: `ns-oevm/tools/ns_compile_oevm.py` — `MethodCompiler.__init__` (~26-46), `resolve_name` (~85-116)

**Why a no-op:** every block is still its own frame, so a frame's root scope has
exactly one frame between it and each enclosing frame — identical to today's
`outer`-hop counting. `self` is intentionally **not** declared in block root
scopes, so `resolve("self")` walks to the nearest method frame and returns a
`frame_depth > 0` → `("lexical", 0, depth)`, exactly matching the current
special-cased behaviour. `local_vars` is left intact (it is also read at line
713) and simply mirrors the root scope.

- [ ] **Step 1: Create the root scope in `__init__`**

At the end of `MethodCompiler.__init__` (after `self.instructions = []`), add:
```python
        # Lexical scope (see oevm/docs/topics.md#scope-vs-frame). Phase A: one
        # root scope per frame, mirroring local_vars. `self` lives only in
        # non-block frames so it resolves lexically (via home/regAt:) inside
        # blocks, matching prior behaviour.
        is_block = "_block" in name
        parent_scope = outer_compiler.current_scope if outer_compiler is not None else None
        self.root_scope = Scope(self, parent_scope)
        self.current_scope = self.root_scope
        if not is_block:
            self.root_scope.declare("self", 0)
        for i, arg in enumerate(args):
            self.root_scope.declare(arg, i + 1)
        for i, temp in enumerate(temps):
            self.root_scope.declare(temp, self.first_temp_reg + i)
```

- [ ] **Step 2: Route `resolve_name` local/lexical through the scope**

Replace the first two resolution blocks of `resolve_name` (the `# 1. Local
variable` block and the `# 2. Lexical scope variable...` `while` loop, lines
~86-100) with:
```python
        # 1+2. Lexical resolution via the scope chain (this frame's scopes,
        # then enclosing frames). frame_depth 0 == same frame == a register.
        found = self.current_scope.resolve(name)
        if found is not None:
            reg, frame_depth = found
            if frame_depth == 0:
                return ("local", reg)
            return ("lexical", reg, frame_depth)
```
Leave the slot / nested_class / implicit_send blocks that follow unchanged.

- [ ] **Step 3: Take the post-change snapshot**

Run:
```bash
cd /home/david/workspace/ns-oevm
python3 tools/snapshot_images.py /tmp/img_after
```
Expected: `snapshot written to /tmp/img_after`.

- [ ] **Step 4: Prove the no-op (byte-identical diff)**

Run:
```bash
diff -r /tmp/img_before /tmp/img_after && echo NO_OP_CONFIRMED
```
Expected: `NO_OP_CONFIRMED` (no diff output). If anything differs, the
resolution refactor changed codegen — stop and reconcile before continuing.

- [ ] **Step 5: Run the conformance gate**

Run:
```bash
cd /home/david/workspace/ns-oevm
python3 tools/run_tests_oevm.py 2>&1 | tail -4
```
Expected: `Passed: 19`, `Failed: 10`, `Skipped: 1`.

- [ ] **Step 6: Commit**

```bash
cd /home/david/workspace/ns-oevm
git add tools/ns_compile_oevm.py
git commit -m "compiler: resolve variables via Scope (no-op; byte-identical images)"
```

---

## Task 5: Label / backpatch jump helpers (unused this phase)

**Files:**
- Modify: `ns-oevm/tools/ns_compile_oevm.py` — `MethodCompiler.__init__`, add methods; wire `finalize_jumps()` at body-completion sites.

**Why a no-op:** no caller emits jumps in Phase A, so `_pending_jumps` is empty
and `finalize_jumps()` does nothing → images stay byte-identical. These helpers
exist so Phase B can emit conditional control flow.

- [ ] **Step 1: Initialise jump state in `__init__`**

At the end of `MethodCompiler.__init__`, add:
```python
        self._label_counter = 0
        self._label_targets = {}     # label id -> instruction index
        self._pending_jumps = []     # (placeholder_index, op, cond_reg, label)
```

- [ ] **Step 2: Add the helper methods**

Add these methods to `MethodCompiler` (next to `emit_*`):
```python
    def new_label(self):
        self._label_counter += 1
        return self._label_counter

    def bind_label(self, label):
        self._label_targets[label] = len(self.instructions)

    def emit_jump(self, op, cond_reg, label):
        # Reserve a slot; real offset is patched in finalize_jumps().
        idx = len(self.instructions)
        self.instructions.append(None)
        self._pending_jumps.append((idx, op, cond_reg, label))

    def finalize_jumps(self):
        # Offsets are relative to the instruction AFTER the jump, because the
        # interpreter does pc++ before decoding (see oevm/src/vm.c OP_JMP).
        for idx, op, cond_reg, label in self._pending_jumps:
            target = self._label_targets[label]
            offset = target - (idx + 1)
            if op == OP_JMP:
                inst = ((offset & 0xFFFF) << 8) | (op & 0xFF)
            elif op == OP_JMP_IF_FALSE:
                inst = ((offset & 0xFFFF) << 16) | ((cond_reg & 0xFF) << 8) | (op & 0xFF)
            else:
                raise ValueError(f"finalize_jumps: unexpected op {op}")
            self.instructions[idx] = inst
        self._pending_jumps = []
```

- [ ] **Step 3: Call `finalize_jumps()` at body-completion sites**

There are two places a frame's body finishes being emitted. Find them and add a
`finalize_jumps()` call after the last statement is compiled:
1. **Block bodies** — in `compile_expr`, `et == "block"` branch, immediately
   after the block body has been compiled into `blk_compiler` (after the
   `if/else` that emits the final `OP_RET`, before
   `self.parent.block_methods.append(blk_compiler)`):
   ```python
            blk_compiler.finalize_jumps()
   ```
2. **Method bodies** — locate where the driver compiles a method's statements
   (grep: `grep -n "compile_statement" tools/ns_compile_oevm.py`). After the
   loop that compiles a method's statements completes, add:
   ```python
            compiler.finalize_jumps()
   ```
   (Use the actual compiler variable name at that site.)

- [ ] **Step 4: Re-snapshot and prove no-op**

Run:
```bash
cd /home/david/workspace/ns-oevm
python3 tools/snapshot_images.py /tmp/img_after2
diff -r /tmp/img_before /tmp/img_after2 && echo NO_OP_CONFIRMED
```
Expected: `NO_OP_CONFIRMED`.

- [ ] **Step 5: Run the conformance gate**

Run:
```bash
cd /home/david/workspace/ns-oevm
python3 tools/run_tests_oevm.py 2>&1 | tail -4
```
Expected: `Passed: 19`, `Failed: 10`, `Skipped: 1`.

- [ ] **Step 6: Commit**

```bash
cd /home/david/workspace/ns-oevm
git add tools/ns_compile_oevm.py
git commit -m "compiler: add label/backpatch jump helpers (unused; no-op)"
```

---

## Task 6: Phase A wrap-up — record in the optimization log

**Files:**
- Modify: `oevm/docs/optimizations.md`

- [ ] **Step 1: Append a Phase A entry**

Under the "Opt #3 — structural fix: block inlining (in progress)" section, add:
```markdown
### Phase A — foundation landed (no-op)

Scope/frame model (`Scope` class), label/backpatch jump helpers, and the
`OP_JMP_IF_FALSE` truthiness fix landed as a behaviour-preserving no-op: every
`.ns` test produced a byte-identical `solidified_image.c` before/after (proved
with `ns-oevm/tools/snapshot_images.py` + `diff -r`), conformance unchanged at
19/29. No perf change expected yet (no inlining). Next: Phase B (if-family
inlining).
```

- [ ] **Step 2: Commit (oevm repo)**

```bash
cd /home/david/workspace/oevm
git add nodus/oevm/docs/optimizations.md
git commit -m "docs: record block-inlining Phase A (foundation, no-op)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Roadmap — Phases B–E (separate plans)

Each phase gets its own bite-sized plan once the prior phase's concrete code is
in place (the exact emit code depends on the landed `Scope`/jump APIs). All keep
the gate ≥19 with no passing test regressing, measure fib + queens/sieve via
`oevm/bench.sh`, and log results in `oevm/docs/optimizations.md`.

- **Phase B — if-family inlining.** In `compile_expr` `et == "send"`, detect
  `ifTrue:` / `ifFalse:` / `ifTrue:ifFalse:` / `ifFalse:ifTrue:` with literal
  block args; push a child `Scope` on the current frame for each block, compile
  bodies inline with `emit_jump`/`bind_label`, reclaiming the block's registers
  on scope pop. Not-taken value = `false` symbol (matches the primitive).
  Acceptance: fib speedup, `02_block_inlining` partial progress, 19/29 held.
- **Phase C — `and:` / `or:`** short-circuit inlining (same machinery).
- **Phase D — loops** (`whileTrue:` / `whileFalse:` with literal receiver+arg;
  `to:do:` / `to:by:do:` with literal block + induction variable). Acceptance:
  queens/sieve speedup.
- **Phase E — refactor only**: extract the shared cond/branch-emit skeleton
  duplicated across Phases B–D (pure no-op cleanup). NOTE: the original goal
  here — "close `02_block_inlining`, target 20/29" — is moot; that file already
  passes (it needed the `Array>>do:` library method, since landed, not deeper
  inlining).

---

## Self-Review

- **Spec coverage:** Phase A covers the spec's "Supporting infrastructure"
  (Scope, labels/backpatch, VM fix) and the "Phase A — Foundation" rollout item,
  including the mechanical byte-diff exit criterion. Inlining transformation and
  Phases B–E are intentionally deferred to their own plans (roadmap above).
- **Placeholder scan:** none — every code step shows the exact code; the only
  "locate" step (finalize_jumps method-body site) gives the grep command and the
  exact line to insert, because that call site is driver-specific.
- **Type/name consistency:** `Scope(frame, parent)`, `.declare`, `.resolve →
  (reg, frame_depth)`, `current_scope`, `root_scope`, `new_label`/`bind_label`/
  `emit_jump`/`finalize_jumps`, `_pending_jumps`/`_label_targets`/
  `_label_counter`, and `is_truthy` are used consistently across tasks.
