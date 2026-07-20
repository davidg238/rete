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
#                               [--conf-extra FILE.conf]
#
# --conf-extra forwards straight through to run.sh's own --conf-extra (Task 6),
# which turns it into -DEXTRA_CONF_FILE=FILE.conf. Used to bisect a radio-layered
# config (e.g. the C6's prj_esp32c6_thread.conf OpenThread layer) on top of the
# board's base CONF_FILE, without needing a separate script.
set -euo pipefail

BOARD_ID=esp32 NODE=TimerNode.nsl LO=16384 HI=524288 CONF_EXTRA=""
while [ $# -gt 0 ]; do
  case "$1" in
    --board)      BOARD_ID="$2";  shift 2 ;;
    --node)       NODE="$2";      shift 2 ;;
    --lo)         LO="$2";        shift 2 ;;
    --hi)         HI="$2";        shift 2 ;;
    --conf-extra) CONF_EXTRA="$2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GRAIN=4096          # bisect to the nearest 4 KB; finer is noise
LAST_FAIL_MSG=""
FAIL_KIND=""        # "capacity" (region overflow) or "error" (anything else)

# --- grain-alignment validation ---------------------------------------------
# MID=$(( (LO+HI)/2/GRAIN*GRAIN )) with the `[ "$MID" -le "$LO" ] && break`
# convergence guard is only provably correct when LO and HI both start as
# exact multiples of GRAIN. An un-aligned bound can make MID round down to LO
# on an early iteration, breaking the loop before the search has actually
# converged to a GRAIN-resolution answer, and silently reporting a coarser
# (and possibly wrong) LO as "the" ceiling.
#
# Choice: REJECT un-aligned input rather than silently rounding it. This is a
# decision-critical measurement tool — silently snapping a caller's --lo/--hi
# to a different value they didn't ask for is exactly the kind of quiet
# behavior-change that produces a confident wrong number. Forcing the caller
# to pass aligned bounds costs nothing (the defaults already are) and keeps
# the reported ceiling traceable to the range that was actually asked for.
if [ $((LO % GRAIN)) -ne 0 ] || [ $((HI % GRAIN)) -ne 0 ]; then
  echo "ERROR: --lo ($LO) and --hi ($HI) must both be exact multiples of GRAIN ($GRAIN)." >&2
  echo "        Un-aligned bounds can trip the bisect's convergence guard early and" >&2
  echo "        misreport a ceiling. Round them yourself and retry." >&2
  exit 2
fi

# --- per-run log directory ---------------------------------------------------
# Failure logs used to be unconditionally `rm -f`'d in try(), including on the
# path where the overflow-regex found nothing and LAST_FAIL_MSG pointed AT
# that same (now-deleted) file. Logs now live under a stable run directory and
# are only removed on the SUCCESS path; failing probes leave their log behind
# for inspection.
RUN_DIR="$(mktemp -d "${TMPDIR:-/tmp}/heap-ceiling.XXXXXX")"
echo "[bisect] probe logs: $RUN_DIR" >&2

# build-dir tag: a --conf-extra run gets its own build-dir suffix so it never
# shares (and can never be confused with, in logs or on disk) the no-radio
# bisect's "build-heapprobe-$BOARD_ID" directory for the same board. --pristine
# (-p always, below) would force a clean reconfigure either way, so reuse would
# not have been silently wrong — but a distinct name is one less thing to reason
# about when comparing two cells for the same board side by side.
CONF_TAG=""
[ -n "$CONF_EXTRA" ] && CONF_TAG="-$(basename "$CONF_EXTRA" .conf)"

try() {   # $1 = candidate heap size; 0 = links, 1 = does not (see FAIL_KIND)
  local size="$1" log
  log="$RUN_DIR/probe-$size.log"
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
  #
  # --pristine also satisfies the newly-added-overlay trap (Task 6 decision 1):
  # CMake caches devicetree overlay discovery, so a --conf-extra run's overlay
  # would be silently dropped on an incremental build. -p always on every probe
  # means that can never happen here, by construction.
  local CONF_EXTRA_ARG=()
  [ -n "$CONF_EXTRA" ] && CONF_EXTRA_ARG=(--conf-extra "$CONF_EXTRA")
  if OEVM_HEAP_SIZE="$size" \
       bash "$ROOT/zephyr/run.sh" --board "$BOARD_ID" --model B --node "$NODE" \
       --pristine --build-dir "build-heapprobe-$BOARD_ID$CONF_TAG" \
       "${CONF_EXTRA_ARG[@]}" \
       >"$log" 2>&1; then
    rm -f "$log"; return 0
  fi
  # Anchored to the linker's actual region-overflow message ("region `NAME'
  # overflowed by N bytes"), not just the bare word "overflow" or "will not
  # fit" anywhere in the log. The old, looser alternation could match
  # unrelated log noise (anything merely containing "overflowed" or "will
  # not fit") and misclassify a non-capacity build error as a capacity
  # result — the false-positive direction that narrows the bisect and
  # produces a confident wrong ceiling, exactly what this tool exists to
  # prevent. See probe()'s FAIL_KIND handling below.
  LAST_FAIL_MSG="$(grep -E "region .* overflowed by [0-9]+ bytes" "$log" | head -2 || true)"
  if [ -n "$LAST_FAIL_MSG" ]; then
    FAIL_KIND="capacity"
    echo "    fail @ $size (capacity): $LAST_FAIL_MSG" >&2
  else
    FAIL_KIND="error"
    LAST_FAIL_MSG="(no overflow diagnostic — see $log)"
    echo "    fail @ $size (NON-CAPACITY build error): $LAST_FAIL_MSG" >&2
  fi
  return 1
}

# probe() wraps try() and enforces that only a recognized capacity failure
# (region overflow / "will not fit") is allowed to narrow the bisect. Any
# other build failure — toolchain crash, OOM, missing prerequisite, transient
# I/O — is a build ERROR, not evidence about capacity, and must abort the
# whole run loudly rather than be folded into the search (which would
# silently produce a too-low ceiling). This applies uniformly to the floor
# probe, the HI-verification probe, and every interior bisect probe — not
# just the first one.
probe() {
  local size="$1"
  if try "$size"; then
    return 0
  fi
  if [ "$FAIL_KIND" = "error" ]; then
    echo >&2
    echo "ABORT: probe at $size failed WITHOUT a recognizable region-overflow" >&2
    echo "diagnostic. This looks like a build error unrelated to heap capacity" >&2
    echo "(toolchain crash, OOM, missing prerequisite, transient I/O, ...), not" >&2
    echo "evidence that $size is 'too big'. Treating it as a capacity result would" >&2
    echo "silently produce a too-low, wrong ceiling. See $RUN_DIR/probe-$size.log" >&2
    exit 1
  fi
  return 1
}

echo "[bisect] board=$BOARD_ID node=$NODE range=[$LO,$HI] grain=$GRAIN conf-extra=${CONF_EXTRA:-none}"

probe "$LO" || { echo "FLOOR $LO does not even link — widen downward" >&2; exit 1; }

# HI was, until now, trusted as known-bad but never actually verified. If a
# caller's --hi links, the bisect converges toward it and reports it as "the"
# ceiling with nothing indicating the bound was never falsified. Probe HI up
# front: it must NOT link, or the search range itself is wrong.
echo "  probe HI=$HI (must NOT link)"
if probe "$HI"; then
  echo "CEILING $HI links — the search range is not wide enough for this board/node." >&2
  echo "Widen --hi upward and retry; do not trust $HI as a ceiling." >&2
  exit 1
fi

while [ $((HI - LO)) -gt $GRAIN ]; do
  MID=$(( (LO + HI) / 2 / GRAIN * GRAIN ))
  [ "$MID" -le "$LO" ] && break
  echo "  probe $MID"
  if probe "$MID"; then LO="$MID"; else HI="$MID"; fi
done

echo
echo "RESULT board=$BOARD_ID node=$NODE conf-extra=${CONF_EXTRA:-none} max-linkable-OEVM_HEAP_SIZE=$LO"
# LAST_FAIL_MSG is never cleared on a subsequent success — it is whatever the
# MOST RECENT failing probe produced, not necessarily the probe at LO+GRAIN.
# In practice HI only ever moves together with a fresh LAST_FAIL_MSG (every
# failing try() sets both in the same call, and successes touch neither), so
# by construction this is the diagnostic from the probe that set the final
# HI — i.e. LO+GRAIN. Labeled for what the variable actually tracks rather
# than implying a stronger ("the very first failure seen") guarantee.
echo "most-recent-failing-diagnostic (ceiling+grain, $((LO + GRAIN))): $LAST_FAIL_MSG"
