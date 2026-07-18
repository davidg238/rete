#!/usr/bin/env python3
# Single-test runner — the fast inner loop for one `.nsl` file.
#
# run_tests_oevm.py main() ignores argv (it globs the WHOLE suite and also builds
# the C unit tests / integration gates — the "controller owns full-suite runs"
# path). This tool imports its per-file functions directly so a single test runs
# on the identical code paths, in seconds, without touching the full gate.
#
# Runs both equivalence paths by default: Path A (source) and Path B (oimg).
#
# Usage:
#   python3 tools/run-one.py <path-to.nsl> [A|B|AB]     # default AB
# Example:
#   python3 tools/run-one.py ../nsl-tests/01_expressions/01_addition.nsl
import os, sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
os.environ.setdefault("OEVM_PATH", "AB")
sys.path.insert(0, str(TOOLS))
import run_tests_oevm as R

if len(sys.argv) < 2:
    sys.exit(__doc__)

f = Path(sys.argv[1]).resolve()
if not f.exists():
    sys.exit(f"no such test file: {f}")
which = (sys.argv[2] if len(sys.argv) > 2 else "AB").upper()

rc = 0
if which in ("A", "AB"):
    sa, ma = R.run_test_file(f); print(f"Path A: {sa}  {ma}"); rc |= (sa != "PASS")
if which in ("B", "AB"):
    sb, mb = R.run_test_file_oimg(f); print(f"Path B: {sb}  {mb}"); rc |= (sb != "PASS")
sys.exit(1 if rc else 0)
