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

# Repo root, for gates that live outside nodus/ (review/tests/t0-gates/). Derived from
# THIS file's location, not from OEVM_DIR — OEVM_DIR may be pointed at another tree.
REPO_ROOT = Path(__file__).resolve().parent.parent.parent.parent

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
                f"src/backend/{OEVM_TARGET}/sensor_{OEVM_TARGET}.c",
                # rete#25: the image-push seam. Every OEVM_TARGET has a backend —
                # ota_zephyr.c exists and is deliberately inert (no flash map yet).
                f"src/backend/{OEVM_TARGET}/ota_{OEVM_TARGET}.c"]

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
        "src/udp.c", "src/http.c", "src/http_core.c", "src/mirror.c", "src/ota.c",
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
    """Build + run standalone C unit tests — oevm#14 proof, extended by #165 for
    the method-profile gate.

    Links against src/oimg_loader.c (provides oimg_blob_base/end, builtin_registry,
    oimg_singleton) + src/solidified_image.c (provides init_solidified_vtables) —
    NOT kernel_boot.c, whose static Vtable_* defs would clash with solidified_image.c.
    """
    common_srcs = [
        "src/vm.c", "src/gc.c", "src/dispatch.c", "src/platform.c", "src/space.c", "src/handles.c", "src/span.c",
        "src/serial.c",
        "src/udp.c", "src/http.c", "src/http_core.c", "src/mirror.c", "src/ota.c", *OEVM_BACKEND,
        "src/oimg_loader.c", "src/solidified_image.c",
    ]

    build = subprocess.run(
        ["gcc", "-Wall", "-std=c99", "-Isrc",
         *common_srcs,
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

    build = subprocess.run(
        ["gcc", "-Wall", "-std=c99", "-DOEVM_METHOD_PROFILE", "-Isrc",
         *common_srcs,
         "tests/test_mprof_gate.c", "-o", "/tmp/test_mprof_gate", "-lm"],
        cwd=str(oevm_dir), capture_output=True, text=True)
    if build.returncode != 0:
        print("test_mprof_gate build FAILED:\n" + build.stderr)
        return False
    run = subprocess.run(["/tmp/test_mprof_gate"], cwd=str(oevm_dir),
                         capture_output=True, text=True)
    if run.returncode != 0:
        print("test_mprof_gate FAILED:\n" + run.stdout + run.stderr)
        return False
    print("C unit test test_mprof_gate: PASSED")

    build = subprocess.run(
        ["gcc", "-Wall", "-std=c99", "-DOEVM_METHOD_PROFILE", "-Isrc",
         *common_srcs,
         "tests/test_mprof_alloc.c", "-o", "/tmp/test_mprof_alloc", "-lm"],
        cwd=str(oevm_dir), capture_output=True, text=True)
    if build.returncode != 0:
        print("test_mprof_alloc build FAILED:\n" + build.stderr)
        return False
    run = subprocess.run(["/tmp/test_mprof_alloc"], cwd=str(oevm_dir),
                         capture_output=True, text=True)
    if run.returncode != 0:
        print("test_mprof_alloc FAILED:\n" + run.stdout + run.stderr)
        return False
    print("C unit test test_mprof_alloc: PASSED")
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
    src = Path(__file__).parent.parent.parent / "tests" / "04_somns_basic" / "01_method_call.nsl"
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
             "src/udp.c", "src/http.c", "src/http_core.c", "src/mirror.c", "src/ota.c", *OEVM_BACKEND,
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


def run_pytest_gate(label: str, rel_dir: str) -> str:
    """Run a pytest suite living outside nodus/nsl as a subprocess gate.

    #165: `nodus/tuvm/tools/test_mprof_resolve.py` and the seven suites under
    `nodus/tuvm/zephyr/tools/console/` were referenced by nothing — no gate, no
    CI, no script. `mprof_resolve.py` could break, and the profile view with it,
    with all three CLAUDE.md gates green. A test that is never run is worse than
    no test, because the suite still reads green and the coverage is now
    believed rather than merely absent.

    cwd is the suite's own directory: these files import their siblings by bare
    name (`import mprof_resolve`, `import view`, `import logstore`), which is
    what makes them runnable by hand from where they live.

    Returns "PASS", "SKIP" (pytest is not installed — a real environment gap,
    coloured like every other skip so it cannot read as a pass) or "FAIL"."""
    d = REPO_ROOT / rel_dir
    if not d.is_dir():
        print(f"{COLOR_RED}{label}: FAIL{COLOR_RESET} (no such directory: {d})")
        return "FAIL"
    try:
        proc = subprocess.run(
            [sys.executable, "-m", "pytest", "-q", "--no-header"],
            cwd=str(d), capture_output=True, text=True, timeout=600)
    except Exception as e:
        print(f"{COLOR_RED}{label}: FAIL{COLOR_RESET} (runner error: {e})")
        return "FAIL"
    out = (proc.stdout or "") + (proc.stderr or "")
    if proc.returncode == 4 and "No module named pytest" in out:
        print(f"{COLOR_YELLOW}{label}: SKIP{COLOR_RESET} (pytest is not installed)")
        return "SKIP"
    if "No module named pytest" in out:
        print(f"{COLOR_YELLOW}{label}: SKIP{COLOR_RESET} (pytest is not installed)")
        return "SKIP"
    if proc.returncode != 0:
        print(f"{COLOR_RED}{label}: FAIL{COLOR_RESET}")
        print("  " + "\n  ".join(out.strip().splitlines()[-30:]))
        return "FAIL"
    # The pass COUNT is printed, not just the word: "0 passed" exits 0 under
    # some pytest configurations, and a gate that cannot tell "everything
    # passed" from "nothing ran" is the shape this whole change exists to fix.
    tail = [ln for ln in out.strip().splitlines()
            if " passed" in ln or "no tests ran" in ln]
    if not tail or "no tests ran" in tail[-1]:
        print(f"{COLOR_RED}{label}: FAIL{COLOR_RESET} (exit 0, but pytest "
              f"collected nothing — a green gate over an empty suite)")
        return "FAIL"
    print(f"{COLOR_GREEN}{label}: PASSED{COLOR_RESET} ({tail[-1].strip()})")
    return "PASS"


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
    """Gate T0.3 — the footprint ratchet (review/tests/t0-gates/ratchet.sh).

    Runs the T0.3 gate itself rather than footprint.sh (#143). footprint.sh now
    only MEASURES and BANKS; ratchet.sh is what applies the spec's +2% tolerance
    (review/test-suites-spec.md §T0.3) to host text, mcu text and mcu data+bss —
    RAM is a gate failure like text (addendum A2 / tuvm#5). Calling footprint.sh
    here meant the suite enforced a zero tolerance the spec never asked for while
    the actual T0.3 script had no caller at all and had never run.

    Returns "PASS", "SKIP" (exit 77 — no arm-zephyr-eabi, so the MCU half did not
    run), or "FAIL"."""
    script = REPO_ROOT / "review" / "tests" / "t0-gates" / "ratchet.sh"
    if not script.is_file():
        # An in-repo gate that cannot be found is a FAIL, never a SKIP: a missing
        # path is the restructure hazard from #143/#110, not an absent toolchain.
        print(f"{COLOR_RED}footprint(T0.3): FAIL{COLOR_RESET} (gate script not found at {script})")
        return "FAIL"
    try:
        proc = subprocess.run(
            ["bash", str(script)],
            cwd=str(REPO_ROOT), capture_output=True, text=True, timeout=600,
            # ratchet.sh resolves the tree it measures from its own location; honour
            # an OEVM_DIR override so a branch worktree still measures itself.
            env={**os.environ, "TUVM_DIR": str(OEVM_DIR)})
    except Exception as e:
        print(f"{COLOR_RED}footprint(T0.3): FAIL{COLOR_RESET} (runner error: {e})")
        return "FAIL"
    out = (proc.stdout or "") + (proc.stderr or "")
    if proc.returncode == 77:
        # Yellow, and it says what did NOT run: an unrun gate must not read as a
        # passing one (tuvm#31 — this gate reported SKIP for weeks and scrolled past).
        print(f"{COLOR_YELLOW}footprint(T0.3): SKIP{COLOR_RESET} — the MCU half did NOT run "
              f"(no arm-zephyr-eabi on PATH; source ns-zephyr/tools/env.sh). Host half only.")
        return "SKIP"
    if proc.returncode != 0:
        print(f"{COLOR_RED}footprint(T0.3): FAIL{COLOR_RESET}")
        print("  " + "\n  ".join(out.strip().splitlines()[-30:]))
        return "FAIL"
    print(f"{COLOR_GREEN}footprint(T0.3): PASSED{COLOR_RESET}")
    for line in out.strip().splitlines():
        if line.startswith("ratchet ok:"):
            print(f"  {line}")
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
    test_dir = Path(__file__).parent.parent.parent / "tests"
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
        "src/udp.c", "src/http.c", "src/http_core.c", "src/mirror.c", "src/ota.c",
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
    test_dir = Path(__file__).parent.parent.parent / "tests"
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
    # tuvm#94 stage 5: the deep-sleep node harness drives the REAL MagTagDeepSleepNode
    # across simulated sleeps via OEVM_RTC_FILE. It uses absolute paths + its own
    # _build_pathb_once(), so it does NOT clobber ../tuvm/src/solidified_image.c the way
    # the grant/debugger gates do — safe here, after the checkout restore.
    deepsleep_ok = run_integration_gate("test_deepsleep_node", "test_deepsleep_node.py")
    footprint = run_footprint_gate()   # "PASS" | "SKIP" | "FAIL"
    # #165: the host-tool Python suites. They build nothing and touch no shared
    # /tmp path, so they are safe here; they were run by NOTHING until now.
    mprof_py = run_pytest_gate("mprof-tools-py", "nodus/tuvm/tools")
    console_py = run_pytest_gate("console-py", "nodus/tuvm/zephyr/tools/console")
    # #216: the target harness's OBSERVATION WINDOW — the assertion that a
    # scenario keeps being watched after its TAPs pass. The window needs no board:
    # it is logic over the collector's log, so it is falsifiable here, and the
    # board is then spent only on what genuinely needs one. This is the first
    # thing under tests/target/ that this suite can run, and it is exactly the
    # piece whose absence let a gate score PASS 3/3 against a node that died four
    # ticks later (#213).
    observe_py = run_pytest_gate("target-observe-py", "nodus/tuvm/tests/target")

    suite_ok = (failed == 0 and c_units_ok and grant_ok and dbg_int_ok
                and portability != "FAIL" and boot_gc_ok
                and footprint != "FAIL" and deepsleep_ok
                and mprof_py != "FAIL" and console_py != "FAIL"
                and observe_py != "FAIL")
    print(f"\n{COLOR_GREEN if suite_ok else COLOR_RED}Test Execution Summary:{COLOR_RESET}")
    print(f"  Passed:  {passed}")
    print(f"  Failed:  {failed}")
    print(f"  Skipped: {skipped}")
    # A SKIP is coloured, because an aux gate that did not run must not read as one
    # that passed (tuvm#31, #63, #143): footprint sat at SKIP for weeks under a green
    # header while the real T0.3 numbers were three-quarters red.
    def _mark(status: str) -> str:
        if status == "SKIP":
            return f"{COLOR_YELLOW}SKIP{COLOR_RESET}"
        if status == "FAIL":
            return f"{COLOR_RED}FAIL{COLOR_RESET}"
        return status

    print(f"  grant: {_mark('PASS' if grant_ok else 'FAIL')}  "
          f"debugger-integration: {_mark('PASS' if dbg_int_ok else 'FAIL')}  "
          f"portability-32: {_mark(portability)}  "
          f"boot-gc-asan: {_mark('PASS' if boot_gc_ok else 'FAIL')}  "
          f"deepsleep: {_mark('PASS' if deepsleep_ok else 'FAIL')}  "
          f"footprint(T0.3): {_mark(footprint)}\n"
          f"  mprof-tools-py: {_mark(mprof_py)}  "
          f"console-py: {_mark(console_py)}  "
          f"target-observe-py: {_mark(observe_py)}")

    # NOT a skip, and deliberately not coloured like one: a skip names a cause it
    # has not verified, and this names one that IS verified. timer_zephyr.c posts
    # through the pulse mailbox; timer_posix.c posts straight into the space via
    # space_post. Every test counted above ran on the latter, so this suite is
    # STRUCTURALLY silent about coalescing, shedding and stale handles — a green
    # header here says nothing whatever about them (#151, #56 step 4).
    #
    # The reader most likely to be misled by "N passed, 0 failed" is the one who
    # never opens review/tests/CLAIMS.md, and that reader is often an agent
    # reading this line and nothing else. So the line says it here.
    target_dir = Path(__file__).resolve().parents[2] / "tuvm" / "tests" / "target"
    n_scen = len(list(target_dir.glob("*.env"))) if target_dir.is_dir() else 0
    print(f"  target gates: {COLOR_YELLOW}NOT RUN BY THIS SUITE{COLOR_RESET} — the posix timer "
          f"backend bypasses the pulse\n"
          f"    mailbox, so coalescing, shedding and stale handles are UNTESTED here.\n"
          f"    (target-observe-py above is the harness's OWN logic, not the seams — #216.)\n"
          f"    Run: nodus/tuvm/tests/target/run.sh   ({n_scen} scenarios, needs a board)")

    if not suite_ok:
        sys.exit(1)

if __name__ == "__main__":
    main()
