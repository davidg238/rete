# ns-oevm — Stratify base-level execution off reflection

**Status:** ratified design (brainstorm w/ David, 2026-06-23). Next: writing-plans → SDD.
**Predecessor:** 3c-3b (Serial), merged + pushed (master: ns-oevm `5c6c4b5`, oevm `b7c3554`,
ns-tests `63eb4e2`; suite 63/0/3 both paths).
**Successor / enables:** the **mirror confinement** step
(`2026-06-23-ns-oevm-mirror-confinement-design.md`) — once base-level execution no longer
sends reflective selectors, the full reflection surface (incl. the stack-walk trio) becomes
confinable, and the remote/post-mortem vision in that spec's §11.3 is unblocked. Confinement
becomes a small follow-up once this lands.
**Specs cross-ref:** `2026-06-23-ns-oevm-mirror-confinement-design.md` (the confinement
follow-up + the Bracha–Ungar *Mirrors* alignment, esp. **§11** stratification principle and
§11.3 remote/post-mortem), `docs/NEWSPEAK_DIALECT.md` (divergence governance).

---

## 0. Scope decision (ratified 2026-06-23)

This step **de-entangles base-level execution from reflection** and nothing more. It does
**not** build the mirror, does **not** confine anything, and does **not** add a debugger.
It is a **pure lowering change**: the *same* program semantics, expressed with dedicated
bytecodes instead of reflective message-sends. The conformance suite is the spec — it must
stay **63/0/3 both paths** at every step.

**Why this is the real first move (discovered via the confinement probe):** ns-oevm
implements lexical closures, captured-variable access, and non-local return by **sending
reflective/introspection messages to contexts and closures** in compiler-generated and
hand-written kernel code. That is Bracha & Ungar's *stratification* violation (base level
built on the meta level) made concrete — and it blocks confinement (you cannot confine
`sender`/`home`/`regAt:` while the interpreter emits them) and blocks the remote/post-mortem
debugging story (which needs exactly those ops as confined, possibly-remote mirror
operations). Stratifying first removes the entanglement; confinement then falls out cheaply.

---

## 1. Goal & motivation

Replace compiler-/kernel-emitted **reflective message-sends for core execution** with
dedicated bytecodes the interpreter handles directly. Three payoffs, in priority order:

1. **Stratification (primary).** After this step, `home`, `regAt:`, `regAt:put:`,
   `createClosure:`, and `sender` are **no longer emitted by code generation** — they become
   pure introspection selectors with no base-level dependents. This is Bracha principle #2
   realised: the base level stops being built on the meta level.
2. **Unblocks confinement + remote + post-mortem (enabling).** With no base-level senders,
   the mirror step can confine the *entire* reflection surface (including the stack-walk
   trio `sender`/`home`/`regAt:`), and those can later become remote/post-mortem-capable
   mirror operations (`mirror senderOf: ctx`, `mirror reg: ctx at: i`) without protocol
   change — the §11.3 vision.
3. **Hot-path performance (expected, to be measured).** Block-`self`, captured-variable
   access, closure creation, and NLR are frequent. Each currently pays full message
   dispatch (selector interning via `selector_prim_id` + the `switch(pid)` in `dispatch.c`).
   A dedicated opcode replaces that with a C pointer-walk + array index. **Measured, not
   assumed** — see §7; the claim is retracted in the doc if benchmarks don't support it.

---

## 2. The entanglement (what must be de-entangled)

Every site below is compiler- or kernel-generated base-level execution implemented via a
reflective send. Located by quoted code (line numbers are orientation only):

| Mechanism | Today (reflective send) | Site |
|---|---|---|
| Block `self` | walk `home` ×depth, then `regAt: 0` | `tools/ns_compile_oevm.py:398-416` (`if "_block" in self.name:` … `home_idx = self.add_constant("home")` … `reg_at_idx = self.add_constant("regAt:")`) |
| Captured-var **read/write** | walk `home` ×depth, then `regAt:` / `regAt:put:` | `tools/ns_compile_oevm.py:601-617` (the `elif res[0] == "lexical":` branch; `home_idx = self.add_constant("home")` … `reg_at_put_idx = self.add_constant("regAt:put:")`) |
| Closure creation | `createClosure:` send | `tools/ns_compile_oevm.py:995-1003` (`cc_idx = self.add_constant("createClosure:")`) |
| NLR cleanup walk | `sender` send | `tools/solidify.py:283-309` (`MethodContextVtable >> unwindTo:value:`, constant `"sender"`, instruction `OP_SEND … ("sender")`) **and** `../oevm/src/kernel_boot.c:66-82` (`Method_unwindTo_value__insts`/`__consts`, `KSym_sender`) |

Note: the captured-var **read** path (`regAt:` with a non-zero index, in the `lexical`
resolution case) is the read sibling of the `regAt:put:` write shown at `:601-617`; the plan
locates it in the same resolution branch. `outer` (module access) and
`executeCleanup`/`resumeWith:` (NLR control) are **out of scope** — `outer` is base-level
module navigation (not in the mirror surface), and the NLR-control prims are base-level
continuation operations, not introspection (see §5).

---

## 3. Design — four dedicated opcodes

Current opcode set (`../oevm/src/oevm.h:221-241`): `LD_CONST, LD_REG, SEND, SEND_IC, RET,
NLR, ADD, SUB, MUL, DIV, JMP, JMP_IF_FALSE, LD_SLOT, ST_SLOT, NEW, SEND_IMPLICIT,
BREAK(0x10), SEND_SUPER(0x11)`. Instruction format is 4 bytes: `op | dest | src1 | src2`
(each 8 bits; decoded in `vm.c` as `op = inst & 0xFF; dest = (inst>>8)&0xFF; …`), with an
`ABx` form packing a 16-bit constant index (used by `OP_LD_CONST`). Ample room remains.

| New opcode | Form | Replaces | Interpreter semantics |
|---|---|---|---|
| `OP_LD_LEXICAL` | `dest, src1=depth, src2=idx` | block `self` (idx 0) + captured-var read | from current frame's closure, walk the home chain `depth` times (§4), then `regs[idx]` → `dest` |
| `OP_ST_LEXICAL` | `dest=value, src1=depth, src2=idx` | captured-var write | walk home chain `depth` times, set `regs[idx] = regs[value]` |
| `OP_CLOSURE` | `ABx: dest, method-const-idx` | `createClosure:` | allocate a Closure: `home_ctx = current_frame`, `entry_point = <method const>`, `home_generation = current_frame->gen_tag.tag32` (mirrors the existing `PRIM_createClosure` body in `dispatch.c`) → `dest` |
| `OP_LD_SENDER` | `dest, src1=ctx-reg` | the `sender` send in `unwindTo:value:` | `dest = ((MethodContext*)regs[src1])->prev_frame` (§5) |

`depth` and register indices comfortably fit 8 bits (block nesting and register counts are
small); the method index for `OP_CLOSURE` uses the 16-bit `ABx` form like `OP_LD_CONST`. The
new opcodes go after `OP_SEND_SUPER` in the enum; the `_block` self read becomes
`OP_LD_LEXICAL dest, depth, 0`.

### 3.1 The opcodes REUSE the prim bodies (identical by construction)

This is the core safety property, so it is a **design rule, not an implementation detail**:
each new opcode handler **calls the same C function the old message dispatched to** — it does
**not** reimplement the logic.

- `OP_LD_LEXICAL` calls `primitive_closure_home()` (in a `depth`-loop) then
  `primitive_method_context_reg_at()`.
- `OP_ST_LEXICAL` calls `primitive_closure_home()` then `primitive_method_context_reg_at_put()`.
- `OP_CLOSURE` calls a small helper extracted from the existing inline `PRIM_createClosure`
  case in `dispatch.c` (the one exception, since that logic is inline today, not a named
  function); the old case and the opcode both call the extracted helper.
- `OP_LD_SENDER` calls `primitive_method_context_sender()`.

Consequence: the *work* is byte-for-byte the same code as before — so the home-walk /
register-access / closure-capture / sender behavior is **identical by construction**. The
**only genuinely new logic is in the compiler** (computing `depth`/`idx`, choosing the
opcode), which is exactly where the differential testing in §7 is aimed. What is removed is
purely the *dispatch path*: selector interning (`selector_prim_id`), the `send()` prologue,
the `switch(pid)`, and args-array marshaling — which is also where the expected perf win
comes from (the work is unchanged; the overhead around it is gone).

The `primitive_*` C bodies are therefore **left in place** and remain reachable by explicit
message-send (e.g. a program that literally writes `ctx sender`) — this step only stops
*code generation* from emitting those sends. Removing the message-level reachability is the
separate confinement follow-up (§8); these same prim bodies become the mirror's shims there.

---

## 4. The closure-vs-context home subtlety (load-bearing)

The home-chain walk in `OP_LD_LEXICAL`/`OP_ST_LEXICAL` **must** replicate the step-3a
`primitive_closure_home` fix exactly: for a **MethodContext** the home is `home_ctx`
(MethodContext offset 3), **not** `prev_frame` (Closure offset 1). The original 3a bug read
the wrong field and broke doubly-nested-block `self` once `Array>>do:` added an interpreter
frame. The new opcodes walk the *same* corrected chain `primitive_closure_home` walks.

**Guards:** the existing regression `ns-tests/04_somns_basic/27_nested_block_self.ns`
(doubly-nested-block `self`) plus **deltablue** (the proof-of-necessity case for the 3a fix)
must stay green. The plan adds an explicit captured-variable read **and** write across ≥2
nesting levels if 27 doesn't already cover the write path.

---

## 5. NLR rework — surgical `OP_LD_SENDER` (ratified)

`unwindTo:value:` is a hand-written bytecode method present in **both** image paths
(`solidify.py:283-309` for Path A; `../oevm/src/kernel_boot.c:66-82` for Path B). It walks
the sender chain by sending `sender`, running `executeCleanup` at each frame until the target
frame (set by `resumeWith:` via the `unwind_target_reached`/`unwind_target_frame` flags in
`vm.c`), then resumes.

**Decision:** add `OP_LD_SENDER` and replace the single `sender` `OP_SEND` in *both* copies
with it, dropping `"sender"` from their constant tables. **`executeCleanup` and `resumeWith:`
stay as sends** — they are base-level NLR *control* operations, not introspection, and are
outside both the stratification target and the mirror surface. Rejected alternative: moving
the whole unwind loop into a C primitive — larger, touches more, and yields no additional
stratification (it would only relocate `executeCleanup`/`resumeWith:`, which we are keeping).

Both hand-written bytecode methods must be edited in parallel and their instruction counts /
constant arrays kept consistent (the `kernel_boot.c` struct hardcodes `instruction_count` and
`constant_count`). Path B parity is a first-class probe (see §9).

---

## 6. Both-path impact

- **Interpreter (`vm.c`)** gains the four opcodes — shared by both image paths, so Path A
  (solidified `.c`) and Path B (`.oimg`) execute them identically with no per-path work.
- **Compiler (`tools/ns_compile_oevm.py`)** emits the new opcodes instead of the reflective
  sends — one change, both paths consume it.
- **Hand-written kernel methods** (`unwindTo:value:`) edited in both `solidify.py` and
  `kernel_boot.c`.
- **No `.oimg` format-version bump:** bytecode *content* changes, not record *layout*. The
  serializer/loader are untouched. (Confirm in the plan that no serialized structure encodes
  the old selector set positionally.)

---

## 7. Verification — the central concern

The risk here is **not** a crash; it is a **silent divergence** between a new opcode and the
message it replaced, in a corner the suite never exercises (the 3a bug was exactly this:
wrong home field, surfaced only by doubly-nested `self` under an extra `do:` frame). The
standing suite is **necessary but not sufficient**. The decisive asset is that **the old
implementation survives** (§3.1) — so the pre-change behavior is still runnable and serves as
an **oracle**. The strategy has four layers, in leverage order.

### 7.1 Whole-suite differential (master vs branch) — the centerpiece

Build the runner from a `git worktree` at `master` (old reflective lowering) and from the
branch (new opcodes), run the **entire** corpus through both, and require **byte-for-byte
identical stdout** — not merely "both pass." Across all tests × both image paths × both
lowerings. This converts "did we preserve semantics" into a mechanical diff against the real
old code. (The worktree harness already exists — it was used for the oevm#8 32-bit repro.)
Run as a gate **in every opcode task**, not just at the end.

### 7.2 Targeted corner battery — the cases the suite probably misses

Output-diffing only catches a subtle bug if the bug is **observable in output**, so each test
is written to make internal state visible (print the captured/shared/returned value). New
`ns-tests` covering, each **also** re-run inside `Array>>do:` (the extra-interpreter-frame
trigger that exposed 3a):
- captured-var **read** at nesting depth ≥2; captured-var **write** at depth ≥2 then read back;
- **two closures capturing the same mutable variable** — one writes, the other observes
  (shared-environment aliasing — the classic capture bug);
- a closure that **escapes its home frame** (returned from a method, invoked later) — proves
  `home_ctx` survives, including across GC (§7.3);
- **NLR across multiple frames with `ensure:`/cleanup at intermediate frames** (cleanup
  ordering via `executeCleanup`); NLR from a deeply nested block;
- **recursion with per-activation captures** (each activation's capture distinct).

**Process rule (load-bearing):** this battery is written and confirmed **green on `master`
first** (Task 0), establishing it as a valid oracle, *before* any opcode work. A test that
only ever ran on the new code cannot prove the new code matches the old.

### 7.3 GC torture

Closures pin their home frame; a tracing/root bug stays latent until a collection happens
mid-capture (3a had a GC angle). Run 7.1–7.2 under an allocate-triggers-GC-often mode. If the
VM lacks such a knob, add a minimal `OEVM_GC_STRESS` env gate (collect on every N
allocations) — small, and independently useful. Gate the escaping-closure tests under it.

### 7.4 C-prim unit tests stay green

`../oevm/tests/test_debugger.c` exercises `regAt:`/`regAt:put:`/`sender` at the C level; the
prim bodies are unchanged (§3.1), so it remains an independent check that the prims (the
future mirror shims) still behave — orthogonal to the codegen change. Build + run it in the
final task.

### 7.5 Standing gates (unchanged, still required)

- **Suite 63/0/3 both paths at every task** (`python3 tools/run_tests_oevm.py`, `OEVM_PATH=AB`,
  from the **ns-oevm** cwd).
- **Grep-clean proof (the stratification deliverable):** after the change, no `add_constant`
  for `"home"`, `"regAt:"`, `"regAt:put:"`, `"createClosure:"` in `ns_compile_oevm.py`, and no
  `"sender"` constant in either `unwindTo:value:` copy. Pinned by a documented grep/assertion
  in the final task so the entanglement cannot silently return.
- **Benchmarks before/after** (`ns-tests/00_perf` — richards / deltablue / fib, via
  `run_benchmarks.py`): record both numbers; substantiate the §1.3 perf claim or **retract it
  honestly**. No silent claim either way.
- **Regression guards:** `27_nested_block_self.ns` + deltablue.

---

## 8. Relationship to mirror confinement (the follow-up)

This step makes the confinement spec (`2026-06-23-ns-oevm-mirror-confinement-design.md`)
**correct and complete**: its §3/§4 surface assumed all 11 reflective selectors were
movable, but the probe showed `sender`/`home`/`regAt:`/`regAt:put:` were base-entangled.
After this step they are not — so the confinement follow-up can confine the **full** surface
(no partial-confinement compromise), and §11.3's remote/post-mortem stack-walk becomes
reachable. The confinement spec will be updated to depend on this one; no design in it is
discarded, only re-sequenced.

---

## 9. Risks & open probes (front-loaded into the plan)

1. **Home-walk correctness (highest risk).** Getting the closure-vs-context home field wrong
   silently breaks nested-block `self`/capture (the 3a pattern). *Mitigation:* mirror
   `primitive_closure_home` exactly; 27 + deltablue + an added write-path test are the gate;
   build the opcode and its test before touching the compiler emit (TDD).
2. **Path B parity for the `unwindTo:value:` edit.** The two hand-written copies
   (`solidify.py`, `kernel_boot.c`) must change identically, including `instruction_count`/
   `constant_count`. *Mitigation:* edit both in the same task; run `OEVM_PATH=AB`; a Path B
   NLR test (deltablue runs both paths) is the gate.
2b. **Captured-var read site.** Confirm the `regAt:` *read* emit location in the `lexical`
   resolution branch (sibling of the `:613` write) before editing — locate by quoted code,
   not the line number.
3. **Encoding limits.** Verify `depth`/`idx` never exceed 8 bits in practice (deep nesting /
   high register counts). *Mitigation:* assert in the compiler emit; fall back to `ABx` if a
   field would overflow (unlikely; note it).
4. **Opcode dispatch placement.** New opcodes must be handled in the interpreter's main
   `switch(op)` and in any secondary decoders (e.g. a disassembler/`.stmap` consumer, the
   debugger single-step). *Mitigation:* grep for the existing `case OP_LD_REG:` sites and
   cover each.

---

## 10. Task decomposition (outline for writing-plans)

0. **Coverage audit + oracle battery + differential/GC harness (do FIRST, before any opcode).**
   Audit what closure/capture/NLR behavior the 63 tests actually exercise; write the §7.2
   corner battery + the `Array>>do:` re-runs; add the §7.3 `OEVM_GC_STRESS` knob if absent;
   stand up the §7.1 master-vs-branch differential runner (worktree at `master` + diff). **All
   new tests must pass on `master` (old code) — that is the gate that proves they are a valid
   oracle.** No opcode work in this task. Suite 63/0/3 (master) + new battery green on master.
1. **`OP_LD_LEXICAL` / `OP_ST_LEXICAL`** — add the opcodes; handlers **call**
   `primitive_closure_home` (loop) + `primitive_method_context_reg_at`/`_at_put` (§3.1, §4);
   switch the compiler's block-`self` (`:398-416`) and captured-var read/write
   (`:601-617`) emit to the new opcodes. TDD against the Task-0 battery. **Gates:** suite
   63/0/3 both paths + §7.1 differential identical + §7.3 GC-stress green.
2. **`OP_CLOSURE`** — extract the shared closure-capture helper from the inline
   `PRIM_createClosure` case; add the opcode (handler calls the helper); switch the compiler's
   closure-creation emit (`:995-1003`). **Gates:** suite 63/0/3 + differential identical + GC-stress.
3. **`OP_LD_SENDER` + NLR rewrite** — add the opcode (handler calls
   `primitive_method_context_sender`); rewrite the `sender` send out of *both* `unwindTo:value:`
   copies (`solidify.py` + `kernel_boot.c`), keeping `executeCleanup`/`resumeWith:`, fixing
   `instruction_count`/`constant_count`. **Gates:** suite 63/0/3 both paths + differential
   identical + the multi-frame-NLR-with-`ensure:` battery green (deltablue is the integration gate).
4. **Verify & document** — grep-clean assertion (no reflective emission left, §7.5); §7.4
   `test_debugger.c` built + run; benchmarks before/after recorded (substantiate-or-retract
   §1.3); `docs/NEWSPEAK_DIALECT.md` note (base-level execution stratified off the
   meta-protocol; dedicated lexical/closure/sender opcodes); regenerate committed default
   `solidified_image.c`. Final differential pass over the whole suite. Suite 63/0/3 both paths.
