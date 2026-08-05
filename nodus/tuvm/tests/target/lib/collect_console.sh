# nodus/tuvm/tests/target/lib/collect_console.sh
#
# The BENCH implementation of D2's collect seam: read the workload's TAP lines
# out of the #56 console collector's log.
#
#   collect::console <build_id> <scenario> <timeout_s>  -> the TAP lines on stdout
#
# The field implementation (collect::porta — the tel/test/<scenario> stream read
# from a baseline cursor) is not built here. Same signature; its transport-shaped
# configuration is read from the environment, so the runner never learns which
# one it is on.
#
# TWO THINGS THIS DOES DELIBERATELY.
#
# It NEVER OPENS THE PORT. On the C6 the collector holds the CP2102N permanently,
# and a second reader splits the stream rather than doubling it — both halves then
# look plausible and neither is complete.
#
# It ANCHORS ON THE BUILD ID and refuses to read anything before it. Today's log
# already holds result lines from earlier runs; without the anchor a gate scores a
# green against firmware it never wrote. That is failure mode 1 of nine on this
# bench's own false-green list, and the one this class of gate would otherwise hit
# on every single run.
#
# The anchor is the BOOT BANNER, not the plan line, because the node cannot name
# its own build: obs/build carries #chip alone, which is #74 and still open. When
# #74 closes, the workload can carry the id itself and the field path gets the
# same anchor for free. Until then the ordering of the log is the anchor, and it
# is sound on the bench because the collector writes one stream in order.

collect::console() {
  local build_id=$1 scenario=$2 timeout_s=$3 board=${4:-$TARGET_DEFAULT_BOARD}
  local deadline logfile tail_from

  deadline=$(( $(date +%s) + timeout_s ))
  while [ "$(date +%s)" -lt "$deadline" ]; do
    logfile=$(target::logfile "$board")     # re-resolved: the log rolls at midnight
    if [ -f "$logfile" ]; then
      tail_from=$(awk -v id="$build_id" 'index($0, id) { found = 1 } found' "$logfile")
      if echo "$tail_from" | grep -q "^.*TAP $scenario done"; then
        echo "$tail_from" | grep "TAP $scenario "
        return 0
      fi
    fi
    sleep 2
  done

  # Say WHICH of the two silences this was. "Nothing arrived" and "the image
  # never booted" have different causes and different next steps, and a single
  # timeout message that cannot tell them apart sends you to the wrong one.
  logfile=$(target::logfile "$board")
  if [ -f "$logfile" ] && grep -q "$build_id" "$logfile"; then
    echo "TIMEOUT: the image booted (build id is in the log) but '$scenario' never reached its done line within ${timeout_s}s" >&2
  else
    echo "TIMEOUT: build id $build_id never appeared in $logfile within ${timeout_s}s — the image did not boot, or its console is not being read" >&2
  fi
  return 1
}
