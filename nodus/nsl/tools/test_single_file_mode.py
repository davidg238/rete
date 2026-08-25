"""#252 — single-file mode: run_tests_oevm.py <file.nsl> runs just that file.

The iterate-on-one-test loop is seconds; the suite is minutes. Naming a file —
including one OUTSIDE nodus/tests — must run only it under the harness contract
(Path B by default), never fall through to the full-suite scan.

Each test shells out to the real harness: this is the user-facing contract
(argv in, exit code + report out), so the process boundary IS the unit.
"""
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
HARNESS = HERE / "run_tests_oevm.py"

# Generous for a cold Path B build (gcc of the image VM); far below a suite run.
TIMEOUT_S = 90


def run_harness(args, tmp_path, extra_env=None):
    env = dict(os.environ)
    # Never inherit a suite-run's Path A/B selection into these assertions.
    env.pop("OEVM_PATH", None)
    # Isolate by default: this file runs as a gate INSIDE a suite run, and the
    # suite's own Path B artifacts live at the /tmp default.
    env["OEVM_BUILD_DIR"] = str(tmp_path / "harness-bd")
    if extra_env:
        env.update(extra_env)
    return subprocess.run(
        [sys.executable, str(HARNESS), *[str(a) for a in args]],
        capture_output=True, text=True, timeout=TIMEOUT_S, env=env,
    )


def write_scratch(tmp_path, name, body):
    f = tmp_path / name
    f.write_text(body)
    return f


def test_named_file_outside_tests_runs_alone_and_passes(tmp_path):
    scratch = write_scratch(tmp_path, "scratch_pass.nsl",
                            "(*\nexpect:\n7\n*)\n(3 + 4) println\n")
    proc = run_harness([scratch], tmp_path)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "PASS" in proc.stdout
    # The full-suite scan announces itself; single-file mode must not.
    assert "Discovered" not in proc.stdout


def test_named_file_expectation_mismatch_exits_nonzero(tmp_path):
    scratch = write_scratch(tmp_path, "scratch_fail.nsl",
                            "(*\nexpect:\n8\n*)\n(3 + 4) println\n")
    proc = run_harness([scratch], tmp_path)
    assert proc.returncode == 1, proc.stdout + proc.stderr
    assert "FAIL" in proc.stdout


def test_missing_file_is_a_loud_setup_error(tmp_path):
    proc = run_harness([tmp_path / "no_such.nsl"], tmp_path)
    assert proc.returncode == 2, proc.stdout + proc.stderr


def test_pytest_gate_accepts_a_single_file_target():
    """The suite gates this file via run_pytest_gate — which must therefore
    take a FILE target, so the heavy integration siblings in this directory
    (grant, debugger, deepsleep) are not collected twice per suite run."""
    import run_tests_oevm as R
    assert R.run_pytest_gate("probe", "nodus/nsl/tools/test_gen_builtins.py") == "PASS"


def test_build_dir_env_isolates_path_b_artifacts(tmp_path):
    """OEVM_BUILD_DIR moves every Path B artifact out of the shared /tmp
    namespace — the 'never run two suites concurrently' rule exists only
    because of those shared paths."""
    bd = tmp_path / "bd"
    scratch = write_scratch(tmp_path, "scratch_iso.nsl",
                            "(*\nexpect:\n7\n*)\n(3 + 4) println\n")
    proc = run_harness([scratch], tmp_path, extra_env={"OEVM_BUILD_DIR": str(bd)})
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert (bd / "oevm_img").is_file()
    assert (bd / "oevm_layout.json").is_file()
    assert list(bd.glob("*.oimg")), "compiled image should land in OEVM_BUILD_DIR"
