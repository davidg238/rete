#!/usr/bin/env python3
"""Newspeak Official VM (Primordial Soup) Test Harness.

This script executes the integration tests using the official Primordial Soup VM
and Compiler. It allows verifying test correctness against the reference implementation.

Because the tests are written using bare expressions and `println` for the
simplified ns-berry environment, this runner dynamically:
1. Replaces `println` with `printString out`.
2. Wraps the code inside a standard Newspeak class declaration (`TestApp`).
3. Compiles the wrapped app using the official `CompilerApp.vfuel` compiler.
4. Executes the output snapshot using the official `primordialsoup` VM.
"""

import sys
import os
import subprocess
import re
import shutil
import tempfile
from pathlib import Path

# Setup paths relative to this script
NS_TESTS_DIR = Path(__file__).resolve().parent
WORKSPACE_DIR = NS_TESTS_DIR.parent
SOUP_DIR = WORKSPACE_DIR / "primordialsoup"
SOUP_EXE = SOUP_DIR / "out" / "ReleaseHost" / "primordialsoup"
SOUP_COMPILER_SNAPSHOT = SOUP_DIR / "out" / "snapshots" / "CompilerApp.vfuel"
SOUP_STD_LIB = SOUP_DIR / "newspeak"

# Colors for terminal output
COLOR_GREEN = "\033[92m"
COLOR_RED = "\033[91m"
COLOR_YELLOW = "\033[93m"
COLOR_BLUE = "\033[94m"
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
        "skip_soup": False,
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
        elif line.startswith("skip-soup:"):
            in_expect = False
            metadata["skip_soup"] = line[len("skip-soup:"):].strip().lower() == "true"
        elif line.startswith("skip-berry:"):
            in_expect = False
        elif in_expect:
            metadata["expect"].append(line)
            
    return metadata


def adapt_and_wrap_code(src_path: Path, class_name: str) -> str:
    """Read the source code, translate println, and wrap it in a Newspeak class."""
    code = src_path.read_text(encoding="utf-8")
    
    # Remove the metadata comment block at the top
    comment_pattern = re.compile(r"^\s*\(\*.*?\*\)\s*", re.DOTALL)
    code_body = comment_pattern.sub("", code)
    
    # Replace println with printString out
    adapted_body = re.sub(r"\bprintln\b", "printString out", code_body)
    
    # Wrap in a standard Newspeak class definition
    wrapped = f"""class {class_name} packageUsing: manifest = (
) (
public main: platform args: args = (
{adapted_body}
)
) : (
)
"""
    return wrapped


def run_test_file(src_path: Path, tmp_dir: Path) -> tuple[str, str]:
    """Run a single test file using Primordial Soup and compare outcomes to expected metadata."""
    metadata = parse_test_metadata(src_path)
    
    if metadata.get("skip_soup"):
        return "SKIP", "Skipped (skip-soup: true)"
        
    if not (metadata.get("expect") or metadata.get("expect_compile_fail") or metadata.get("expect_runtime_fail")):
        return "FAIL", "No expectation metadata found in file comments."

    # Read the file to see if it starts with a class declaration
    code = src_path.read_text(encoding="utf-8")
    
    # Strip comments first to find the actual code start
    comment_pattern = re.compile(r"^\s*\(\*.*?\*\)\s*", re.DOTALL)
    code_no_comments = comment_pattern.sub("", code)
    
    class_match = re.match(r"^\s*class\s+(\w+)", code_no_comments)
    
    if class_match:
        # It's already a full Newspeak class test!
        class_name = class_match.group(1)
        temp_src_path = tmp_dir / f"{class_name}.ns"
        temp_src_path.write_text(code_no_comments, encoding="utf-8")
    else:
        # Bare expression, wrap and adapt
        class_name = f"soup_test_{src_path.stem}"
        temp_src_path = tmp_dir / f"{class_name}.ns"
        wrapped_code = adapt_and_wrap_code(src_path, class_name)
        temp_src_path.write_text(wrapped_code, encoding="utf-8")
        
    temp_bec_path = tmp_dir / f"{class_name}.vfuel"
    
    # 2. Compile using official CompilerApp.vfuel
    # Command: primordialsoup CompilerApp.vfuel newspeak/*.ns <temp_src> Runtime <class_name> <temp_bec>
    std_lib_files = list(SOUP_STD_LIB.glob("*.ns"))
    cmd_compile = [
        str(SOUP_EXE),
        str(SOUP_COMPILER_SNAPSHOT),
    ] + [str(f) for f in std_lib_files] + [
        str(temp_src_path),
        "Runtime",
        class_name,
        str(temp_bec_path)
    ]
    
    proc_compile = subprocess.run(cmd_compile, capture_output=True, text=True)
    
    if proc_compile.returncode != 0:
        err_msg = proc_compile.stderr or proc_compile.stdout
        expected_fail = metadata.get("expect_compile_fail")
        if expected_fail:
            # We don't verify exact message match as error string formats differ
            return "PASS", "Passed (expected compilation failure)"
        return "FAIL", f"Compilation failed unexpectedly:\n{err_msg}"
        
    if metadata.get("expect_compile_fail"):
        return "FAIL", f"Compilation succeeded, but expected failure: {metadata['expect_compile_fail']}"

    # 3. Run using official VM executor
    cmd_run = [str(SOUP_EXE), str(temp_bec_path)]
    proc_run = subprocess.run(cmd_run, capture_output=True, text=True)
    
    if proc_run.returncode != 0:
        err_msg = proc_run.stderr or proc_run.stdout
        expected_runtime_fail = metadata.get("expect_runtime_fail")
        if expected_runtime_fail:
            return "PASS", "Passed (expected runtime failure)"
        return "FAIL", f"Runtime execution failed (exit code {proc_run.returncode}):\n{err_msg}"
        
    if metadata.get("expect_runtime_fail"):
        return "FAIL", "Runtime execution succeeded, but expected failure"

    # 4. Verify Output
    actual_lines = [line.strip() for line in proc_run.stdout.strip().splitlines() if line.strip()]
    expected_lines = [line.strip() for line in metadata.get("expect", []) if line.strip()]
    
    if actual_lines != expected_lines:
        return "FAIL", f"Output mismatch.\nExpected:\n  " + "\n  ".join(expected_lines) + "\nGot:\n  " + "\n  ".join(actual_lines)
        
    return "PASS", "Passed"


def main():
    if not SOUP_EXE.exists():
        print(f"{COLOR_RED}Error: Primordial Soup VM executor not found at {SOUP_EXE}.{COLOR_RESET}")
        print("Please build it first by running `uvx scons` in ~/workspace/primordialsoup.")
        return 1

    if not SOUP_COMPILER_SNAPSHOT.exists():
        print(f"{COLOR_RED}Error: Primordial Soup compiler snapshot not found at {SOUP_COMPILER_SNAPSHOT}.{COLOR_RESET}")
        return 1

    # Gather all .ns files, grouped by directories
    test_files = sorted(list(NS_TESTS_DIR.glob("**/*.ns")))
    # Exclude temporary scratch files
    test_files = [f for f in test_files if "scratch" not in f.name]
    
    if not test_files:
        print(f"{COLOR_YELLOW}No Newspeak (.ns) test files found in {NS_TESTS_DIR}{COLOR_RESET}")
        return 0
        
    print(f"Discovered {len(test_files)} tests in {NS_TESTS_DIR} for Primordial Soup VM\n")
    
    passed = 0
    failed = 0
    skipped = 0
    
    with tempfile.TemporaryDirectory() as tmp_dir_name:
        tmp_dir = Path(tmp_dir_name)
        
        current_dir = None
        for test_file in test_files:
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
