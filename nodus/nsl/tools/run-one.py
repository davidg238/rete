#!/usr/bin/env python3
# Single-test runner — the fast inner loop for one `.nsl` file.
#
# run_tests_oevm.py main() ignores argv (it globs the WHOLE suite and also builds
# the C unit tests / integration gates — the "controller owns full-suite runs"
# path). This tool imports its per-file functions directly so a single test runs
# on the identical code paths, in seconds, without touching the full gate.
#
# Runs Path B (the .oimg) by default, the path the suite runs. Path A (source, baked
# as C) is asked for by name: its bake overwrites the TRACKED
# nodus/oevm/src/solidified_image.c, so the safe path is the one that needs no
# argument (docs/development.md §4).
#
# Usage:
#   python3 tools/run-one.py <path-to.nsl> [B|A|AB]     # default B
# Example:
#   python3 tools/run-one.py ../tests/01_expressions/01_addition.nsl
import os, sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
USAGE = "usage: python3 tools/run-one.py <path-to.nsl> [B|A|AB]   (default B)"
if len(sys.argv) < 2:
    sys.exit(USAGE)
which = (sys.argv[2] if len(sys.argv) > 2 else "B").upper()
if which not in ("A", "B", "AB"):
    sys.exit(USAGE)
os.environ.setdefault("OEVM_PATH", which)
sys.path.insert(0, str(TOOLS))
import run_tests_oevm as R
R.hold_suite_lock("run-one.py")

f = Path(sys.argv[1]).resolve()
if not f.exists():
    sys.exit(f"no such test file: {f}")

rc = 0
if which in ("A", "AB"):
    sa, ma = R.run_test_file(f); print(f"Path A: {sa}  {ma}"); rc |= (sa != "PASS")
if which in ("B", "AB"):
    sb, mb = R.run_test_file_oimg(f); print(f"Path B: {sb}  {mb}"); rc |= (sb != "PASS")
sys.exit(1 if rc else 0)
