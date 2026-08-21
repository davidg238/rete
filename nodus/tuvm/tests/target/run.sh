#!/usr/bin/env bash
# nodus/tuvm/tests/target/run.sh — the target gates.
#
#   ./run.sh                     every scenario
#   ./run.sh stale-handle        one of them
#   ./run.sh --board c6bench …   override the disposable board (rarely wanted)
#   ./run.sh --no-deploy --build-id <id> stale-handle
#                                assert against an image already running
#
# A scenario's TAPs passing is not the verdict. Each manifest declares
# OBSERVE_S and OBSERVE_PROGRESS, and the runner keeps watching that long past
# the done line: a gate that passes and then lets the node die is a false green
# (rete#216, found by #213). The window's own logic is falsifiable WITHOUT a
# board — `python3 -m pytest tests/target/test_observe.py`, which the nsl suite
# now runs.
#
# WHY THIS IS NOT PART OF THE NSL SUITE. It needs hardware, it costs a flash
# cycle per scenario, and a boardless run would have to degrade to a SKIP — which
# on this bench is how a gate reports success for a run that never happened. The
# nsl suite instead prints a POSITIVE statement of what it cannot test and points
# here (D8).
#
# WHAT IT IS FOR. `timer_zephyr.c` posts through the pulse mailbox;
# `timer_posix.c` posts straight into the space via `space_post`. The host path
# has no mailbox, so on host a timer event can be neither coalesced nor shed nor
# staled — and all 243 nsl tests run there. Everything in this directory speaks
# to a seam the host suite is structurally silent about.
#
# Design: docs/design/2026-08-05-target-test-harness-design.md
set -euo pipefail
cd "$(dirname "$0")"
HERE=$PWD
ZDIR="$HERE/../../zephyr"

source "$HERE/lib/target.sh"
source "$HERE/lib/deploy_flash.sh"
source "$HERE/lib/collect_console.sh"

BOARD=$TARGET_DEFAULT_BOARD
DO_DEPLOY=1
OVERRIDE_TIMEOUT=""
GIVEN_BUILD_ID=""
WANTED=()

while [ $# -gt 0 ]; do
  case "$1" in
    --board)      BOARD=$2; shift 2 ;;
    --no-deploy)  DO_DEPLOY=0; shift ;;
    --build-id)   GIVEN_BUILD_ID=$2; shift 2 ;;
    --timeout)    OVERRIDE_TIMEOUT=$2; shift 2 ;;
    -h|--help)    sed -n '2,15p' "$0"; exit 0 ;;
    -*)           target::die "unknown flag $1" ;;
    *)            WANTED+=("$1"); shift ;;
  esac
done

# D7 — THE ANCHOR IS MANDATORY. Skipping the deploy is legal; asserting against
# "whatever happens to be running" is not. Without an anchor, result lines from an
# earlier run sit in the same log and score a green against firmware that was
# never written.
if [ "$DO_DEPLOY" = 0 ] && [ -z "$GIVEN_BUILD_ID" ]; then
  target::die "--no-deploy requires --build-id: an assertion with no anchor cannot say which image it tested"
fi

if [ ${#WANTED[@]} -eq 0 ]; then
  for f in "$HERE"/*.env; do WANTED+=("$(basename "$f" .env)"); done
fi

# The bell interlock runs FIRST — before the lock, before the collector check,
# before anything reads the board's state. Refusing to flash the bell must not
# depend on how the bell happens to be behaving today, and the refusal must be
# the FIRST thing said, or the operator acts on whatever cause was reported
# instead. Only gated on the deploy: pointing --no-deploy at the bell to read its
# log is legitimate and is how you would inspect it.
[ "$DO_DEPLOY" = 1 ] && target::assert_not_the_bell "$BOARD"

target::lock "$BOARD"
target::collector_ok "$BOARD"

echo "target gates — board $BOARD, ${#WANTED[@]} scenario(s)"
echo

ROWS=()
PASSED=0
for name in "${WANTED[@]}"; do
  manifest="$HERE/$name.env"
  [ -f "$manifest" ] || target::die "no manifest $manifest"

  # Each manifest is sourced into a subshell-free scope, so clear what it sets
  # rather than inheriting the previous scenario's values — a manifest that omits
  # CONF_EXTRA must not silently pick up the last one's.
  SCENARIO=""; NODE=""; BOARD_ID=""; CONF_EXTRA=""; CLASS=""; TIMEOUT_S=""
  OBSERVE_S=""; OBSERVE_PROGRESS=""
  unset OBSERVE_POLL_S   # rete#223: a per-manifest silence budget, so it must not leak
                         # into a scenario that declares none. tests/target/test_observe.py
                         # asserts this over EVERY key any manifest sets, because the
                         # hazard is remembering the second place, not the key itself.
  # shellcheck disable=SC1090
  unset TAP_NAME   # rete#215: never inherit the previous manifest's override
  source "$manifest"
  [ -n "$SCENARIO" ] || target::die "$manifest sets no SCENARIO"
  # rete#215: the name the WORKLOAD stamps on its TAP lines, which is not always the
  # scenario's own. One node can serve two scenarios that differ only in how the build is
  # configured — WedgeProbeC6 is wedge-note with the synthetic trigger layered on and
  # wedge-detect without it — and an .nsl file cannot see Kconfig, so it cannot name which
  # one it is running as. Defaulting to $SCENARIO leaves every existing manifest unchanged.
  TAP_NAME=${TAP_NAME:-$SCENARIO}
  [ -n "$NODE" ]     || target::die "$manifest sets no NODE"
  [ -n "$CLASS" ]    || target::die "$manifest sets no CLASS (observe or disruptive)"
  # rete#216. Both are MANDATORY, for the same reason CLASS is: the default that
  # would be applied silently is the one that produced the false green. A new
  # scenario must state how long it is watched past its verdict and what its
  # progress looks like — `none` being a legal, and visible, answer for a
  # workload that stops by design.
  [ -n "$OBSERVE_S" ] || target::die "$manifest sets no OBSERVE_S (rete#216: how long to keep watching AFTER the TAPs pass)"
  [ -n "$OBSERVE_PROGRESS" ] || target::die "$manifest sets no OBSERVE_PROGRESS (a progress regex, or 'none' if the workload halts by design)"
  [ -f "$ZDIR/$NODE" ] || target::die "workload $ZDIR/$NODE not found"

  target::assert_deployable "$BOARD" "$CLASS"

  echo "── $SCENARIO (class=$CLASS)"
  started=$SECONDS
  if [ "$DO_DEPLOY" = 1 ]; then
    target::say "deploying $NODE"
    build_id=$(deploy::flash "$ZDIR" "$BOARD")
  else
    build_id=$GIVEN_BUILD_ID
    target::say "no deploy; anchored on $build_id"
  fi
  target::say "build $build_id"

  if tap=$(collect::console "$build_id" "$TAP_NAME" "${OVERRIDE_TIMEOUT:-${TIMEOUT_S:-120}}" "$BOARD"); then
    verdict=$(echo "$tap" | target::tap_verify "$TAP_NAME") || true
  else
    verdict="FAIL 0/0 no TAP output (see the TIMEOUT line above)"
  fi

  # rete#216 — the verdict above is PROVISIONAL. `bridge-health` returned PASS
  # 3/3 in 22 s on 2026-08-16 against a build that was dead four ticks later
  # (#213): it scores at tick 2 and the node dies at tick 6. Only a PASS is
  # observed — a scenario that already failed has nothing left to protect, and
  # spending the window on it would just make a red run slower.
  case "$verdict" in
    PASS*)
      if ! collect::observe "$build_id" "$SCENARIO" "$OBSERVE_PROGRESS" "$OBSERVE_S" "$BOARD"; then
        verdict="FAIL ${verdict#PASS } — died inside the ${OBSERVE_S}s observation window (see OBSERVE line above)"
      fi
      ;;
  esac

  elapsed=$(( SECONDS - started ))
  target::say "$verdict"
  echo
  ROWS+=("$(printf '  %-16s %-4s %-8s %3ss  %s' \
      "$SCENARIO" "${verdict%% *}" "$(echo "$verdict" | cut -d' ' -f2)" \
      "$elapsed" "$build_id")")
  case "$verdict" in PASS*) PASSED=$((PASSED + 1)) ;; esac
done

echo "target gates — board $BOARD"
printf '%s\n' "${ROWS[@]}"
total=${#WANTED[@]}
if [ "$PASSED" = "$total" ]; then
  echo "TARGET $PASSED/$total PASS"
else
  echo "TARGET $PASSED/$total PASS — $((total - PASSED)) FAILED"
  exit 1
fi
