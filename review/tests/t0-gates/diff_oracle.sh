#!/usr/bin/env bash
# T0.2 — differential oracle: run the shared (pared) tests on the FROZEN original VM
# and the seeded tuvm, and diff stdout. Divergence = the deletion changed semantics.
# The frozen repos are a free oracle; this gate turns "did A change behavior?" into a
# mechanical question. Run SOLO + FOREGROUND (harness not concurrency-safe).
#
# Usage: diff_oracle.sh [test-filter-glob]
# Exit: 0 = no divergence, 1 = divergence (report printed), 2 = setup error.
set -u

# Repo root, independent of cwd: this script lives at review/tests/t0-gates/,
# four levels below the root, regardless of where it is invoked from.
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && git rev-parse --show-toplevel)"

FROZEN_DIR=${FROZEN_DIR:-$HOME/workspace/oevm}          # frozen VM (never modified)
TUVM_DIR=${TUVM_DIR:-$REPO_ROOT/nodus/tuvm}             # seeded VM under test
TESTS_DIR=${TESTS_DIR:-$REPO_ROOT/nodus/tests}
RUNNER=${RUNNER:-$REPO_ROOT/nodus/nsl/tools/run_tests_oevm.py}
MANIFEST=${MANIFEST:-$TESTS_DIR/pared-subset.txt}       # shared-test list (Milestone A artifact)
OUT=${OUT:-$(mktemp -d)}
FILTER=${1:-*}

[ -x "$(command -v python3)" ] || { echo "python3 missing" >&2; exit 2; }
[ -f "$MANIFEST" ] || { echo "pared manifest not found: $MANIFEST" >&2; exit 2; }

# Each side runs the same runner against its VM; per-test stdout is captured under
# $OUT/<side>/<test>.out. The runner is expected to honor OEVM_DIR (it already does)
# and OEVM_CAPTURE_DIR (harness extension #4 — add: dump per-test stdout there).
run_side () { # $1=label $2=vmdir
  mkdir -p "$OUT/$1"
  OEVM_DIR="$2" OEVM_PATH=AB OEVM_CAPTURE_DIR="$OUT/$1" \
    python3 "$RUNNER" --manifest "$MANIFEST" --filter "$FILTER" \
    > "$OUT/$1/_summary.txt" 2>&1
}

run_side frozen "$FROZEN_DIR" || true   # PASS/FAIL judged by diff, not exit code
run_side tuvm   "$TUVM_DIR"   || true

DIVERGED=0
while IFS= read -r t; do
  case "$t" in \#*|"") continue ;; esac
  f="$OUT/frozen/${t//\//_}.out"; s="$OUT/tuvm/${t//\//_}.out"
  if ! diff -q "$f" "$s" >/dev/null 2>&1; then
    DIVERGED=1
    echo "DIVERGENCE: $t"
    diff "$f" "$s" | head -20
    echo "---"
  fi
done < "$MANIFEST"

if [ "$DIVERGED" -eq 0 ]; then
  echo "oracle: no divergence across $(grep -cv '^\s*\(#\|$\)' "$MANIFEST") shared tests"
else
  echo "oracle: DIVERGENCE — the deletion changed observable semantics (see above)" >&2
fi
exit $DIVERGED
