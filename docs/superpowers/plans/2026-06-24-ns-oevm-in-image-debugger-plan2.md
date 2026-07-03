# In-Image Debugger (Plan 2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the Plan-1 `Supervisor` stub into a working, agent-drivable in-image debugger: an HLL mirror facade, a `Debugger` module that sets breakpoints / single-steps / inspects via the confined `mirror` capability, and a `dbg:*` stdin/stdout line transport — usable today on x86-linux under `OEVM_DEBUG=1`, with the 69/0/3 suite byte-for-byte unchanged when the gate is off.

**Architecture:** A new `lib/debugger.ns` module (compiled in via the same `ast["body"].extend(...)` path as kernel/supervisor) holds `ObjectMirror`/`ContextMirror`/`Debugger`. The `Supervisor` (sole mirror holder) constructs a `Debugger`, registers a `.ns` hook block via `mirror onBreak:`, and — when `OEVM_DEBUG` is set — runs a `dbg:*` command loop over POSIX stdin/stdout before and during execution. Breakpoints are addressed by a **runtime methodId** (an index into a registry the `Debugger` builds by walking the app-entry vtable chain) plus a bytecode offset; a `dbg:methods` command lists `id selector vtableTag` so the driving agent can pick one. The compile-time `.dsym` sidecar and source-line translation are **Plan 3**, out of scope here.

**Tech Stack:** C99 VM (`oevm/`), Python solidifier + test runner (`ns-oevm/tools/`), Newspeak `.ns` library modules, pexpect for the integration harness.

## Global Constraints

- **Confinement invariant (do not weaken):** only `Supervisor` ever holds `mirror_object()`. The app source never names the mirror. All new mirror selectors are reachable ONLY via `mirror_object()` through the existing identity hook in `send()` (`oevm/src/dispatch.c:622`); `oevm/tools/check_no_ambient_reflection.sh` (or `check_no_reflective_emit.sh`) must still pass.
- **Gate-off byte-for-byte:** with `OEVM_DEBUG` unset the `Supervisor` must behave exactly as today (forward `entry main: plat args: args`). Registering the hook is allowed because with no breakpoints set and stepping off, `OP_BREAK` never appears and the hook never fires. Full suite stays **69/0/3 both paths**.
- **Run/verify from `ns-oevm/`:** `OEVM_PATH=AB python3 tools/run_tests_oevm.py`. The runner **clobbers** `../oevm/src/solidified_image.c` (+`.stmap`) every run — after any suite run, `git -C ../oevm checkout src/solidified_image.c src/solidified_image.stmap`.
- **C prim parity:** the `dispatch.c` enum (`~517-538`) and `PRIM_NAMES[]` (`~541-556`) must stay index-parallel (pid = index+1, see `selector_prim_id` `~569-582`). `prim_ids.h` mirrors the `PRIM_pf_*` values and must stay in sync.
- **Three sibling repos:** `ns-oevm/`, `../oevm/`, `../ns-tests/`. Commit per-repo with required trailers. **Push only on David's explicit say-so.** Subagents locate edits by quoted code, not line numbers.
- **No `.oimg` header change** and **no `.dsym` file** this cycle. Registry is runtime-only.

## Deviation from spec (read before starting)

The spec (`docs/superpowers/specs/2026-06-23-ns-oevm-in-image-debugger-design.md`) said the debugger would "reuse only existing mirror prims." Grounding in the code showed that is **not sufficient** for a real interactive debugger. This plan adds a small, deliberately confined C surface, all reachable only through `mirror_object()`:

1. **Hook-controllable stepping (vm.c mechanism).** The hook save/restore at `vm.c:175-187` and `:716-728` does `fib->single_step = was_stepping` *after* the hook, which **clobbers** any stepping the hook armed. Interactive `dbg:step`/`dbg:over`/`dbg:out`/continue-past-breakpoint all arm stepping *from inside the hook*. Fix: a `hook_step_request` channel that survives the restore (Task 1).
2. **Five new confined mirror selectors** (Tasks 2–3): `offsetOf:` (ctx→SMI bytecode offset), `method:instructionAt:` (read a bytecode word, to save before patching `OP_BREAK`), `selectorsOf:`/`methodsOf:` (a vtable's bindings as iterable kernel `Array`s — `bindingsOf:` returns a non-iterable raw `BoxedBinding` array), and `requestStep:` (set the *current* fiber's hook step request — avoids exposing the fiber oop).
3. **One new host I/O prim** (Task 4): `readLine` on the platform capability (stdin line read; the POSIX serial backend is a loopback FIFO, not stdin).

These are additive, read-mostly, and preserve confinement. This is dialect entry **#16**. Add a NEWSPEAK_DIALECT.md row for it. **If any reviewer reads this as reversing the "reuse only existing prims" intent, escalate to David rather than silently dropping a prim** (per [[escalate-design-reversals]]) — but the prims are load-bearing for the chosen feature set (real breakpoints + stepping), which David selected.

## STRUCTURAL CORRECTION (mid-execution, controller-verified 2026-06-24)

The original Tasks 5–9 assumed a separate top-level `lib/debugger.ns` whose classes the Supervisor names directly. **That does not work in this dialect:** top-level sibling classes are NOT in a shared bare-name namespace (the app never names `Kernel` — it receives the instance as a param). Verified against `ns-tests/04_somns_basic/{18_class_side_methods,07_super_sends,14_object_super}.ns`: classes are **nested** and referenced by name within their enclosing class; factories are `^ self new setX: y` (class-side, after `: ( ... )`); a nested class reaches a sibling/itself by bare name or `outer <Enclosing> <Name>`.

**Therefore — and matching the SPEC, which placed "Supervisor + Debugger + facade" all in `lib/supervisor.ns`:** `ObjectMirror`, `ContextMirror`, and `Debugger` are **nested classes INSIDE the `Supervisor` class in `lib/supervisor.ns`**. There is NO `lib/debugger.ns` and NO `ns_compile_oevm.py` `extend()` change (supervisor.ns already compiles in). Wherever Tasks 5–9 say "create/edit `lib/debugger.ns`" or "third `extend()`", read "add a nested class to `Supervisor` in `lib/supervisor.ns`". The Supervisor (sole mirror holder) passes `mirror` into each facade/Debugger instance. Recursive/sibling instantiation uses the proven `Name new: ...` / `outer Supervisor Name ...` idiom — the implementer verifies the exact resolution empirically and reports which form works.

## BREAKPOINT MECHANISM — C-ASSISTED (decided + measured with David, 2026-06-24)

The original plan (and spec) said breakpoints = patch `OP_BREAK` into the method via `method:instructionAt:put:` (COW from flash). **Task 7 discovered this CANNOT work on real images** (controller-verified in `oevm/src/vm.c`): Path A solidified methods are `static const` in `.rodata` (outside the registered `flash_rom` range, so the COW guard is skipped → SIGSEGV on write); Path B methods are in the blob (`is_in_blob` → clean abort). `test_debugger.c` only works because it builds methods on the **heap** with a **dedicated** `OP_BREAK` slot — it never patches over a real instruction. A second problem: `saved_pc = pc+1` would skip the displaced instruction even on a writable method.

**Decision (David): C-assisted step-based breakpoints.** No method mutation. A breakpoint is a `(method, offset)` entry in a **C-side table**; while the table is non-empty the VM arms the per-instruction check and matches `(current method, current offset)` against the table **in C**, firing the existing `.ns` `onBreak:` hook only on a match. This works on read-only images, and continue-past-breakpoint becomes trivial (no restore/re-arm/`saved_pc` dance — the instruction was never modified; the breakpoint simply re-fires next time that site is reached).

**Measured cost (controller, temp vm.c scaffold, `-O2`, x86-linux; reverted):** overhead applies ONLY while a breakpoint is armed under `OEVM_DEBUG` — normal/gate-off runs are byte-for-byte unchanged (the per-instruction check is gated on `bp_count > 0 || single_step`).

| benchmark | instrs | armed, 1 bp | armed, 8 bps (linear) |
| --- | --- | --- | --- |
| mandelbrot | 834 M | **1.42×** | 2.45× |
| richards | 10.9 M | **1.33×** | 1.96× |

So a realistic debug session (1–few breakpoints) costs **~1.3–1.5×** while armed; the rejected naive pure-`.ns`-hook-every-step approach was estimated ~50–150×. Patch-based breakpoints (faster, no global step) could return in a later plan with proper VM COW/rebind + `saved_pc` rewind.

**Re-measured on the SHIPPED build (2026-06-24, `-O2`, x86-linux; mandelbrot size 750, 834 M instrs):** baseline gate-off median **3.34 s** (3.32/3.42/3.34) vs armed-with-1-breakpoint-that-never-fires median **4.26 s** (4.28/4.24/4.26) = **1.28×** — *better* than the 1.42× scaffold estimate. Method: solidify mandelbrot → Path A binary, time `OEVM_DEBUG` unset vs `dbg:break 1 99999` (offset never matches, so `bp_count>0` arms the per-instruction C check across the *entire* run while the `.ns onBreak:` hook never fires — i.e. worst-case run-to-breakpoint with a full-program "distance"). Run-to-breakpoint is the only latency that matters; once you single-step, execution is human/agent-paced and speed is irrelevant. **1.28× is the worst case** because mandelbrot is a tight arithmetic loop — minimal work per bytecode — so the fixed per-instruction branch+compare is the *largest possible* fraction of each instruction. Orchestration workloads (sends, allocation, RPC, I/O — the ns-oevm-as-ZephyrOS-scripting-layer target) do far more per bytecode, so the same fixed check is a *smaller* fraction → run-to-breakpoint overhead trends toward ≈1.0×. Gate-off pays a single not-taken branch (~0) and `mirror.c` is link-omittable, so a shipped no-debugger firmware image pays nothing.

---

## File structure

| File | New/Mod | Responsibility |
| --- | --- | --- |
| `oevm/src/oevm.h` | Mod | Add `int hook_step_request;` to `struct Fiber`; prototypes for the 5 new prims. |
| `oevm/src/vm.c` | Mod | Hook-step mechanism (both hook sites + the `singleStep:` prim); implement the 5 new `primitive_*`. |
| `oevm/src/mirror.c` | Mod | Add 5 `strcmp` shims in `mirror_dispatch` routing reflectee→arg0 (or no-reflectee for `requestStep:`). |
| `oevm/src/platform_backend.h` | Mod | Declare `int platform_debug_readline(char *buf, int cap);`. |
| `oevm/src/backend/posix/platform_posix.c` | Mod | Implement it via `fgets(stdin)`. |
| `oevm/src/prim_ids.h` | Mod | `#define PRIM_pf_readLine 70`. |
| `oevm/src/dispatch.c` | Mod | Add `PRIM_pf_readLine` to enum + `"readLine"` to `PRIM_NAMES[]` (index-parallel). |
| `oevm/src/platform.c` | Mod | `case PRIM_pf_readLine:` → String or nil-at-EOF. |
| `oevm/tests/test_debugger.c` | Mod | Add C-level assertions for the new prims + the hook-armed-stepping fix (closes oevm#14 positive proof). |
| `ns-oevm/lib/kernel.ns` | Mod | `public readLine = ( ^ platform readLine )` forwarding to the platform cap. |
| `ns-oevm/lib/debugger.ns` | **New** | `ObjectMirror`, `ContextMirror`, `Debugger`. |
| `ns-oevm/lib/supervisor.ns` | Mod | Build `Debugger`, register hook, run command loop, launch. |
| `ns-oevm/tools/ns_compile_oevm.py` | Mod | Third `extend()` for `debugger.ns`. |
| `ns-tests/04_somns_basic/46_debug_target.ns` | **New** | Debuggable app with a known method/local; passes the normal suite gate-off. |
| `ns-oevm/tools/test_debugger_integration.py` | **New** | pexpect harness driving `dbg:*` on both paths. |
| `ns-oevm/tools/run_tests_oevm.py` | Mod | Gate `test_supervisor_grant.py` + the new integration test into the suite (ns-oevm#5). |
| `ns-oevm/tools/test_supervisor_grant.py` | Mod | Honor `OEVM_BACKEND`/`OEVM_EXTRA_CFLAGS`; derive grant argc (ns-oevm#5). |

---

## Task 1: Hook-controllable stepping (vm.c mechanism)

**Files:**
- Modify: `oevm/src/oevm.h` (`struct Fiber`, ~line 201-209)
- Modify: `oevm/src/vm.c` (OP_BREAK hook ~168-193; step hook ~709-731; `primitive_fiber_set_single_step` ~1040-1058)
- Test: `oevm/tests/test_debugger.c`

**Interfaces:**
- Produces: a `.ns`/C hook that calls `fiber:singleStep:` (or the later `requestStep:`) now sees its requested stepping state applied *after* the hook returns, instead of being reverted. Passive hooks (that don't touch stepping) keep today's behavior.

- [ ] **Step 1: Write the failing test.** In `oevm/tests/test_debugger.c`, add a test where the hook arms stepping *from inside itself* and assert stepping actually engages afterward. Add near the existing single-step test:

```c
/* TEST: a hook that arms stepping from inside itself must take effect. */
static int armed_step_count = 0;
static int arm_once = 0;
static oop arm_step_hook_c(oop receiver, int argc, oop *args) {
    (void)receiver; (void)argc;
    oop ctx = args[0];
    oop pc_val = send(mirror_object(), create_symbol("pcOf:"), 1, &ctx);
    const uint32_t *pc_ptr = (const uint32_t *)(((uintptr_t)pc_val) >> 1);
    struct MethodContext *mc = (struct MethodContext *)ctx;
    struct CompiledMethod *cm = (struct CompiledMethod *)((struct Closure *)mc->closure)->entry_point;
    uint32_t last = cm->instructions[(pc_ptr - cm->instructions) - 1];
    if ((last & 0xFF) == OP_BREAK && !arm_once) {
        arm_once = 1;                       /* on the breakpoint, request stepping */
        oop sa[] = { current_fiber, TAG_SMI(1) };
        send(mirror_object(), create_symbol("fiber:singleStep:"), 2, sa);
    } else if (arm_once) {
        armed_step_count++;                 /* these fire only if the arm survived */
    }
    return NULL;
}
```
Register it, patch one `OP_BREAK`, run, and `assert(armed_step_count > 0);`.

- [ ] **Step 2: Run it to confirm it FAILS.**

Run (from `oevm/`): `gcc -std=c99 -Isrc src/vm.c src/gc.c src/dispatch.c src/platform.c src/backend/posix/platform_posix.c src/serial.c src/backend/posix/serial_posix.c src/mirror.c tests/test_debugger.c tests/test_debugger_image.c -o /tmp/td -lm && /tmp/td`
Expected: FAIL — `armed_step_count` is 0 because `fib->single_step = was_stepping` reverted the arm.
(Confirm the exact source list against `run_tests_oevm.py`'s `run_c_unit_tests` lines; match it.)

- [ ] **Step 3: Add the request channel to `struct Fiber`.** In `oevm/src/oevm.h`, change:

```c
struct Fiber {
    struct Object base;
    oop *stack;
    size_t stack_capacity;
    struct MethodContext *current_context;
    size_t stack_ptr;
    oop status; // Runnable, Suspended, or Dead
    int single_step;
};
```
to add a field initialized to `-1` (none):
```c
    int single_step;
    int hook_step_request;   /* -1 = none; 0/1 = hook-requested post-hook stepping */
};
```
Find every `gc_allocate(... Fiber ...)` / fiber-init site (grep `single_step =` and fiber allocation) and set `hook_step_request = -1` there.

- [ ] **Step 4: Set a "in hook" flag and honor the request at both hook sites.** In `oevm/src/vm.c`, add a file-scope `static int in_debugger_hook = 0;` near `debugger_hook`. At the OP_BREAK site, replace:

```c
                int was_stepping = fib ? fib->single_step : 0;
                if (fib) fib->single_step = 0;
                
                oop dbg_arg = (oop)ctx;
                /* ... call hook ... */

                if (fib) fib->single_step = was_stepping;
                pc = ctx->saved_pc;
```
with:
```c
                int was_stepping = fib ? fib->single_step : 0;
                if (fib) { fib->single_step = 0; fib->hook_step_request = -1; }
                in_debugger_hook = 1;

                oop dbg_arg = (oop)ctx;
                /* ... call hook ... (unchanged) ... */

                in_debugger_hook = 0;
                if (fib) {
                    fib->single_step = (fib->hook_step_request >= 0)
                                     ? fib->hook_step_request : was_stepping;
                    fib->hook_step_request = -1;
                }
                pc = ctx->saved_pc;
```
Apply the identical transformation at the step-hook site (`vm.c:709-731`).

- [ ] **Step 5: Route `singleStep:` through the request channel when inside a hook.** In `primitive_fiber_set_single_step` (`vm.c:1040-1058`), after computing `step_val`:

```c
    extern int in_debugger_hook; /* or move the static to a shared decl */
    if (in_debugger_hook) {
        fib->hook_step_request = step_val;   /* applied after the hook returns */
    }
    fib->single_step = step_val;
    return receiver;
```
(Declare `in_debugger_hook` so the prim can see it — simplest is a non-static file global `int in_debugger_hook = 0;` with an `extern` in the prim, or keep both in vm.c.)

- [ ] **Step 6: Run the test — expect PASS**, and the existing TEST 1..N (breakpoint + top-level single-step) still pass (top-level stepping leaves `hook_step_request == -1`, so `was_stepping` is restored — backward compatible).

Run: `/tmp/td` → Expected: all asserts pass, `armed_step_count > 0`.

- [ ] **Step 7: Commit (oevm repo).**

```bash
git -C ../oevm add src/oevm.h src/vm.c tests/test_debugger.c
git -C ../oevm commit -m "feat(vm): hook-controllable single-step via Fiber.hook_step_request

Hook-armed stepping previously clobbered by the post-hook restore; an
interactive debugger arms stepping from inside the break hook. Adds a
request channel honored after the hook returns; passive hooks unchanged."
```

---

## Task 2: New read-only mirror prims — `offsetOf:`, `method:instructionAt:`, `selectorsOf:`, `methodsOf:`

**Files:**
- Modify: `oevm/src/vm.c` (new `primitive_*` near the other reflection prims ~745-1140)
- Modify: `oevm/src/mirror.c` (`mirror_dispatch` ~48-74)
- Modify: `oevm/src/oevm.h` (prototypes)
- Test: `oevm/tests/test_debugger.c`

**Interfaces:**
- Produces (mirror selectors, reflectee = arg0):
  - `mirror offsetOf: ctx` → `SMI` bytecode offset of `ctx`'s saved_pc within its method (`saved_pc - cm->instructions`), or `0` if no saved_pc.
  - `mirror method: cm instructionAt: idx` → `SMI` the bytecode word at `idx` (read; symmetric to existing `...put:`).
  - `mirror selectorsOf: vtable` → kernel `Array` of the vtable's binding selector `Symbol`s.
  - `mirror methodsOf: vtable` → kernel `Array` of the vtable's binding methods (parallel to `selectorsOf:`).

- [ ] **Step 1: Write the failing test.** In `test_debugger.c`, after patching an instruction, read it back and assert; and assert `offsetOf:` equals the known offset; and that `selectorsOf:` on the test vtable contains the bound selector. Sketch:

```c
/* read-back of a patched instruction */
oop ia[] = { cl_step, TAG_SMI(1) };
oop word = send(mirror_object(), create_symbol("method:instructionAt:"), 2, ia);
assert(IS_SMI(word) && (uint32_t)UNTAG_SMI(word) == 0x10 /* OP_BREAK we patched */);

/* offsetOf: in the break hook equals the breakpoint offset (capture via a global) */
assert(captured_offset == expected_offset);

/* selectorsOf: returns an iterable kernel Array containing our selector */
oop sels = send(mirror_object(), create_symbol("selectorsOf:"), 1, &TestVtable);
/* assert kernel Array: send size/at: and find Symbol_debug */
```

- [ ] **Step 2: Run — expect FAIL** (selectors not understood → DNU/abort).

- [ ] **Step 3: Implement the four prims in `vm.c`.** Model the CompiledMethod resolution on `primitive_compiled_method_set_instruction_at` (`vm.c:1099`) and the bindings walk on `primitive_vtable_bindings` (`vm.c:773-789`). For the iterable Arrays, allocate a kernel `Array` exactly the way the kernel does (find how `Array new:`/indexable allocation works — `PRIM_ix_array`/`indexable_new`; reuse that helper so `.ns` `size`/`at:`/`do:` work). Concretely:

```c
oop primitive_method_offset(oop receiver, int argc, oop *args) {
    (void)argc; (void)args;
    struct MethodContext *ctx = (struct MethodContext *)receiver;
    struct CompiledMethod *cm = (struct CompiledMethod *)((struct Closure *)ctx->closure)->entry_point;
    if (ctx->saved_pc == NULL) return TAG_SMI(0);
    return TAG_SMI((long)((const uint32_t *)ctx->saved_pc - cm->instructions));
}

oop primitive_compiled_method_instruction_at(oop receiver, int argc, oop *args) {
    (void)argc;
    struct Closure *cl = (struct Closure *)receiver;
    struct CompiledMethod *cm = (obj_vtable((oop)cl) == BytecodeClosureVtable)
        ? (struct CompiledMethod *)closure_entry(cl) : (struct CompiledMethod *)receiver;
    int idx = (int)UNTAG_SMI(args[0]);
    return TAG_SMI((long)(uint32_t)cm_instructions(cm)[idx]);
}

/* selectorsOf: / methodsOf: : walk Vtable bindings (see primitive_vtable_bindings),
   allocate a kernel Array of vt->size, fill with bb->symbol / bb->method. */
```
For `selectorsOf:`/`methodsOf:`, read `struct Vtable` (`oevm.h:78-85`: `bindings`, `size`) and `struct BoxedBinding` (`oevm.h:268-272`: `symbol`, `method`), and allocate the result as a real kernel `Array` (reuse the kernel indexable-allocation helper so the result responds to `size`/`at:`/`do:`).

- [ ] **Step 4: Add the four shims to `mirror_dispatch`** (`mirror.c:48-74`), each routing reflectee→arg0 like the others:

```c
    if (strcmp(sel, "offsetOf:") == 0)              return primitive_method_offset(args[0], argc-1, args+1);
    if (strcmp(sel, "method:instructionAt:") == 0)  return primitive_compiled_method_instruction_at(args[0], argc-1, args+1);
    if (strcmp(sel, "selectorsOf:") == 0)           return primitive_vtable_selectors(args[0], argc-1, args+1);
    if (strcmp(sel, "methodsOf:") == 0)             return primitive_vtable_methods(args[0], argc-1, args+1);
```
Add an `argc >= 1` guard at the top of `mirror_dispatch` if not already present (oevm#15 added it — confirm).

- [ ] **Step 5: Add prototypes to `oevm.h`** next to the existing reflection prim prototypes.

- [ ] **Step 6: Run the test — expect PASS.**

- [ ] **Step 7: Commit (oevm repo).**

```bash
git -C ../oevm add src/vm.c src/mirror.c src/oevm.h tests/test_debugger.c
git -C ../oevm commit -m "feat(mirror): add confined read prims offsetOf:/instructionAt:/selectorsOf:/methodsOf:

Read-only reflection the in-image debugger needs (classify break vs step,
save instruction before patching, iterate vtable bindings as kernel Arrays).
Confined: reachable only via mirror_object()."
```

---

## Task 3: `requestStep:` control prim

**Files:** `oevm/src/vm.c`, `oevm/src/mirror.c`, `oevm/src/oevm.h`, `oevm/tests/test_debugger.c`

**Interfaces:**
- Produces: `mirror requestStep: aBool` (no reflectee — like `onBreak:`) → sets the *current* fiber's `hook_step_request` (and `single_step`) from inside a hook, so the debugger never needs the fiber oop. `aBool` is `1`/non-nil to step, `0`/nil to stop.

- [ ] **Step 1: Write the failing test** — a hook that calls `requestStep:` (instead of `fiber:singleStep:`) and assert stepping engages (mirror Task 1's test, but through the no-reflectee selector).

- [ ] **Step 2: Run — expect FAIL** (selector not understood).

- [ ] **Step 3: Implement `primitive_request_step(oop receiver, int argc, oop *args)`** in `vm.c` — ignore receiver, read `args[0]`, compute `step_val`, set `current_fiber`'s `single_step` and (since this is always called in-hook) `hook_step_request`:

```c
oop primitive_request_step(oop receiver, int argc, oop *args) {
    (void)receiver; (void)argc;
    struct Fiber *fib = (struct Fiber *)current_fiber;
    int v = (args[0] != NULL && (!IS_SMI(args[0]) || UNTAG_SMI(args[0]) != 0)) ? 1 : 0;
    if (fib) { fib->single_step = v; fib->hook_step_request = v; }
    return NULL;
}
```

- [ ] **Step 4: Add the shim** to `mirror_dispatch` (no reflectee, like `onBreak:`):

```c
    if (strcmp(sel, "requestStep:") == 0) return primitive_request_step(NULL, argc, args);
```

- [ ] **Step 5: Prototype in `oevm.h`. Run the test — expect PASS.**

- [ ] **Step 6: Commit (oevm repo).**

```bash
git -C ../oevm add src/vm.c src/mirror.c src/oevm.h tests/test_debugger.c
git -C ../oevm commit -m "feat(mirror): add confined requestStep: to drive stepping from inside the break hook"
```

---

## Task 4: POSIX `readLine` host I/O prim + Kernel accessor

**Files:**
- Modify: `oevm/src/platform_backend.h`, `oevm/src/backend/posix/platform_posix.c`, `oevm/src/prim_ids.h`, `oevm/src/dispatch.c`, `oevm/src/platform.c`
- Modify: `ns-oevm/lib/kernel.ns`
- Test: `oevm/tests/test_debugger.c`

**Interfaces:**
- Produces: `platform readLine` → a kernel `String` (one line, newline stripped) or `nil` at EOF. `Kernel>>readLine` forwards `^ platform readLine`.

- [ ] **Step 1: Write the failing test (C).** In `test_debugger.c`, pipe a line into stdin (the harness will `printf` then the test reads) — or simpler, a focused check: temporarily `freopen` a temp file as stdin, send `readLine` to `platform_object()`, assert the returned String bytes, then send again at EOF and assert `nil`.

```c
FILE *tf = tmpfile(); fputs("dbg:hello\n", tf); rewind(tf);
/* point stdin at tf for the test */
oop line = send(platform_object(), create_symbol("readLine"), 0, NULL);
assert(line != NULL /* String "dbg:hello" */);
```

- [ ] **Step 2: Run — expect FAIL** (`readLine` not a known platform prim → DNU).

- [ ] **Step 3: Declare + implement the seam.** `platform_backend.h`:

```c
int platform_debug_readline(char *buf, int cap);   /* line len, newline stripped; -1 at EOF */
```
`backend/posix/platform_posix.c`:
```c
int platform_debug_readline(char *buf, int cap) {
    if (fgets(buf, cap, stdin) == NULL) return -1;
    int n = (int)strlen(buf);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
    return n;
}
```

- [ ] **Step 4: Wire the prim id (keep parity).** `prim_ids.h`: `#define PRIM_pf_readLine 70`. `dispatch.c`: add `PRIM_pf_readLine` to the enum immediately after `PRIM_pf_string` (`~537`), and `"readLine"` to `PRIM_NAMES[]` immediately after `"string:"` (`~555`) — both at the same parallel position.

- [ ] **Step 5: Implement the case in `platform_dispatch`** (`platform.c`, after `PRIM_pf_string`):

```c
    case PRIM_pf_readLine: {
        char buf[512];
        int n = platform_debug_readline(buf, sizeof buf);
        if (n < 0) { *out = NULL; return 1; }           /* EOF -> nil */
        *out = gc_allocate(StringVtable, sizeof(struct String) + (size_t)n);
        ((struct String *)*out)->length = (size_t)n;     /* match String field name in platform.c:121 path */
        if (n) memcpy(((struct String *)*out)->bytes, buf, (size_t)n);
        return 1;
    }
```
(Match the exact `String` allocation/field names used by `PRIM_pf_string` at `platform.c:121` — reuse that idiom verbatim.)

- [ ] **Step 6: Add the Kernel accessor.** In `ns-oevm/lib/kernel.ns`, add to `Kernel` (next to where it uses `platform string:`):

```newspeak
  public readLine = ( ^ platform readLine )
```

- [ ] **Step 7: Run the C test — expect PASS.** Then a quick `.ns` smoke (optional): a one-off where Supervisor echoes a piped line.

- [ ] **Step 8: Commit (oevm + ns-oevm).**

```bash
git -C ../oevm add src/platform_backend.h src/backend/posix/platform_posix.c src/prim_ids.h src/dispatch.c src/platform.c tests/test_debugger.c
git -C ../oevm commit -m "feat(platform): add readLine stdin prim (host debug transport input)"
git add lib/kernel.ns
git commit -m "feat(kernel): forward readLine to the platform capability"
```

---

## Task 5: `ObjectMirror` / `ContextMirror` facade (nested in `Supervisor`)

Per the STRUCTURAL CORRECTION above: the facade classes are NESTED inside the `Supervisor` class in `lib/supervisor.ns` (NOT a separate `debugger.ns`, NO `extend()` change).

**Files:**
- Modify: `ns-oevm/lib/supervisor.ns` (add two nested classes inside `class Supervisor ...`)
- Verify: `grammar/ns-parse.js` parse + build both paths + suite

**Interfaces:**
- Produces:
  - `ObjectMirror usingMirror: m reflecting: obj` with `vtable`, `bindings`, `reflectee`.
  - `ContextMirror usingMirror: m context: ctx` with `sender` (→ `ContextMirror` or `nil`), `rawSender`, `pc`, `offset`, `registerAt:`, `registerAt:put:`, `context`.

- [ ] **Step 1: Add the two nested classes inside `class Supervisor usingMirror: mirror = () (`** (before its closing `) : ( )`). Use the PARAM-DIRECT pattern that `supervisor.ns` already proves — the primary-factory params (`m`, `obj`, `ctx`) are usable directly in every instance method with NO slot declaration (exactly as `Supervisor` uses `mirror` in `announceReadyFor:`). Empty slot list `()`:

```newspeak
  public class ObjectMirror usingMirror: m reflecting: obj = () (
    public vtable    = ( ^ m vtableOf: obj )
    public bindings  = ( ^ m bindingsOf: self vtable )
    public reflectee = ( ^ obj )
  ) : ()

  public class ContextMirror usingMirror: m context: ctx = () (
    public context   = ( ^ ctx )
    public rawSender = ( ^ m senderOf: ctx )
    public sender = (
      | s |
      s:: m senderOf: ctx.
      ^ s == nil ifTrue: [ nil ] ifFalse: [ ContextMirror usingMirror: m context: s ] )
    public pc            = ( ^ m pcOf: ctx )
    public offset        = ( ^ m offsetOf: ctx )
    public registerAt: i = ( ^ m reg: ctx at: i )
    public registerAt: i put: v = ( ^ m reg: ctx at: i put: v )
  ) : ()
```
Note the dialect idioms (verified in `ns-tests/04_somns_basic/{04_initializers,07_super_sends,18_class_side_methods}.ns`): nested class with primary factory `usingMirror:reflecting:`; instantiated by sending that keyword message to the class name (`ObjectMirror usingMirror: x reflecting: y`); a nested class references a sibling/itself by bare name, falling back to `outer Supervisor ContextMirror` if bare-name resolution fails. The implementer MUST determine empirically which form resolves and use it.

- [ ] **Step 2: Verify it parses.** `node grammar/ns-parse.js lib/supervisor.ns` (or the parse path the compiler uses) — expect PARSED_OK / valid JSON, no syntax error.

- [ ] **Step 3: Prove instantiation + param persistence with a TEMPORARY smoke, then REVERT it.** The facade needs the mirror (held only by Supervisor) — so smoke it from inside `Supervisor`. Temporarily rewrite `announceReadyFor: entry` to go THROUGH the facade:
```newspeak
  announceReadyFor: entry = (
    | om | om:: ObjectMirror usingMirror: mirror reflecting: entry.
    om vtable == nil ifTrue: [ 'dbg:error no-vtable' println ] ifFalse: [ 'dbg:ready' println ] )
```
Run `python3 tools/test_supervisor_grant.py` (drives Supervisor with `OEVM_DEBUG`, expects `dbg:ready` both paths). If it prints `dbg:ready`, ObjectMirror instantiation + param persistence + a facade method all work. Then **REVERT `announceReadyFor:` to its original** (the `mirror vtableOf: entry` form) — this smoke must not land in the commit. Record in the report which instantiation idiom worked. (ContextMirror's behavioral proof necessarily waits for a real breakpoint context in Task 9.)

- [ ] **Step 4: Build both paths + regression.** `OEVM_PATH=AB python3 tools/run_tests_oevm.py` → still 69/0/4 (facade classes present but unreferenced by the suite). `git -C ../oevm checkout src/solidified_image.c src/solidified_image.stmap`.

- [ ] **Step 5: Commit (ns-oevm).** Only the two nested classes (smoke reverted).

```bash
git add lib/supervisor.ns
git commit -m "feat(debugger): ObjectMirror/ContextMirror facade nested in Supervisor"
```

---

## Task 6: `Debugger` registry + `dbg:methods` + minimal Supervisor loop + harness

This task makes the debugger end-to-end testable: it adds the `Debugger` (nested in `Supervisor`), wires a MINIMAL `OEVM_DEBUG` command loop in `Supervisor run:` (handling `dbg:methods`/`dbg:continue` only), and creates the pexpect harness + target app. Tasks 7–8 then EXTEND this same loop and harness.

**Files:**
- Modify: `ns-oevm/lib/supervisor.ns` (add nested `Debugger`; wire `run:` initial loop)
- Create: `ns-tests/04_somns_basic/46_debug_target.ns` (target app, with `(* expect: *)` so it runs clean gate-off)
- Create: `ns-oevm/tools/test_debugger_integration.py` (pexpect harness, Path A first)

**Interfaces:**
- Produces: `Debugger usingMirror: m channel: io` (io = the Kernel instance `plat`, used for input `io readLine`, output `println`, AND collection allocation `io OrderedCollection new`); `registerEntry: anObject`; `listMethods`; `methodAt: id` → method oop; `initialLoop` (reads `dbg:*` until continue/EOF).

### Critical dialect constraints (from the structural correction)
- **Collections come from the kernel instance, NOT bare names.** `Debugger` and `Supervisor` are top-level; kernel classes (`OrderedCollection`) live in `Kernel` and are NOT bare-name reachable. Allocate via the kernel instance: `io OrderedCollection new` (Kernel exposes `public OrderedCollection = ( ^ OrderedCollection )`, kernel.ns ~243). Same for any `Array`/`WriteStream` need.
- **`selectorsOf:`/`methodsOf:` return RAW indexables** (IndexableVtable), not kernel `Array`s — they answer the vtable-agnostic prims `basicSize` and `basicAt:` (NOT necessarily `size`/`at:`). Use `sels basicSize` and `sels basicAt: i`. **Confirm the index base** by reading how `Kernel>>Array>>at:` maps to `basicAt:` in kernel.ns (match that base — likely 1-based).
- **`Debugger` is nested in `Supervisor`**; `ContextMirror`/`ObjectMirror` are sibling-nested — reach them by BARE name (Task 5 proved this works).

- [ ] **Step 1: Add the nested `Debugger`** inside `class Supervisor` with state. Slots hold the mirror, kernel instance, entry, and the registry (parallel `OrderedCollection`s are simplest, or a nested `DebugMethod` record class — bare-name nested classes resolve). Use param-direct for `m`/`io` like the facade:

```newspeak
  public class Debugger usingMirror: m channel: io = ( | entry ids sels meths tags bps stepMode stepDepth | ) (
    public registerEntry: anEntry = ( entry:: anEntry. ^ self )
    ... build/list/lookup below ...
  ) : ()
```

- [ ] **Step 2: Registry walk** — lazily on first `listMethods`/lookup. Walk the entry's vtable delegate chain; per vtable use `selectorsOf:`/`methodsOf:` (raw indexables via `basicSize`/`basicAt:`):

```newspeak
    buildRegistry = (
      ids:: io OrderedCollection new. sels:: io OrderedCollection new.
      meths:: io OrderedCollection new. tags:: io OrderedCollection new.
      | vt | vt:: m vtableOf: entry.
      [ vt == nil ] whileFalse: [
        | s mm tag | s:: m selectorsOf: vt. mm:: m methodsOf: vt. tag:: vt identityHash.
        1 to: s basicSize do: [ :i |
          ids add: ids size + 1. sels add: (s basicAt: i). meths add: (mm basicAt: i). tags add: tag ].
        vt:: m delegateOf: vt ] )
```
(Adjust the `basicAt:` index base to the kernel's convention.)

- [ ] **Step 3: `listMethods`** — ensure built, then for each i print `<id> <selector printString> <tag>` (one line, via `println`), then `'dbg:ok methods' println`.

- [ ] **Step 4: `methodAt: id`** — ensure built; linear scan `ids` for `id`, return the parallel `meths` element (or `nil`).

- [ ] **Step 5: Minimal Supervisor wiring.** In `run: entry platform: plat args: args debug: debugFlag`, when `debugFlag = 1`: build `dbg:: Debugger usingMirror: mirror channel: plat`, `dbg registerEntry: entry`, print `'dbg:ready'`, then `dbg initialLoop`, THEN `^ entry main: plat args: args`. When `debugFlag` ≠ 1, behave exactly as today (forward). `initialLoop` reads `io readLine`; on `dbg:methods` → `self listMethods`; on `dbg:continue`/`dbg:run`/nil(EOF) → return; else `'dbg:error unknown' println`. (Keep it tolerant: an empty/`nil` line returns.)

- [ ] **Step 6: Target app** `ns-tests/04_somns_basic/46_debug_target.ns` — a small app whose `main:args:` calls a method with a known local and a loop; add `(* expect: <its normal gate-off output> *)` so the suite runs it clean.

- [ ] **Step 7: pexpect harness** `tools/test_debugger_integration.py` — build+run the target with `OEVM_DEBUG=1` (Path A: solidify via ns_compile_oevm.py then run the produced binary; model the build on `tools/test_supervisor_grant.py`). Drive: expect `dbg:ready`; send `dbg:methods`; assert a known target selector (e.g. the method you put the loop in) appears with a numeric id; send `dbg:continue`; expect the app's normal output. Make the harness reusable (Tasks 7–8 add more verbs).

- [ ] **Step 8: Verify.** Run the harness (Path A) → PASS. Then `OEVM_PATH=AB python3 tools/run_tests_oevm.py` → 70/0/4 (the new target app adds one pass), gate-off transparent. `git -C ../oevm checkout src/solidified_image.c src/solidified_image.stmap`.

- [ ] **Step 9: Commit (ns-oevm + ns-tests).**

```bash
git add lib/supervisor.ns tools/test_debugger_integration.py
git commit -m "feat(debugger): runtime methodId registry + dbg:methods + minimal OEVM_DEBUG loop + harness"
git -C ../ns-tests add 04_somns_basic/46_debug_target.ns
git -C ../ns-tests commit -m "test(debugger): debuggable target app for the integration harness"
```

---

## Task 7: C-assisted breakpoints — VM bp-table + match + `dbg:break`/`dbg:clear`/`dbg:continue`

Per the BREAKPOINT MECHANISM decision: NO `OP_BREAK` patching. A breakpoint is a `(method, offset)` entry in a C-side table; the interpreter matches it in C and fires the existing `.ns` `onBreak:` hook. This spans BOTH repos.

**Files:**
- Modify: `oevm/src/vm.c` (bp table + top-of-loop match + 3 new confined prims), `oevm/src/mirror.c` (3 shims), `oevm/src/oevm.h` (prototypes)
- Modify: `ns-oevm/lib/supervisor.ns` (extend nested `Debugger`; register hook in `run:`)
- Modify: `ns-oevm/tools/test_debugger_integration.py`
- Test: `oevm/tests/test_debugger.c` (new C test for the bp-table match path)

Dialect: hoist ALL temps to method top; key breakpoints by per-run methodId; reach `ContextMirror` by bare name.

**Interfaces:**
- Produces (C, confined mirror selectors): `mirror breakAt: method offset: off` (add a `(method,off)` entry, return self), `mirror unbreakAt: method offset: off` (remove), `mirror methodOf: ctx` (→ the context's CompiledMethod oop, for reverse-mapping to a registry id). Plus the VM per-instruction bp match firing `debugger_hook`.
- Produces (.ns): `break: id at: off`, `clear: id at: off`, `onBreakContext: ctx`, `commandLoop`. `dbg:paused break <id> <off>` on hit.

### C side (oevm)

- [ ] **Step 1: bp table + match (vm.c).** Add a file-scope table: `static struct { oop method; long off; } g_bp[64]; static int g_bp_count = 0;` plus helpers `bp_add(method, off)`, `bp_remove(method, off)`, `int bp_hit(oop method, long off)` (linear scan). At the **TOP of the interpreter `while` loop** (before reading `inst`), gated cheaply:
```c
  if (g_bp_count > 0 && debugger_hook != NULL && !in_debugger_hook) {
      long curoff = (long)(pc - cm_instructions(method));
      if (bp_hit((oop)method, curoff)) {
          ctx->saved_pc = pc;              /* pause BEFORE executing pc */
          ctx->vtable = MethodContextVtable;
          int was = fib ? fib->single_step : 0;   /* fib = (struct Fiber*)current_fiber */
          if (fib) { fib->single_step = 0; fib->hook_step_request = -1; }
          in_debugger_hook = 1;
          oop a = (oop)ctx; /* fire hook exactly like the OP_BREAK path */
          ...invoke debugger_hook with 1 arg a (Primitive vs Bytecode closure, same as vm.c:187-192)...
          in_debugger_hook = 0;
          if (fib) { fib->single_step = (fib->hook_step_request>=0)?fib->hook_step_request:was; fib->hook_step_request = -1; }
          pc = ctx->saved_pc;              /* unchanged → fall through and execute pc normally */
      }
  }
```
(The gate `g_bp_count > 0` is one predicted-not-taken branch per instruction when no breakpoints — measured always-on cost <~3%, zero when the debugger isn't compiled into a production build. While armed: ~1.3–1.5× for 1–few bps, per the measurement table above. Continue is trivial: returning from the hook lets `pc` execute; the bp re-fires next visit — no restore/re-arm.)

- [ ] **Step 2: 3 confined mirror prims.** In `vm.c` add `primitive_break_add(method, off)`, `primitive_break_remove(method, off)` (UNTAG the SMI off; call bp_add/bp_remove), `primitive_method_of(ctx)` (→ `closure_entry((struct Closure*)ctx->closure)` as oop). In `mirror.c` add shims: `breakAt:offset:` → `primitive_break_add(args[0], args[1])` (reflectee=method=arg0, off=arg1); `unbreakAt:offset:` → `primitive_break_remove(args[0],args[1])`; `methodOf:` → `primitive_method_of(args[0])`. Prototypes in `oevm.h`. Confinement: these live ONLY in mirror_dispatch, never in dispatch.c.

- [ ] **Step 3: C test (test_debugger.c).** Build a heap method, `mirror breakAt: method offset: K`, register a hook that records the hit + asserts `mirror offsetOf: ctx == K` and `mirror methodOf: ctx == method`; run it; assert the hook fired at offset K WITHOUT any instruction mutation; `mirror unbreakAt:` then assert no re-fire. Build/run with the canonical recipe (vm.c+...+tests/test_debugger.c). RED first (selectors DNU), then GREEN.

### .ns side (Supervisor/Debugger)

- [ ] **Step 4: Register the hook** in `Supervisor run:`'s `debugFlag = 1` block, BEFORE `dbg initialLoop`: `mirror onBreak: [ :ctx | dbg onBreakContext: ctx ]`.

- [ ] **Step 5: `break: id at: off` / `clear: id at: off`** (extend `Debugger`; bp record store = parallel collections `bpIds bpMethods bpOffs` via `io OrderedCollection new`). `break:`: `m:: self methodAt: id`. nil → `'dbg:error no-method' println`. else record (id, m, off), `mirror breakAt: m offset: off`, `'dbg:ok break' println`. `clear:`: find record, `mirror unbreakAt: m offset: off`, remove, `'dbg:ok clear' println`. Handle `dbg:break`/`dbg:clear` in BOTH `initialLoop` (pre-launch) and `commandLoop` (paused). Tokenize + `parseInt:` per the Task-7 brief.

- [ ] **Step 6: `onBreakContext: ctx`** — `off:: mirror offsetOf: ctx. meth:: mirror methodOf: ctx.` find the bp record with matching `meth` + `off` → its `id`; emit `'dbg:paused break <id> <off>'` (via `io WriteStream`); then `self commandLoop`. (stepMode handling deferred to Task 8 — for now stepMode is always nil ⇒ always a breakpoint.)

- [ ] **Step 7: `commandLoop`** (paused) — `io readLine`; `dbg:continue` → return (resume; bp re-fires later, no re-arm); `dbg:break`/`dbg:clear`/`dbg:methods` → act, keep looping; `dbg:step`/`over`/`out`/`inspect` → `'dbg:error not-yet' println` (Task 8 fills); nil(EOF) → return.

- [ ] **Step 8: Harness** — parse `dbg:methods` for the `countTo:` id; `dbg:break <id> 0`; expect `dbg:ok break`; `dbg:continue`; expect `dbg:paused break <id> 0`; `dbg:continue`; expect the app output (`15`). Keep the Task-6 dbg:methods scenario.

- [ ] **Step 9: Verify.** Parse clean; harness PASS (Path A); `OEVM_PATH=AB python3 tools/run_tests_oevm.py` → 70/0/4 gate-off (bp table empty ⇒ the top-of-loop gate is a single not-taken branch; suite correctness unaffected); C unit `test_debugger` PASS. Then `git -C ../oevm checkout src/solidified_image.c src/solidified_image.stmap`.

- [ ] **Step 10: Commit (oevm + ns-oevm).**

```bash
git -C ../oevm add src/vm.c src/mirror.c src/oevm.h tests/test_debugger.c
git -C ../oevm commit -m "feat(vm): C-assisted breakpoint table + confined breakAt:/unbreakAt:/methodOf: prims"
git add lib/supervisor.ns tools/test_debugger_integration.py
git commit -m "feat(debugger): breakpoints via C bp-table match + dbg:break/clear/continue"
```

---

## Task 8: Stepping (`dbg:step`/`over`/`out`) + `dbg:inspect`

**Files:** `ns-oevm/lib/supervisor.ns` (extend the nested `Debugger`); extend `ns-oevm/tools/test_debugger_integration.py` (anchor the app-output match, don't substring on a bare number). Dialect: hoist temps to method top.

**Interfaces:**
- Produces: step-mode handling in `onBreakContext:` using sender-chain depth; `inspect: frameIndex` → `dbg:stack <fields>`.

- [ ] **Step 1: Depth helper** — `depthOf: ctxMirror` counts `rawSender` hops to `nil`.

- [ ] **Step 2: `dbg:step`** — `stepMode:: #into`, `mirror requestStep: 1`, return from loop. In `onBreakContext:` step branch with `#into`: always pause, emit `dbg:paused step <methodId> <off>`, enter `commandLoop`.

- [ ] **Step 3: `dbg:over`** — record `stepDepth:: depth`, `#over`, `requestStep: 1`. Step branch: pause only when `depth <= stepDepth`.

- [ ] **Step 4: `dbg:out`** — `stepDepth:: depth - 1`, `#out`, `requestStep: 1`. Step branch: pause only when `depth < stepDepth + 1` (i.e. returned to caller).

- [ ] **Step 5: `inspect: frameIndex`** — walk `frameIndex` `sender` hops from the current `ContextMirror`; emit `dbg:stack off=<n> r0=<printString> r1=<...>` up to a small register cap (via `registerAt:` + kernel `printString`). NOTE: this is a flat delimited line, **not** real JSON (no in-image JSON encoder) — the spec's "JSON" is relaxed to a parseable line here; the agent splits on spaces.

- [ ] **Step 6: Test via harness** — set a breakpoint, `dbg:inspect`, assert a known local's `printString` value appears; `dbg:step` advances `off`; `dbg:continue` finishes. Both paths.

- [ ] **Step 7: Commit (ns-oevm).**

```bash
git add lib/debugger.ns
git commit -m "feat(debugger): step/over/out via sender-depth + dbg:inspect frame dump"
```

---

## Task 9: Supervisor wiring + initial command loop + integration test

**Files:**
- Modify: `ns-oevm/lib/supervisor.ns`
- Create: `ns-tests/04_somns_basic/46_debug_target.ns`
- Create: `ns-oevm/tools/test_debugger_integration.py`

**Interfaces:**
- Consumes: `Debugger`, `Kernel>>readLine`, the `OEVM_DEBUG` flag already threaded into `run:platform:args:debug:`.
- Produces: a Supervisor that, gate-on, runs an initial `dbg:*` loop (set breakpoints before launch) then launches; gate-off, forwards unchanged.

- [ ] **Step 1: Write the failing integration test** `tools/test_debugger_integration.py` (pexpect): build/run the target app with `OEVM_DEBUG=1`, expect `dbg:ready`, send `dbg:methods`, parse the target id, send `dbg:break <id> <off>` (off = a constant for the target app — Plan 3 derives it from `.stmap`), `dbg:continue`, expect `dbg:paused break`, `dbg:inspect`, assert a known value, `dbg:step`, `dbg:continue`, expect the app's normal stdout. Parameterize over Path A and Path B.

- [ ] **Step 2: Run — expect FAIL** (Supervisor doesn't yet build a Debugger or read commands).

- [ ] **Step 3: Create the target app** `46_debug_target.ns` — a tiny app with a method containing a known local and a loop, plus a `(* expect: ... *)` block matching its normal (gate-off) output so the regular suite runs it clean.

- [ ] **Step 4: Wire the Supervisor** (`supervisor.ns`):

```newspeak
  public run: entry platform: plat args: args debug: debugFlag = (
    | dbg |
    dbg:: Debugger usingMirror: mirror channel: plat.
    dbg registerEntry: entry.
    mirror onBreak: [ :ctx | dbg onBreakContext: ctx ].
    debugFlag = 1 ifTrue: [
      'dbg:ready' println.
      dbg initialLoop ].
    ^ entry main: plat args: args )
```
`initialLoop`: read `dbg:*` via `plat readLine`; handle `dbg:methods`/`dbg:break`/`dbg:clear`; return on `dbg:continue`/`dbg:run`/nil. Keep the old `announceReadyFor:` smoke or fold it into `dbg:ready`.

- [ ] **Step 5: Run the integration test — expect PASS** on both paths.

- [ ] **Step 6: Gate-off regression.** `OEVM_PATH=AB python3 tools/run_tests_oevm.py` → **69/0/4** (existing 69 + the new `46_debug_target` runs clean gate-off; adjust expected counts to include the new test). Confirm the structural guard still passes. `git -C ../oevm checkout src/solidified_image.c src/solidified_image.stmap`.

- [ ] **Step 7: Commit (ns-oevm + ns-tests).**

```bash
git add lib/supervisor.ns tools/test_debugger_integration.py
git commit -m "feat(supervisor): build Debugger, register hook, run dbg:* command loop under OEVM_DEBUG"
git -C ../ns-tests add 04_somns_basic/46_debug_target.ns
git -C ../ns-tests commit -m "test(debugger): debuggable target app for the integration harness"
```

---

## Task 10: Suite hygiene — ns-oevm#5

**Files:**
- Modify: `ns-oevm/tools/run_tests_oevm.py`, `ns-oevm/tools/test_supervisor_grant.py`

- [ ] **Step 1:** Gate `test_supervisor_grant.py` and `test_debugger_integration.py` into `run_tests_oevm.py` next to where `run_c_unit_tests` is invoked (`~:429`); fail the suite if either fails. Add their pass/fail to the summary line.

- [ ] **Step 2:** In `test_supervisor_grant.py`, replace the hard-coded gcc source list (`~:20-25, :57-66`) with the `OEVM_BACKEND`/`OEVM_EXTRA_CFLAGS`-honoring build (mirror `run_tests_oevm.py`'s build_a/build_b source assembly). Derive the grant argc from the app's params rather than hard-coding.

- [ ] **Step 3:** Run `OEVM_PATH=AB python3 tools/run_tests_oevm.py`; confirm both gated tests run + pass and the count is unchanged otherwise.

- [ ] **Step 4: Commit (ns-oevm).**

```bash
git add tools/run_tests_oevm.py tools/test_supervisor_grant.py
git commit -m "test(suite): gate supervisor-grant + debugger-integration tests; honor backend/cflags (ns-oevm#5)"
```

---

## Task 11: Final regression, dialect doc, issue closure

**Files:** `ns-oevm/NEWSPEAK_DIALECT.md`, docs.

- [ ] **Step 1:** Add NEWSPEAK_DIALECT.md entry **#16** describing the debugger capability surface (the 5 new confined mirror selectors + `readLine` + hook-controllable stepping).
- [ ] **Step 2:** Full run: `OEVM_PATH=AB python3 tools/run_tests_oevm.py` → all green both paths (69 base + new target + C unit incl. revived `test_debugger.c` + integration + grant). `git -C ../oevm checkout src/solidified_image.c src/solidified_image.stmap`.
- [ ] **Step 3:** Re-run `../oevm/tools/check_no_ambient_reflection.sh` (or `check_no_reflective_emit.sh`) from the correct cwd — still passes (app cannot name `mirror`).
- [ ] **Step 4:** Comment/close **oevm#14** (test_debugger.c now exercises the full debugger prim surface). Note remaining `test_mirrors.c` link gap if still open. Update progress ledger `.superpowers/sdd/progress.md`.
- [ ] **Step 5: Commit (ns-oevm).**

```bash
git add NEWSPEAK_DIALECT.md .superpowers/sdd/progress.md
git commit -m "docs(debugger): dialect #16 + Plan 2 ledger; close oevm#14"
```

---

## Verification

1. **Agent-driven integration** (`tools/test_debugger_integration.py`, both paths): `OEVM_DEBUG=1` → `dbg:ready` → `dbg:methods` → `dbg:break <id> <off>` → `dbg:continue` → `dbg:paused break <id> <off>` → `dbg:inspect` shows a known local → `dbg:step` advances → `dbg:continue` → app's normal stdout.
2. **C-level mechanics** (`oevm/tests/test_debugger.c`): breakpoint trap, hook-armed stepping (Task 1), the 5 new prims, `readLine`. Closes oevm#14's positive proof.
3. **Regression, gate OFF:** `OEVM_PATH=AB python3 tools/run_tests_oevm.py` → all base tests pass both paths, byte-for-byte for the existing 69; structural reflection guard still passes; then `git -C ../oevm checkout src/solidified_image.c src/solidified_image.stmap`.
4. **Confinement:** the app never names `mirror`; only `Supervisor` holds it; the guard script enforces no reflective emit in app code.

## Risks (carried from design)

- **R6/re-entrancy:** confirmed safe — the hook re-enters `run_interpreter`, which brackets `saved_stack_ptr`/`current_frame` (vm.c:122/132/736-737). Prove early (Task 7 first break-hit) that a nested command loop doing mirror sends + `readLine` doesn't corrupt the paused frame.
- **R7/registry reachability:** the vtable walk reaches only the entry class's delegate chain. The target app's breakpoint method must live on the entry class (or a superclass). Document; broader reachability is Plan 3.
- **R8/Path B serialization:** confirm `debugger.ns` classes serialize identically (no missing symbols/unreached vtables) — run the integration test on Path B early (Task 9 step 5).
- **R4/blocking stdin:** `fgets` blocks the single thread while paused — intended. Ensure every `dbg:*` response ends with a flushed newline (`println` flushes) so the pexpect harness never deadlocks.
```