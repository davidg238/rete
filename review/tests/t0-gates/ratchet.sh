#!/usr/bin/env bash
# T0.3 — footprint ratchet: FAIL if the engine grows past the banked baseline + 2%,
# in either flash (text) or RAM (data + bss under the MCU-shaped config; addendum A2).
# Deliberate growth = re-bank the baseline IN THE SAME COMMIT, with rationale.
#
# Baselines (banked, committed):
#   tuvm/tools/footprint-baseline.txt        — host build,   `size` totals
#   tuvm/tools/footprint-baseline-mcu.txt    — MCU-shaped build (small arenas), totals
# Usage: ratchet.sh   (from anywhere; TUVM_DIR overridable)
set -eu

# Repo root, independent of cwd: this script lives at review/tests/t0-gates/,
# four levels below the root, regardless of where it is invoked from.
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && git rev-parse --show-toplevel)"
TUVM_DIR=${TUVM_DIR:-$REPO_ROOT/nodus/tuvm}
TOL_PCT=2

# footprint.sh (Milestone A, Task 1) prints a `size`-style table; totals line is last.
# MCU-shaped config: same compiler, arenas/stack-cap set to target sizes via -D flags —
# the flags live in footprint.sh behind FOOTPRINT_CONFIG=mcu (harness extension).
current () { # $1=config -> "text data bss"
  FOOTPRINT_CONFIG="$1" "$TUVM_DIR/tools/footprint.sh" | awk 'END { print $1, $2, $3 }'
}
banked () { # $1=baseline-file
  awk 'END { print $1, $2, $3 }' "$1"
}

check () { # $1=label $2=current-val $3=banked-val
  local max=$(( $3 + $3 * TOL_PCT / 100 ))
  if [ "$2" -gt "$max" ]; then
    echo "RATCHET FAIL: $1 = $2 exceeds banked $3 (+${TOL_PCT}%). Re-bank with rationale or shrink." >&2
    return 1
  fi
  echo "ratchet ok: $1 = $2 (banked $3)"
}

FAIL=0
read -r ct cd cb <<<"$(current host)"
read -r bt bd bb <<<"$(banked "$TUVM_DIR/tools/footprint-baseline.txt")"
check "host text" "$ct" "$bt" || FAIL=1

if [ -f "$TUVM_DIR/tools/footprint-baseline-mcu.txt" ]; then
  read -r mt md mb <<<"$(current mcu)"
  read -r nt nd nb <<<"$(banked "$TUVM_DIR/tools/footprint-baseline-mcu.txt")"
  check "mcu text"      "$mt" "$nt" || FAIL=1
  check "mcu data+bss"  "$((md + mb))" "$((nd + nb))" || FAIL=1
else
  echo "note: MCU baseline not banked yet (addendum A2) — RAM ratchet inactive" >&2
fi
exit $FAIL
