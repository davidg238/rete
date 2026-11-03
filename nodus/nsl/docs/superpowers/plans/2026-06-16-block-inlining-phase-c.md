# Block Inlining — Phase C (`and:` / `or:` short-circuit inlining) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Compile `and:` / `or:` with a literal zero-parameter block argument inline (short-circuit jumps + direct registers, no closure), reusing the Phase B `compile_inlined_block` machinery.

**Architecture:** Add a `compile_logic_inline` method to `MethodCompiler` (sibling of Phase B's `compile_if_inline`) and a detection hook in `compile_expr`'s `et == "send"` branch. The lowering matches the `PRIM_and`/`PRIM_or` semantics exactly: `and:` truthy → block value, falsy → the `false` symbol; `or:` truthy → the `true` symbol, falsy → block value. The branch decision uses `OP_JMP_IF_FALSE` (same `is_truthy` the primitive uses). Non-literal-block args (`cond and: aBlockVar`) keep the existing closure+send path.

**Tech Stack:** Python compiler (`ns-oevm/tools/ns_compile_oevm.py`); conformance harness (`ns-oevm/tools/run_tests_oevm.py`); the C VM already supports `OP_JMP`/`OP_JMP_IF_FALSE` and `PRIM_and`/`PRIM_or`.

**Design refs:** spec `ns-oevm/docs/design/2026-06-16-block-inlining-design.md` (§"Inlining transformation": "`cond and: [B]` → short-circuit … `or:` symmetric"); `oevm/docs/topics.md#block-inlining`.

## Global Constraints

- **Regression gate:** the **20 currently-passing** conformance tests must never regress. Baseline before this plan is `Passed: 20, Failed: 10, Skipped: 1`. After **Task 1** it is `Passed: 21, Failed: 10, Skipped: 1` (a new test that already passes via the primitives). After **Task 2** it stays `Passed: 21, Failed: 10, Skipped: 1` (the new test keeps passing, now via inlining). Gate command: `cd ~/workspace/ns-oevm && python3 tools/run_tests_oevm.py 2>&1 | tail -4`.
- **Both `and:` and `or:` are already primitives** (`PRIM_and`/`PRIM_or` in `oevm/src/dispatch.c`). Phase C is therefore a structural/perf change, not a new capability — there is **no** RED behavioral case. The new test is GREEN before and after; the proof that inlining engaged is the structural fixture in Task 2 (`createClosure:` count drops to 0).
- **This is NOT a byte-identical no-op.** Inlining changes codegen for every test using `and:`/`or:` with literal blocks; the Phase A snapshot gate does not apply.
- **Exact semantic equivalence (verified against `dispatch.c:500-512`):**
  - `and:` — condition truthy → value of the inlined block; condition falsy → the `false` symbol (load via `self.add_constant("false")`).
  - `or:` — condition truthy → the `true` symbol (load via `self.add_constant("true")`); condition falsy → value of the inlined block.
  - The fixed `true`/`false` symbols are returned regardless of receiver type, so the lowering matches even for non-boolean truthy/falsy receivers — do not return the receiver value.
- **Short-circuit is mandatory:** the block must only be evaluated on its live path (and:→truthy, or:→falsy). The jump topology guarantees this; the test's side-effect cases verify it.
- **Inline only a single literal zero-param block.** Lower inline only when `len(args) == 1 and args[0]["type"] == "block" and not args[0].get("params")`. Otherwise fall through to the closure+send path.
- **Reuse Phase B/A infrastructure as-is.** `compile_inlined_block`, `new_label`/`bind_label`/`emit_jump`, `alloc_reg`/`free_reg`/`next_free_reg`, `emit_ABx`/`add_constant` already exist. Do not re-implement; do not add `finalize_jumps()` calls.
- **Commit trailer (oevm repo only):**
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

---

## File Structure

- `ns-tests/04_somns_basic/10_and_or_inlining.ns` — **new**: regression + behavior test. Covers `and:`/`or:` truthy/falsy, short-circuit side-effect proofs, non-boolean block value, parenthesized chains, an `and:`-result feeding an inlined `ifTrue:ifFalse:`, and the non-literal fallback. GREEN before and after (it passes via the primitives today, via inlining after Task 2).
- `ns-oevm/tools/ns_compile_oevm.py` — **modify**: add `compile_logic_inline` to `MethodCompiler`; add the `and:`/`or:` detection hook in the `et == "send"` branch.
- `oevm/docs/optimizations.md` — **modify** (Task 3): record Phase C.

---

## Task 1: Add the and:/or: regression test

**Files:**
- Create: `ns-tests/04_somns_basic/10_and_or_inlining.ns`

**Why GREEN now:** `and:`/`or:` work via the primitives today, so every case passes immediately (verified empirically). All conditions use comparison receivers (`(1 < 2)` / `(1 > 2)`); chains are explicitly parenthesized because `a and: [b] and: [c]` would otherwise parse as the single selector `and:and:` (a runtime dnu).

- [ ] **Step 1: Write the test file**

Create `ns-tests/04_somns_basic/10_and_or_inlining.ns` with exactly:

```
(*
expect:
true
false
false
true
true
false
true
true
0
0
5
7
true
*)
class AndOrTest packageUsing: manifest = () (
public class AndOr = () (
	public testAndTrueTrue     = ( ^ (1 < 2) and: [ 3 > 2 ] )
	public testAndTrueFalse    = ( ^ (1 < 2) and: [ 3 < 2 ] )
	public testAndShortCircuit = ( ^ (1 > 2) and: [ 3 > 2 ] )
	public testOrTrue       = ( ^ (1 < 2) or: [ 3 < 2 ] )
	public testOrFalseTrue  = ( ^ (1 > 2) or: [ 3 > 2 ] )
	public testOrFalseFalse = ( ^ (1 > 2) or: [ 3 < 2 ] )
	public testAndChain = ( ^ ((1 < 2) and: [ 2 < 3 ]) and: [ 3 < 4 ] )
	public testOrChain  = ( ^ ((1 > 2) or: [ 2 > 3 ]) or: [ 3 < 4 ] )
	public testAndSideEffect = ( | x | x:: 0. (1 > 2) and: [ x:: 1. 3 < 2 ]. ^ x )
	public testOrSideEffect  = ( | x | x:: 0. (1 < 2) or: [ x:: 1. 3 < 2 ]. ^ x )
	public testAndValue  = ( ^ (1 < 2) and: [ 5 ] )
	public testAndThenIf = ( ^ ((1 < 2) and: [ 2 < 3 ]) ifTrue: [ 7 ] ifFalse: [ 8 ] )
	public testFallback  = ( | b | b:: [ 3 > 2 ]. ^ (1 < 2) and: b )
)
public main: platform args: args = (
	| t = AndOr new. |
	t testAndTrueTrue printString out.
	t testAndTrueFalse printString out.
	t testAndShortCircuit printString out.
	t testOrTrue printString out.
	t testOrFalseTrue printString out.
	t testOrFalseFalse printString out.
	t testAndChain printString out.
	t testOrChain printString out.
	t testAndSideEffect printString out.
	t testOrSideEffect printString out.
	t testAndValue printString out.
	t testAndThenIf printString out.
	t testFallback printString out.
)
) : (
)
```

- [ ] **Step 2: Confirm the test passes (GREEN via primitives)**

Run:
```bash
cd ~/workspace/ns-oevm
python3 tools/run_tests_oevm.py 2>&1 | tail -4
```
Expected: `Passed: 21`, `Failed: 10`, `Skipped: 1` (the new test passes; the 20 prior passes are intact). If the new test fails, report the actual vs expected output — do not change the expected block to match a wrong value.

- [ ] **Step 3: Commit**

```bash
cd ~/workspace/ns-tests
git add 04_somns_basic/10_and_or_inlining.ns
git commit -m "test: add and:/or: short-circuit inlining test (green via primitives)"
```

---

## Task 2: Implement `and:` / `or:` short-circuit inlining

**Files:**
- Modify: `ns-oevm/tools/ns_compile_oevm.py`

**Interfaces consumed (all exist — do not redefine):** `compile_inlined_block(block, target_reg)`, `Scope`, `new_label`/`bind_label`/`emit_jump`, `alloc_reg`/`free_reg`/`next_free_reg`, `emit_ABx(OP_LD_CONST, reg, cidx)`, `add_constant`, `compile_expr`; opcodes `OP_JMP`/`OP_JMP_IF_FALSE`/`OP_LD_CONST`.

- [ ] **Step 1: Capture the structural RED baseline**

Run:
```bash
cd ~/workspace/oevm
printf '((1 < 2) and: [ 3 < 2 ]) println.\n((1 > 2) or: [ 3 > 2 ]) println.\n' > /tmp/aofix.ns
python3 ../ns-oevm/tools/ns_compile_oevm.py /tmp/aofix.ns /tmp/aofix_before.c >/dev/null 2>&1
echo "createClosure: $(grep -c createClosure /tmp/aofix_before.c)"
```
Expected (RED): `createClosure: 3` — the two `and:`/`or:` blocks compile to closures today.

- [ ] **Step 2: Add the `compile_logic_inline` helper**

Add this method to `MethodCompiler`, immediately after the existing `compile_if_inline` method:

```python
    def compile_logic_inline(self, selector, receiver, block, target_reg):
        # Short-circuit `and:` / `or:` with a literal zero-arg block, matching
        # PRIM_and / PRIM_or exactly (oevm/src/dispatch.c):
        #   and: truthy -> block value;     falsy -> the `false` symbol
        #   or:  truthy -> the `true` symbol; falsy -> block value
        # The condition test uses OP_JMP_IF_FALSE (is_truthy), same as the
        # primitive. See oevm/docs/topics.md#block-inlining.
        cond_reg = self.alloc_reg()
        self.compile_expr(receiver, cond_reg)
        l_false = self.new_label()
        l_end = self.new_label()
        self.emit_jump(OP_JMP_IF_FALSE, cond_reg, l_false)
        # condition truthy:
        if selector == "and:":
            self.compile_inlined_block(block, target_reg)
        else:  # "or:"
            self.emit_ABx(OP_LD_CONST, target_reg, self.add_constant("true"))
        self.emit_jump(OP_JMP, 0, l_end)
        # condition falsy:
        self.bind_label(l_false)
        if selector == "and:":
            self.emit_ABx(OP_LD_CONST, target_reg, self.add_constant("false"))
        else:  # "or:"
            self.compile_inlined_block(block, target_reg)
        self.bind_label(l_end)
        self.free_reg()  # cond_reg
```

- [ ] **Step 3: Add the detection hook in `compile_expr`**

In the `et == "send"` branch, immediately **after** the Phase B if-family detection
hook (the `if selector in ("ifTrue:", ...)` block that ends with `return`) and
before the `if receiver["type"] == "implicitReceiver":` block, insert:

```python
            # Phase C: short-circuit `and:` / `or:` with a literal zero-arg block.
            # Otherwise fall through to the closure+send path (e.g. `cond and: aBlockVar`).
            if (selector in ("and:", "or:")
                    and len(args) == 1 and args[0]["type"] == "block"
                    and not args[0].get("params")):
                self.compile_logic_inline(selector, receiver, args[0], target_reg)
                return
```

- [ ] **Step 4: Prove the structural GREEN**

Run:
```bash
cd ~/workspace/oevm
python3 ../ns-oevm/tools/ns_compile_oevm.py /tmp/aofix.ns /tmp/aofix_after.c >/dev/null 2>&1
echo "createClosure: $(grep -c createClosure /tmp/aofix_after.c)"
```
Expected (GREEN): `createClosure: 0`. If non-zero, the `and:`/`or:` blocks did not inline — debug the detection hook.

- [ ] **Step 5: Run the conformance gate (no regression, test now inlined)**

Run:
```bash
cd ~/workspace/ns-oevm
python3 tools/run_tests_oevm.py 2>&1 | tail -4
```
Expected: `Passed: 21`, `Failed: 10`, `Skipped: 1`. The `10_and_or_inlining` test still passes (now via inlining), the `09_if_inlining` test still passes, and **every one of the 20 previously-passing tests still passes**. If any regresses, the inlined lowering diverged from the primitive — stop and debug (do not edit existing tests' expected output). The short-circuit side-effect cases (`testAndSideEffect`/`testOrSideEffect` → `0`) are the key proof that the block is not evaluated on the dead path.

- [ ] **Step 6: Sanity-check fib is unaffected**

Run:
```bash
cd ~/workspace/ns-oevm
python3 tools/run_tests_oevm.py 2>&1 | grep -i fib || echo "(fib runs under perf; no and:/or: in it — unaffected)"
```
fib uses no `and:`/`or:`, so this phase does not change it; the conformance result above already covers correctness.

- [ ] **Step 7: Commit**

```bash
cd ~/workspace/ns-oevm
git add tools/ns_compile_oevm.py
git commit -m "compiler: inline and:/or: short-circuit (literal block args)"
```

---

## Task 3: Record Phase C in the optimization log

**Files:**
- Modify: `oevm/docs/optimizations.md`

- [ ] **Step 1: Append a Phase C entry**

Under the "Opt #3 — structural fix: block inlining" section, immediately after the
"### Phase B — if-family inlining" entry, add:

```markdown
### Phase C — and:/or: short-circuit inlining

`and:` / `or:` with a literal zero-arg block now compile inline via
`compile_logic_inline` (the Phase B `compile_inlined_block` machinery + an
`OP_JMP_IF_FALSE` short-circuit), matching `PRIM_and`/`PRIM_or` exactly: `and:`
truthy → block value / falsy → `false` symbol; `or:` truthy → `true` symbol /
falsy → block value. Non-literal-block args (`cond and: aBlockVar`) keep the
closure+send path. A pure-`and:`/`or:` fixture dropped from `createClosure: 3`
to `0`. Conformance held at 20/29 passing (the new
`04_somns_basic/10_and_or_inlining.ns` regression test — short-circuit
side-effect proofs, chains, `and:`-result-into-inlined-if — passes via inlining;
no test regressed). fib is unaffected (no `and:`/`or:`). Next: Phase D (loops:
`whileTrue:`/`whileFalse:`/`to:do:`/`to:by:do:`).
```

(Note: the conformance "passing" count in this repo's log is tracked as 20/29 — the `10_and_or` test brings the harness file count to 21 passing of 31, but the project's headline `X/29` metric excludes the perf/added scaffolding tests; keep the prose's "20/29" wording consistent with the Phase B entry, and state the harness line `Passed: 21, Failed: 10, Skipped: 1` for precision.)

- [ ] **Step 2: Commit (oevm repo)**

```bash
cd ~/workspace/oevm
git add nodus/oevm/docs/optimizations.md
git commit -m "docs: record block-inlining Phase C (and:/or: short-circuit)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Roadmap — Phases D–E (separate plans)

- **Phase D — loops.** `whileTrue:`/`whileFalse:` (literal receiver block + literal arg block) and `to:do:`/`to:by:do:` (literal one-param block + induction variable). `to:do:` needs a block-**parameter** binding path in `compile_inlined_block` (today it only declares temps). Acceptance: queens/sieve speedup.
- **Phase E — refactor only**: extract the shared cond/branch-emit skeleton duplicated across Phases B–D (pure no-op cleanup). NOTE: the original goal here — "close `02_block_inlining`, target 20/29" — is moot; that file already passes (it needed the `Array>>do:` library method, since landed, not deeper inlining).

---

## Self-Review

- **Spec coverage:** implements the spec's "`cond and: [B]` → short-circuit … `or:` symmetric" lowering and the Phase C rollout item. Loops and the full `02_block_inlining` acceptance are correctly deferred to D–E.
- **Placeholder scan:** none — all code and expected outputs are concrete and empirically verified.
- **Type/name consistency:** `compile_logic_inline(selector, receiver, block, target_reg)` reuses the verified Phase B `compile_inlined_block` contract and the same Phase A jump/register APIs; the detection guard mirrors the if-family guard (`a.get("params")`). Register discipline is identical to `compile_if_inline` (`cond_reg` allocated above the caller's `target_reg`; single `free_reg()` balances it).
- **Equivalence:** `and:`/`or:` constant returns (`false`/`true` symbols) and the `is_truthy`-based branch were checked against `dispatch.c:500-512`; the test's side-effect cases lock the short-circuit property.
