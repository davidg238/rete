#!/usr/bin/env python3
"""Newspeak Performance Benchmarking and VM Comparison.

This script compiles and runs benchmarks (05_somns_benchmarks) and limit tests (06_limit_tests)
on both the Primordial Soup reference VM and the oevm target VM, measuring exact execution durations.
It outputs a comparison table as a Markdown file and raw data as a JSON file.

(ns-berry was the previous target VM; it is retired now that oevm is the way-forward
backend, so the target column reports oevm.)
"""

import sys
import subprocess
import time
import json
import re
import tempfile
import argparse
from pathlib import Path
from statistics import median

# Setup paths relative to this script
NS_TESTS_DIR = Path(__file__).resolve().parent
WORKSPACE_DIR = NS_TESTS_DIR.parent
SOUP_DIR = WORKSPACE_DIR / "primordialsoup"
SOUP_EXE = SOUP_DIR / "out" / "ReleaseHost" / "primordialsoup"
SOUP_COMPILER_SNAPSHOT = SOUP_DIR / "out" / "snapshots" / "CompilerApp.vfuel"
SOUP_STD_LIB = SOUP_DIR / "newspeak"

# oevm target VM: the ns-oevm frontend compiles each .ns to oevm's solidified
# image (src/solidified_image.c), which is then built into a self-contained
# native binary. Unlike Soup/Berry there is no separate bytecode loaded by a
# fixed VM executable — each benchmark IS its own binary, so "execution time"
# is just running that binary.
OEVM_DIR = WORKSPACE_DIR / "oevm"
NS_OEVM_DIR = WORKSPACE_DIR / "ns-oevm"
OEVM_COMPILER = NS_OEVM_DIR / "tools" / "ns_compile_oevm.py"
OEVM_IMAGE = OEVM_DIR / "src" / "solidified_image.c"
# Full appliance source set. Expanded 2026-07-02: the harness's original list
# (vm/gc/dispatch/solidified_image/oevm_main) rotted once the net/serial/mirror
# capabilities landed — those + the posix backend are now required to link, so
# every oevm benchmark failed with COMPILE_ERR. Keep in sync with the appliance
# build in ns-oevm/tools/run_tests_oevm.py.
OEVM_SRCS = [
    "src/vm.c", "src/gc.c", "src/dispatch.c",
    "src/platform.c", "src/serial.c", "src/udp.c", "src/mirror.c",
    "src/backend/posix/platform_posix.c",
    "src/backend/posix/serial_posix.c",
    "src/backend/posix/udp_posix.c",
    "src/solidified_image.c", "src/oevm_main.c",
]

# Colors for terminal output
COLOR_GREEN = "\033[92m"
COLOR_RED = "\033[91m"
COLOR_YELLOW = "\033[93m"
COLOR_BLUE = "\033[94m"
COLOR_RESET = "\033[0m"


def parse_test_metadata(file_path: Path):
    content = file_path.read_text(encoding="utf-8")
    comment_pattern = re.compile(r"\(\*(.*?)\*\)", re.DOTALL)
    match = comment_pattern.search(content)
    if not match:
        return {}
    
    comment_text = match.group(1)
    metadata = {"skip_soup": False, "skip_oevm": False}
    for line in comment_text.splitlines():
        line = line.strip()
        if line.startswith("skip-oevm:"):
            metadata["skip_oevm"] = line[len("skip-oevm:"):].strip().lower() == "true"
        elif line.startswith("skip-soup:"):
            metadata["skip_soup"] = line[len("skip-soup:"):].strip().lower() == "true"
    return metadata


def build_oevm(src_path: Path, tmp_dir: Path):
    """Compile a .ns to oevm's solidified image and build a native binary.

    Returns (binary_path, None) on success or (None, error). The build mutates
    oevm/src/solidified_image.c (a checked-in artifact), so it is restored via
    `git checkout` afterwards, mirroring tools/run_tests_oevm.py.
    """
    # 1. Frontend: .ns -> src/solidified_image.c
    proc_c = subprocess.run(
        [sys.executable, str(OEVM_COMPILER), str(src_path), str(OEVM_IMAGE)],
        capture_output=True, text=True
    )
    if proc_c.returncode != 0:
        return None, proc_c.stderr or proc_c.stdout

    # 2. Build the native binary (-O2, link libm for float primitives).
    #    -DOEVM_APPLIANCE selects Model A (the gcc-optimized static
    #    solidified_image.c). Without it the default build is Model B, which
    #    links the .oimg blob (oimg_blob_base/end) and gates main() out, so the
    #    solidified_image.c sources fail to link.
    binary = tmp_dir / (src_path.stem + ".oevm")
    proc_b = subprocess.run(
        ["gcc", "-O2", "-std=c99", "-Isrc", "-DOEVM_APPLIANCE", *OEVM_SRCS, "-o", str(binary), "-lm"],
        cwd=str(OEVM_DIR), capture_output=True, text=True
    )
    # Restore the checked-in image artifact regardless of build outcome.
    subprocess.run(["git", "checkout", "--", "src/solidified_image.c"],
                   cwd=str(OEVM_DIR), capture_output=True)
    if proc_b.returncode != 0:
        return None, proc_b.stderr or proc_b.stdout
    return binary, None


def compile_soup(src_path: Path, tmp_dir: Path):
    """Compile test file to Primordial Soup snapshot (.vfuel)."""
    code = src_path.read_text(encoding="utf-8")
    
    # Strip metadata comments
    comment_pattern = re.compile(r"^\s*\(\*.*?\*\)\s*", re.DOTALL)
    code_no_comments = comment_pattern.sub("", code)
    
    class_match = re.match(r"^\s*class\s+(\w+)", code_no_comments)
    if class_match:
        class_name = class_match.group(1)
        temp_src_path = tmp_dir / f"{class_name}.ns"
        temp_src_path.write_text(code_no_comments, encoding="utf-8")
    else:
        class_name = f"soup_bench_{src_path.stem}"
        temp_src_path = tmp_dir / f"{class_name}.ns"
        adapted_body = re.sub(r"\bprintln\b", "printString out", code_no_comments)
        wrapped = f"class {class_name} packageUsing: manifest = () (\npublic main: platform args: args = (\n{adapted_body}\n)\n) : ()\n"
        temp_src_path.write_text(wrapped, encoding="utf-8")
        
    temp_bec_path = tmp_dir / f"{class_name}.vfuel"
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
        return None, proc_compile.stderr or proc_compile.stdout, None
    return temp_bec_path, None, class_name


def measure_exec_time(cmd: list[str], runs: int) -> float:
    """Run command 'runs' times and return the median execution time in seconds."""
    times = []
    for _ in range(runs):
        start = time.perf_counter()
        proc = subprocess.run(cmd, capture_output=True)
        end = time.perf_counter()
        if proc.returncode == 0:
            times.append(end - start)
        else:
            return -1.0  # Execution failed
    return median(times) if times else -1.0


def main():
    parser = argparse.ArgumentParser(description="Benchmark and compare Newspeak VMs.")
    parser.add_argument("--runs", type=int, default=3, help="Number of benchmark runs (default: 3)")
    args = parser.parse_args()

    if not SOUP_EXE.exists():
        print(f"{COLOR_RED}Error: Primordial Soup VM executor not found at {SOUP_EXE}.{COLOR_RESET}")
        return 1

    # Gather test files from groups 5 (benchmarks) and 6 (limit tests)
    test_dirs = ["05_somns_benchmarks", "06_limit_tests"]
    test_files = []
    for d in test_dirs:
        dir_path = NS_TESTS_DIR / d
        if dir_path.exists():
            test_files.extend(sorted(list(dir_path.glob("*.ns"))))

    # Exclude temporary scratch files
    test_files = [f for f in test_files if "scratch" not in f.name]

    if not test_files:
        print(f"{COLOR_YELLOW}No benchmark or limit tests found.{COLOR_RESET}")
        return 0

    print(f"Benchmarking {len(test_files)} tests (Runs per test: {args.runs})...\n")

    results = []

    with tempfile.TemporaryDirectory() as tmp_dir_name:
        tmp_dir = Path(tmp_dir_name)

        for test_file in test_files:
            group = test_file.parent.name
            name = test_file.name
            metadata = parse_test_metadata(test_file)

            print(f"Benchmarking {group}/{name}...")

            # 1. Benchmark on Primordial Soup Reference VM
            soup_time = -1.0
            soup_status = "OK"
            if metadata.get("skip_soup"):
                soup_status = "SKIPPED"
            else:
                soup_bec, err, actual_class = compile_soup(test_file, tmp_dir)
                if err:
                    soup_status = "COMPILE_ERR"
                else:
                    soup_time = measure_exec_time([str(SOUP_EXE), str(soup_bec)], args.runs)
                    if soup_time < 0:
                        soup_status = "EXEC_ERR"

            # 2. Benchmark on oevm Target VM
            oevm_time = -1.0
            oevm_status = "OK"
            if metadata.get("skip_oevm"):
                oevm_status = "SKIPPED"
            else:
                oevm_bin, err = build_oevm(test_file, tmp_dir)
                if err:
                    oevm_status = "COMPILE_ERR"
                else:
                    oevm_time = measure_exec_time([str(oevm_bin)], args.runs)
                    if oevm_time < 0:
                        oevm_status = "EXEC_ERR"

            results.append({
                "group": group,
                "name": name,
                "soup_time_s": soup_time if soup_time >= 0 else None,
                "soup_status": soup_status,
                "oevm_time_s": oevm_time if oevm_time >= 0 else None,
                "oevm_status": oevm_status
            })

    # Generate Markdown Table Comparison
    markdown_lines = [
        "# Newspeak VM Performance Comparison",
        f"Generated on: {time.strftime('%Y-%m-%d %H:%M:%S')}",
        f"Runs per test: {args.runs} (median time reported)",
        "",
        "| Group | Benchmark / Test | Reference (Soup) Time | Target (oevm) Time | oevm / Soup |",
        "|---|---|---|---|---|"
    ]

    print("\nBenchmark Results:")
    print("=" * 100)
    print(f"{'Group':<22} | {'Test Name':<22} | {'Soup (Ref)':<12} | {'oevm (Target)':<14} | {'oevm / Soup':<18}")
    print("-" * 100)

    for r in results:
        # Format Reference Time
        if r["soup_status"] == "OK":
            soup_str = f"{r['soup_time_s']:.6f}s"
        else:
            soup_str = f"N/A ({r['soup_status']})"

        # Format Target Time
        if r["oevm_status"] == "OK":
            oevm_str = f"{r['oevm_time_s']:.6f}s"
        else:
            oevm_str = f"N/A ({r['oevm_status']})"

        # Slowdown factor: oevm / Soup (1.0x == parity; >1 == oevm slower).
        if r["soup_status"] == "OK" and r["oevm_status"] == "OK":
            ratio_str = f"{r['oevm_time_s'] / r['soup_time_s']:.2f}x"
        else:
            ratio_str = "N/A"

        print(f"{r['group']:<22} | {r['name']:<22} | {soup_str:<12} | {oevm_str:<14} | {ratio_str:<18}")

        # Add to Markdown list
        markdown_lines.append(
            f"| {r['group']} | {r['name']} | {soup_str} | {oevm_str} | {ratio_str} |"
        )

    # Write Markdown results
    md_file = NS_TESTS_DIR / "benchmark_comparison.md"
    md_file.write_text("\n".join(markdown_lines), encoding="utf-8")
    print(f"\nSaved Markdown comparison to: {md_file}")

    # Write JSON results
    json_file = NS_TESTS_DIR / "benchmark_results.json"
    json_file.write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(f"Saved JSON raw results to: {json_file}")


if __name__ == "__main__":
    main()
