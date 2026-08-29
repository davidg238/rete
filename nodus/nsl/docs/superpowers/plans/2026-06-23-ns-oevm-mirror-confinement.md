# Mirror Confinement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Flip privileged reflection from **ambient** (any receiver, by selector name) to **confined** (reachable only by holding the `mirror` capability object), completing the three-tier capability model.

**Architecture:** Reuse the serial/platform pattern — one capability = one C module (`mirror.c`) + one receiver-identity hook in `send()`. The 12 reflective prim *bodies* in `vm.c` are **untouched**; `mirror_dispatch` re-exposes them mirror-mediated (reflectee moves from receiver → arg 0), and the corresponding `case` blocks are **removed from the universal `switch(pid)`** in `dispatch.c` so an ordinary receiver sending `sender`/`pc`/etc. now falls through to normal vtable lookup → DNU. **No grant this cycle:** no boot path hands a mirror to any `.ns` code, so confinement is total.

**Tech Stack:** C99 (oevm VM), Python build/test harness (ns-oevm), Newspeak conformance tests (ns-tests). No new `.oimg` format version, no registry bump, no GC root (static BSS singleton).

## Global Constraints

- **Three sibling repos**, commit per-repo, never mix: `ns-oevm` (this repo), `../oevm` (VM/C), `../ns-tests` (conformance suite). Specs/plans commit to `ns-oevm` master.
- **Push only on David's explicit say-so.** Commit freely; do not push.
- **Every commit needs the trailers** (append to every commit message):
  ```
  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01AK6gyZpVqgoWnzDG6CvHz4
  ```
- **Locate every C edit site by the QUOTED code below, not by line number** — the two probe agents reported inconsistent `PRIM_` id values; line numbers drift. Match the verbatim strings.
- **The runner clobbers `../oevm/src/solidified_image.c` (+ `.stmap`) on every suite run.** After any `run_tests_oevm.py` run, `git -C ../oevm checkout src/solidified_image.c src/solidified_image.stmap` to discard the clobber (the mirror change does NOT alter compiler output, so the committed image must stay byte-identical).
- **Suite gate:** run from the `ns-oevm` cwd with `OEVM_PATH=AB python3 tools/run_tests_oevm.py`. **Confirm the live baseline first** (memory says **68/0/3** both paths post-stratify; the spec's "63/0/3" predates the stratify step). Target after this plan = **baseline + 1** (the new confinement pin), i.e. **69/0/3** if baseline is 68.
- **The reflectee-as-argument shape is load-bearing** for the future remote/post-mortem debugger (spec §11.3). Mirror selectors take the reflected object as arg 0 (`mirror senderOf: ctx`), never receiver-side. Do not "simplify" this back to receiver-side.

---

## File Structure

| File | Repo | Responsibility |
|---|---|---|
| `src/mirror.h` | oevm | Public interface: `mirror_object()`, `mirror_dispatch()` (new) |
| `src/mirror.c` | oevm | `&MirrorObj` singleton + `mirror_dispatch` shimming to the unchanged `vm.c` prim bodies (new) |
| `src/dispatch.c` | oevm | Add the `send()` receiver-identity hook; remove the 12 reflective `case` blocks (modify) |
| `tools/check_no_ambient_reflection.sh` | oevm | Structural guard: zero reflective `case` labels remain in the switch (new) |
| `tests/test_mirrors.c`, `tests/test_debugger.c` | oevm | Rewrite receiver-side reflective sends → mirror-mediated (modify) |
| `tools/run_tests_oevm.py` | ns-oevm | Add `src/mirror.c` to both Path A and Path B build commands (modify) |
| `tests/test_load_run.sh`, `tests/test_portability_32.sh` | oevm | Add `src/mirror.c` to the gcc source list (modify) |
| `04_somns_basic/40_mirror_confined.ns` | ns-tests | Negative confinement pin: an app reflecting DNUs (new) |
| `docs/NEWSPEAK_DIALECT.md` | ns-oevm | Dialect entry **#15** (reflection is confined) (modify) |

---

## Authoritative classification (probe complete — §4 of the spec)

The probe ran during planning (the stratify-execution step already replaced every base-level use of these selectors with dedicated opcodes). Result:

**MOVE behind the mirror (12 selectors — all pure reflection, zero `.ns`/compiler sends):**
`vtable`, `delegate`, `bindings`, `sender`, `pc`, `home`, `regAt:`, `regAt:put:`, `singleStep:`, `bind:to:`, `instructionAt:put:`, `registerDebuggerHook:`

> `home` was a §3 "candidate pending probe" — the probe confirms `OP_LD_LEXICAL` replaced all base-level `home` uses, so it moves.

**STAYS universal (must NOT be confined — load-bearing):**
`==`, `executeCleanup`, `resumeWith:`, `fiberCreate:`, `outer`, `fiberSwitch:value:`, `createClosure:`, all arithmetic/collection/boolean prims.

> `outer` is actively sent by compiled `.ns` code (lexical scoping) — confirmed load-bearing, stays.

**Mirror protocol (the new keyword selectors `mirror_dispatch` answers):**

| Mirror selector | `mirror_dispatch` args | Shims to (`vm.c`, unchanged) |
|---|---|---|
| `vtableOf: o` | `[o]` | `primitive_object_vtable(args[0], argc-1, args+1)` |
| `delegateOf: vt` | `[vt]` | `primitive_vtable_delegate(args[0], argc-1, args+1)` |
| `bindingsOf: vt` | `[vt]` | `primitive_vtable_bindings(args[0], argc-1, args+1)` |
| `senderOf: ctx` | `[ctx]` | `primitive_method_context_sender(args[0], argc-1, args+1)` |
| `pcOf: ctx` | `[ctx]` | `primitive_method_context_pc(args[0], argc-1, args+1)` |
| `homeOf: cl` | `[cl]` | `primitive_closure_home(args[0], argc-1, args+1)` |
| `reg: ctx at: i` | `[ctx,i]` | `primitive_method_context_reg_at(args[0], argc-1, args+1)` |
| `reg: ctx at: i put: v` | `[ctx,i,v]` | `primitive_method_context_reg_at_put(args[0], argc-1, args+1)` |
| `fiber: f singleStep: b` | `[f,b]` | `primitive_fiber_set_single_step(args[0], argc-1, args+1)` |
| `vtable: vt bind: s to: m` | `[vt,s,m]` | `primitive_vtable_bind(args[0], argc-1, args+1)` |
| `method: m instructionAt: i put: x` | `[m,i,x]` | `primitive_compiled_method_set_instruction_at(args[0], argc-1, args+1)` |
| `onBreak: cl` | `[cl]` | `primitive_register_debugger_hook(NULL, argc, args)` ⚠️ no reflectee |

> The uniform shim `prim(args[0], argc-1, args+1)` works for 11 selectors because every reflective prim reads its reflectee from the `receiver` slot and extra args from `args[0..]` (verified in `vm.c`). **`onBreak:` is the exception** — `primitive_register_debugger_hook` does `(void)receiver;` (vm.c:983) and reads the hook from `args[0]`, so it gets the mirror args passed straight through.

> **Removal mechanism (Option chosen — minimal & safe):** delete only the 12 `case` blocks from `switch(pid)`. **Leave the enum and `PRIM_NAMES` entries intact.** A removed case → `default: break;` → falls through to normal vtable dispatch → DNU. Editing the parallel enum/`PRIM_NAMES` arrays (the 12 names are interspersed with kept prims) is fragile and unnecessary; behaviorally identical. This also means a user-defined method named `sender` correctly wins via vtable.

---

### Task 1: Lock classification + capture the green baseline

**Files:**
- Modify: `../oevm/src/dispatch.c` (comment only — no behavior change)

**Interfaces:**
- Produces: a recorded baseline suite count that Tasks 3–5 must not regress below (minus the deliberate +1 pin).

- [ ] **Step 1: Capture the live baseline both paths**

Run (from `ns-oevm` cwd):
```bash
cd /home/david/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | tail -20
```
Expected: a pass/fail/skip summary (memory predicts **68/0/3** both paths). Record the exact numbers — call it **BASELINE**.

- [ ] **Step 2: Restore the clobbered default image**

Run:
```bash
git -C /home/david/workspace/oevm checkout src/solidified_image.c src/solidified_image.stmap
git -C /home/david/workspace/oevm status --short
```
Expected: clean working tree in `../oevm` (no diff).

- [ ] **Step 3: Add a classification breadcrumb comment above the switch**

In `../oevm/src/dispatch.c`, find this exact line:
```c
            switch (pid) {
```
Replace with:
```c
            /* Reflection is confined to the `mirror` capability (mirror step).
               The 12 reflective cases (vtable/delegate/bindings/sender/pc/home/
               regAt:/regAt:put:/singleStep:/bind:to:/instructionAt:put:/
               registerDebuggerHook:) were lifted out of this switch into
               mirror.c; an ordinary receiver sending them now falls through to
               normal vtable dispatch -> DNU. See tools/check_no_ambient_reflection.sh. */
            switch (pid) {
```

- [ ] **Step 4: Verify it still compiles (comment-only change)**

Run:
```bash
cd /home/david/workspace/ns-oevm && OEVM_PATH=A python3 tools/run_tests_oevm.py 2>&1 | tail -5
git -C /home/david/workspace/oevm checkout src/solidified_image.c src/solidified_image.stmap
```
Expected: Path A count unchanged from BASELINE; `../oevm` clean after checkout.

- [ ] **Step 5: Commit**

```bash
cd /home/david/workspace/oevm
git add src/dispatch.c
git commit -m "docs(mirror): breadcrumb above switch(pid) for reflection confinement

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01AK6gyZpVqgoWnzDG6CvHz4"
```

---

### Task 2: The `mirror.c` module (built, not yet hooked)

**Files:**
- Create: `../oevm/src/mirror.h`
- Create: `../oevm/src/mirror.c`
- Modify: `../oevm/tests/test_load_run.sh`
- Modify: `../oevm/tests/test_portability_32.sh`
- Modify: `tools/run_tests_oevm.py` (ns-oevm)

**Interfaces:**
- Produces: `oop mirror_object(void)` and `oop mirror_dispatch(oop selector, int argc, oop *args)` — consumed by the `send()` hook in Task 3.
- Consumes: the 12 `primitive_*` declarations from `../oevm/src/oevm.h` (lines 298–324), `VtableVtable`, `symbol_name`, `struct Object`/`struct Vtable`.

- [ ] **Step 1: Create `../oevm/src/mirror.h`**

```c
#ifndef OEVM_MIRROR_H
#define OEVM_MIRROR_H
#include "oevm.h"
oop mirror_object(void);                                 /* lazy &MirrorObj singleton */
oop mirror_dispatch(oop selector, int argc, oop *args);  /* confined reflection */
#endif
```

- [ ] **Step 2: Create `../oevm/src/mirror.c`**

```c
/* The mirror capability: confined privileged reflection (mirror step).
   One capability = one C module + one receiver-identity hook in send() (the
   serial.c / platform.c pattern). The reflective prim BODIES live in vm.c and are
   UNCHANGED; this module only re-exposes them mirror-mediated -- the reflected
   object moves from the prim's receiver slot to arg 0 (`mirror senderOf: ctx`).
   Because an ordinary receiver can no longer name the mirror, and the reflective
   `case` blocks were removed from the universal switch(pid) in dispatch.c, sending
   `sender`/`pc`/etc. to any other receiver falls through to vtable lookup -> DNU.
   That DNU is the confinement.

   No GC root: &MirrorObj and its vtable are static BSS, outside the heap range
   gc.c scans (is_valid_heap_object), exactly like &SerialObj / &PlatformObj.

   BUILD SEAM (designed, NOT wired this cycle -- spec ss6): reflection is now
   self-contained here and reached only via the single send() hook, so a production
   target that grants no mirror can drop the whole reflection surface from the
   binary. To do that later: (1) wrap the `if (receiver == mirror_object())` line in
   dispatch.c with `#ifndef OEVM_NO_REFLECTION`, and (2) remove src/mirror.c from the
   build source lists (run_tests_oevm.py, test_load_run.sh, test_portability_32.sh).
   Nothing else changes -- the vm.c prim bodies are not guarded this cycle. */
#include "mirror.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct Object MirrorObj;
static struct Vtable MirrorVtable;
static int mirror_initialized = 0;

oop mirror_object(void) {
    if (!mirror_initialized) {
        MirrorVtable.base.vtable = VtableVtable;
        MirrorVtable.delegate = NULL;
        MirrorVtable.bindings = NULL;
        MirrorVtable.capacity = 0;
        MirrorVtable.size = 0;
        MirrorVtable.is_nested = 0;
        MirrorObj.vtable = (oop)&MirrorVtable;
        mirror_initialized = 1;
    }
    return (oop)&MirrorObj;
}

/* Each branch shims to the unchanged vm.c prim. The reflectee is arg 0, so the
   prim's receiver := args[0] and its own args := args+1 (argc-1). The one
   exception is onBreak:, whose prim ignores receiver and reads the hook from
   args[0] -- pass the mirror args straight through. */
oop mirror_dispatch(oop selector, int argc, oop *args) {
    const char *sel = symbol_name((struct Symbol *)selector);
    /* read-side queries */
    if (strcmp(sel, "vtableOf:") == 0)   return primitive_object_vtable(args[0], argc - 1, args + 1);
    if (strcmp(sel, "delegateOf:") == 0) return primitive_vtable_delegate(args[0], argc - 1, args + 1);
    if (strcmp(sel, "bindingsOf:") == 0) return primitive_vtable_bindings(args[0], argc - 1, args + 1);
    if (strcmp(sel, "senderOf:") == 0)   return primitive_method_context_sender(args[0], argc - 1, args + 1);
    if (strcmp(sel, "pcOf:") == 0)       return primitive_method_context_pc(args[0], argc - 1, args + 1);
    if (strcmp(sel, "homeOf:") == 0)     return primitive_closure_home(args[0], argc - 1, args + 1);
    if (strcmp(sel, "reg:at:") == 0)     return primitive_method_context_reg_at(args[0], argc - 1, args + 1);
    if (strcmp(sel, "reg:at:put:") == 0) return primitive_method_context_reg_at_put(args[0], argc - 1, args + 1);
    /* mutations */
    if (strcmp(sel, "fiber:singleStep:") == 0)         return primitive_fiber_set_single_step(args[0], argc - 1, args + 1);
    if (strcmp(sel, "vtable:bind:to:") == 0)           return primitive_vtable_bind(args[0], argc - 1, args + 1);
    if (strcmp(sel, "method:instructionAt:put:") == 0) return primitive_compiled_method_set_instruction_at(args[0], argc - 1, args + 1);
    /* onBreak: -- no reflectee; the closure IS args[0] (prim does (void)receiver). */
    if (strcmp(sel, "onBreak:") == 0)    return primitive_register_debugger_hook(NULL, argc, args);
    /* Unknown selector on the mirror capability: DNU. */
    fprintf(stderr, "VM Error: Mirror doesNotUnderstand #%s\n", sel);
    abort();
}
```

- [ ] **Step 3: Wire `mirror.c` into the conformance build (both paths)**

In `tools/run_tests_oevm.py` (ns-oevm), find **both** occurrences of this exact line:
```python
        "src/vm.c", "src/gc.c", "src/dispatch.c", "src/platform.c", "src/serial.c",
```
Replace each with:
```python
        "src/vm.c", "src/gc.c", "src/dispatch.c", "src/platform.c", "src/serial.c",
        "src/mirror.c",
```
(There are two — the Path B image-VM build and the Path A appliance build. Use `replace_all`.)

- [ ] **Step 4: Wire `mirror.c` into the oevm C-test shell scripts**

In `../oevm/tests/test_load_run.sh`, find:
```sh
gcc -Wall -std=c99 -Isrc src/vm.c src/gc.c src/dispatch.c src/platform.c "src/backend/$TARGET/platform_$TARGET.c" src/serial.c "src/backend/$TARGET/serial_$TARGET.c" src/oimg_loader.c \
```
Replace `src/serial.c "src/backend/$TARGET/serial_$TARGET.c"` with `src/serial.c "src/backend/$TARGET/serial_$TARGET.c" src/mirror.c`.

In `../oevm/tests/test_portability_32.sh`, find:
```sh
gcc -m32 -Wall -std=c99 -Isrc src/vm.c src/gc.c src/dispatch.c src/platform.c "src/backend/$TARGET/platform_$TARGET.c" src/serial.c "src/backend/$TARGET/serial_$TARGET.c" src/oimg_loader.c src/kernel_boot.c oevm_image_main.c -o /tmp/oevm_img32 -lm
```
Replace `src/serial.c "src/backend/$TARGET/serial_$TARGET.c"` with `src/serial.c "src/backend/$TARGET/serial_$TARGET.c" src/mirror.c`.

- [ ] **Step 5: Verify it compiles and links (mirror not yet referenced — proves no link error from the new unit)**

Run:
```bash
cd /home/david/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | tail -5
git -C /home/david/workspace/oevm checkout src/solidified_image.c src/solidified_image.stmap
```
Expected: count == BASELINE (mirror.c compiles but is dead code — `mirror_object`/`mirror_dispatch` unreferenced is fine for a non-`-Werror` build; the conformance build uses `-Wall -Wextra` without `-Werror`, so an unused-function warning does not fail it). If it *does* fail on an unused-function error, proceed to Task 3 which references both symbols.

- [ ] **Step 6: Commit (per repo)**

```bash
cd /home/david/workspace/oevm
git add src/mirror.h src/mirror.c tests/test_load_run.sh tests/test_portability_32.sh
git commit -m "feat(mirror): mirror.c capability module shimming reflective prims

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01AK6gyZpVqgoWnzDG6CvHz4"

cd /home/david/workspace/ns-oevm
git add tools/run_tests_oevm.py
git commit -m "build(mirror): link src/mirror.c into Path A and Path B

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01AK6gyZpVqgoWnzDG6CvHz4"
```

---

### Task 3: Confine — add the `send()` hook, remove the 12 switch cases, add the structural guard

**Files:**
- Modify: `../oevm/src/dispatch.c`
- Create: `../oevm/tools/check_no_ambient_reflection.sh`

**Interfaces:**
- Consumes: `mirror_object()`, `mirror_dispatch()` from Task 2.
- Produces: confined reflection — reflective selectors DNU on ordinary receivers; reachable only via `mirror_object()`.

- [ ] **Step 1: Write the structural guard (it must FAIL now — cases still present)**

Create `../oevm/tools/check_no_ambient_reflection.sh`:
```sh
#!/bin/sh
# Structural guard (mirror step): the 12 reflective prims must NOT be reachable as
# ambient receiver-side primitives -- they live behind the `mirror` capability only.
# Fails if any reflective `case PRIM_x:` reappears in the universal switch(pid) of
# dispatch.c. Run from the oevm repo root. Companion to the conformance suite, which
# proves nothing load-bearing was confined; this proves confinement actually happened
# (a behavior oracle alone cannot -- a still-working ambient prim looks identical).
set -e
DISPATCH="src/dispatch.c"
hits=$(grep -nE 'case PRIM_(vtable|delegate|bindings|sender|pc|home|registerDebuggerHook|regAt|regAt_put|singleStep|bind|instructionAt_put):' "$DISPATCH" || true)
if [ -n "$hits" ]; then
  echo "FAIL: ambient reflective case(s) still present in switch(pid):"
  echo "$hits"
  exit 1
fi
echo "OK: no ambient reflective cases in dispatch.c switch(pid)"
```

- [ ] **Step 2: Run the guard — verify it FAILS (cases still there)**

Run:
```bash
cd /home/david/workspace/oevm && chmod +x tools/check_no_ambient_reflection.sh && sh tools/check_no_ambient_reflection.sh
```
Expected: `FAIL: ambient reflective case(s) still present...` listing all 12 cases, exit 1.

- [ ] **Step 3: Add the `#include` and the receiver-identity hook in `send()`**

In `../oevm/src/dispatch.c`, find the existing serial gate (exact):
```c
    if (receiver == serial_object()) return serial_dispatch(selector, argc, args);
```
Replace with:
```c
    if (receiver == serial_object()) return serial_dispatch(selector, argc, args);
    /* Confined reflection: the mirror capability (mirror step). Receiver-identity
       gate; own selector matching in mirror.c, adds nothing to PRIM_NAMES. A
       confined app never names the mirror, so it never reaches mirror_dispatch. */
    if (receiver == mirror_object()) return mirror_dispatch(selector, argc, args);
```

Then add the include. Find the existing serial include (or platform include) near the top of `dispatch.c`:
```c
#include "serial.h"
```
Replace with:
```c
#include "serial.h"
#include "mirror.h"
```
(If `dispatch.c` does not `#include "serial.h"` directly, add `#include "mirror.h"` immediately after the last existing `#include "..."` of a project header near the top of the file.)

- [ ] **Step 4: Remove the first contiguous block of 6 reflective cases**

In `../oevm/src/dispatch.c`, find this exact block (the six cases between `switch (pid) {` and `case PRIM_eqeq:`):
```c
            case PRIM_vtable: {
                return primitive_object_vtable(receiver, argc, args);
            }
            break;
            case PRIM_delegate: {
                return primitive_vtable_delegate(receiver, argc, args);
            }
            break;
            case PRIM_bindings: {
                return primitive_vtable_bindings(receiver, argc, args);
            }
            break;
            case PRIM_sender: {
                return primitive_method_context_sender(receiver, argc, args);
            }
            break;
            case PRIM_pc: {
                return primitive_method_context_pc(receiver, argc, args);
            }
            break;
            case PRIM_home: {
                return primitive_closure_home(receiver, argc, args);
            }
            break;
            case PRIM_eqeq: {
```
Replace with (keeping `PRIM_eqeq` and everything after it):
```c
            case PRIM_eqeq: {
```

- [ ] **Step 5: Remove the second contiguous block of 6 reflective cases**

In `../oevm/src/dispatch.c`, find this exact block (between `case PRIM_fiberSwitch:` ... and `case PRIM_createClosure:`):
```c
            case PRIM_registerDebuggerHook: {
                return primitive_register_debugger_hook(receiver, argc, args);
            }
            break;
            case PRIM_regAt: {
                return primitive_method_context_reg_at(receiver, argc, args);
            }
            break;
            case PRIM_regAt_put: {
                return primitive_method_context_reg_at_put(receiver, argc, args);
            }
            break;
            case PRIM_singleStep: {
                return primitive_fiber_set_single_step(receiver, argc, args);
            }
            break;
            case PRIM_bind: {
                return primitive_vtable_bind(receiver, argc, args);
            }
            break;
            case PRIM_instructionAt_put: {
                return primitive_compiled_method_set_instruction_at(receiver, argc, args);
            }
            break;
            case PRIM_createClosure: {
```
Replace with (keeping `PRIM_createClosure`):
```c
            case PRIM_createClosure: {
```

- [ ] **Step 6: Run the guard — verify it now PASSES**

Run:
```bash
cd /home/david/workspace/oevm && sh tools/check_no_ambient_reflection.sh
```
Expected: `OK: no ambient reflective cases in dispatch.c switch(pid)`, exit 0.

- [ ] **Step 7: Run the full suite both paths — must stay at BASELINE (no load-bearing prim confined)**

Run:
```bash
cd /home/david/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | tail -8
git -C /home/david/workspace/oevm checkout src/solidified_image.c src/solidified_image.stmap
```
Expected: count == BASELINE both paths (e.g. 68/0/3). **If any test regresses, a load-bearing prim was confined** — STOP, use systematic-debugging, and re-check the classification (default-to-universal). Do not proceed.

- [ ] **Step 8: Commit**

```bash
cd /home/david/workspace/oevm
git add src/dispatch.c tools/check_no_ambient_reflection.sh
git commit -m "feat(mirror): confine reflection behind mirror capability

Add the send() receiver-identity hook routing to mirror_dispatch; remove the 12
reflective cases from the universal switch(pid) so ordinary receivers DNU.
Structural guard tools/check_no_ambient_reflection.sh proves the cases are gone.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01AK6gyZpVqgoWnzDG6CvHz4"
```

---

### Task 4: Rewrite the C tests mirror-mediated (positive proof the prims still work)

**Files:**
- Modify: `../oevm/tests/test_mirrors.c`
- Modify: `../oevm/tests/test_debugger.c`

**Interfaces:**
- Consumes: `mirror_object()` (via `#include "../src/mirror.h"`), the mirror protocol selectors from Task 2.

> **Scope note:** `docs/archive/ROADMAP.md:214` records that `test_mirrors.c` / `test_solidified.c` "don't link" today (pre-existing, unrelated to this change). These C tests are **manual** (not in the conformance runner). Goal here: convert the receiver-side reflective sends to mirror-mediated and confirm they build+pass against the documented link line **augmented with `src/mirror.c` + `src/platform.c` + `src/serial.c` + the posix backend**. If the test is pre-broken for reasons unrelated to confinement (e.g. missing solidified symbols), file/append to a tracking issue (`oevm`) and rely on the negative pin (Task 5) + the conformance suite as the shipped proof — do not rabbit-hole reviving unrelated breakage.

- [ ] **Step 1: Rewrite `test_mirrors.c` reflective sends to mirror-mediated**

In `../oevm/tests/test_mirrors.c`, the reflective inspection sends the selectors receiver-side, e.g.:
```c
        oop sender = send(frame, sym_sender, 0, NULL);
        oop pc_smi = send(frame, sym_pc, 0, NULL);
```
Change the selector symbols to the mirror keyword selectors and send to `mirror_object()` with the reflectee as arg 0. First update the symbol setup — find:
```c
    oop sym_sender = gc_allocate(SymbolVtable, sizeof(struct Symbol));
    ((struct Symbol *)sym_sender)->name = "sender";
    ((struct Symbol *)sym_sender)->length = 6;

    oop sym_pc = gc_allocate(SymbolVtable, sizeof(struct Symbol));
    ((struct Symbol *)sym_pc)->name = "pc";
    ((struct Symbol *)sym_pc)->length = 2;
```
Replace with:
```c
    oop sym_sender = gc_allocate(SymbolVtable, sizeof(struct Symbol));
    ((struct Symbol *)sym_sender)->name = "senderOf:";
    ((struct Symbol *)sym_sender)->length = 9;

    oop sym_pc = gc_allocate(SymbolVtable, sizeof(struct Symbol));
    ((struct Symbol *)sym_pc)->name = "pcOf:";
    ((struct Symbol *)sym_pc)->length = 5;
```
Then find the receiver-side sends:
```c
        oop sender = send(frame, sym_sender, 0, NULL);
        oop pc_smi = send(frame, sym_pc, 0, NULL);
```
Replace with (reflectee as arg 0, sent to the mirror):
```c
        oop sender = send(mirror_object(), sym_sender, 1, &frame);
        oop pc_smi = send(mirror_object(), sym_pc, 1, &frame);
```
Add the include near the other includes at the top of the file — find:
```c
#include "../src/oevm.h"
```
Replace with:
```c
#include "../src/oevm.h"
#include "../src/mirror.h"
```
Apply the same transformation to the `delegate`/`bindings` sends in this file: change selector `"delegate"`→`"delegateOf:"` (length 11), `"bindings"`→`"bindingsOf:"` (length 11), and change `send(CounterVtable, sym_delegate, 0, NULL)` → `send(mirror_object(), sym_delegate, 1, &CounterVtable)` (and likewise for bindings). Use the actual local variable names present in the file as the reflectee.

- [ ] **Step 2: Rewrite `test_debugger.c` reflective sends to mirror-mediated**

In `../oevm/tests/test_debugger.c`, convert each receiver-side reflective send. Examples present in the file:
- `send(context, create_symbol("pc"), 0, NULL)` → `{ oop _r = context; send(mirror_object(), create_symbol("pcOf:"), 1, &_r); }`
- `send(context, Symbol_regAt, 1, r3_args)` (where `r3_args = {index}`) → build `oop ra[] = { context, r3_args[0] };` then `send(mirror_object(), create_symbol("reg:at:"), 2, ra)`
- `send(context, Symbol_regAtPut, 2, val_args)` (where `val_args = {index, value}`) → build `oop rap[] = { context, val_args[0], val_args[1] };` then `send(mirror_object(), create_symbol("reg:at:put:"), 3, rap)`
- `send(NULL, Symbol_registerDebuggerHook, 1, reg_args)` → `send(mirror_object(), create_symbol("onBreak:"), 1, reg_args)`

Add `#include "../src/mirror.h"` after `#include "../src/oevm.h"`. Use whatever symbol-construction helper the file already uses (`create_symbol(...)` if present, else replicate the existing `gc_allocate(SymbolVtable, ...)` idiom with the correct `name`/`length`).

- [ ] **Step 3: Build and run `test_mirrors` with mirror linked in**

Run (the phase3 link line augmented with the current capability deps + mirror.c):
```bash
cd /home/david/workspace/oevm && TARGET=posix \
gcc -Wall -Wextra -std=c99 -Isrc tests/test_mirrors.c src/solidified_image.c \
  src/dispatch.c src/vm.c src/gc.c src/platform.c "src/backend/posix/platform_posix.c" \
  src/serial.c "src/backend/posix/serial_posix.c" src/mirror.c \
  -lm -o /tmp/test_mirrors && /tmp/test_mirrors; echo "exit=$?"
git -C /home/david/workspace/oevm checkout src/solidified_image.c src/solidified_image.stmap
```
Expected: the stack/vtable reflection output prints and the test asserts pass (`exit=0`). If it fails to **link** for reasons unrelated to confinement (missing solidified symbols per ROADMAP:214), record that in the commit message / a tracking issue and continue — the conformance + negative pin carry the shipped proof.

- [ ] **Step 4: Build and run `test_debugger` with mirror linked in**

Run:
```bash
cd /home/david/workspace/oevm && TARGET=posix \
gcc -Wall -Wextra -std=c99 -Isrc tests/test_debugger.c src/solidified_image.c \
  src/dispatch.c src/vm.c src/gc.c src/platform.c "src/backend/posix/platform_posix.c" \
  src/serial.c "src/backend/posix/serial_posix.c" src/mirror.c \
  -lm -o /tmp/test_debugger && /tmp/test_debugger; echo "exit=$?"
git -C /home/david/workspace/oevm checkout src/solidified_image.c src/solidified_image.stmap
```
Expected: `exit=0` (or pre-existing unrelated failure recorded per the scope note — e.g. oevm#12 already tracks a `test_debugger` Test 2 single-step issue).

- [ ] **Step 5: Commit**

```bash
cd /home/david/workspace/oevm
git add tests/test_mirrors.c tests/test_debugger.c
git commit -m "test(mirror): rewrite C reflection tests mirror-mediated

senderOf:/pcOf:/delegateOf:/bindingsOf:/reg:at:/reg:at:put:/onBreak: sent to
mirror_object() with the reflectee as arg 0 -- proves the prims still work through
the mirror after confinement.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01AK6gyZpVqgoWnzDG6CvHz4"
```

---

### Task 5: Confinement pin + dialect entry + final image hygiene

**Files:**
- Create: `../ns-tests/04_somns_basic/40_mirror_confined.ns`
- Modify: `docs/NEWSPEAK_DIALECT.md` (ns-oevm)

**Interfaces:**
- Consumes: the confined dispatch from Task 3 (reflective selector → DNU).
- Produces: the +1 conformance pin (BASELINE → BASELINE+1).

- [ ] **Step 1: Write the negative confinement pin**

Create `../ns-tests/04_somns_basic/40_mirror_confined.ns`:
```newspeak
(*
Confinement pin: reflection is no longer ambient (mirror step). An ordinary app
cannot reflect -- sending a reflective selector (`sender`) to any receiver is now a
runtime DNU, because the 12 reflective prims were removed from the universal
switch(pid) and no boot path hands this app a `mirror` capability. The compiler
exits 0 (`sender` is just an unknown unary send); the runner aborts with DNU. Like
39_serial_confined.ns, the failure IS the passing evidence that reflection is
confined and no grant exists this cycle.
expect-runtime-fail: Message 'sender' not understood by receiver
*)
class MirrorConfinedTest usingPlatform: platform = () (
  public main: platform2 args: args = (
    self sender.
    'SHOULD NOT REACH' println.
    ^ 0
  )
) : (
  public packageUsing: manifest = ( ^ self new )
)
```

- [ ] **Step 2: Run the new test alone — verify it's counted as a PASS (the DNU is expected)**

Run:
```bash
cd /home/david/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | tail -10
git -C /home/david/workspace/oevm checkout src/solidified_image.c src/solidified_image.stmap
```
Expected: total count == **BASELINE + 1** both paths (the new file passes via `expect-runtime-fail`). Confirm the runner reports `40_mirror_confined.ns` as passing (the abort message matches `Message 'sender' not understood by receiver`).

> If the actual DNU text differs from the `expect-runtime-fail:` line, update the directive in the test to match the runner's exact stderr (it must be a substring of the abort output — compare against how `39_serial_confined.ns` matches `Message 'serial' not understood by receiver`).

- [ ] **Step 3: Add dialect entry #15**

In `docs/NEWSPEAK_DIALECT.md` (ns-oevm), find the last table row (entry **#14**, the stratify-execution row — it ends with `...fully mechanical, no semantic change. |`). Append a new row immediately after it:
```markdown
| 15 | **Reflection is a confined capability, not ambient** (mirror step) | Reflection (stack walk, frame read/write, class introspection, single-step, hot-swap, break-hook registration) is ambient Object protocol -- any receiver answers `sender`/`pc`/`vtable`/`regAt:`/etc. | Reflection is reachable **only** through a `mirror` capability object, in mirror-mediated form (`mirror senderOf: ctx`, not `ctx sender`); the reflectee is always an argument, never the receiver. The 12 reflective prims (`vtable`/`delegate`/`bindings`/`sender`/`pc`/`home`/`regAt:`/`regAt:put:`/`singleStep:`/`bind:to:`/`instructionAt:put:`/`registerDebuggerHook:`) were lifted out of the universal `switch(pid)` into `mirror.c` (one capability = one C module + one receiver-identity hook, the serial/platform pattern); sending them to an ordinary receiver is now a DNU. There is **no ambient reflection protocol on Object**, and **no boot path grants a mirror this cycle** -- so no `.ns` code can reflect at all yet (grant is designed later with the debugger). Builds on #14: because base-level execution uses dedicated opcodes (no reflective sends), the full reflective surface is confinable without disturbing ordinary execution. The reflectee-as-argument shape is load-bearing for the future remote/post-mortem debugger (a second mirror impl needs no protocol change). | Bracha-Ungar mirror principles: stratification (reflection imposes no cost unused and is structurally removable -- `mirror.c` can be omitted at link time, spec ss6) and encapsulation at the gateway. Confinement is master design ss2.1; matches SOMns's segregated `vmMirror`. The probe confirmed all 12 selectors are pure reflection (zero `.ns`/compiler sends after #14). | If read-only introspection (e.g. `class`) is ever wanted as universal Object protocol, carve out a documented read-only subset then (reversible -- re-add specific universal cases). Granting a mirror, and an HLL `ObjectMirror`/`ClassMirror` facade returning abstract mirrors instead of raw oops, are the next steps (in-image debugger). |
```

> The spec §8 said "#14" but #14 was consumed by the stratify-execution step; this is **#15**.

- [ ] **Step 4: Final full suite + image hygiene**

Run:
```bash
cd /home/david/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | tail -8
sh /home/david/workspace/oevm/tools/check_no_ambient_reflection.sh
git -C /home/david/workspace/oevm checkout src/solidified_image.c src/solidified_image.stmap
git -C /home/david/workspace/oevm status --short
git -C /home/david/workspace/ns-tests status --short
```
Expected: suite == **BASELINE + 1** both paths; guard prints `OK`; `../oevm` clean after checkout; `../ns-tests` shows only the new untracked test file.

- [ ] **Step 5: Commit (per repo)**

```bash
cd /home/david/workspace/ns-tests
git add 04_somns_basic/40_mirror_confined.ns
git commit -m "test(mirror): 40_mirror_confined.ns -- reflection DNUs without a mirror

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01AK6gyZpVqgoWnzDG6CvHz4"

cd /home/david/workspace/ns-oevm
git add docs/NEWSPEAK_DIALECT.md docs/superpowers/plans/2026-06-23-ns-oevm-mirror-confinement.md
git commit -m "docs(mirror): dialect #15 (reflection confined) + implementation plan

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01AK6gyZpVqgoWnzDG6CvHz4"
```

---

## Self-Review

**Spec coverage:**
- §0/§5 confinement-only, no grant → Tasks 3 (hook + removal) + 5 (negative pin proves no grant). ✓
- §2 one C module + one hook, no GC root, no format bump → Task 2 (`mirror.c` static BSS singleton). ✓
- §3 protocol surface (11 rows + `homeOf:`) → Task 2 `mirror_dispatch` table (12 selectors). ✓
- §4 classify move/stay/probe; `home` resolved, `outer` stays → done in planning; recorded in "Authoritative classification". ✓
- §6 build seam documented (not wired) → Task 2 `mirror.c` header comment. ✓
- §7 testing: C-test rewrite (Task 4), negative `.ns` pin (Task 5), full suite both paths (every task), image regen/hygiene (Tasks 1/5 `git checkout`). ✓
- §8 dialect entry → Task 5 (as **#15**, spec's #14 was taken). ✓
- §9 risks: mis-classification gated by full suite (Task 3 Step 7); structural guard catches "removal didn't happen" (behavior oracle alone can't — [[behavior-proofs-vs-intent-proofs]]); C-test breakage expected → Task 4 scope note. ✓
- §11 reflectee-as-argument preserved for remote/post-mortem → enforced in mirror protocol + Global Constraints. ✓

**Placeholder scan:** every code/command step contains literal content; no TBD/TODO. ✓

**Type consistency:** `mirror_object()`/`mirror_dispatch(oop,int,oop*)` defined in Task 2, consumed identically in Tasks 3–4; shim arg convention `prim(args[0], argc-1, args+1)` (with `onBreak:` exception `prim(NULL, argc, args)`) stated once and reused. Mirror selector strings + lengths consistent between `mirror.c` (Task 2) and the C-test rewrite (Task 4). ✓

**Open verification deferred to execution:** confirm the live BASELINE (Task 1) and the exact DNU stderr string (Task 5 Step 2) against the running system — both have explicit "if it differs, adjust" instructions.
