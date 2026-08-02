#!/usr/bin/env python3
"""Compile every ns-tests/**/*.ns to a per-test solidified C image under <outdir>.
Used to prove a refactor is a no-op: snapshot before, snapshot after, `diff -r`.
Compile failures are recorded as a stable marker so expect-compile-fail tests
stay comparable."""
import sys, subprocess, pathlib

HERE = pathlib.Path(__file__).resolve().parent
COMPILER = HERE / "ns_compile_oevm.py"
TESTS = HERE.parent.parent / "ns-tests"

def main():
    outdir = pathlib.Path(sys.argv[1])
    outdir.mkdir(parents=True, exist_ok=True)
    for ns in sorted(TESTS.glob("**/*.ns")):
        rel = ns.relative_to(TESTS)
        out = outdir / (str(rel).replace("/", "__") + ".c")
        r = subprocess.run(
            [sys.executable, str(COMPILER), str(ns), str(out)],
            capture_output=True, text=True,
        )
        if r.returncode != 0:
            out.write_text("COMPILE_FAIL\n")
    print(f"snapshot written to {outdir}")

if __name__ == "__main__":
    main()
