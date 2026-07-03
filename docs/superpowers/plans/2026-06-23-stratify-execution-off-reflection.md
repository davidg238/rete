# Stratify Base-Level Execution Off Reflection — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace compiler-/kernel-emitted reflective message-sends for lexical-closure access, closure creation, and NLR with dedicated bytecodes the interpreter handles directly — a pure-lowering change that de-entangles base-level execution from reflection.

**Architecture:** Add four opcodes (`OP_LD_LEXICAL`, `OP_ST_LEXICAL`, `OP_CLOSURE`, `OP_LD_SENDER`). Each opcode handler **calls the same C prim body the old message dispatched to** (identical by construction; only the dispatch path — selector interning + `send()` + `switch(pid)` — is removed). The compiler emits the new opcodes instead of `home`/`regAt:`/`regAt:put:`/`createClosure:` sends; the two hand-written `unwindTo:value:` bytecode methods drop their `sender` send. Correctness is proven by **differential testing against `master` (the surviving old implementation) as an oracle.**

**Tech Stack:** C99 VM (`../oevm/src`), Python compiler/serializer (`ns-oevm/tools`), Newspeak conformance tests (`../ns-tests`). Three sibling repos: `ns-oevm` (compiler/lib/docs), `../oevm` (C VM), `../ns-tests` (tests).

## Global Constraints

- **Suite must stay 63 pass / 0 fail / 3 skip on BOTH image paths at every task.** Run from the **ns-oevm** cwd: `OEVM_PATH=AB python3 tools/run_tests_oevm.py`. NEVER run from `../oevm` (picks a stale harness).
- **Reuse, don't reimplement:** every opcode handler calls the existing `primitive_*` C body (or a helper extracted from one). No new copy of home-walk / reg-access / capture / sender logic.
- **Differential oracle:** correctness = byte-for-byte identical stdout between a `master` worktree (old lowering) and the branch (new opcodes), across the whole corpus, both paths. This is a gate in every opcode task.
- **Locate edit sites by the quoted code shown, not line numbers** — line numbers drift; the quoted snippets are authoritative.
- **Opcode numbering must agree** between the C enum (`../oevm/src/oevm.h`) and the compiler constants (`ns-oevm/tools/ns_compile_oevm.py`): `OP_LD_LEXICAL=0x12`, `OP_ST_LEXICAL=0x13`, `OP_CLOSURE=0x14`, `OP_LD_SENDER=0x15`.
- **Instruction packing:** `inst = (c<<24)|(b<<16)|(a<<8)|op` (`emit_ABC(op,a,b,c)`); 16-bit form `inst = (bx<<16)|(a<<8)|op` (`emit_ABx(op,a,bx)`). Decoded as `op=inst&0xFF; dest=(inst>>8)&0xFF; src1=(inst>>16)&0xFF; src2=(inst>>24)&0xFF`.
- **Commit per repo**, with the trailers this project requires. **Do not push** — David pushes on explicit say-so.
- Feature branches `feat/stratify-execution` in all three repos before Task 1.
- `ns-oevm/grammar/node_modules` (ohm-js) must exist to run the compiler (symlink into fresh worktrees).

---

### Task 0: Coverage audit + oracle battery + differential/GC harness

**No opcode work.** Establish the safety net and prove it valid on `master` BEFORE any lowering change. Nothing here changes VM or compiler behavior.

**Files:**
- Create: `../ns-tests/04_somns_basic/41_capture_read.ns`, `42_capture_write_shared.ns`, `43_closure_escape.ns`, `44_nlr_ensure_multiframe.ns`, `45_capture_recursion.ns`
- Create: `ns-oevm/tools/diff_lowering.sh` (master-vs-branch differential runner)
- Modify: `../oevm/src/gc.c` (add `OEVM_GC_STRESS` knob — only if absent)

**Interfaces:**
- Produces: a corner-case battery + a differential harness that Tasks 1–4 use as gates. No code symbols consumed downstream.

- [ ] **Step 1: Audit existing closure/NLR coverage**

Run and read, to know what the 63 tests already exercise:
```bash
cd /home/david/workspace/ns-tests && grep -rln '\[.*:.*\]\|whileTrue\|ensure:\|do:\|inject:into:' 04_somns_basic | sort
```
Write a 5-line note in the PR/commit body listing which corners (capture-write-shared, escaping closure, multi-frame NLR+ensure:) are NOT covered. This justifies the new tests.

- [ ] **Step 2: Write the corner battery (observable-state tests)**

Each prints the value that a home-walk/sender bug would corrupt, and each is also exercised inside `Array>>do:` (the 3a extra-frame trigger). `41_capture_read.ns`:
```newspeak
(* Captured-var READ at nesting depth >= 2, inside and outside do:.
expect: 7
expect: 7
*)
class Main usingPlatform: platform = () (
  public main: platform2 args: args = (
    | x |
    x := 7.
    [ [ x println ] value ] value.       (* depth-2 read *)
    (platform array: 1) do: [ :e | [ x println ] value ].  (* read under do: frame *)
    ^ 0
  )
) : ( public packageUsing: manifest = ( ^ self new ) )
```
`42_capture_write_shared.ns` (two closures share one mutable captured var):
```newspeak
(* Shared-environment aliasing: writer mutates, reader observes.
expect: 99
*)
class Main usingPlatform: platform = () (
  public main: platform2 args: args = (
    | x writer reader |
    x := 1.
    writer := [ x := 99 ].
    reader := [ x println ].
    writer value.
    reader value.
    ^ 0
  )
) : ( public packageUsing: manifest = ( ^ self new ) )
```
`43_closure_escape.ns` (closure outlives the method that made it):
```newspeak
(* Escaping closure: home frame must survive return + a later call + GC.
expect: 42
*)
class Main usingPlatform: platform = () (
  makeCounter = ( | n | n := 42. ^ [ n println ] )
  public main: platform2 args: args = (
    | blk |
    blk := self makeCounter.
    (platform array: 3) do: [ :e | e ].   (* churn allocations between capture and use *)
    blk value.
    ^ 0
  )
) : ( public packageUsing: manifest = ( ^ self new ) )
```
`44_nlr_ensure_multiframe.ns` (NLR across frames with intermediate cleanup):
```newspeak
(* Non-local return unwinding past an ensure: block, multiple frames.
expect: cleanup
expect: 5
*)
class Main usingPlatform: platform = () (
  doReturn = ( [ ^ 5 ] ensure: [ 'cleanup' println ]. ^ 0 )
  public main: platform2 args: args = (
    | r |
    r := self doReturn.
    r println.
    ^ 0
  )
) : ( public packageUsing: manifest = ( ^ self new ) )
```
`45_capture_recursion.ns` (per-activation captures distinct):
```newspeak
(* Each recursion level captures its own n; closures must not alias.
expect: 1
expect: 2
expect: 3
*)
class Main usingPlatform: platform = () (
  rec: k = (
    | n | n := k.
    k > 0 ifTrue: [ self rec: k - 1 ].
    [ n println ] value.
  )
  public main: platform2 args: args = ( self rec: 3. ^ 0 )
) : ( public packageUsing: manifest = ( ^ self new ) )
```
> NOTE: if the `ensure:`/`array:`/block-syntax forms above don't match this dialect, adjust to the forms used in existing tests (grep `04_somns_basic` for `ensure:`, `do:`, `value`). The REQUIREMENT is the *behavior* each test observes, not the exact surface syntax.

- [ ] **Step 3: Confirm the battery passes on master (the oracle gate)**

Run: `cd /home/david/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py`
Expected: **68 pass / 0 fail / 3 skip** both paths (63 baseline + 5 new). If any new test FAILS on master, the test is wrong (or describes behavior the old code doesn't have) — fix the test until green on master. A test that can't pass on the old code is not a valid oracle.

- [ ] **Step 4: Write the differential harness**

`ns-oevm/tools/diff_lowering.sh`:
```bash
#!/usr/bin/env bash
# Run the whole suite on a master worktree (old lowering) and the current branch
# (new opcodes); diff per-test stdout. Identical output == semantics preserved.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MASTER_WT="/tmp/stratify-master-oevm"
# oevm sibling worktree at master for the OLD VM:
git -C /home/david/workspace/oevm worktree add -f "$MASTER_WT" master 2>/dev/null || true
echo "Run suite on master oevm and branch oevm; compare /tmp dumps."
# Path A+B suite already compares against expected; the STRONGER check is that
# master and branch produce the SAME stdout for every test. Implementation:
# point run_tests at each oevm via env, capture, diff. (Filled in Step 5.)
```

- [ ] **Step 5: Wire diff_lowering.sh to actually diff, and add the GC-stress knob**

In `run_tests_oevm.py` the oevm dir is resolved relative to the script; add an `OEVM_DIR` env override so `diff_lowering.sh` can run the same tests against `$MASTER_WT` and the branch, dumping each test's stdout to `/tmp/lower_master/` and `/tmp/lower_branch/`, then `diff -r`. Expected on master-vs-master: empty diff (sanity). For `OEVM_GC_STRESS`: in `../oevm/src/gc.c`, if no existing "collect frequently" hook, add `if (getenv("OEVM_GC_STRESS")) gc_collect();` at the top of `gc_allocate` (guarded, cheap when unset). Confirm suite still 68/0/3 under `OEVM_GC_STRESS=1` on master.

- [ ] **Step 6: Commit (all three repos as needed)**

```bash
cd /home/david/workspace/ns-tests && git add 04_somns_basic/4*.ns && git commit -m "test(stratify): capture/NLR corner battery (oracle, green on master)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_016R9ztgZMd1VS31fe1T8pHh"
cd /home/david/workspace/ns-oevm && git add tools/diff_lowering.sh && git commit -m "test(stratify): master-vs-branch differential harness + GC-stress knob

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_016R9ztgZMd1VS31fe1T8pHh"
# commit the gc.c knob in ../oevm if added
```

---

### Task 1: `OP_LD_LEXICAL` / `OP_ST_LEXICAL` (lexical environment access)

**Files:**
- Modify: `../oevm/src/oevm.h` (opcode enum — after `OP_SEND_SUPER = 0x11`)
- Modify: `../oevm/src/vm.c` (interpreter `switch(op)` — new cases near `case OP_LD_REG:`)
- Modify: `ns-oevm/tools/ns_compile_oevm.py` (opcode constants near `OP_LD_SLOT = 12`; emit at the `if "_block" in self.name:` block and the `elif res[0] == "lexical":` branch)

**Interfaces:**
- Consumes: `primitive_closure_home(oop,int,oop*)`, `primitive_method_context_reg_at(oop,int,oop*)`, `primitive_method_context_reg_at_put(oop,int,oop*)` (existing, `../oevm/src/vm.c`).
- Produces: opcodes `OP_LD_LEXICAL=0x12`, `OP_ST_LEXICAL=0x13` with semantics: start from `ctx->regs[0]` (the running receiver/closure), apply `primitive_closure_home` `depth` times, then read/write register `idx`.

- [ ] **Step 1: Add the opcode enum values (C)**

In `../oevm/src/oevm.h`, the enum ending `OP_BREAK = 0x10,` / `OP_SEND_SUPER = 0x11`. Add:
```c
    OP_SEND_SUPER = 0x11,
    OP_LD_LEXICAL = 0x12,   /* dest, src1=depth, src2=idx : lexical read  */
    OP_ST_LEXICAL = 0x13,   /* dest=value, src1=depth, src2=idx : lexical write */
```

- [ ] **Step 2: Add matching compiler constants (Python)**

In `ns-oevm/tools/ns_compile_oevm.py`, near `OP_LD_SLOT = 12` / `OP_NEW = 14`, add:
```python
OP_LD_LEXICAL = 0x12
OP_ST_LEXICAL = 0x13
```

- [ ] **Step 3: Implement the interpreter cases (reuse the prims)**

In `../oevm/src/vm.c`, alongside `case OP_LD_REG:` (which does `ctx->regs[dest] = ctx->regs[src1];`), add. Declare the prim prototypes if not visible (they are in `oevm.h`).
```c
            case OP_LD_LEXICAL: {
                /* Identical to the old `R0 home..home regAt: idx` send chain (§3.1):
                   start at the running receiver/closure, walk home `depth` times via
                   the SAME prim, then read register `idx`. */
                oop cur = ctx->regs[0];
                for (uint8_t d = 0; d < src1; d++) cur = primitive_closure_home(cur, 0, NULL);
                oop idx = TAG_SMI(src2);
                ctx->regs[dest] = primitive_method_context_reg_at(cur, 1, &idx);
                break;
            }
            case OP_ST_LEXICAL: {
                oop cur = ctx->regs[0];
                for (uint8_t d = 0; d < src1; d++) cur = primitive_closure_home(cur, 0, NULL);
                oop putargs[2] = { TAG_SMI(src2), ctx->regs[dest] };
                primitive_method_context_reg_at_put(cur, 2, putargs);
                break;
            }
```
> The home-walk MUST match `primitive_closure_home` (the 3a-correct chain). By calling that exact function, it does.

- [ ] **Step 4: Switch the block-`self` emit (compiler)**

In `ns_compile_oevm.py`, the `if "_block" in self.name:` block currently emits (quoted):
```python
            base_reg = self.alloc_reg()
            self.emit_ABC(OP_LD_REG, base_reg, 0, 0)
            home_idx = self.add_constant("home")
            for _ in range(outer_depth):
                self.emit_ABC(OP_SEND, 0, home_idx, base_reg)
            arg_reg = self.alloc_reg()
            c_idx_val = self.add_constant("SMI:0")
            self.emit_ABx(OP_LD_CONST, arg_reg, c_idx_val)
            reg_at_idx = self.add_constant("regAt:")
            self.emit_ABC(OP_SEND, 1, reg_at_idx, base_reg)
            self.free_reg() # free arg_reg
            return base_reg
```
Replace the body with a single lexical read of register 0 at `outer_depth`:
```python
            base_reg = self.alloc_reg()
            self.emit_ABC(OP_LD_LEXICAL, base_reg, outer_depth, 0)
            return base_reg
```

- [ ] **Step 5: Switch the captured-var read/write emit (compiler)**

In the `elif res[0] == "lexical":` branch (the write path, quoted):
```python
                rcvr_reg = self.alloc_reg()
                self.emit_ABC(OP_LD_REG, rcvr_reg, 0, 0)
                home_idx = self.add_constant("home")
                for _ in range(depth):
                    self.emit_ABC(OP_SEND, 0, home_idx, rcvr_reg)
                arg1_reg = self.alloc_reg()
                c_idx_val = self.add_constant(f"SMI:{reg_idx}")
                self.emit_ABx(OP_LD_CONST, arg1_reg, c_idx_val)
                arg2_reg = self.alloc_reg()
                self.emit_ABC(OP_LD_REG, arg2_reg, target_reg, 0)
                reg_at_put_idx = self.add_constant("regAt:put:")
                self.emit_ABC(OP_SEND, 2, reg_at_put_idx, rcvr_reg)
                self.free_reg()
                self.free_reg()
                self.free_reg()
```
Replace with:
```python
                self.emit_ABC(OP_ST_LEXICAL, target_reg, depth, reg_idx)
```
Then find the sibling **read** path in the same lexical-resolution area (the one emitting `add_constant("regAt:")` with a non-zero index for reading a captured var, distinct from the block-self read in Step 4). Replace its `OP_LD_REG`+`home`-loop+`regAt:` sequence with:
```python
                self.emit_ABC(OP_LD_LEXICAL, dest_reg, depth, reg_idx)
```
(use that branch's existing destination/`reg_idx`/`depth` variable names).

- [ ] **Step 6: Build + run the suite (both paths) + differential + GC-stress**

```bash
cd /home/david/workspace/ns-oevm
OEVM_PATH=AB python3 tools/run_tests_oevm.py            # expect 68/0/3
OEVM_GC_STRESS=1 OEVM_PATH=AB python3 tools/run_tests_oevm.py   # expect 68/0/3
bash tools/diff_lowering.sh                              # expect: empty diff
```
Expected: 68/0/3 both paths, all three runs; differential diff empty. If `41/42/43/45` regress, the lexical opcode diverges from the old chain — debug against `primitive_closure_home` (likely the `regs[0]` start point or the depth count).

- [ ] **Step 7: Commit (oevm + ns-oevm)**

```bash
cd /home/david/workspace/oevm && git add src/oevm.h src/vm.c && git commit -m "feat(vm): OP_LD_LEXICAL/OP_ST_LEXICAL — lexical access via opcode, reusing prims

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_016R9ztgZMd1VS31fe1T8pHh"
cd /home/david/workspace/ns-oevm && git add tools/ns_compile_oevm.py && git commit -m "feat(compiler): emit OP_LD_LEXICAL/OP_ST_LEXICAL for block self + captured vars

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_016R9ztgZMd1VS31fe1T8pHh"
```

---

### Task 2: `OP_CLOSURE` (closure creation)

**Files:**
- Modify: `../oevm/src/oevm.h` (enum: `OP_CLOSURE = 0x14`)
- Modify: `../oevm/src/dispatch.c` (extract a `make_closure` helper from the inline `PRIM_createClosure` case)
- Modify: `../oevm/src/vm.c` (interpreter `case OP_CLOSURE:`)
- Modify: `ns-oevm/tools/ns_compile_oevm.py` (constant `OP_CLOSURE = 0x14`; emit at the `cc_idx = self.add_constant("createClosure:")` site)

**Interfaces:**
- Consumes: the existing `PRIM_createClosure` body in `dispatch.c` (quoted in Step 1).
- Produces: opcode `OP_CLOSURE` (ABx form: `dest`, 16-bit method-const index) → a Closure whose `home_ctx = current_frame`.

- [ ] **Step 1: Extract the shared `make_closure` helper**

In `../oevm/src/dispatch.c`, the `case PRIM_createClosure:` body is (quoted):
```c
                oop cl = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
                ((struct Closure *)cl)->home_ctx = (oop)current_frame;
                ((struct Closure *)cl)->home_generation = current_frame ? current_frame->gen_tag.tag32 : 0;
                ((struct Closure *)cl)->entry_point = (void *)args[0];
                return cl;
```
Lift into a file-scope helper and call it from the case:
```c
oop make_closure(oop method) {
    extern struct MethodContext *current_frame;
    oop cl = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
    ((struct Closure *)cl)->home_ctx = (oop)current_frame;
    ((struct Closure *)cl)->home_generation = current_frame ? current_frame->gen_tag.tag32 : 0;
    ((struct Closure *)cl)->entry_point = (void *)method;
    return cl;
}
```
Declare `oop make_closure(oop method);` in `../oevm/src/oevm.h`. Make `case PRIM_createClosure:` `return make_closure(args[0]);`.

- [ ] **Step 2: Add enum + compiler constant**

`oevm.h`: `OP_CLOSURE = 0x14,` after `OP_ST_LEXICAL`. `ns_compile_oevm.py`: `OP_CLOSURE = 0x14`.

- [ ] **Step 3: Interpreter case (reuse the helper)**

In `../oevm/src/vm.c`, the method object is the constant at the ABx index:
```c
            case OP_CLOSURE: {
                uint16_t mconst = (inst >> 16) & 0xFFFF;
                ctx->regs[dest] = make_closure(cm_const_at(method, mconst));
                break;
            }
```
> Confirm the method-constant resolves the same value the old `createClosure:` arg did — in the old emit, `arg_reg` was loaded from `add_constant("METHOD:...")` then passed as the send arg. Here the const index is encoded directly.

- [ ] **Step 4: Switch the compiler emit**

In `ns_compile_oevm.py` the closure-creation site (quoted):
```python
            cidx = self.add_constant(f"METHOD:{blk_name}:{blk_selector}")
            rcvr_reg = self.alloc_reg()
            self.emit_ABC(OP_LD_REG, rcvr_reg, 0, 0)
            arg_reg = self.alloc_reg()
            self.emit_ABx(OP_LD_CONST, arg_reg, cidx)
            cc_idx = self.add_constant("createClosure:")
            self.emit_ABC(OP_SEND, 1, cc_idx, rcvr_reg)
            self.emit_ABC(OP_LD_REG, target_reg, rcvr_reg, 0)
            self.free_reg()
            self.free_reg()
```
Replace with:
```python
            cidx = self.add_constant(f"METHOD:{blk_name}:{blk_selector}")
            self.emit_ABx(OP_CLOSURE, target_reg, cidx)
```

- [ ] **Step 5: Build + suite + differential + GC-stress**

```bash
cd /home/david/workspace/ns-oevm
OEVM_PATH=AB python3 tools/run_tests_oevm.py             # 68/0/3
OEVM_GC_STRESS=1 OEVM_PATH=AB python3 tools/run_tests_oevm.py  # 68/0/3
bash tools/diff_lowering.sh                               # empty diff
```
Expected: 68/0/3 both paths; differential empty. Any closure-heavy test (richards/deltablue) is the canary.

- [ ] **Step 6: Commit (oevm + ns-oevm)** — analogous to Task 1 Step 7, messages `feat(vm): OP_CLOSURE — closure capture via opcode (shared make_closure helper)` and `feat(compiler): emit OP_CLOSURE for block creation`.

---

### Task 3: `OP_LD_SENDER` + NLR `sender`-removal (both image paths)

**Files:**
- Modify: `../oevm/src/oevm.h` (enum: `OP_LD_SENDER = 0x15`)
- Modify: `../oevm/src/vm.c` (interpreter `case OP_LD_SENDER:`)
- Modify: `ns-oevm/tools/solidify.py` (the `unwindTo:value:` method def — Path A)
- Modify: `../oevm/src/kernel_boot.c` (`Method_unwindTo_value__insts`/`__consts` — Path B)

**Interfaces:**
- Consumes: `primitive_method_context_sender(oop,int,oop*)`.
- Produces: opcode `OP_LD_SENDER` (`dest`, `src1=ctx-reg`) → `prev_frame` of the context in `regs[src1]`.

- [ ] **Step 1: Enum + interpreter case**

`oevm.h`: `OP_LD_SENDER = 0x15,`. `vm.c`:
```c
            case OP_LD_SENDER: {
                ctx->regs[dest] = primitive_method_context_sender(ctx->regs[src1], 0, NULL);
                break;
            }
```

- [ ] **Step 2: Rewrite Path A `unwindTo:value:` (solidify.py)**

In `tools/solidify.py` the method has `constants=["==", "executeCleanup", "sender", "resumeWith:", "unwindTo:value:", "SMI:0"]` and instruction 14 `0x03020002, # OP_SEND ... ("sender")`. Two coordinated edits:
1. Remove `"sender"` (const index 2) → `constants=["==", "executeCleanup", "resumeWith:", "unwindTo:value:", "SMI:0"]`. This shifts `resumeWith:` 3→2, `unwindTo:value:` 4→3, `SMI:0` 5→4.
2. Change instruction 14 from `OP_SEND` of the sender const to `OP_LD_SENDER`. The old `0x03020002` (OP_SEND dest=2 src1=2 src2=3 → `R2 = R3 sender`) becomes `OP_LD_SENDER dest=2 src1=3`: `inst = (3<<16)|(2<<8)|0x15 = 0x00031502`.
3. Decrement every const index ≥3 used by other instructions (the `resumeWith:`(was 3), `unwindTo:value:`(was 4), `SMI:0`(was 5) loads) by 1. Inspect each `OP_SEND`/`OP_LD_CONST` word and rewrite its `src1`/`bx` const field. **The differential NLR test (`44`) + deltablue are the gate that this renumber is correct.**

- [ ] **Step 3: Rewrite Path B `unwindTo:value:` (kernel_boot.c) identically**

`../oevm/src/kernel_boot.c` mirrors the same method (quoted):
```c
static const uint32_t Method_unwindTo_value__insts[] = {
    0x00000301, 0x00010401, 0x03000102, 0x0007030B, 0x00000301, 0x03010002,
    0x00000301, 0x00020401, 0x03030102, 0x00050300, 0x00000304, 0x00000301,
    0x03010002, 0x00000301, 0x03020002, 0x00010401, 0x00020501, 0x03040202,
    0x00000304
};
static const oop Method_unwindTo_value__consts[] = {
    (oop)&KSym__eq_eq, (oop)&KSym_executeCleanup, (oop)&KSym_sender,
    (oop)&KSym_resumeWith_, (oop)&KSym_unwindTo_value_, TAG_SMI(0)
};
static const struct CompiledMethod Method_unwindTo_value__compiled = {
    Method_unwindTo_value__insts, 19,
    6, 2,
    Method_unwindTo_value__consts, 6,
    0
};
```
Apply the SAME transform: drop `(oop)&KSym_sender` from `__consts` (now 5 entries → update `constant_count` `6`→`5`), change instruction index 14 `0x03020002` → `0x00031502` (OP_LD_SENDER), and decrement const-index fields of instructions referencing old indices ≥3. (`instruction_count` 19 is unchanged — same number of instructions.) The two methods (A and B) must end byte-identical in their instruction streams.

- [ ] **Step 4: Build + suite + differential (NLR is the focus)**

```bash
cd /home/david/workspace/ns-oevm
OEVM_PATH=AB python3 tools/run_tests_oevm.py     # 68/0/3 — 44_nlr + deltablue are the gates
bash tools/diff_lowering.sh                       # empty diff
```
Expected 68/0/3 both paths. If `44`/deltablue fail on ONE path only, the A/B bytecode diverged — re-diff the two `unwindTo:value:` instruction arrays.

- [ ] **Step 5: Commit (oevm + ns-oevm)** — `feat(vm): OP_LD_SENDER + drop sender send from unwindTo:value: (both paths)`.

---

### Task 4: Verify, measure, document

**Files:**
- Create: `ns-oevm/tools/check_no_reflective_emit.sh`
- Modify: `ns-oevm/docs/NEWSPEAK_DIALECT.md`
- Modify: `../oevm/src/solidified_image.c` (regenerated default image)

- [ ] **Step 1: Grep-clean assertion (the stratification deliverable)**

`ns-oevm/tools/check_no_reflective_emit.sh`:
```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
fail=0
for sel in '"home"' '"regAt:"' '"regAt:put:"' '"createClosure:"'; do
  if grep -n "add_constant($sel)" tools/ns_compile_oevm.py; then
    echo "FAIL: codegen still emits $sel"; fail=1
  fi
done
grep -n 'KSym_sender\|"sender"' ../oevm/src/kernel_boot.c tools/solidify.py && { echo "FAIL: sender still in unwindTo:value:"; fail=1; } || true
[ $fail -eq 0 ] && echo "OK: base-level execution emits no reflective sends"
exit $fail
```
Run: `bash tools/check_no_reflective_emit.sh` → expect `OK`.

- [ ] **Step 2: Benchmarks before/after**

```bash
cd /home/david/workspace/ns-tests && python3 run_benchmarks.py | tee /tmp/bench_branch.txt
# compare to a master-worktree run captured the same way
```
Record both numbers in the commit body. Substantiate the perf claim, or state plainly "no measurable change" — no silent claim.

- [ ] **Step 3: C-prim unit test still green**

```bash
cd /home/david/workspace/oevm
gcc -std=c99 -Isrc tests/test_debugger.c src/vm.c src/gc.c src/dispatch.c src/platform.c src/serial.c src/backend/posix/platform_posix.c src/backend/posix/serial_posix.c -o /tmp/test_debugger -lm && /tmp/test_debugger
```
Expected: test passes (prims unchanged). If the build needs more units, add per the linker errors.

- [ ] **Step 4: Dialect note**

Append to `ns-oevm/docs/NEWSPEAK_DIALECT.md`: base-level execution (block `self`, captured-variable access, closure creation, NLR sender-walk) is implemented by **dedicated bytecodes** (`OP_LD_LEXICAL`/`OP_ST_LEXICAL`/`OP_CLOSURE`/`OP_LD_SENDER`), not by reflective message-sends — stratifying the base level off the meta-protocol (Bracha principle #2). The introspection prims remain for the forthcoming mirror capability. Include the benchmark delta from Step 2.

- [ ] **Step 5: Regenerate the committed default image + final differential**

```bash
cd /home/david/workspace/ns-oevm
# regenerate solidified_image.c per the project's image-regen step, then:
OEVM_PATH=AB python3 tools/run_tests_oevm.py   # 68/0/3
bash tools/diff_lowering.sh                     # empty diff (final whole-suite proof)
git -C /home/david/workspace/oevm worktree remove /tmp/stratify-master-oevm --force
```

- [ ] **Step 6: Commit (all repos), clean up worktree**

Commit `check_no_reflective_emit.sh` + `NEWSPEAK_DIALECT.md` (ns-oevm) and the regenerated `solidified_image.c` (oevm) with the trailers. **Do not push.**

---

## Self-Review notes (for the executor)

- **Every opcode handler calls a prim** (`primitive_closure_home`/`reg_at`/`reg_at_put`/`sender`) or the extracted `make_closure` — if you find yourself writing a new home-walk loop or capture logic, stop: that breaks the identical-by-construction guarantee.
- **The differential diff is the real test.** 68/0/3 means "matches expected"; empty `diff_lowering.sh` means "matches the old implementation." Both are required; the second is what catches subtle divergence.
- **Task 3 is the dangerous one** (hand-bytecode renumbering in two places). Edit A and B together; the two instruction arrays must match; `44_nlr` + deltablue on `OEVM_PATH=AB` are the gate.
- **Order is fixed:** Task 0 (oracle, green on master) precedes all opcode work. Do not start Task 1 until the battery passes on master.
- **Encoding overflow (spec §9.3):** `depth`/`idx` are 8-bit operand fields. In Task 1, add an `assert(outer_depth < 256 and reg_idx < 256)` (compiler-side) so a pathological nesting/register count fails loudly instead of silently truncating. In practice both are tiny.
- **Secondary opcode decoders (spec §9.4):** before finishing Task 1, `grep -n "case OP_LD_REG\|case OP_SEND\b" ../oevm/src/*.c` to confirm the interpreter's main `switch(op)` is the ONLY place opcodes are decoded (single-step uses `fiber->single_step` at instruction granularity, and `.stmap` maps offsets not opcodes, so neither should need the new cases — but verify, don't assume).
