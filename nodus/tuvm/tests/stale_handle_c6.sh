#!/usr/bin/env bash
# #151 — THE HARDWARE GATE. A timer event must survive a later, unrelated
# registration, on a real ESP32-C6.
#
# THIS GATE CANNOT BE MOVED TO THE HOST, and that is the point rather than an
# inconvenience. The two backends deliver timer events by different paths:
#
#   target   backend/zephyr/timer_zephyr.c    pulse_post_isr_value(...)  -> pulse mailbox
#   host     backend/posix/timer_posix.c:122  space_post("sys/timer", …) -> straight in
#
# The host never enters the mailbox, so on host a timer event can be neither
# coalesced nor shed nor staled, and the whole nsl suite runs there. A green suite
# is SILENT about this seam. See review/tests/CLAIMS.md, "A claim about
# ISR-context event delivery is UNPROVEN by the host suite".
#
# What the probe does: both arms arm a one-shot and burn past its expiry inside a
# single reaction. They differ by ONE line — the test arm registers another timer
# afterwards. Before the fix that registration ran reap_oneshots(), which freed the
# M6 handle the queued post named, so pulse_pump resolved NULL and DISCARDED the
# event (tuvm#13's ABA guard, correctly preferring a loss to a mis-delivery):
#
#   before   control (no registration after): delivered 1
#            test    (registration after)   : delivered 0
#   after    both 1
#
# HOW IT READS THE CONSOLE, and why not by opening the port. On the C6 the console
# collector (#56) holds the CP2102N permanently, and two readers split the stream
# rather than doubling it — so this gate reads the collector's log instead of
# opening the port itself. It anchors on the BUILD ID that run.sh archives for this
# very flash and refuses to read anything before it: without that anchor the two
# result lines from a PREVIOUS run sit in the same file and score a green against
# firmware that was never built. That is failure mode 1 of nine in the bench's own
# false-green list, and it is the one this gate would otherwise hit every time.
set -euo pipefail
cd "$(dirname "$0")/.."          # nodus/tuvm

ZDIR="zephyr"
NODE=${NODE:-StaleHandleProbeC6.nsl}
BOARD=${BOARD:-esp32c6}
DO_FLASH=${DO_FLASH:-1}
STORE=${STORE:-$HOME/.local/state/rete/console}
# c6bench, NOT c6, and the default is the point. This bench has TWO ESP32-C6
# devkits and they are otherwise identical — only the CP2102N by-id hash tells
# them apart, and mistaking one for the other cost most of a morning on
# 2026-08-05. Instance `c6` is the SHIP'S BELL, which lives on a wallwart and
# whose whole value is soaking uninterrupted for days; flashing a probe over it
# to run a test would destroy the thing the test exists to protect. Instance
# `c6bench` is the spare. Override only if you know which board is which.
CONSOLE_BOARD=${CONSOLE_BOARD:-c6bench}
TIMEOUT_S=${TIMEOUT_S:-120}

say()  { echo "  $*"; }
fail() {
  echo "FAIL: $*" >&2
  if [ -n "${LOGFILE:-}" ] && [ -f "$LOGFILE" ]; then
    echo "--- last 25 console lines ---" >&2
    grep -v heartbeat "$LOGFILE" | tail -25 >&2 || true
  fi
  exit 1
}

logfile_for_today() { echo "$STORE/console-$CONSOLE_BOARD-$(date +%F).log"; }

echo "#151 stale-handle gate — $NODE on $BOARD"

# ---- preflight -------------------------------------------------------------
# Every check below FAILS rather than SKIPS. A skip whose message names an
# unverified cause is how a gate reports success for a run that never happened.
[ -f "$ZDIR/$NODE" ] || fail "probe node $ZDIR/$NODE not found"

if [ "$DO_FLASH" = "1" ]; then
  ls /dev/serial/by-id/ >/dev/null 2>&1 || fail "no /dev/serial/by-id — is a C6 attached?"
  command -v west >/dev/null 2>&1 || fail "west not on PATH — source ~/zephyr-esp32/.venv/bin/activate and ~/zephyr-esp32/zephyr/zephyr-env.sh first (run.sh does NOT source them)"
fi

systemctl --user is-active --quiet rete-console@$CONSOLE_BOARD \
  || fail "console collector rete-console@$CONSOLE_BOARD is not running; this gate reads its log"

# The collector reporting port=down means it is alive and seeing nothing — which
# would make every assertion below a timeout with a misleading cause. Say so now.
LOGFILE=$(logfile_for_today)
[ -f "$LOGFILE" ] || fail "no console log at $LOGFILE"
if tail -3 "$LOGFILE" | grep -q "port=down"; then
  fail "collector reports port=down — the board's console is not being read (wrong by-id hash, or the board is unplugged)"
fi

# ---- flash -----------------------------------------------------------------
BUILD_ID=""
if [ "$DO_FLASH" = "1" ]; then
  say "building and flashing $NODE"
  OUT=$(bash "$ZDIR/run.sh" --board "$BOARD" --node "$NODE" --flash 2>&1) \
    || { echo "$OUT" | tail -20 >&2; fail "run.sh failed"; }
  # run.sh prints: [archive] <build-id> -> <path>
  BUILD_ID=$(echo "$OUT" | sed -n 's/^\[archive\] \([^ ]*\) ->.*/\1/p' | tail -1)
  [ -n "$BUILD_ID" ] || fail "could not read the build id from run.sh output — the anchor below depends on it"
  say "build id $BUILD_ID"
else
  say "DO_FLASH=0 — asserting against whatever is already running"
fi

# ---- wait for OUR image, then for its result -------------------------------
# The log rolls at midnight, so re-resolve it each poll rather than caching a
# path a long run could outlive.
say "waiting up to ${TIMEOUT_S}s for the probe to report"
deadline=$(( $(date +%s) + TIMEOUT_S ))
RESULT=""
while [ "$(date +%s)" -lt "$deadline" ]; do
  LOGFILE=$(logfile_for_today)
  if [ -f "$LOGFILE" ]; then
    if [ -n "$BUILD_ID" ]; then
      # Everything from the boot that carries OUR build id onwards. Nothing
      # before it can testify about the image we just wrote.
      TAIL=$(awk -v id="$BUILD_ID" 'index($0, id) {found=1} found' "$LOGFILE" || true)
    else
      TAIL=$(cat "$LOGFILE")
    fi
    if echo "$TAIL" | grep -q "stale-handle probe done"; then
      RESULT="$TAIL"
      break
    fi
  fi
  sleep 2
done

[ -n "$RESULT" ] || fail "probe did not report within ${TIMEOUT_S}s (build id ${BUILD_ID:-<none>} never reached 'stale-handle probe done')"

# ---- assert ----------------------------------------------------------------
# POSITIVE assertions on both arms. Asserting only that the test arm is nonzero
# would pass a build where the probe never armed anything at all.
control=$(echo "$RESULT" | sed -n 's/.*control (no registration after): delivered \([0-9]*\).*/\1/p' | tail -1)
test_arm=$(echo "$RESULT" | sed -n 's/.*test    (registration after)   : delivered \([0-9]*\).*/\1/p' | tail -1)

[ -n "$control" ]  || fail "no control-arm line in the probe output"
[ -n "$test_arm" ] || fail "no test-arm line in the probe output"

say "control $control / test $test_arm"

[ "$control" = "1" ] \
  || fail "control arm delivered $control, expected 1 — the probe itself is broken, so the test arm proves nothing"
[ "$test_arm" = "1" ] \
  || fail "test arm delivered $test_arm, expected 1 — a registration destroyed an already-queued timer event (#151 regressed)"

echo "STALE HANDLE OK"
