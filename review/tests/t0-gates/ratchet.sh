#!/usr/bin/env bash
# T0.3 — footprint ratchet: FAIL if the engine grows past the banked baseline + 2%,
# in either flash (text) or RAM (data + bss under the MCU-shaped config; addendum A2).
# Deliberate growth = re-bank the baseline IN THE SAME COMMIT, with rationale.
#
# Baselines (banked, committed):
#   nodus/oevm/tools/footprint-baseline.txt      — host build, `size` TOTALS row
#   nodus/oevm/tools/footprint-mcu-baseline.txt  — MCU-shaped build, `text N` / `data_bss M`
# Usage: ratchet.sh   (from anywhere; OEVM_DIR overridable)
# Exit: 0 = all gates ran and passed, 1 = a ratchet failed, 77 = the MCU half could
#       not run (no cross toolchain) — that is NOT a pass (tuvm#31).
set -eu

# Repo root, independent of cwd: this script lives at review/tests/t0-gates/,
# four levels below the root, regardless of where it is invoked from.
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && git rev-parse --show-toplevel)"
OEVM_DIR=${OEVM_DIR:-$REPO_ROOT/nodus/oevm}
TOL_PCT=2

# footprint.sh's two configs print DIFFERENT shapes, and conflating them is how the MCU
# half of this gate stayed dead (tuvm#110): `host` prints a `size` table whose last line
# is the TOTALS row, while `mcu` prints one `footprint(mcu): text=N data+bss=M` line and
# then ratchets internally at zero tolerance. Parse each on its own terms.
# The TOTALS row is addressed by name, not by position: host mode's LAST line has been
# `data+bss (host proxy for MCU static RAM): N` since the A2 addendum, so `END` picked up
# the string "data+bss" and every run printed "ratchet ok: host text = data+bss".
current_host () { # -> "text data bss"
  FOOTPRINT_CONFIG=host "$OEVM_DIR/tools/footprint.sh" | awk '/\(TOTALS\)/ { print $1, $2, $3 }'
}
# -> "text data_bss". Returns 77 when the cross toolchain is absent (footprint.sh's own
# SKIP code); footprint.sh's exit 1 for ITS ratchet is ignored here — this gate applies
# its own +TOL_PCT% tolerance to the same figures, so only the numbers are wanted.
current_mcu () {
  local out rc=0
  out=$(FOOTPRINT_CONFIG=mcu "$OEVM_DIR/tools/footprint.sh" 2>&1) || rc=$?
  [ "$rc" -eq 77 ] && return 77
  printf '%s\n' "$out" |
    sed -n 's/^footprint(mcu): text=\([0-9][0-9]*\) data+bss=\([0-9][0-9]*\).*$/\1 \2/p'
  return 0
}
# Same matcher as current_host, deliberately: re-banking by piping footprint.sh's host
# output into this file would otherwise leave the trailing data+bss line as END.
banked_host () { awk '/\(TOTALS\)/ { print $1, $2, $3 }' "$1"; }
banked_mcu () { # $1=baseline-file -> "text data_bss" (labelled lines, not a size table)
  # A missing field emits "-" rather than nothing: an empty field would collapse under
  # word-splitting and shift the SURVIVING number into the missing one's slot, so a
  # baseline with only `data_bss` would report "mcu text ... (banked <the data_bss>)".
  awk '/^text /{t=$2} /^data_bss /{d=$2}
       END { print (t == "" ? "-" : t), (d == "" ? "-" : d) }' "$1"
}

# Every value reaching an integer test is checked for being an integer first. A blank or
# non-numeric operand makes `[ x -gt y ]` exit 2 with a diagnostic, and an `if` condition
# swallows that under set -e — so the old code fell through and printed "ratchet ok" with
# an empty figure. An unmeasured gate must never read as a passing one.
check () { # $1=label $2=current-val $3=banked-val
  case "$2" in ''|*[!0-9]*)
    echo "RATCHET FAIL: $1 — current value is not a number ('$2'): the gate did not measure." >&2
    return 1 ;;
  esac
  case "$3" in ''|*[!0-9]*)
    echo "RATCHET FAIL: $1 — banked value is not a number ('$3'): baseline present but unreadable." >&2
    return 1 ;;
  esac
  local max=$(( $3 + $3 * TOL_PCT / 100 ))
  if [ "$2" -gt "$max" ]; then
    echo "RATCHET FAIL: $1 = $2 exceeds banked $3 (+${TOL_PCT}%). Re-bank with rationale or shrink." >&2
    return 1
  fi
  echo "ratchet ok: $1 = $2 (banked $3)"
}

FAIL=0
SKIPPED=0
read -r ct cd cb <<<"$(current_host)"
read -r bt bd bb <<<"$(banked_host "$OEVM_DIR/tools/footprint-baseline.txt")"
check "host text" "$ct" "$bt" || FAIL=1

MCU_BASELINE="$OEVM_DIR/tools/footprint-mcu-baseline.txt"
if [ ! -f "$MCU_BASELINE" ]; then
  echo "note: no MCU baseline at $MCU_BASELINE — RAM ratchet inactive." \
       "Bank one: FOOTPRINT_CONFIG=mcu FOOTPRINT_UPDATE_BASELINE=1 nodus/oevm/tools/footprint.sh" >&2
else
  MCU_RC=0
  MCU_OUT=$(current_mcu) || MCU_RC=$?
  if [ "$MCU_RC" -eq 77 ]; then
    echo "RATCHET SKIP: mcu — no arm-zephyr-eabi on PATH (source ns-zephyr/tools/env.sh)." \
         "The RAM ratchet did NOT run; this is not a pass (tuvm#31)." >&2
    SKIPPED=1
  else
    read -r mt mdb <<<"$MCU_OUT"
    read -r nt ndb <<<"$(banked_mcu "$MCU_BASELINE")"
    check "mcu text"      "$mt"  "$nt"  || FAIL=1
    check "mcu data+bss"  "$mdb" "$ndb" || FAIL=1
  fi
fi

[ "$FAIL" -ne 0 ] && exit 1
[ "$SKIPPED" -ne 0 ] && exit 77
exit 0
