# Std-lib Step 2 — Compiler Platform Wiring Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Thread a real `platform` capability object through the boot seam and make the compiler resolve capitalised library globals (`Array`, …) through that threaded `platform` instead of an implicit-send-to-self, with no behaviour change (47/47 stays green on both image paths).

**Architecture:** Two surgical changes. (1) The two entry seams (Path A generated C, Path B `.oimg` loader) pass `platform_object()` — the real step-1 capability singleton — where they currently pass `fake_platform = TAG_SMI(0)`. (2) The compiler's name resolver routes any unresolved *capitalised* name through the lexically-threaded `platform` (a module param → slot, reachable from nested classes via the existing outer-walk) as a unary send, falling back to the legacy implicit-send when no `platform` is in scope. No `dispatch.c` change is needed: `send()` intercepts `PRIM_Array` before vtable lookup and returns the existing mock regardless of receiver, so `send(platform, "Array")` yields the same mock — the bridge. The mock retires in step 3 / step 6.

**Tech Stack:** Python compiler (`tools/ns_compile_oevm.py`), C VM in the **sibling repo `../oevm`** (`src/oimg_loader.c`, `src/dispatch.c`), Ohm grammar (unchanged), pytest compiler unit tests, the `../ns-tests` conformance suite driven by `tools/run_tests_oevm.py`.

## Global Constraints

- **Repo layout:** this repo (`ns-oevm`) holds the compiler/grammar/docs/tests-harness; the VM is a **sibling repo at `../oevm`**. All `../oevm/...` paths below are relative to *this* repo's cwd.
- **Run the conformance suite from the `ns-oevm` cwd:** `python3 tools/run_tests_oevm.py` (a wrong cwd picks a stale harness and falsely reports 0/47). Default gate runs **both** Path A (solidified-C) and Path B (`.oimg`).
- **Regression bar:** the suite must stay green on **both** paths (the migration's safety net — spec §7). "Green" = same pass/skip counts as `master` (47 pass).
- **Capability-purity:** `platform` must be reached as a *threaded value* (param/slot), never as a reachable VM global from image code. The entry seam is the only place C hands it in.
- **Scope (ratified this session):** redirect **capitalised** global-name resolution only; lowercase implicit sends (`foo: x` to self) stay `OP_SEND_IMPLICIT` against `self`. Keep the `PRIM_Array` mock alive (bridge); do **not** touch `dispatch.c`.
- **Opcode values (from `tools/ns_compile_oevm.py`):** `OP_LD_REG = 1`, `OP_SEND = 2`, `OP_SEND_IMPLICIT = 15`. Reference them as `C.OP_SEND` etc. in tests, never hardcode.
- **Commit after each task.** Work on a branch, not `master`. Do **not** push (push only on explicit say-so).

---

## File Structure

- `tools/ns_compile_oevm.py` (modify) — name resolution + emit + both-path entry generator.
  - `MethodCompiler.resolve_name` (~line 346) — add the capitalised→`platform_global` branch.
  - `MethodCompiler.compile_expr`, `varRef` arm (~line 451) — add the `platform_global` emit branch.
  - `MethodCompiler.compile_expr`, `dynArray` arm (~line 572) — acquire the `Array` class via the unified `varRef` path.
  - `patched_generate_c` entry block (~line 1841) — Path A: pass `platform_object()`.
- `../oevm/src/oimg_loader.c` (modify) — `oimg_bootstrap` (lines 128, 131): Path B, pass `platform_object()`.
- `../ns-tests/04_somns_basic/24_platform_capability.ns` (create) — e2e: proves the real platform is threaded into `main:args:` and through a `usingPlatform:` submodule.
- `tools/test_platform_routing.py` (create) — compiler unit tests pinning both resolver branches (platform-in-scope → `OP_SEND` to platform; no-platform → legacy `OP_SEND_IMPLICIT`).

---

### Task 1: Entry seams hand the real `platform` capability to the image

**Files:**
- Modify: `tools/ns_compile_oevm.py:1841-1852` (Path A generator)
- Modify: `../oevm/src/oimg_loader.c:128,131` (Path B loader)
- Test: `../ns-tests/04_somns_basic/24_platform_capability.ns` (create)

**Interfaces:**
- Consumes: `platform_object(void)` — the step-1 capability singleton, declared in `../oevm/src/oevm.h:295`; available on both build lines (`dispatch.c` is compiled into both runners) and to the generated C (which `#include`s `oevm.h`).
- Produces: at runtime, the entry class's `init_<sig>` and `main:args:` receive `platform_object()` as argument 0. Programs reach it via their declared `usingPlatform: platform` / `main: platform args:` params.

- [ ] **Step 1: Write the failing e2e test**

Create `../ns-tests/04_somns_basic/24_platform_capability.ns`. It exercises the step-1 platform prims (`print:`/`printNewline:`/`objHashcode:`), which are gated on `receiver == PlatformObj` (`dispatch.c:755`) and therefore *only* work if the real platform is threaded in — both directly into `main`, and through a `usingPlatform:` submodule construction:

```
(*
expect:
42
7
*)
class PlatformCapabilityTest packageUsing: manifest = () (
class Holder usingPlatform: platform = () (
  public show = (
    platform print: (platform objHashcode: 7).
    platform printNewline:.
  )
) : ()
public main: platform args: args = (
    | h |
    platform print: (platform objHashcode: 42).
    platform printNewline:.
    h:: Holder usingPlatform: platform.
    h show.
)
) : (
)
```

- [ ] **Step 2: Run the test to verify it fails (fake platform today)**

Run (manual single-file inner loop, mirrors `run_test_file`):

```bash
python3 tools/ns_compile_oevm.py ../ns-tests/04_somns_basic/24_platform_capability.ns ../oevm/src/solidified_image.c
(cd ../oevm && gcc -Wall -Wextra -std=c99 -DOEVM_APPLIANCE src/vm.c src/gc.c src/dispatch.c src/solidified_image.c src/oevm_main.c -o oevm_runner -lm && ./oevm_runner)
```

Expected: FAIL — with `fake_platform = TAG_SMI(0)`, `platform objHashcode: 42` sends to a SMI (not `PlatformObj`), falls through the gated block, and hits `does_not_understand` → error / no `42` printed.

- [ ] **Step 3: Path A — generate the real platform in the entry block**

In `tools/ns_compile_oevm.py`, replace the fake-platform lines (currently `:1842-1850`):

```python
        # Fake platform and args (NULL / Smis)
        c_code += f"    oop fake_platform = TAG_SMI(0);\n"
        c_code += f"    oop fake_args = TAG_SMI(0);\n"
        c_code += f"    oop init_args[2] = {{ fake_platform, fake_args }};\n"
```

with (hand the real step-1 capability object; args stays empty):

```python
        # std-lib step 2: hand the real `platform` capability (step-1 singleton)
        # to the entry class's initializer and to main:args:. args is still empty.
        c_code += f"    oop platform = platform_object();\n"
        c_code += f"    oop fake_args = TAG_SMI(0);\n"
        c_code += f"    oop init_args[2] = {{ platform, fake_args }};\n"
```

and update the `main_args` line (currently `:1850`) from:

```python
        c_code += f"    oop main_args[2] = {{ fake_platform, fake_args }};\n"
```

to:

```python
        c_code += f"    oop main_args[2] = {{ platform, fake_args }};\n"
```

- [ ] **Step 4: Path B — hand the real platform in `oimg_bootstrap`**

In `../oevm/src/oimg_loader.c`, change the two arg arrays in `oimg_bootstrap` (lines 128 and 131) from `{ TAG_SMI(0), TAG_SMI(0) }` to hand the real platform as arg 0:

```c
    oop init_args[2] = { platform_object(), TAG_SMI(0) };
    send(inst, oimg_resolve(img_init_sel_ref), (int)img_init_argc, init_args);

    oop main_args[2] = { platform_object(), TAG_SMI(0) };
    oop res = send(inst, oimg_resolve(img_main_sel_ref), 2, main_args);
```

(`platform_object()` is declared via the `#include "oevm.h"` already at the top of the file.)

- [ ] **Step 5: Run the e2e test on both paths to verify it passes**

```bash
python3 tools/ns_compile_oevm.py ../ns-tests/04_somns_basic/24_platform_capability.ns ../oevm/src/solidified_image.c
(cd ../oevm && gcc -Wall -Wextra -std=c99 -DOEVM_APPLIANCE src/vm.c src/gc.c src/dispatch.c src/solidified_image.c src/oevm_main.c -o oevm_runner -lm && ./oevm_runner)
```

Expected (Path A): two lines, `42` then `7`.

Then the full suite (covers Path B build of the modified `oimg_loader.c`):

```bash
python3 tools/run_tests_oevm.py
```

Expected: 47 pass, same skip count as `master`, **both** paths — including `04_somns_basic/24_platform_capability.ns` PASS.

- [ ] **Step 6: Commit**

```bash
git add tools/ns_compile_oevm.py ../ns-tests/04_somns_basic/24_platform_capability.ns
git -C ../oevm add src/oimg_loader.c
git commit -m "feat(stdlib): hand real platform_object() through both entry seams"
git -C ../oevm commit -m "feat(stdlib): oimg_bootstrap hands real platform_object() to init/main"
```

(Two repos → two commits. The generated `../oevm/src/solidified_image.c` is a build artifact regenerated per test run; do not commit it.)

---

### Task 2: Resolve capitalised library globals through the threaded `platform`

**Files:**
- Modify: `tools/ns_compile_oevm.py` — `resolve_name` (~line 369), `compile_expr` `varRef` arm (~line 516), `compile_expr` `dynArray` arm (~line 578-581)
- Test: `tools/test_platform_routing.py` (create)

**Interfaces:**
- Consumes: `resolve_name(name)` results `("local", reg)` / `("lexical", reg, depth)` / `("slot", idx, depth)` for the literal name `"platform"` (lowercase ⇒ never re-enters the capitalised branch); `compile_expr({"type":"varRef","name":...}, reg)` to load a value into a register; `add_constant(str)`; `emit_ABC(op, a, b, c)` packing `(c<<24)|(b<<16)|(a<<8)|op`.
- Produces: a new resolver result `("platform_global", name)` and bytecode that loads `platform` then `OP_SEND name (0 args) → platform`. Constants carry the bare capitalised `name` string. Later steps (3/6) rely on capitalised globals being a single `platform`-routed send site.

- [ ] **Step 1: Write the failing unit tests**

Create `tools/test_platform_routing.py` (mirrors `tools/test_source_map.py`'s parse→compile→inspect idiom). It decodes each method's instruction words and asserts how the `Array` constant is sent — through `platform` when one is in scope, via legacy implicit-send when not:

```python
import json
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
GRAMMAR = TOOLS.parent / "grammar" / "ns-parse.js"
sys.path.insert(0, str(TOOLS))


def _parse(src_text, tmp_path):
    f = tmp_path / "frag.ns"
    f.write_text(src_text)
    r = subprocess.run(["node", str(GRAMMAR), str(f)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return json.loads(r.stdout)


def _method_using(compiler, needle):
    """Return (instructions, constants) for the first solidified method whose
    id mentions `needle`."""
    for m in compiler.solidify.methods:
        if needle in m["id"]:
            return m["instructions"], m["constants"]
    raise AssertionError(f"no method id contains {needle!r}")


def _sends_to_const(insts, constants, op, name):
    """How many `op` instructions reference the constant string `name`."""
    if name not in constants:
        return 0
    cidx = constants.index(name)
    return sum(1 for w in insts
               if (w & 0xFF) == op and ((w >> 16) & 0xFF) == cidx)


# platform IS in scope (Probe takes `usingPlatform: platform`, so it is a slot
# reachable from the method): `Array` must route through platform via OP_SEND.
WITH_PLATFORM = """class Probe usingPlatform: platform = () (
public make = ( ^ Array new: 3 )
) : ( )
"""

# platform is NOT in scope (packageUsing: manifest): legacy implicit-send stays.
WITHOUT_PLATFORM = """class Probe packageUsing: manifest = () (
public make = ( ^ Array new: 3 )
) : ( )
"""


def test_capitalised_global_routes_through_platform(tmp_path):
    import ns_compile_oevm as C
    ast = _parse(WITH_PLATFORM, tmp_path)
    compiler = C.OevmCompiler(ast)
    compiler.compile()
    insts, consts = _method_using(compiler, "make")
    assert _sends_to_const(insts, consts, C.OP_SEND, "Array") >= 1, \
        "Array should be a plain OP_SEND to the platform receiver"
    assert _sends_to_const(insts, consts, C.OP_SEND_IMPLICIT, "Array") == 0, \
        "Array must NOT be an implicit-send-to-self when platform is in scope"


def test_capitalised_global_falls_back_without_platform(tmp_path):
    import ns_compile_oevm as C
    ast = _parse(WITHOUT_PLATFORM, tmp_path)
    compiler = C.OevmCompiler(ast)
    compiler.compile()
    insts, consts = _method_using(compiler, "make")
    assert _sends_to_const(insts, consts, C.OP_SEND_IMPLICIT, "Array") >= 1, \
        "without a platform in scope, Array keeps the legacy implicit-send"
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python3 -m pytest tools/test_platform_routing.py -v`
Expected: `test_capitalised_global_routes_through_platform` FAILS (today `Array` emits `OP_SEND_IMPLICIT`, so the `OP_SEND` count is 0 and the implicit count is ≥1). `test_capitalised_global_falls_back_without_platform` already PASSES (legacy behaviour) — that one is the regression guard for the fallback.

- [ ] **Step 3: Add the `platform_global` resolver branch**

In `tools/ns_compile_oevm.py`, `resolve_name`, replace the implicit-send fallback (currently `:369-370`):

```python
        # 4. Implicit send fallback
        return ("implicit_send", name)
```

with:

```python
        # 4. A capitalised name with no lexical/slot/nested-class binding is a
        #    library global (Array, OrderedCollection, ...). Route it through the
        #    handed `platform` capability when one is in lexical scope — reached
        #    as a threaded param/slot, never a VM global (capability-pure). The
        #    "platform" lookup is lowercase, so it cannot re-enter this branch.
        #    Falls back to the legacy implicit-send when no platform is in scope,
        #    keeping pre-std-lib programs green during the migration.
        if name[:1].isupper():
            plat = self.resolve_name("platform")
            if plat[0] in ("local", "lexical", "slot"):
                return ("platform_global", name)

        # 5. Implicit send fallback (lowercase self-sends; or no platform handed)
        return ("implicit_send", name)
```

- [ ] **Step 4: Add the `platform_global` emit branch (varRef read)**

In `tools/ns_compile_oevm.py`, `compile_expr`, the `varRef` arm, immediately after the `implicit_send` branch (which ends at `:516` with `self.free_reg()`), add:

```python
            elif res[0] == "platform_global":
                # `Name` (capitalised library global) => `platform Name`: a unary
                # send to the handed platform capability. platform is loaded via
                # the ordinary varRef path (local/lexical/slot), so it is reached
                # through the lexically-threaded slot, not a VM global. send()
                # intercepts the capitalised selector (PRIM_Array etc.) and
                # returns the class object (mock today; real .ns class in step 3).
                plat_reg = self.alloc_reg()
                self.compile_expr({"type": "varRef", "name": "platform"}, plat_reg)
                sel_idx = self.add_constant(name)
                self.emit_ABC(OP_SEND, 0, sel_idx, plat_reg)
                self.emit_ABC(OP_LD_REG, target_reg, plat_reg, 0)
                self.free_reg()
```

(`name` is bound at the top of the `varRef` arm, `:452`.)

- [ ] **Step 5: Unify the `dynArray` Array-class acquisition through the same path**

In `tools/ns_compile_oevm.py`, `compile_expr`, the `dynArray` arm, replace the explicit implicit-send that fetches the Array class (currently `:578-581`):

```python
            arr_reg = self.alloc_reg()
            # Receiver of `new:` is the Array class (an implicit "Array" send).
            arr_cls_idx = self.add_constant("Array")
            self.emit_ABC(OP_LD_REG, arr_reg, 0, 0)                      # self
            self.emit_ABC(OP_SEND_IMPLICIT, 0, arr_cls_idx, arr_reg)     # arr_reg = Array class
```

with (acquire the Array class exactly like a bare `Array` reference, so it picks up the platform route or the legacy fallback uniformly):

```python
            arr_reg = self.alloc_reg()
            # Acquire the Array class the same way a bare `Array` reference does:
            # through the handed platform (platform_global) when one is in scope,
            # else the legacy implicit-send. Keeps `{ ... }` literals consistent
            # with explicit `Array new:` resolution.
            self.compile_expr({"type": "varRef", "name": "Array"}, arr_reg)
```

- [ ] **Step 6: Run the unit tests to verify they pass**

Run: `python3 -m pytest tools/test_platform_routing.py -v`
Expected: both tests PASS (routing test now sees `OP_SEND` to platform and zero `OP_SEND_IMPLICIT` for `Array`; fallback test still PASS).

- [ ] **Step 7: Run the full conformance suite (regression — the real safety net)**

Run: `python3 tools/run_tests_oevm.py`
Expected: 47 pass, same skip count as `master`, **both** paths. (richards/deltablue/sieve/queens all thread `platform` via `usingPlatform:` and use `Array`/`{ }`, so they exercise the new route end-to-end through the mock.)

- [ ] **Step 8: Also confirm the existing source-map suite is unaffected**

The `varRef`/`dynArray` edits change emitted instructions; confirm statement-granularity source maps still build:

Run: `python3 -m pytest tools/test_source_map.py tools/test_stmap.py -v`
Expected: all PASS.

- [ ] **Step 9: Commit**

```bash
git add tools/ns_compile_oevm.py tools/test_platform_routing.py
git commit -m "feat(stdlib): route capitalised library globals through threaded platform"
```

---

## Self-Review

**Spec coverage (against `docs/design/2026-06-20-ns-oevm-stdlib-design.md` §3 / §8 step 2):**
- "Thread `platform` through module construction" → Task 1 (real `platform_object()` into both entry seams; programs thread it via their `usingPlatform:`/`main:` params, stored to slots by the existing `compile_initializer`).
- "Resolve capitalised names via the injected namespace instead of `OP_SEND_IMPLICIT` string-match" → Task 2 (`platform_global` resolver + emit; `dynArray` unified).
- "real name resolution against the injected namespace, not a VM string-match" → satisfied at the *compiler* layer: the capitalised name is now sent to a threaded value. The VM-level `PRIM_Array` string interception remains as the bridge (chosen "bridge via platform"); its removal is spec §8 step 6, explicitly out of scope here.
- §3.1 per-target variant selection → enabled-by (capitalised names now resolve through the handed object) but not exercised until real classes land (step 3); no task needed now.
- §7 testing — "stay green on both image paths" → Task 1 Step 5, Task 2 Step 7; new-protocol/mirror tests are step 3+/step 5, out of scope.

**Placeholder scan:** none — every code step shows exact before/after text and exact run commands with expected output.

**Type/name consistency:** resolver tag `"platform_global"` is produced in Task 2 Step 3 and consumed in Step 4; the `dynArray` change (Step 5) routes through the same `varRef` path that handles it. `platform_object()` (Task 1) matches the existing `../oevm/src/oevm.h:295` declaration and step-1 `dispatch.c:439` definition. Opcode names (`OP_SEND`, `OP_SEND_IMPLICIT`, `OP_LD_REG`) match `tools/ns_compile_oevm.py` and are referenced via `C.<NAME>` in tests. Instruction decode `(w>>16)&0xFF` for the constant index matches `emit_ABC`'s `(b&0xFF)<<16` packing for `OP_SEND*` (where `b == cidx`).

**Carry-forward from step 1 (deferred Minors — fold opportunistically, not gating):** add `fflush(stdout)` to platform `print:`/`printNewline:` in `dispatch.c`; add an `indexable_new` prototype to `oevm.h`; 3 clarifying comments. These touch `../oevm` files adjacent to this step; address if convenient during Task 1's `oimg_loader.c` commit, otherwise defer to step 3.
