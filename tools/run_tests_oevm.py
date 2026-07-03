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
        elif in_expect:
            metadata["expect"].append(line)
            
    return metadata

def run_test_file(src_path: Path) -> tuple[str, str]:
    metadata = parse_test_metadata(src_path)
    
    # Note: skip-berry is an ns-berry limitation; oevm attempts those tests.
    # Use skip-oevm to mark tests oevm genuinely cannot run yet.
    if metadata.get("skip_oevm"):
        return "SKIP", "Skipped (skip-oevm: true)"

    if not (metadata.get("expect") or metadata.get("expect_compile_fail") or metadata.get("expect_runtime_fail")):
        return "SKIP", "No expectations defined."

    # 1. Compile .ns to src/solidified_image.c
    compiler_script = Path(__file__).parent / "ns_compile_oevm.py"
    target_c = Path(__file__).parent.parent / "src" / "solidified_image.c"
    
    proc_compile = subprocess.run(
        [sys.executable, str(compiler_script), str(src_path), str(target_c)],
        capture_output=True, text=True
    )
    
    if proc_compile.returncode != 0:
        err_msg = proc_compile.stderr or proc_compile.stdout
        if metadata.get("expect_compile_fail"):
            return "PASS", "Expected compilation failure."
        return "FAIL", f"Compilation failed:\n{err_msg}"
        
    if metadata.get("expect_compile_fail"):
        return "FAIL", "Expected compilation failure but compiled successfully."

    # 2. Build the C runner
    oevm_dir = Path(__file__).parent.parent
    runner_bin = oevm_dir / "oevm_runner"
    
    # We clean the previous bin first
    if runner_bin.exists():
        runner_bin.unlink()
        
    cmd_build = [
        "gcc", "-Wall", "-Wextra", "-std=c99",
        "src/vm.c", "src/gc.c", "src/dispatch.c", "src/platform.c", "src/solidified_image.c", "src/oevm_main.c",
        "-o", "oevm_runner"
    ]
    proc_build = subprocess.run(cmd_build, cwd=str(oevm_dir), capture_output=True, text=True)
    if proc_build.returncode != 0:
        return "FAIL", f"C Build failed:\n{proc_build.stderr or proc_build.stdout}"

    # 3. Run the compiled runner
    proc_run = subprocess.run([str(runner_bin)], capture_output=True, text=True)
    if proc_run.returncode != 0:
        err_msg = proc_run.stderr or proc_run.stdout
        if metadata.get("expect_runtime_fail"):
            return "PASS", f"Expected runtime failure: {err_msg.strip()}"
        return "FAIL", f"Runtime crashed:\n{err_msg}"

    if metadata.get("expect_runtime_fail"):
        return "FAIL", "Expected runtime failure but ran successfully."

    # Compare stdout lines
    actual_lines = [line.strip() for line in proc_run.stdout.strip().splitlines() if line.strip()]
    expected_lines = [line.strip() for line in metadata.get("expect", []) if line.strip()]

    if actual_lines != expected_lines:
        return "FAIL", f"Output mismatch.\nExpected: {expected_lines}\nActual:   {actual_lines}"

    return "PASS", "Output matches expectations."

def main():
    test_dir = Path(__file__).parent.parent.parent / "ns-tests"
    if not test_dir.exists():
        print(f"Error: tests directory not found at {test_dir}")
        sys.exit(1)

    # Scan and sort test files
    test_files = sorted(list(test_dir.glob("**/*.ns")))
    
    passed = 0
    failed = 0
    skipped = 0
    
    print(f"Discovered {len(test_files)} Newspeak conformance test files.\n")
    
    for f in test_files:
        rel_path = f.relative_to(test_dir.parent)
        print(f"Running {str(rel_path):50} ... ", end="", flush=True)
        
        try:
            status, msg = run_test_file(f)
            if status == "PASS":
                passed += 1
                print(f"{COLOR_GREEN}PASS{COLOR_RESET}")
            elif status == "SKIP":
                skipped += 1
                print(f"{COLOR_YELLOW}SKIP{COLOR_RESET} ({msg})")
            else:
                failed += 1
                print(f"{COLOR_RED}FAIL{COLOR_RESET}\n  {msg}")
        except Exception as e:
            failed += 1
            print(f"{COLOR_RED}ERROR{COLOR_RESET}\n  Unexpected execution error: {e}")
            
    print(f"\n{COLOR_GREEN if failed == 0 else COLOR_RED}Test Execution Summary:{COLOR_RESET}")
    print(f"  Passed:  {passed}")
    print(f"  Failed:  {failed}")
    print(f"  Skipped: {skipped}")
    
    if failed > 0:
        sys.exit(1)

if __name__ == "__main__":
    main()
