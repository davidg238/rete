import os
import sys
import re
import subprocess
from pathlib import Path

# Colors for terminal output
COLOR_GREEN = "\033[92m"
COLOR_RED = "\033[91m"
COLOR_YELLOW = "\033[93m"
COLOR_RESET = "\033[0m"

# The oevm VM (the "bedrock") lives in a sibling repo. Override with $OEVM_DIR.
OEVM_DIR = Path(os.environ.get("OEVM_DIR") or (Path(__file__).resolve().parent.parent.parent / "tuvm"))

# OEVM_PATH controls which execution paths are exercised.
#   (unset / "B")  — run Path B only (.oimg / image-VM workflow) — DEFAULT.
#                    Path B builds the image VM once, then per test only
#                    compiles .nsl -> .oimg and runs it. Path A instead did a
#                    full-VM gcc rebuild (incl. the ~13k-line solidified_image.c)
#                    per test — ~3.4s each over ~209 tests, ~12 min of the run.
#                    B is the routine gate; use "A"/"AB" to validate the
#                    solidified_image.c path that actually ships on ESP32.
#   "A"            — run Path A only (solidified_image.c workflow)
#   "AB"           — run both Path A and Path B (full pre-device/release gate)
OEVM_PATH = os.environ.get("OEVM_PATH", "B").upper()
OEVM_EXTRA_CFLAGS = os.environ.get("OEVM_EXTRA_CFLAGS", "").split()
# Per-target backend: the *_backend.h seam impls live in src/backend/$OEVM_TARGET/.
OEVM_TARGET = os.environ.get("OEVM_TARGET", "posix")
OEVM_BACKEND = [f"src/backend/{OEVM_TARGET}/platform_{OEVM_TARGET}.c",
                f"src/backend/{OEVM_TARGET}/serial_{OEVM_TARGET}.c",
                f"src/backend/{OEVM_TARGET}/udp_{OEVM_TARGET}.c",
                f"src/backend/{OEVM_TARGET}/pulse_{OEVM_TARGET}.c",
                f"src/backend/{OEVM_TARGET}/timer_{OEVM_TARGET}.c",
                # tuvm#61: the job-latency microsecond clock seam. Under the DEFAULT
                # virtual clock it scales timer_backend_millis() to microseconds, so
                # advanceMillis: moves the span clock by an exact amount and every
                # bucket assertion in 19_latency stays deterministic.
                f"src/backend/{OEVM_TARGET}/span_{OEVM_TARGET}.c",
                f"src/backend/{OEVM_TARGET}/http_{OEVM_TARGET}.c",
                f"src/backend/{OEVM_TARGET}/sensor_{OEVM_TARGET}.c"]

# OEVM_DUMP_DIR: differential harness knob. When set, every test's raw program stdout
# (the bytes the .nsl program printed — what gets compared against expectations) is
# written to $OEVM_DUMP_DIR/<relpath>.<PATH>.out (one file per test per path A/B).
# Pure observation — does NOT change pass/fail logic or lowering. Cheap when unset.
OEVM_DUMP_DIR = os.environ.get("OEVM_DUMP_DIR")

# Module-level cache for Path B one-time builds.
# None = not yet attempted; True = success; False = failed (hard error)
_pathb_layout_built = None   # None | True
_pathb_imgvm_built = None    # None | True
_pathb_build_error = None    # str | None — set on first failure, blocks all Path B


def _build_pathb_once():
    """Build layout.json and the image VM binary (each exactly once per run).
    Sets module globals. Returns (ok: bool, error_msg: str|None)."""
    global _pathb_layout_built, _pathb_imgvm_built, _pathb_build_error

    # Already attempted
    if _pathb_build_error is not None:
        return False, _pathb_build_error
    if _pathb_layout_built and _pathb_imgvm_built:
        return True, None

    oevm_dir = str(OEVM_DIR)

    # Step 1: build dump_layout tool and produce layout.json
    if not _pathb_layout_built:
        proc = subprocess.run(
            ["gcc", "-std=c99", "-Isrc", "tools/dump_layout.c", "-o", "/tmp/oevm_dump_layout"],
            cwd=oevm_dir, capture_output=True, text=True
        )
        if proc.returncode != 0:
            _pathb_build_error = f"Path B: failed to build dump_layout:\n{proc.stderr or proc.stdout}"
            return False, _pathb_build_error

        proc2 = subprocess.run(
            ["/tmp/oevm_dump_layout"],
            capture_output=True, text=True
        )
        if proc2.returncode != 0:
            _pathb_build_error = f"Path B: dump_layout failed:\n{proc2.stderr or proc2.stdout}"
            return False, _pathb_build_error

        with open("/tmp/oevm_layout.json", "w") as f:
            f.write(proc2.stdout)

        _pathb_layout_built = True

    # Step 2: build the image VM binary
    if not _pathb_imgvm_built:
        proc = subprocess.run(
            [
                "gcc", "-Wall", "-std=c99", "-Isrc",
                "src/vm.c", "src/gc.c", "src/dispatch.c", "src/platform.c", "src/space.c", "src/handles.c", "src/span.c",
                "src/serial.c",
        "src/udp.c", "src/http.c", "src/http_core.c", "src/mirror.c",
                *OEVM_BACKEND,
                "src/oimg_loader.c", "src/kernel_boot.c",
                "oevm_image_main.c",
                "-o", "/tmp/oevm_img",
                *OEVM_EXTRA_CFLAGS,
                "-lm",
            ],
            cwd=oevm_dir, capture_output=True, text=True
        )
        if proc.returncode != 0:
            _pathb_build_error = f"Path B: failed to build image VM:\n{proc.stderr or proc.stdout}"
            return False, _pathb_build_error

        _pathb_imgvm_built = True

    return True, None


def run_c_unit_tests(oevm_dir):
    """Build + run standalone C unit tests (test_debugger.c) — oevm#14 proof.

    Links against src/oimg_loader.c (provides oimg_blob_base/end, builtin_registry,
    oimg_singleton) + src/solidified_image.c (provides init_solidified_vtables) —
    NOT kernel_boot.c, whose static Vtable_* defs would clash with solidified_image.c.
    """
    build = subprocess.run(
        ["gcc", "-Wall", "-std=c99", "-Isrc",
         "src/vm.c", "src/gc.c", "src/dispatch.c", "src/platform.c", "src/space.c", "src/handles.c", "src/span.c",
         "src/serial.c",
         "src/udp.c", "src/http.c", "src/http_core.c", "src/mirror.c", *OEVM_BACKEND,
         "src/oimg_loader.c", "src/solidified_image.c",
         "tests/test_debugger.c", "-o", "/tmp/test_debugger", "-lm"],
        cwd=str(oevm_dir), capture_output=True, text=True)
    if build.returncode != 0:
        print("test_debugger build FAILED:\n" + build.stderr)
        return False
    run = subprocess.run(["/tmp/test_debugger"], cwd=str(oevm_dir),
                         capture_output=True, text=True)
    if run.returncode != 0:
        print("test_debugger FAILED:\n" + run.stdout + run.stderr)
        return False
    print("C unit test test_debugger: PASSED")
    return True


def run_boot_gc_asan_gate() -> bool:
    """Regression gate for oevm#21 (boot-time GC roots incomplete).

    During run_interpreter's lazy exec-context bootstrap (a Stack object +
    current_frame, no Fiber wrapper — Milestone A Task 6), the boot ENTRY INSTANCE
    (the `self` passed to the first send, e.g. Kernel>>init_platform:) is live but
    held only in a C local — rooted solely by the conservative C-stack scan. ASan's
    fake-stack (detect_stack_use_after_return=1, the ASan DEFAULT) relocates that
    local off the machine stack, so under OEVM_GC_STRESS=1 a collection during the
    bootstrap sweeps the entry instance and reuses its slot for g_active_stack —
    after which `self` aliases the Stack object and the entry method's slot-store
    corrupts the live stack frame (SEGV in scan_vm_stack on the next GC).

    Build a tiny appliance WITH AddressSanitizer (fake-stack ON) under GC_STRESS and
    require the expected output. Requires the entry instance to be a PRECISE root.
    Skips cleanly (PASS) if the compiler lacks -fsanitize=address support."""
    src = Path(__file__).parent.parent.parent / "nsl-tests" / "04_somns_basic" / "01_method_call.nsl"
    if not src.exists():
        print(f"{COLOR_RED}boot-gc-asan: FAIL{COLOR_RESET} (missing {src})")
        return False
    tmp_c = OEVM_DIR / "_boot_gc_asan.c"
    runner = OEVM_DIR / "oevm_boot_gc_asan"
    try:
        comp = subprocess.run(
            [sys.executable, str(Path(__file__).parent / "ns_compile_oevm.py"),
             str(src), str(tmp_c)], capture_output=True, text=True)
        if comp.returncode != 0:
            print(f"{COLOR_RED}boot-gc-asan: FAIL{COLOR_RESET} (compile)")
            print("  " + (comp.stderr or comp.stdout).strip()[-500:])
            return False
        build = subprocess.run(
            ["gcc", "-g", "-fsanitize=address", "-Wall", "-std=c99", "-Isrc", "-DOEVM_APPLIANCE",
             "src/vm.c", "src/gc.c", "src/dispatch.c", "src/platform.c", "src/space.c", "src/handles.c", "src/span.c",
             "src/serial.c",
             "src/udp.c", "src/http.c", "src/http_core.c", "src/mirror.c", *OEVM_BACKEND,
             str(tmp_c), "src/oevm_main.c", "-o", str(runner), "-lm"],
            cwd=str(OEVM_DIR), capture_output=True, text=True)
        if build.returncode != 0:
            # No ASan toolchain available — skip rather than fail the suite.
            print(f"{COLOR_YELLOW}boot-gc-asan: SKIP{COLOR_RESET} (no -fsanitize=address)")
            return True
        # detect_stack_use_after_return defaults ON; set it explicitly so the gate
        # reproduces the fake-stack condition regardless of any inherited ASAN_OPTIONS.
        env = {**os.environ, "OEVM_GC_STRESS": "1",
               "ASAN_OPTIONS": "detect_stack_use_after_return=1"}
        try:
            run = subprocess.run([str(runner)], cwd=str(OEVM_DIR),
                                 capture_output=True, text=True, timeout=120, env=env)
        except subprocess.TimeoutExpired:
            print(f"{COLOR_RED}boot-gc-asan: FAIL{COLOR_RESET} (timeout)")
            return False
        actual = [l.strip() for l in run.stdout.strip().splitlines() if l.strip()]
        if run.returncode != 0 or actual != ["42"]:
            print(f"{COLOR_RED}boot-gc-asan: FAIL{COLOR_RESET} "
                  f"(exit={run.returncode}, output={actual})")
            print("  " + (run.stderr or "").strip()[-500:])
            return False
        print(f"{COLOR_GREEN}boot-gc-asan: PASSED{COLOR_RESET}")
        return True
    finally:
        for p in (tmp_c, runner, tmp_c.with_suffix(".stmap")):
            try:
                p.unlink()
            except FileNotFoundError:
                pass


def run_integration_gate(label: str, script_name: str) -> bool:
    """Run a tools/<script_name> integration harness as a subprocess gate.

    These scripts assume cwd == the ns-oevm ROOT (this tools/ dir's parent), and
    they clobber ../tuvm/src/solidified_image.c during their builds (the caller is
    responsible for restoring it afterward). A non-zero exit OR an exception marks
    the gate FAILED without crashing the suite runner; on failure we print enough
    captured output to diagnose."""
    nsroot = Path(__file__).resolve().parent.parent
    script = nsroot / "tools" / script_name
    try:
        proc = subprocess.run(
            [sys.executable, str(script)],
            cwd=str(nsroot), capture_output=True, text=True, timeout=300)
    except Exception as e:
        print(f"{COLOR_RED}{label}: FAIL{COLOR_RESET} (runner error: {e})")
        return False
    if proc.returncode != 0:
        print(f"{COLOR_RED}{label}: FAIL{COLOR_RESET}")
        out = (proc.stdout or "") + (proc.stderr or "")
        print("  " + "\n  ".join(out.strip().splitlines()[-30:]))
        return False
    print(f"{COLOR_GREEN}{label}: PASSED{COLOR_RESET}")
    return True


def run_portability_gate() -> str:
    """Gate the 32-bit (-m32) serialize+load+run path (oevm/tests/test_portability_32.sh).

    The .oimg header descriptor block is word-strided (W = oop_size), so the
    serializer/loader contract only holds if it is exercised at BOTH word sizes;
    a 64-bit-only suite cannot catch a W=4 layout divergence. Returns "PASS",
    "SKIP" (exit 77 — no gcc-multilib on this host), or "FAIL"."""
    script = OEVM_DIR / "tests" / "test_portability_32.sh"
    try:
        proc = subprocess.run(["bash", str(script)],
                              cwd=str(OEVM_DIR), capture_output=True, text=True, timeout=300)
    except Exception as e:
        print(f"{COLOR_RED}portability-32: FAIL{COLOR_RESET} (runner error: {e})")
        return "FAIL"
    if proc.returncode == 77:
        print(f"portability-32: SKIP (no gcc-multilib)")
        return "SKIP"
    if proc.returncode != 0:
        print(f"{COLOR_RED}portability-32: FAIL{COLOR_RESET}")
        out = (proc.stdout or "") + (proc.stderr or "")
        print("  " + "\n  ".join(out.strip().splitlines()[-30:]))
        return "FAIL"
    print(f"{COLOR_GREEN}portability-32: PASSED{COLOR_RESET}")
    return "PASS"


def run_footprint_gate() -> str:
    """Gate the MCU cross-compile footprint ratchet (tuvm/tools/footprint.sh).

    Cross-compiles the engine core with arm-zephyr-eabi and ratchets text +
    data+bss against tools/footprint-mcu-baseline.txt: a size regression is a
    gate failure like a text regression (design addendum A2 / tuvm#5). Returns
    "PASS", "SKIP" (exit 77 — no cross toolchain on PATH), or "FAIL"."""
    script = OEVM_DIR / "tools" / "footprint.sh"
    try:
        proc = subprocess.run(
            ["bash", str(script)],
            cwd=str(OEVM_DIR), capture_output=True, text=True, timeout=300,
            env={**os.environ, "FOOTPRINT_CONFIG": "mcu"})
    except Exception as e:
        print(f"{COLOR_RED}footprint: FAIL{COLOR_RESET} (runner error: {e})")
        return "FAIL"
    if proc.returncode == 77:
        print(f"footprint: SKIP (no arm-zephyr-eabi toolchain)")
        return "SKIP"
    if proc.returncode != 0:
        print(f"{COLOR_RED}footprint: FAIL{COLOR_RESET}")
        out = (proc.stdout or "") + (proc.stderr or "")
        print("  " + "\n  ".join(out.strip().splitlines()[-30:]))
        return "FAIL"
    print(f"{COLOR_GREEN}footprint: PASSED{COLOR_RESET}")
    return "PASS"


def parse_test_metadata(file_path: Path):
    content = file_path.read_text(encoding="utf-8")

    # Newspeak comments are (* ... *)
    comment_pattern = re.compile(r"\(\*(.*?)\*\)", re.DOTALL)
    match = comment_pattern.search(content)
    if not match:
        return {}

    comment_text = match.group(1)
    metadata = {
        "expect": [],
        "expect_compile_fail": None,
        "expect_runtime_fail": None,
        "skip_berry": False,
        "skip_oevm": False,
        "env": {},
    }

    lines = comment_text.splitlines()
    in_expect = False

    for line in lines:
        line = line.strip()
        if not line:
            continue

        if line.startswith("expect:"):
            in_expect = True
            rest = line[len("expect:"):].strip()
            if rest:
                metadata["expect"].append(rest)
        elif line.startswith("expect-compile-fail:"):
            in_expect = False
            metadata["expect_compile_fail"] = line[len("expect-compile-fail:"):].strip()
        elif line.startswith("expect-runtime-fail:"):
            in_expect = False
            metadata["expect_runtime_fail"] = line[len("expect-runtime-fail:"):].strip()
        elif line.startswith("skip-berry:"):
            in_expect = False
            metadata["skip_berry"] = line[len("skip-berry:"):].strip().lower() == "true"
        elif line.startswith("skip-oevm:"):
            in_expect = False
            metadata["skip_oevm"] = line[len("skip-oevm:"):].strip().lower() == "true"
        elif line.startswith("skip-soup:"):
            in_expect = False
        elif line.startswith("env:"):
            # `env: KEY=VALUE` — one variable per line, applied ONLY to this
            # test's VM process (tuvm#88). Some seams are gated by getenv and
            # are otherwise untestable: OEVM_NO_DISPLAY makes the host claim it
            # has no panel, which is the only way to reach the
            # #displayUnavailable path from a host test. Setting such a variable
            # globally is not equivalent — it would leak into every other test
            # in the suite (14_display/01 and /02 would then assert against a
            # display that reports itself absent), so it has to be per-test.
            in_expect = False
            assignment = line[len("env:"):].strip()
            if "=" in assignment:
                key, _, value = assignment.partition("=")
                metadata["env"][key.strip()] = value.strip()
        elif in_expect:
            metadata["expect"].append(line)

    return metadata

def _check_runtime_fail(metadata: dict, err_msg: str, stdout: str) -> tuple[bool, str]:
    """Validate an `expect-runtime-fail:` test (Task 10b smallest-extension): the
    diagnostic (stderr, or stdout if stderr is empty) must CONTAIN the header's
    substring, and any preceding `expect:` lines must match the stdout emitted
    before the crash. Previously the harness treated ANY nonzero exit as a PASS
    regardless of the header's substring or preceding output — verified against
    07_no_nlr/04_stack_exhaustion.nsl, which needs both checks to be a meaningful
    proof of controlled (vs. C-level) termination. Returns (ok, detail)."""
    expected_substr = metadata["expect_runtime_fail"]
    if expected_substr not in err_msg:
        return False, (f"Runtime failed as expected, but diagnostic did not contain "
                        f"'{expected_substr}':\n{err_msg}")
    expected_lines = [line.strip() for line in metadata.get("expect", []) if line.strip()]
    if expected_lines:
        actual_lines = [line.strip() for line in stdout.strip().splitlines() if line.strip()]
        if actual_lines != expected_lines:
            return False, (f"Runtime failed with the expected diagnostic, but preceding "
                            f"output mismatched.\nExpected: {expected_lines}\nActual:   {actual_lines}")
    return True, f"Expected runtime failure: {err_msg.strip()}"


def _check_compile_fail(metadata: dict, err_msg: str) -> tuple[bool, str]:
    """Validate an `expect-compile-fail:` test (final-review smallest-extension,
    mirrors _check_runtime_fail): the compiler's stderr+stdout must CONTAIN the
    header's substring. Previously the harness treated ANY nonzero compile exit
    as a PASS regardless of the header's substring, so a test could declare the
    wrong diagnostic (or the compiler could fail for an unrelated reason) and
    still pass. Returns (ok, detail)."""
    expected_substr = metadata["expect_compile_fail"]
    if expected_substr not in err_msg:
        return False, (f"Compilation failed as expected, but diagnostic did not contain "
                        f"'{expected_substr}':\n{err_msg}")
    return True, f"Expected compilation failure: {err_msg.strip()}"


def _dump_stdout(src_path: Path, path_label: str, stdout: str) -> None:
    """Write raw program stdout to $OEVM_DUMP_DIR/<relpath>.<path_label>.out.
    Called only when OEVM_DUMP_DIR is set; the dump does NOT affect pass/fail."""
    if not OEVM_DUMP_DIR:
        return
    test_dir = Path(__file__).parent.parent.parent / "nsl-tests"
    try:
        rel = src_path.relative_to(test_dir)
    except ValueError:
        rel = Path(src_path.name)
    out_path = Path(OEVM_DUMP_DIR) / (str(rel) + f".{path_label}.out")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(stdout, encoding="utf-8")


def run_test_file(src_path: Path) -> tuple[str, str]:
    metadata = parse_test_metadata(src_path)

    # Note: skip-berry is an ns-berry limitation; oevm attempts those tests.
    # Use skip-oevm to mark tests oevm genuinely cannot run yet.
    if metadata.get("skip_oevm"):
        return "SKIP", "Skipped (skip-oevm: true)"

    if not (metadata.get("expect") or metadata.get("expect_compile_fail") or metadata.get("expect_runtime_fail")):
        return "SKIP", "No expectations defined."

    # 1. Compile .nsl to src/solidified_image.c
    compiler_script = Path(__file__).parent / "ns_compile_oevm.py"
    target_c = OEVM_DIR / "src" / "solidified_image.c"

    proc_compile = subprocess.run(
        [sys.executable, str(compiler_script), str(src_path), str(target_c)],
        capture_output=True, text=True
    )

    if proc_compile.returncode != 0:
        err_msg = proc_compile.stderr or proc_compile.stdout
        if metadata.get("expect_compile_fail"):
            ok, detail = _check_compile_fail(metadata, err_msg)
            if not ok:
                return "FAIL", detail
            _dump_stdout(src_path, "A", "")
            return "PASS", detail
        return "FAIL", f"Compilation failed:\n{err_msg}"

    if metadata.get("expect_compile_fail"):
        return "FAIL", "Expected compilation failure but compiled successfully."

    # 2. Build the C runner
    oevm_dir = OEVM_DIR
    runner_bin = oevm_dir / "oevm_runner"

    # We clean the previous bin first
    if runner_bin.exists():
        runner_bin.unlink()

    cmd_build = [
        "gcc", "-Wall", "-Wextra", "-std=c99", "-Isrc",
        "-DOEVM_APPLIANCE",
        "src/vm.c", "src/gc.c", "src/dispatch.c", "src/platform.c", "src/space.c", "src/handles.c", "src/span.c",
        "src/serial.c",
        "src/udp.c", "src/http.c", "src/http_core.c", "src/mirror.c",
        *OEVM_BACKEND,
        "src/solidified_image.c", "src/oevm_main.c",
        "-o", "oevm_runner",
        *OEVM_EXTRA_CFLAGS,
        "-lm",  # VM float primitives (sqrt) need libm
    ]
    proc_build = subprocess.run(cmd_build, cwd=str(oevm_dir), capture_output=True, text=True)
    if proc_build.returncode != 0:
        return "FAIL", f"C Build failed:\n{proc_build.stderr or proc_build.stdout}"

    # 3. Run the compiled runner (30s timeout guards against infinite loops)
    try:
        proc_run = subprocess.run([str(runner_bin)], capture_output=True, text=True, timeout=30,
                                  env={**os.environ, **metadata.get("env", {})})
    except subprocess.TimeoutExpired:
        return "FAIL", "Runtime timed out (>30s)"
    if proc_run.returncode != 0:
        err_msg = proc_run.stderr or proc_run.stdout
        if metadata.get("expect_runtime_fail"):
            ok, detail = _check_runtime_fail(metadata, err_msg, proc_run.stdout)
            if not ok:
                return "FAIL", detail
            _dump_stdout(src_path, "A", proc_run.stdout)
            return "PASS", detail
        return "FAIL", f"Runtime crashed:\n{err_msg}"

    if metadata.get("expect_runtime_fail"):
        return "FAIL", "Expected runtime failure but ran successfully."

    # Dump raw stdout for differential harness (no-op when OEVM_DUMP_DIR unset).
    _dump_stdout(src_path, "A", proc_run.stdout)

    # Compare stdout lines
    actual_lines = [line.strip() for line in proc_run.stdout.strip().splitlines() if line.strip()]
    expected_lines = [line.strip() for line in metadata.get("expect", []) if line.strip()]

    if actual_lines != expected_lines:
        return "FAIL", f"Output mismatch.\nExpected: {expected_lines}\nActual:   {actual_lines}"

    return "PASS", "Output matches expectations."


def run_test_file_oimg(src_path: Path) -> tuple[str, str]:
    """Path B: compile to .oimg and run via the image VM."""
    metadata = parse_test_metadata(src_path)

    if metadata.get("skip_oevm"):
        return "SKIP", "Skipped (skip-oevm: true)"

    if not (metadata.get("expect") or metadata.get("expect_compile_fail") or metadata.get("expect_runtime_fail")):
        return "SKIP", "No expectations defined."

    # Ensure the one-time Path B builds are done
    ok, err = _build_pathb_once()
    if not ok:
        return "FAIL", err

    # Derive a unique name for the temp image file (parent dir + stem avoids
    # collisions when two tests in different directories share the same stem).
    safe_stem = re.sub(r"[^A-Za-z0-9_.-]", "_", src_path.stem)
    safe_parent = re.sub(r"[^A-Za-z0-9_.-]", "_", src_path.parent.name)
    oimg_path = f"/tmp/{safe_parent}__{safe_stem}.oimg"

    # 1. Compile .nsl to .oimg
    compiler_script = Path(__file__).parent / "ns_compile_oevm.py"
    proc_compile = subprocess.run(
        [
            sys.executable, str(compiler_script),
            str(src_path), oimg_path,
            "--format=oimg",
            "--layout=/tmp/oevm_layout.json",
        ],
        capture_output=True, text=True
    )

    if proc_compile.returncode != 0:
        err_msg = proc_compile.stderr or proc_compile.stdout
        if metadata.get("expect_compile_fail"):
            ok, detail = _check_compile_fail(metadata, err_msg)
            if not ok:
                return "FAIL", detail
            return "PASS", detail
        return "FAIL", f"Compilation failed:\n{err_msg}"

    if metadata.get("expect_compile_fail"):
        return "FAIL", "Expected compilation failure but compiled successfully."

    # 2. Run image VM (30s timeout)
    try:
        proc_run = subprocess.run(
            ["/tmp/oevm_img", oimg_path],
            capture_output=True, text=True, timeout=30,
            env={**os.environ, **metadata.get("env", {})}
        )
    except subprocess.TimeoutExpired:
        return "FAIL", "Runtime timed out (>30s)"

    if proc_run.returncode != 0:
        err_msg = proc_run.stderr or proc_run.stdout
        if metadata.get("expect_runtime_fail"):
            ok, detail = _check_runtime_fail(metadata, err_msg, proc_run.stdout)
            if not ok:
                return "FAIL", detail
            _dump_stdout(src_path, "B", proc_run.stdout)
            return "PASS", detail
        return "FAIL", f"Runtime crashed:\n{err_msg}"

    if metadata.get("expect_runtime_fail"):
        return "FAIL", "Expected runtime failure but ran successfully."

    # Dump raw stdout for differential harness (no-op when OEVM_DUMP_DIR unset).
    _dump_stdout(src_path, "B", proc_run.stdout)

    # Compare stdout lines
    actual_lines = [line.strip() for line in proc_run.stdout.strip().splitlines() if line.strip()]
    expected_lines = [line.strip() for line in metadata.get("expect", []) if line.strip()]

    if actual_lines != expected_lines:
        return "FAIL", f"Output mismatch.\nExpected: {expected_lines}\nActual:   {actual_lines}"

    return "PASS", "Output matches expectations."


def main():
    test_dir = Path(__file__).parent.parent.parent / "nsl-tests"
    if not test_dir.exists():
        print(f"Error: tests directory not found at {test_dir}")
        sys.exit(1)

    # Scan and sort test files
    test_files = sorted(list(test_dir.glob("**/*.nsl")))

    passed = 0
    failed = 0
    skipped = 0

    print(f"Discovered {len(test_files)} Newspeak conformance test files.\n")

    for f in test_files:
        rel_path = f.relative_to(test_dir.parent)
        print(f"Running {str(rel_path):50} ... ", end="", flush=True)

        try:
            # --- Path A ---
            if OEVM_PATH in ("A", "AB"):
                status_a, msg_a = run_test_file(f)
            else:
                # Path B only: synthesise a neutral result so branching below works
                status_a, msg_a = "PASS", "Path A skipped (OEVM_PATH=B)"

            # If Path A says SKIP (or we are A-only and Path A skipped),
            # skip the whole test immediately.
            if status_a == "SKIP":
                skipped += 1
                print(f"{COLOR_YELLOW}SKIP{COLOR_RESET} ({msg_a})")
                continue

            # --- Path B ---
            if OEVM_PATH in ("B", "AB"):
                try:
                    status_b, msg_b = run_test_file_oimg(f)
                except Exception as e:
                    status_b = "FAIL"
                    msg_b = f"Unexpected Path B error: {e}"
            else:
                # Path A only: synthesise a neutral result
                status_b, msg_b = "PASS", "Path B skipped (OEVM_PATH=A)"

            # --- Result combination ---
            # Explicit SKIP arm: if Path B independently returns SKIP (e.g. after
            # a future refactor removes the early-exit above), treat it as skipped
            # rather than letting it fall into the FAIL else-arm.
            if status_b == "SKIP":
                skipped += 1
                print(f"{COLOR_YELLOW}SKIP{COLOR_RESET} (B: {msg_b})")
            elif status_a == "PASS" and status_b == "PASS":
                passed += 1
                print(f"{COLOR_GREEN}PASS{COLOR_RESET}")
            elif status_a == "PASS" and status_b == "FAIL":
                failed += 1
                print(f"{COLOR_RED}FAIL{COLOR_RESET}")
                print(f"  A:PASS  B:FAIL — {msg_b}")
            elif status_a == "FAIL" and status_b == "PASS":
                failed += 1
                print(f"{COLOR_RED}FAIL{COLOR_RESET}")
                print(f"  A:FAIL  B:PASS — A: {msg_a}")
            else:
                # Both fail
                failed += 1
                print(f"{COLOR_RED}FAIL{COLOR_RESET}")
                print(f"  A:FAIL — {msg_a}")
                print(f"  B:FAIL — {msg_b}")

        except Exception as e:
            failed += 1
            print(f"{COLOR_RED}ERROR{COLOR_RESET}\n  Unexpected execution error: {e}")

    # Standalone C unit tests (test_debugger.c) — oevm#14 regression proof.
    # Restore the committed solidified image first: the conformance loop clobbers
    # src/solidified_image.c per-test, and the C unit test links against it.
    subprocess.run(
        ["git", "-C", str(OEVM_DIR), "checkout",
         "src/solidified_image.c", "src/solidified_image.stmap"],
        capture_output=True, text=True)
    print()
    c_units_ok = run_c_unit_tests(OEVM_DIR)

    # Integration gates: the supervisor-grant proof and the in-image debugger
    # harness. Both build the target app and CLOBBER ../tuvm/src/solidified_image.c
    # (+ .stmap). test_debugger_integration self-restores via try/finally, but
    # test_supervisor_grant does NOT — so we restore the committed image again
    # after BOTH run, leaving ../tuvm a clean tree.
    print()
    grant_ok = run_integration_gate("test_supervisor_grant", "test_supervisor_grant.py")
    dbg_int_ok = run_integration_gate("test_debugger_integration", "test_debugger_integration.py")
    subprocess.run(
        ["git", "-C", str(OEVM_DIR), "checkout",
         "src/solidified_image.c", "src/solidified_image.stmap"],
        capture_output=True, text=True)

    portability = run_portability_gate()   # "PASS" | "SKIP" | "FAIL"
    boot_gc_ok = run_boot_gc_asan_gate()
    footprint = run_footprint_gate()   # "PASS" | "SKIP" | "FAIL"

    suite_ok = (failed == 0 and c_units_ok and grant_ok and dbg_int_ok
                and portability != "FAIL" and boot_gc_ok
                and footprint != "FAIL")
    print(f"\n{COLOR_GREEN if suite_ok else COLOR_RED}Test Execution Summary:{COLOR_RESET}")
    print(f"  Passed:  {passed}")
    print(f"  Failed:  {failed}")
    print(f"  Skipped: {skipped}")
    print(f"  grant: {'PASS' if grant_ok else 'FAIL'}  "
          f"debugger-integration: {'PASS' if dbg_int_ok else 'FAIL'}  "
          f"portability-32: {portability}  "
          f"boot-gc-asan: {'PASS' if boot_gc_ok else 'FAIL'}  "
          f"footprint: {footprint}")

    if not suite_ok:
        sys.exit(1)

if __name__ == "__main__":
    main()
