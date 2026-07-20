#!/usr/bin/env bash
# tools/heap-ceiling.sh — bisect the largest OEVM_HEAP_SIZE that LINKS.
#
# gc.c:25 declares `static oevm_align_cell oevm_heap_store[OEVM_HEAP_CELLS]`,
# sized from the OEVM_HEAP_SIZE macro at compile time. Too large -> the linker
# reports a region overflow. So the ceiling is a pure build-time property and
# this script needs NO hardware.
#
# A value that links is not automatically a value that boots. This script
# reports the LINK ceiling; boot confirmation is a separate, on-device step.
#
# Usage: tools/heap-ceiling.sh --board esp32|esp32c6 [--node X.nsl] [--lo N] [--hi N]
set -euo pipefail

BOARD_ID=esp32 NODE=TimerNode.nsl LO=16384 HI=524288
while [ $# -gt 0 ]; do
  case "$1" in
    --board) BOARD_ID="$2"; shift 2 ;;
    --node)  NODE="$2";     shift 2 ;;
    --lo)    LO="$2";       shift 2 ;;
    --hi)    HI="$2";       shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GRAIN=4096          # bisect to the nearest 4 KB; finer is noise
LAST_FAIL_MSG=""

try() {   # $1 = candidate heap size; 0 = links, 1 = does not
  local size="$1" log
  log="$(mktemp)"
  # --build-dir, NOT an exported BUILD_DIR: run.sh's defaults line sets
  # BUILD_DIR="" unconditionally before its arg loop, so an exported value is
  # silently clobbered.
  #
  # --pristine on EVERY probe is load-bearing. OEVM_HEAP_SIZE reaches the
  # compiler through target_compile_definitions(... $ENV{OEVM_HEAP_SIZE}),
  # evaluated at CMake CONFIGURE time. An incremental build reuses the cached
  # definition, so probes 2..N would all silently measure probe 1's heap size
  # and the bisect would return a plausible-looking wrong number. Slow but
  # correct: ~7 full rebuilds for a 16 KB..512 KB range at 4 KB grain.
  if OEVM_HEAP_SIZE="$size" \
       bash "$ROOT/zephyr/run.sh" --board "$BOARD_ID" --model B --node "$NODE" \
       --pristine --build-dir "build-heapprobe-$BOARD_ID" \
       >"$log" 2>&1; then
    rm -f "$log"; return 0
  fi
  LAST_FAIL_MSG="$(grep -E "overflowed|region .* overflow|will not fit" "$log" | head -2 || true)"
  [ -n "$LAST_FAIL_MSG" ] || LAST_FAIL_MSG="(no overflow diagnostic — see $log)"
  echo "    fail @ $size: $LAST_FAIL_MSG" >&2
  rm -f "$log"; return 1
}

echo "[bisect] board=$BOARD_ID node=$NODE range=[$LO,$HI] grain=$GRAIN"
try "$LO" || { echo "FLOOR $LO does not even link — widen downward" >&2; exit 1; }

while [ $((HI - LO)) -gt $GRAIN ]; do
  MID=$(( (LO + HI) / 2 / GRAIN * GRAIN ))
  [ "$MID" -le "$LO" ] && break
  echo "  probe $MID"
  if try "$MID"; then LO="$MID"; else HI="$MID"; fi
done

echo
echo "RESULT board=$BOARD_ID node=$NODE max-linkable-OEVM_HEAP_SIZE=$LO"
echo "first-failing-diagnostic: $LAST_FAIL_MSG"
