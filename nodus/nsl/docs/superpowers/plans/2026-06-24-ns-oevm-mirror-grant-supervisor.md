# Mirror grant → Supervisor boot citizen (Plan 1 of 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Mint an unconditional in-image `Supervisor` at boot, grant it the native `mirror_object()` capability, and route the app launch through it (gated by `OEVM_DEBUG`) — on both boot paths — so the confined reflection capability finally has a holder, proven end-to-end.

**Architecture:** A new `lib/supervisor.ns` module (`Supervisor usingMirror: mirror`) is compiled into every image as a top-level sibling of `Kernel`. The C boot (Path A: emitted by `tools/ns_compile_oevm.py`; Path B: `oevm/src/oimg_loader.c` + a new `.oimg` format-v5 descriptor) allocates the supervisor, sends it `init_usingMirror:` with `mirror_object()`, then invokes `run:platform:args:debug:` instead of calling the app's `main:args:` directly. The supervisor forwards to the app transparently; when `OEVM_DEBUG` is set it first prints one `dbg:ready` line using the granted mirror (an un-granted app would DNU on the same reflective send). The interactive command loop and facade are deferred to Plan 2.

**Tech Stack:** C99 (oevm VM), Python 3 (ns-oevm compiler/solidifier/image serializer), Newspeak-dialect `.ns` (kernel/supervisor library), Node Ohm parser (`grammar/`).

## Global Constraints

- C standard: **C99** (`-std=c99`), warnings clean under `-Wall -Wextra`.
- Three sibling repos: `ns-oevm` (cwd), `../oevm`, `../ns-tests`. Commit **per-repo**; required trailers on every commit.
- **Push only on explicit say-so** — all commits here stay local.
- The runner **clobbers** `../oevm/src/solidified_image.c` (+`.stmap`) every run — `git checkout` those after any suite run.
- Build/run from `ns-oevm` cwd: `OEVM_PATH=AB python3 tools/run_tests_oevm.py`.
- Suite baseline that MUST stay green with the gate off: **69 pass / 0 fail / 3 skip, both paths**.
- The app **never** names the mirror; only the `Supervisor` (boot infrastructure) holds it. The `check_no_ambient_reflection.sh` structural guard must still pass.
- `node_modules` for the parser live at `grammar/node_modules`.

---

## File Structure

| File | Responsibility | Change |
| --- | --- | --- |
| `../oevm/src/mirror.c` | confined reflection dispatch | add `argc>=1` guard (oevm#15) |
| `../oevm/tools/check_no_ambient_reflection.sh` | structural guard | fix cwd path resolution (oevm#16) |
| `../oevm/tests/test_debugger.c` | C-level prim/debug proof | revive in the harness (oevm#14) |
| `lib/supervisor.ns` | the Supervisor module | **create** |
| `tools/ns_compile_oevm.py` | Path A boot emit + module load + Path B header args | modify |
| `tools/oimg_serialize.py` | `.oimg` header (format v5 supervisor descriptor) | modify |
| `../oevm/src/oimg_loader.c` | Path B boot: read descriptor, mint/grant/route | modify |
| `ns-tests/04_somns_basic/41_supervisor_grant.ns` | trivial app for the grant proof | **create** |
| `tools/test_supervisor_grant.py` | host harness: assert `dbg:ready` with gate on, silent with gate off | **create** |

---

## Task 1: Harden `mirror_dispatch` with an argc guard (oevm#15)

**Files:**
- Modify: `../oevm/src/mirror.c` (function `mirror_dispatch`, first line of body)

**Interfaces:**
- Produces: no signature change; `mirror_dispatch(selector, argc, args)` now aborts cleanly when `argc < 1` instead of dereferencing `args[0]`.

- [ ] **Step 1: Add the guard.** In `../oevm/src/mirror.c`, locate the start of `mirror_dispatch` body — the line:

```c
    const char *sel = symbol_name((struct Symbol *)selector);
```

Insert immediately **after** it:

```c
    /* oevm#15: every mirror selector reads args[0] (the reflectee). Guard before
       the first dereference so a malformed grant fails loud, not on a wild read. */
    if (argc < 1) {
        fprintf(stderr, "VM Error: Mirror #%s sent with no reflectee (argc=%d)\n", sel, argc);
        abort();
    }
```

- [ ] **Step 2: Build a C compile-check.** Run from `../oevm`:

```bash
gcc -Wall -Wextra -std=c99 -Isrc -c src/mirror.c -o /tmp/mirror.o
```

Expected: compiles with no warnings/errors.

- [ ] **Step 3: Commit (in `../oevm`).**

```bash
cd ../oevm && git add src/mirror.c && git commit -m "$(cat <<'EOF'
fix(mirror): guard mirror_dispatch against argc<1 before reflectee read (oevm#15)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01JVJv3GY4Xy4FR143b86NVt
EOF
)"
```

---

## Task 2: Fix the structural-guard cwd false-pass (oevm#16)

**Files:**
- Modify: `../oevm/tools/check_no_ambient_reflection.sh`

**Interfaces:**
- Produces: the guard resolves its target file relative to the script location and **fails loudly** (non-zero exit) when the file is missing, instead of failing open.

- [ ] **Step 1: Inspect the script.** Read `../oevm/tools/check_no_ambient_reflection.sh` and find the line that greps `dispatch.c` (currently a relative path, with a trailing `|| true` or a grep whose missing-file error is swallowed).

- [ ] **Step 2: Make paths script-relative and strict.** At the top of the script (after the shebang), add:

```bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
DISPATCH="$HERE/../src/dispatch.c"
if [ ! -f "$DISPATCH" ]; then
  echo "check_no_ambient_reflection: cannot find $DISPATCH" >&2
  exit 2
fi
```

Then change the grep target to `"$DISPATCH"` and remove any `|| true` that lets a missing file pass.

- [ ] **Step 3: Verify it passes from the repo root and fails from a wrong cwd.** Run:

```bash
cd ../oevm && bash tools/check_no_ambient_reflection.sh; echo "exit=$?"
cd /tmp && bash ~/workspace/oevm/tools/check_no_ambient_reflection.sh; echo "exit=$?"
```

Expected: first run prints its OK message, `exit=0`; second run (different cwd) **also** `exit=0` now (path is script-relative), proving it no longer depends on cwd. Then temporarily rename `src/dispatch.c` and confirm `exit=2` (fails loud), then rename back.

- [ ] **Step 4: Commit (in `../oevm`).**

```bash
cd ../oevm && git add tools/check_no_ambient_reflection.sh && git commit -m "$(cat <<'EOF'
fix(tools): resolve guard path via dirname so it fails loud, not open (oevm#16)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01JVJv3GY4Xy4FR143b86NVt
EOF
)"
```

---

## Task 3: Revive `test_debugger.c` in the test harness (closes oevm#14)

`test_debugger.c` already exists and exercises the prim-level mechanics the `.ns` debugger will build on (OP_BREAK, single-step, `reg:at:put:`, vtable hot-swap, bytecode mutation) — all through `mirror_object()`. It currently links nowhere. It needs the `init_solidified_vtables()`/`kernel_boot.c` symbols.

**Files:**
- Modify: `tools/run_tests_oevm.py` (add a standalone C-unit-test build+run step)
- Reference: `../oevm/tests/test_debugger.c`, `../oevm/src/kernel_boot.c`

**Interfaces:**
- Consumes: `mirror_object()` (mirror.c), `init_solidified_vtables()` (kernel_boot.c), `MethodContextVtable` (dispatch.c).
- Produces: a new harness step `run_c_unit_tests()` that builds and runs `test_debugger`, asserting exit 0.

- [ ] **Step 1: Confirm the build recipe works by hand.** From `../oevm`:

```bash
gcc -Wall -std=c99 -Isrc \
  src/vm.c src/gc.c src/dispatch.c src/platform.c src/backend/posix/platform_posix.c \
  src/serial.c src/backend/posix/serial_posix.c src/mirror.c src/kernel_boot.c \
  tests/test_debugger.c -o /tmp/test_debugger -lm && /tmp/test_debugger; echo "exit=$?"
```

Expected: prints the per-TEST pass lines and `All OEVM Mirror-Based Interactive Debugger and Hot-Swap tests PASSED`, `exit=0`. If it fails to link, note the undefined symbol — likely a backend file selection; reuse `*OEVM_BACKEND` as the Path B build does.

- [ ] **Step 2: Add a harness step.** In `tools/run_tests_oevm.py`, find the Path B build list (the block beginning):

```python
                "gcc", "-Wall", "-std=c99", "-Isrc",
                "src/vm.c", "src/gc.c", "src/dispatch.c", "src/platform.c", "src/serial.c",
        "src/mirror.c",
                *OEVM_BACKEND,
                "src/oimg_loader.c", "src/kernel_boot.c",
```

Add a sibling function near it (mirror its `subprocess.run` style) that builds `tests/test_debugger.c` with `src/kernel_boot.c` (NOT `oimg_loader.c`/`oevm_image_main.c`) and runs it:

```python
def run_c_unit_tests(oevm_dir):
    """Build + run standalone C unit tests (test_debugger.c) — oevm#14 proof."""
    build = subprocess.run(
        ["gcc", "-Wall", "-std=c99", "-Isrc",
         "src/vm.c", "src/gc.c", "src/dispatch.c", "src/platform.c", "src/serial.c",
         "src/mirror.c", *OEVM_BACKEND, "src/kernel_boot.c",
         "tests/test_debugger.c", "-o", "/tmp/test_debugger", "-lm"],
        cwd=oevm_dir, capture_output=True, text=True)
    if build.returncode != 0:
        print("test_debugger build FAILED:\n" + build.stderr)
        return False
    run = subprocess.run(["/tmp/test_debugger"], cwd=oevm_dir,
                         capture_output=True, text=True)
    if run.returncode != 0:
        print("test_debugger FAILED:\n" + run.stdout + run.stderr)
        return False
    print("C unit test test_debugger: PASSED")
    return True
```

- [ ] **Step 3: Call it from the harness entry.** Locate where the suite summary is printed (`main()` / the bottom of the run flow) and call `run_c_unit_tests(oevm_dir)`, folding its boolean into the overall pass/fail so a regression fails the run. Use the same `oevm_dir` variable the existing builds use.

- [ ] **Step 4: Run it.** From `ns-oevm`:

```bash
OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | grep -E "test_debugger|PASS|FAIL" | tail
git -C ../oevm checkout src/solidified_image.c src/solidified_image.stmap 2>/dev/null || true
```

Expected: `C unit test test_debugger: PASSED` in the output, suite still 69/0/3.

- [ ] **Step 5: Commit (in `ns-oevm`).**

```bash
git add tools/run_tests_oevm.py && git commit -m "$(cat <<'EOF'
test(debugger): wire test_debugger.c into the harness (closes oevm#14)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01JVJv3GY4Xy4FR143b86NVt
EOF
)"
```

---

## Task 4: Create the `Supervisor` module

**Files:**
- Create: `lib/supervisor.ns`

**Interfaces:**
- Produces: a top-level class `Supervisor usingMirror: mirror` with:
  - synthesized initializer selector `init_usingMirror:` (1 arg: the native mirror) — the boot grant target.
  - `run: entry platform: plat args: args debug: debugFlag` (4 args) — the launch entry point; returns the app's `main:args:` result.
  - Module has exactly **1 slot** (`mirror`).

- [ ] **Step 1: Write the module.** Create `lib/supervisor.ns`:

```
(*
ns-oevm supervisor (in-image debugger, step 1: the grant). A confined boot
citizen and the ONLY holder of the native mirror capability. Boot mints one
Supervisor, grants it `mirror_object()` via `init_usingMirror:`, then routes the
app launch through `run:platform:args:debug:` instead of calling the app's
`main:args:` directly. With OEVM_DEBUG off the supervisor is transparent (it just
forwards to the app, so the suite is byte-for-byte unchanged). With it on it
first prints one `dbg:ready` line that exercises the granted mirror — an
un-granted receiver could not even name `mirror`, and `vtableOf:` on a non-holder
would DNU. The interactive `dbg:*` command loop and the ObjectMirror facade
arrive in step 2 (Plan 2).
*)
class Supervisor usingMirror: mirror = () (
  (* entry: the app instance. plat: the Kernel instance the app sees as its
     `platform`. args: the app's main: args. debugFlag: 1 when OEVM_DEBUG set. *)
  public run: entry platform: plat args: args debug: debugFlag = (
    debugFlag = 1 ifTrue: [ self announceReadyFor: entry ].
    ^ entry main: plat args: args )

  (* Smoke-proof the grant: holding the mirror, vtableOf: returns the app's
     vtable (non-nil); a non-holder would DNU on the same send. *)
  announceReadyFor: entry = (
    (mirror vtableOf: entry) isNil
      ifTrue:  [ 'dbg:error no-vtable' println ]
      ifFalse: [ 'dbg:ready' println ] )
) : (
)
```

- [ ] **Step 2: Parse-check the module.** From `ns-oevm`:

```bash
node grammar/ns-parse.js lib/supervisor.ns > /tmp/sup.json && echo "parsed OK" && head -c 200 /tmp/sup.json
```

(Use the same `parse_script` path `ns_compile_oevm.py` uses for `kernel.ns`; if the script name differs, match it.) Expected: `parsed OK` and a JSON AST.

- [ ] **Step 3: Commit (in `ns-oevm`).**

```bash
git add lib/supervisor.ns && git commit -m "$(cat <<'EOF'
feat(supervisor): add Supervisor module (mirror-holding boot citizen)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01JVJv3GY4Xy4FR143b86NVt
EOF
)"
```

---

## Task 5: Path A — load the module, mint/grant/route the launch

**Files:**
- Modify: `tools/ns_compile_oevm.py` (module load near the kernel load; boot emit in `run_solidified_entry_point`)
- Create: `ns-tests/04_somns_basic/41_supervisor_grant.ns`
- Create: `tools/test_supervisor_grant.py`

**Interfaces:**
- Consumes: `Supervisor` from Task 4 (selectors `init_usingMirror:`, `run:platform:args:debug:`); `mirror_object()` (mirror.c).
- Produces: Path A images whose entry point routes through the supervisor; `dbg:ready` printed iff `OEVM_DEBUG` set.

- [ ] **Step 1: Load `supervisor.ns` alongside `kernel.ns`.** Find the kernel-load block:

```python
    kernel_ast = json.loads(kproc.stdout)
    ast["body"].extend(kernel_ast["body"])
```

Insert immediately after it:

```python
    # in-image debugger step 1: compile the Supervisor module in as a top-level
    # sibling of Kernel (same load-after-app rationale: never resolved in app scope).
    supervisor_path = os.path.join(os.path.dirname(__file__), "..", "lib", "supervisor.ns")
    sproc = subprocess.run(["node", parse_script, supervisor_path],
                           capture_output=True, text=True)
    if sproc.returncode != 0:
        print("Supervisor parse failed:")
        print(sproc.stderr or sproc.stdout)
        sys.exit(1)
    supervisor_ast = json.loads(sproc.stdout)
    ast["body"].extend(supervisor_ast["body"])
```

- [ ] **Step 2: Emit the supervisor mint + grant + routed launch.** In the boot emitter, find the final launch block:

```python
        c_code += f"    oop main_args[2] = {{ kernel_inst, fake_args }};\n"
        main_sym_var = compiler.solidify.symbols["main:args:"]
        c_code += f"    oop res = send(entry_inst, (oop)&{main_sym_var}, 2, main_args);\n"
        c_code += f"    return res;\n"
```

Replace it with:

```python
        # in-image debugger step 1: mint the Supervisor, grant it the native
        # mirror, and route the launch through it (gated by OEVM_DEBUG).
        sinit_sym = compiler.solidify.symbols["init_usingMirror:"]
        srun_sym = compiler.solidify.symbols["run:platform:args:debug:"]
        sup_nslots = len(compiler.class_slots["Supervisor"])
        c_code += "    extern oop mirror_object(void);\n"
        c_code += f"    oop supervisor_inst = gc_allocate((oop)&Vtable_Supervisor, sizeof(struct ObjectInstance) + {sup_nslots} * sizeof(oop));\n"
        c_code += "    oop sup_init_args[1] = { mirror_object() };\n"
        c_code += f"    send(supervisor_inst, (oop)&{sinit_sym}, 1, sup_init_args);\n"
        c_code += "    oop dbg_flag = getenv(\"OEVM_DEBUG\") ? TAG_SMI(1) : TAG_SMI(0);\n"
        c_code += "    oop run_args[4] = { entry_inst, kernel_inst, fake_args, dbg_flag };\n"
        c_code += f"    oop res = send(supervisor_inst, (oop)&{srun_sym}, 4, run_args);\n"
        c_code += "    return res;\n"
```

(`getenv` needs `<stdlib.h>`; the solidified file includes `oevm.h` which pulls it in. If a warning appears, add `extern char *getenv(const char *);` to the emitted prologue.)

- [ ] **Step 3: Create the trivial proof app.** Create `ns-tests/04_somns_basic/41_supervisor_grant.ns`:

```
class SupervisorGrant usingPlatform: platform = () (
  public main: platform2 args: args = (
    'app-ran' println.
    ^ 0 )
) : (
)
```

- [ ] **Step 4: Write the host harness.** Create `tools/test_supervisor_grant.py`:

```python
#!/usr/bin/env python3
"""Grant proof: with OEVM_DEBUG set, the Supervisor prints `dbg:ready` (it used
the granted mirror); with it unset, only the app output appears. Path A."""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
NSROOT = os.path.dirname(HERE)
OEVM = os.path.join(NSROOT, "..", "oevm")
APP = os.path.join(NSROOT, "..", "ns-tests", "04_somns_basic", "41_supervisor_grant.ns")

def build():
    # Compile the app to solidified_image.c, then build the Path A runner.
    subprocess.run([sys.executable, os.path.join(HERE, "ns_compile_oevm.py"), APP,
                    os.path.join(OEVM, "src", "solidified_image.c")],
                   cwd=NSROOT, check=True)
    subprocess.run(["gcc", "-Wall", "-Wextra", "-std=c99", "-Isrc", "-DOEVM_APPLIANCE",
                    "src/vm.c", "src/gc.c", "src/dispatch.c", "src/platform.c", "src/serial.c",
                    "src/mirror.c", "src/backend/posix/platform_posix.c",
                    "src/backend/posix/serial_posix.c",
                    "src/solidified_image.c", "src/oevm_main.c",
                    "-o", "/tmp/oevm_grant", "-lm"], cwd=OEVM, check=True)

def run(debug):
    env = dict(os.environ)
    if debug: env["OEVM_DEBUG"] = "1"
    else: env.pop("OEVM_DEBUG", None)
    return subprocess.run(["/tmp/oevm_grant"], cwd=OEVM, env=env,
                          capture_output=True, text=True).stdout

def main():
    build()
    on, off = run(True), run(False)
    assert "dbg:ready" in on, f"expected dbg:ready with OEVM_DEBUG=1, got:\n{on}"
    assert "app-ran" in on, f"app did not run under supervisor:\n{on}"
    assert "dbg:ready" not in off, f"dbg:ready leaked with gate off:\n{off}"
    assert "app-ran" in off, f"app did not run with gate off:\n{off}"
    print("test_supervisor_grant: PASSED (Path A)")

if __name__ == "__main__":
    main()
```

(If `ns_compile_oevm.py`'s CLI signature differs from `<app> <out.c>`, match the actual argument order used elsewhere in the repo, e.g. in `run_tests_oevm.py`.)

- [ ] **Step 5: Run it (expect FAIL first if Steps 1–2 not yet applied, then PASS).**

```bash
python3 tools/test_supervisor_grant.py
git -C ../oevm checkout src/solidified_image.c src/solidified_image.stmap 2>/dev/null || true
```

Expected after Steps 1–2: `test_supervisor_grant: PASSED (Path A)`.

- [ ] **Step 6: Commit (across both repos — note the app lives in `ns-tests`).**

```bash
git add tools/ns_compile_oevm.py tools/test_supervisor_grant.py && git commit -m "$(cat <<'EOF'
feat(boot): Path A routes app launch through mirror-granted Supervisor

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01JVJv3GY4Xy4FR143b86NVt
EOF
)"
git -C ../ns-tests add 04_somns_basic/41_supervisor_grant.ns && git -C ../ns-tests commit -m "$(cat <<'EOF'
test(supervisor): trivial app for the Path A mirror-grant proof

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01JVJv3GY4Xy4FR143b86NVt
EOF
)"
```

---

## Task 6: Path B — emit the `.oimg` format-v5 Supervisor descriptor

The `.oimg` header carries explicit ref-words for the Kernel module (vtable, init sel, nslots) and the serial selector. Path B has no generic name→vtable lookup, so the Supervisor needs the same treatment: a format-version-5 descriptor.

**Files:**
- Modify: `tools/oimg_serialize.py` (`OimgWriter.header_bytes`, `OimgSerializer.__init__` and the header-build call)
- Modify: `tools/ns_compile_oevm.py` (the Path B `OimgSerializer(...)` construction — pass supervisor names)

**Interfaces:**
- Produces: a v5 header with, after `serial_on_sel_ref` (`A+7W`):
  - `A+8W` (W) `supervisor_vtable_ref`
  - `A+9W` (W) `supervisor_init_sel_ref`
  - `A+10W` (W) `supervisor_run_sel_ref`
  - `A+11W` (u16) `supervisor_nslots` (header padded to 8)
- Consumes (in Task 7): the loader reads these when `format_version >= 5`.

- [ ] **Step 1: Extend `header_bytes`.** In `tools/oimg_serialize.py`, find the signature:

```python
    def header_bytes(self, blob_size, cluster_count, root_ref,
                     ...
                     kernel_vtable_ref=0, kernel_init_sel_ref=0, kernel_nslots=0,
                     serial_on_sel_ref=0):
```

Add params:

```python
                     serial_on_sel_ref=0,
                     supervisor_vtable_ref=0, supervisor_init_sel_ref=0,
                     supervisor_run_sel_ref=0, supervisor_nslots=0):
```

Find the format-version selection (the `if serial_on_sel_ref: ... elif kernel_vtable_ref: ...` block near line 96) and add the v5 case **first**:

```python
        if supervisor_vtable_ref:
            version = 5
        elif serial_on_sel_ref:
            version = 4
        elif kernel_vtable_ref:
            version = 3
        else:
            version = 1
```

(Match the existing variable name the function uses for the version it packs into the `<H` at offset 4.) Then, find the serial append (near line 128):

```python
        if serial_on_sel_ref:
            h += self.word(serial_on_sel_ref)
```

Append the supervisor block after it, keeping the "pad to 8" convention the existing fields use:

```python
        # in-image debugger step 1: Supervisor descriptor (format_version 5).
        # Layout from A: supervisor_vtable_ref @A+8W, init_sel @A+9W,
        # run_sel @A+10W, nslots (u16) @A+11W, header padded to 8.
        if supervisor_vtable_ref:
            h += self.word(supervisor_vtable_ref)
            h += self.word(supervisor_init_sel_ref)
            h += self.word(supervisor_run_sel_ref)
            h += self.u16(supervisor_nslots)
            while len(h) % 8 != 0:
                h += b"\x00"
```

- [ ] **Step 2: Thread names through `OimgSerializer`.** Find its `__init__` signature (near line 275):

```python
                 kernel_nslots=0, serial_on_sel_name=None):
```

Add:

```python
                 kernel_nslots=0, serial_on_sel_name=None,
                 supervisor_vtable_name=None, supervisor_init_sel_name=None,
                 supervisor_run_sel_name=None, supervisor_nslots=0):
```

Store them on `self` alongside the kernel/serial equivalents.

In the header-size sentinel calc (near line 339, where it passes `kernel_vtable_ref=_header_sentinel, serial_on_sel_ref=_serial_sentinel`), add a supervisor sentinel so the reserved header length is computed for v5:

```python
        header_len = len(self.w.header_bytes(0, 0, 0,
                                             kernel_vtable_ref=_header_sentinel,
                                             serial_on_sel_ref=_serial_sentinel,
                                             supervisor_vtable_ref=_header_sentinel))
```

In the final `header = self.w.header_bytes(...)` call (near line 469), resolve refs and pass them:

```python
        supervisor_vtable_ref = self._vtable_ref(self.supervisor_vtable_name) if self.supervisor_vtable_name else 0
        supervisor_init_sel_ref = self._symbol_ref(self.supervisor_init_sel_name) if self.supervisor_init_sel_name else 0
        supervisor_run_sel_ref = self._symbol_ref(self.supervisor_run_sel_name) if self.supervisor_run_sel_name else 0
```

and add to the `header_bytes(...)` keyword args:

```python
                                     supervisor_vtable_ref=supervisor_vtable_ref,
                                     supervisor_init_sel_ref=supervisor_init_sel_ref,
                                     supervisor_run_sel_ref=supervisor_run_sel_ref,
                                     supervisor_nslots=self.supervisor_nslots,
```

- [ ] **Step 3: Pass supervisor names from the compiler.** In `tools/ns_compile_oevm.py`, find the `OimgSerializer(...)` construction (the Path B blob build; it already passes `kernel_vtable_name=...`, `serial_on_sel_name=...`). Add:

```python
        supervisor_vtable_name="Supervisor",
        supervisor_init_sel_name="init_usingMirror:",
        supervisor_run_sel_name="run:platform:args:debug:",
        supervisor_nslots=len(compiler.class_slots["Supervisor"]),
```

- [ ] **Step 4: Sanity-check the header builds.** From `ns-oevm`, compile the proof app to an `.oimg` (use the same Path B emission `run_tests_oevm.py` triggers) and dump the version byte:

```bash
OEVM_PATH=B python3 tools/run_tests_oevm.py 2>&1 | tail -5 || true
# locate the produced .oimg and check format_version (byte 4) == 5:
python3 - <<'PY'
import glob,struct,sys
imgs = sorted(glob.glob('/tmp/**/*.oimg', recursive=True)) + sorted(glob.glob('../oevm/**/*.oimg', recursive=True))
print("imgs:", imgs[-3:])
if imgs:
    d=open(imgs[-1],'rb').read(6); print("format_version =", struct.unpack_from('<H',d,4)[0])
PY
git -C ../oevm checkout src/solidified_image.c src/solidified_image.stmap 2>/dev/null || true
```

Expected: `format_version = 5`. (Path B run will still FAIL at boot until Task 7 — that's fine; this step only checks the header.)

- [ ] **Step 5: Commit (in `ns-oevm`).**

```bash
git add tools/oimg_serialize.py tools/ns_compile_oevm.py && git commit -m "$(cat <<'EOF'
feat(oimg): format v5 Supervisor descriptor (vtable/init/run/nslots)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01JVJv3GY4Xy4FR143b86NVt
EOF
)"
```

---

## Task 7: Path B — read the descriptor, mint/grant/route in the loader

**Files:**
- Modify: `../oevm/src/oimg_loader.c` (header doc block, static ref vars, header parsing, `oimg_bootstrap`)

**Interfaces:**
- Consumes: the v5 header fields from Task 6; `mirror_object()` (mirror.c).
- Produces: Path B boot that mints the Supervisor, grants it the mirror, and routes the launch through `run:platform:args:debug:`.

- [ ] **Step 1: Accept v5 and document the layout.** In `../oevm/src/oimg_loader.c`, in the header-layout comment block, after the `serial_on_sel_ref` line, add:

```c
 *   A+8W      W      supervisor_vtable_ref   (format_version 5)
 *   A+9W      W      supervisor_init_sel_ref (format_version 5: init_usingMirror:)
 *   A+10W     W      supervisor_run_sel_ref  (format_version 5: run:platform:args:debug:)
 *   A+11W     2      supervisor_nslots (u16) (format_version 5)
 *   (header padded to 8 again)
```

- [ ] **Step 2: Add static ref vars.** After:

```c
static uintptr_t img_serial_on_sel_ref   = 0;
```

add:

```c
/* in-image debugger step 1: Supervisor descriptor (format_version 5+). */
static uintptr_t img_supervisor_vtable_ref   = 0;
static uintptr_t img_supervisor_init_sel_ref = 0;
static uintptr_t img_supervisor_run_sel_ref  = 0;
static uint16_t  img_supervisor_nslots       = 0;
```

- [ ] **Step 3: Parse the fields.** Find:

```c
    if (format_version < 3) {
        fprintf(stderr, "oimg_load: unsupported format_version %u (expected >= 3)\n", format_version);
        munmap(map, sz); return 1;
    }
```

(no change needed — `< 3` still rejects pre-Kernel images; v5 passes.) Then after:

```c
    img_serial_on_sel_ref   = (format_version >= 4) ? read_word(base + a + 7 * W) : 0;
```

add:

```c
    /* in-image debugger step 1: Supervisor descriptor (format_version 5+). */
    img_supervisor_vtable_ref   = (format_version >= 5) ? read_word(base + a + 8 * W) : 0;
    img_supervisor_init_sel_ref = (format_version >= 5) ? read_word(base + a + 9 * W) : 0;
    img_supervisor_run_sel_ref  = (format_version >= 5) ? read_word(base + a + 10 * W) : 0;
    img_supervisor_nslots       = (format_version >= 5) ? read_u16 (base + a + 11 * W) : 0;
```

- [ ] **Step 4: Route the launch through the supervisor.** In `oimg_bootstrap`, find the final launch:

```c
    oop main_args[2] = { kernel_inst, TAG_SMI(0) };
    oop res = send(inst, oimg_resolve(img_main_sel_ref), 2, main_args);
    return res;
```

Replace with:

```c
    /* in-image debugger step 1: mint the Supervisor, grant it the native mirror,
       and route the app launch through it (gated by OEVM_DEBUG). */
    extern oop mirror_object(void);
    oop supervisor_inst = gc_allocate(oimg_resolve(img_supervisor_vtable_ref),
                                      sizeof(struct ObjectInstance) + img_supervisor_nslots * sizeof(oop));
    oop sup_init_args[1] = { mirror_object() };
    send(supervisor_inst, oimg_resolve(img_supervisor_init_sel_ref), 1, sup_init_args);

    oop dbg_flag = getenv("OEVM_DEBUG") ? TAG_SMI(1) : TAG_SMI(0);
    oop run_args[4] = { inst, kernel_inst, TAG_SMI(0), dbg_flag };
    oop res = send(supervisor_inst, oimg_resolve(img_supervisor_run_sel_ref), 4, run_args);
    return res;
```

If `getenv` is undeclared, add `#include <stdlib.h>` near the top of `oimg_loader.c` (it already includes `<string.h>`, `<sys/mman.h>`, etc.).

- [ ] **Step 5: Run Path B and the grant proof on Path B.** From `ns-oevm`:

```bash
OEVM_PATH=B python3 tools/run_tests_oevm.py 2>&1 | tail -5
git -C ../oevm checkout src/solidified_image.c src/solidified_image.stmap 2>/dev/null || true
```

Expected: Path B suite green (skips counted). Then extend `tools/test_supervisor_grant.py` with a Path B variant (build the image VM per `run_tests_oevm.py`'s Path B recipe, run `/tmp/oevm_img <image>` with/without `OEVM_DEBUG`) and assert the same `dbg:ready` / `app-ran` invariants. Run:

```bash
python3 tools/test_supervisor_grant.py
git -C ../oevm checkout src/solidified_image.c src/solidified_image.stmap 2>/dev/null || true
```

Expected: PASSED for both paths.

- [ ] **Step 6: Commit (across `../oevm` and `ns-oevm`).**

```bash
cd ../oevm && git add src/oimg_loader.c && git commit -m "$(cat <<'EOF'
feat(boot): Path B mints + grants mirror to Supervisor, routes launch (oimg v5)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01JVJv3GY4Xy4FR143b86NVt
EOF
)"
cd ../ns-oevm && git add tools/test_supervisor_grant.py && git commit -m "$(cat <<'EOF'
test(supervisor): add Path B grant proof to the harness

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01JVJv3GY4Xy4FR143b86NVt
EOF
)"
```

---

## Task 8: Full regression + structural guard

**Files:** none (verification only)

- [ ] **Step 1: Run the full suite, both paths, gate OFF.**

```bash
OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | tail -20
```

Expected: **69 pass / 0 fail / 3 skip both paths**, plus `C unit test test_debugger: PASSED`. The new `41_supervisor_grant.ns` may add one pass if it is included in the suite's expected-output set — if so, update the suite's expected count/fixture accordingly (it prints only `app-ran` with the gate off).

- [ ] **Step 2: Restore clobbered generated files.**

```bash
git -C ../oevm checkout src/solidified_image.c src/solidified_image.stmap 2>/dev/null || true
git -C ../oevm status --short
```

Expected: no stray modifications to generated files.

- [ ] **Step 3: Structural guard still passes.**

```bash
bash ../oevm/tools/check_no_ambient_reflection.sh; echo "exit=$?"
```

Expected: OK message, `exit=0`. (The app still cannot name the mirror; only the Supervisor holds it.)

- [ ] **Step 4: Grant proof, both paths.**

```bash
python3 tools/test_supervisor_grant.py
git -C ../oevm checkout src/solidified_image.c src/solidified_image.stmap 2>/dev/null || true
```

Expected: `test_supervisor_grant: PASSED` for Path A and Path B.

- [ ] **Step 5: No commit needed** (verification task). If Step 1 required a suite fixture/count update, commit that change in `ns-oevm` with message `test: register 41_supervisor_grant in the suite expected set`.

---

## Self-Review notes (coverage)

- Spec "the grant" → Tasks 4–7. "Unconditional boot citizen / app never names mirror / no app-declared cap" → supervisor minted unconditionally in both paths; structural guard re-checked (Task 8.3). "`OEVM_DEBUG` gate, suite unaffected" → Tasks 5/7 + Task 8.1. "Production drops via `OEVM_NO_REFLECTION`" → unchanged seam, out of scope (no task needed).
- Spec follow-ups folded: oevm#15 (Task 1), oevm#16 (Task 2), oevm#14 (Task 3).
- **Deferred to Plan 2/3** (NOT in this plan): the HLL `ObjectMirror`/`ContextMirror` facade, the in-image `Debugger` module, the `dbg:*` stdin/stdout transport + the new debug backend seam, the `.dsym` method registry, and source-line breakpoints. The supervisor's `run:platform:args:debug:` and the `dbg:ready` line are the seams those build on.
- Type/selector consistency: `init_usingMirror:` (1 arg) and `run:platform:args:debug:` (4 args) are used identically in `lib/supervisor.ns` (Task 4), Path A emit (Task 5), the oimg descriptor names (Task 6), and the loader (Task 7). `Supervisor` nslots = `len(compiler.class_slots["Supervisor"])` in both paths.
