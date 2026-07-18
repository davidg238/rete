#!/usr/bin/env python3
"""Newspeak-to-Berry VM (nsLite) Test Harness.

This script executes integration tests for the ns-berry compiler and runtime.
It scans the directory recursively for `.ns` files, compiles them to `.bec`,
runs them with the Berry VM, and asserts the output matches the expected metadata.

Test files define their assertions in header comments using the following formats:

    (*
    expect:
    36
    false
    1
    *)

Or for compilation failure testing:

    (*
    expect-compile-fail: non-local return
    *)
"""

import sys
import os
import subprocess
import re
import shutil
from pathlib import Path

# Setup paths relative to this script
NS_TESTS_DIR = Path(__file__).resolve().parent
WORKSPACE_DIR = NS_TESTS_DIR.parent
NS_BERRY_DIR = WORKSPACE_DIR / "ns-berry"
BERRY_EXE = Path(os.environ.get("BERRY", "/home/david/workspace/berry/berry"))

# Inject ns-berry and its vendored backend into sys.path to allow direct imports
if NS_BERRY_DIR.exists():
    sys.path.insert(0, str(NS_BERRY_DIR))
    sys.path.insert(0, str(NS_BERRY_DIR / "backend"))

# Try to import compilation drivers directly; fallback to CLI subprocess if needed
try:
    from compiler.ns_compile import compile_file
    from compiler.ns_lower import NsLowerError
    from compiler.ns_compile import NsCompileError
    DIRECT_COMPILATION = True
except ImportError:
    DIRECT_COMPILATION = False

# Colors for terminal output
COLOR_GREEN = "\033[92m"
COLOR_RED = "\033[91m"
COLOR_YELLOW = "\033[93m"
COLOR_RESET = "\033[0m"


def parse_test_metadata(file_path: Path):
    """Parse expected outcomes from the Newspeak test file's top-level comments."""
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
        elif line.startswith("skip-soup:"):
            in_expect = False
        elif in_expect:
            metadata["expect"].append(line)
            
    return metadata


def compile_test(src_path: Path, tmp_dir: Path):
    """Compile a .ns source file. Returns (bec_path, error_message)."""
    if DIRECT_COMPILATION:
        try:
            bec_path, _ = compile_file(str(src_path), out_dir=str(tmp_dir))
            return Path(bec_path), None
        except (NsLowerError, NsCompileError) as e:
            return None, str(e)
        except Exception as e:
            return None, f"Unexpected compilation exception: {e}"
    else:
        # Fallback to subprocess compiler driver
        compiler_script = NS_BERRY_DIR / "compiler" / "ns_compile.py"
        bec_path = tmp_dir / (src_path.stem + ".bec")
        proc = subprocess.run(
            [sys.executable, str(compiler_script), str(src_path), "-d", str(tmp_dir)],
            capture_output=True, text=True
        )
        if proc.returncode != 0:
            err_msg = proc.stderr or proc.stdout or "Unknown compiler CLI failure"
            return None, err_msg
        return bec_path, None


def run_test_file(src_path: Path, tmp_dir: Path) -> tuple[str, str]:
    """Run a single test file and compare outcomes to expected metadata."""
    metadata = parse_test_metadata(src_path)
    
    if metadata.get("skip_berry"):
        return "SKIP", "Skipped (skip-berry: true)"

    # If no expectations are documented, skip or report error
    if not (metadata.get("expect") or metadata.get("expect_compile_fail") or metadata.get("expect_runtime_fail")):
        return "FAIL", "No expectation metadata (expect: or expect-compile-fail:) found in file comments."

    # 1. Compile
    bec_path, compile_err = compile_test(src_path, tmp_dir)
    
    # Check compile-fail expectations
    if compile_err is not None:
        expected_fail = metadata.get("expect_compile_fail")
        if expected_fail:
            if expected_fail.lower() in compile_err.lower():
                return "PASS", "Passed (expected compilation failure)"
            else:
                return "FAIL", f"Compilation failed, but error message did not match.\nExpected: {expected_fail}\nGot: {compile_err}"
        return "FAIL", f"Compilation failed unexpectedly:\n{compile_err}"
    
    if metadata.get("expect_compile_fail"):
        return "FAIL", f"Compilation succeeded, but expected failure: {metadata['expect_compile_fail']}"

    # Verify VM executable exists
    if not BERRY_EXE.exists() and not shutil.which(str(BERRY_EXE)):
        return "FAIL", f"Berry VM not found at {BERRY_EXE}. Please build it in ~/workspace/berry first."

    # 2. Run in Berry VM
    proc = subprocess.run(
        [str(BERRY_EXE), str(bec_path)],
        capture_output=True, text=True
    )
    
    # Check runtime-fail expectations
    if proc.returncode != 0:
        expected_runtime_fail = metadata.get("expect_runtime_fail")
        err_out = proc.stderr or proc.stdout
        if expected_runtime_fail:
            if expected_runtime_fail.lower() in err_out.lower():
                return "PASS", "Passed (expected runtime failure)"
            else:
                return "FAIL", f"Runtime failed, but error message did not match.\nExpected: {expected_runtime_fail}\nGot: {err_out}"
        return "FAIL", f"Runtime execution failed (exit code {proc.returncode}):\n{err_out}"
        
    if metadata.get("expect_runtime_fail"):
        return "FAIL", "Runtime execution succeeded, but expected failure"

    # 3. Verify Output
    actual_lines = [line.strip() for line in proc.stdout.strip().splitlines() if line.strip()]
    expected_lines = [line.strip() for line in metadata.get("expect", []) if line.strip()]
    
    if actual_lines != expected_lines:
        return "FAIL", f"Output mismatch.\nExpected:\n  " + "\n  ".join(expected_lines) + "\nGot:\n  " + "\n  ".join(actual_lines)
        
    return "PASS", "Passed"


def main():
    import tempfile
    
    # Gather all .ns files, grouped by directories
    test_files = sorted(list(NS_TESTS_DIR.glob("**/*.nsl")))
    # Exclude temporary scratch files
    test_files = [f for f in test_files if "scratch" not in f.name]

    if not test_files:
        print(f"{COLOR_YELLOW}No nsl (.nsl) test files found in {NS_TESTS_DIR}{COLOR_RESET}")
        return 0
        
    print(f"Discovered {len(test_files)} tests in {NS_TESTS_DIR}\n")
    
    passed = 0
    failed = 0
    skipped = 0
    COLOR_BLUE = "\033[94m"
    
    with tempfile.TemporaryDirectory() as tmp_dir_name:
        tmp_dir = Path(tmp_dir_name)
        
        current_dir = None
        for test_file in test_files:
            # Grouping headers
            rel_dir = test_file.parent.relative_to(NS_TESTS_DIR)
            if rel_dir != current_dir:
                current_dir = rel_dir
                dir_label = str(rel_dir) if str(rel_dir) != "." else "Root"
                print(f"\n--- Feature Group: {dir_label} ---")
                
            status, msg = run_test_file(test_file, tmp_dir)
            
            test_name = test_file.name
            if status == "PASS":
                print(f"  [{COLOR_GREEN}PASS{COLOR_RESET}] {test_name}")
                passed += 1
            elif status == "SKIP":
                print(f"  [{COLOR_BLUE}SKIP{COLOR_RESET}] {test_name} ({msg})")
                skipped += 1
            else:
                print(f"  [{COLOR_RED}FAIL{COLOR_RESET}] {test_name}")
                print(f"         Reason: {msg.replace('\n', '\n                 ')}")
                failed += 1
                
    total = passed + failed + skipped
    print("\n" + "=" * 40)
    print(f"Test Run Summary:")
    print(f"  Total:    {total}")
    print(f"  Passed:   {COLOR_GREEN}{passed}{COLOR_RESET}")
    print(f"  Skipped:  {COLOR_BLUE}{skipped}{COLOR_RESET}")
    print(f"  Failed:   {COLOR_RED if failed > 0 else COLOR_RESET}{failed}{COLOR_RESET}")
    print("=" * 40)
    
    return 1 if failed > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
