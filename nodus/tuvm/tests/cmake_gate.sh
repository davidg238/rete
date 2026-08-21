#!/usr/bin/env bash
# nodus/tuvm/tests/cmake_gate.sh — the build files are code, and until rete#226 nothing
# read them without a board.
#
#   ./cmake_gate.sh            parse every tracked CMake listfile (no toolchain, no board)
#   ./cmake_gate.sh --list     print the files it would check and exit
#
# Exit 0 = PASS, 1 = FAIL, 77 = SKIP (no cmake on this host — the one verified cause).
#
# WHY THIS EXISTS. rete#226 deleted both PRIM_CACHE_SIZE blocks from zephyr/CMakeLists.txt
# by removing each `if(...)` and its body but not the `endif()`. The file stopped parsing:
#
#   CMake Error at CMakeLists.txt:160 (endif):
#     Flow control statements are not properly nested.
#
# It stayed broken for a day while the nsl suite read 262/0 with every aux gate PASS and
# PORTA SYNC OK, because nothing on the host reads that file. It surfaced only on the first
# C6 build — which happened to be blocked behind a layout regeneration, so it could as
# easily have surfaced a week later, to somebody else, wearing a costume.
#
# HOW IT PARSES WITHOUT RUNNING ANYTHING. `cmake -P <file>` parses the WHOLE listfile before
# executing any of it (verified: a leading `message(FATAL_ERROR)` never fires when a later
# `endif()` is orphaned), so a parse error always surfaces first. But -P would then EXECUTE
# the file — which for zephyr/CMakeLists.txt means loading Zephyr's module machinery, needing
# ZEPHYR_BASE, and writing to ~/.cache/zephyr. So each file is wrapped:
#
#   if(FALSE)
#   <the file>
#   endif()
#
# The body is parsed and never executed. Nesting is preserved exactly: a file with one
# `endif()` too many orphans the wrapper's own, and one too few makes the wrapper's `endif()`
# close the inner block and leaves the last one orphaned. Either way it is a parse error.
# Cost: ~12 ms per file, no toolchain, no west venv, no board.
#
# WHAT IT DOES NOT CATCH, stated positively rather than left to be assumed: an unknown
# Kconfig symbol, a `target_sources` naming a file that does not exist, a wrong board, an
# option whose dependency is unmet (which Zephyr DISCARDS silently — see the nRF52840 note
# in zephyr/docs/BOARD_PORTS.md). Those need a real configure, which needs the west venv and
# the SDK, and the honest place they get caught today is the first target build. This gate
# is the part that can run everywhere, every time, and it is the part that was missing.
set -uo pipefail
cd "$(dirname "$0")"
REPO_ROOT=$(cd ../../.. && pwd)

command -v cmake >/dev/null 2>&1 || {
  echo "cmake-parse: SKIP (no cmake on this host)" >&2
  exit 77
}

# git ls-files, so generated build trees (build-c6/, build-esp32c6-B/) are excluded by
# construction rather than by a pattern that goes stale. A repo with no git is a FAIL, not a
# silent empty pass: this gate returning 0 over zero files is the false green it exists to
# prevent.
mapfile -t FILES < <(cd "$REPO_ROOT" && git ls-files '*CMakeLists.txt' '*.cmake' 2>/dev/null)
if [ "${#FILES[@]}" -eq 0 ]; then
  echo "cmake-parse: FAIL — found no tracked CMake listfiles under $REPO_ROOT." >&2
  echo "  Zero files is not a pass. Either this is not a git checkout, or the glob is wrong." >&2
  exit 1
fi

[ "${1:-}" = "--list" ] && { printf '%s\n' "${FILES[@]}"; exit 0; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# check_one <path> -> 0 if it parses, 1 if it does not (message on stdout)
check_one() {
  local f=$1 wrapped="$TMP/wrapped.cmake" out
  { echo 'if(FALSE)'; cat "$f"; echo 'endif()'; } > "$wrapped"
  if out=$(cmake -P "$wrapped" 2>&1); then
    return 0
  fi
  # Line numbers are the wrapper's, one greater than the file's. Say so rather than
  # print a number that sends the reader to the wrong line.
  echo "$out" | sed "s#$wrapped#$f#g; s#^#  #"
  echo "  (line numbers are +1 — the file is parsed inside an if(FALSE) wrapper)"
  return 1
}

# THE CANARY RUNS FIRST. This gate reads a nonzero exit from cmake, and a cmake that stopped
# reporting nesting errors — or a wrapper that stopped wrapping — would pass every file
# silently, which is precisely the shape of failure this bench keeps cataloguing. So prove
# the detector detects before trusting it to clear anything.
printf 'if(DEFINED ENV{X})\n  message(STATUS "x")\nendif()\nendif()\n' > "$TMP/canary.cmake"
if check_one "$TMP/canary.cmake" >/dev/null; then
  echo "cmake-parse: FAIL — the canary PARSED." >&2
  echo "  A file with an orphan endif() was accepted, so this gate is blind and every" >&2
  echo "  PASS it has printed is worthless. Check that cmake still reports 'Flow control" >&2
  echo "  statements are not properly nested' and that the if(FALSE) wrapper is intact." >&2
  exit 1
fi

fails=0
for f in "${FILES[@]}"; do
  if ! check_one "$REPO_ROOT/$f"; then
    echo "cmake-parse: FAIL — $f does not parse"
    fails=$((fails + 1))
  fi
done

if [ "$fails" -ne 0 ]; then
  echo "cmake-parse: FAIL ($fails of ${#FILES[@]} listfiles do not parse)"
  exit 1
fi
echo "cmake-parse: PASS (${#FILES[@]} listfiles, canary proved the check can fail)"
exit 0
