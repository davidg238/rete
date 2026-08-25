# nodus/oevm/tests/target/lib/collect_console.sh
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
      # grep consumes the stream (no -q) here and at the WEDGE check below:
      # run.sh sources this under pipefail, and -q's first-match exit SIGPIPEs
      # the echo — here a false timeout, there a HIDDEN detected wedge (rete#260).
      if echo "$tail_from" | grep "^.*TAP $scenario done" >/dev/null; then
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

# collect::observe <build_id> <scenario> <progress_re> <window_s> [board]
#
# rete#216 — THE WINDOW AFTER THE VERDICT.
#
# `collect::console` above returns as soon as the workload's `done` line lands,
# and the runner then scores the TAPs and releases the board. On 2026-08-16
# `bridge-health` returned PASS 3/3 in 22 s against a build that was dead four
# ticks later (#213): the gate scores at tick 2, the node dies at tick 6, and
# nothing is watching. A target gate that passes and then lets the node die is a
# false green — and it is invisible in precisely the case a target gate exists
# to catch, a change that survives the host suite and kills real hardware.
#
# So the verdict is provisional until the node has been watched for a while
# longer. Two independent questions, because the deaths on this bench come in
# both shapes:
#
#   PROGRESS — did the scenario's own output keep arriving? #213 and #192 die
#     SILENTLY: no fault, no panic, no reboot banner, console still `port=up`.
#     No grep for a fault signature will ever match them. The failure IS the
#     absence, so only a positive progress predicate can see it.
#   FAULT — did a death that IS loud appear? Cheap, and it catches the
#     complementary set.
#
# WHY THIS IS NOT `TIMEOUT_S`. The manifests describe TIMEOUT_S as "the outer
# bound on a workload that has already decided, never the thing that decides".
# Lengthening it would make a slow attach and a post-verdict death the same
# event. This runs AFTER the decision and reports its own distinct cause.
#
# WHY IT READS ONLY WHAT ARRIVES AFTER THE VERDICT, not from the anchor. The
# anchor IS a boot banner, and a C6 prints two of them — so a fault scan rooted
# at the anchor would match `rst:0x` on every healthy run. Marking the log's end
# at window start makes a `rst:0x` inside the window mean what it says: the node
# rebooted after passing.
#
# `progress_re` is the SCENARIO's line, not a harness constant, and `none` is a
# legal value for a one-shot probe that emits its TAPs and stops. The fault half
# still applies to those.
collect::observe() {
  local build_id=$1 scenario=$2 progress_re=$3 window_s=$4 board=${5:-$TARGET_DEFAULT_BOARD}
  local poll=${OBSERVE_POLL_S:-10}
  local faults='FATAL|Out of Memory|Stack Overflow|Guru Meditation|CORRUPT|abort\(\)|rst:0x'
  local logfile mark now total new deadline

  logfile=$(target::logfile "$board")
  if [ ! -f "$logfile" ]; then
    echo "OBSERVE: no console log at $logfile — nothing can be observed" >&2
    return 1
  fi
  mark=$(wc -l < "$logfile")
  deadline=$(( $(date +%s) + window_s ))

  target::say "observing ${window_s}s past the verdict (progress: ${progress_re}, anchor $build_id)"

  while [ "$(date +%s)" -lt "$deadline" ]; do
    sleep "$poll"
    logfile=$(target::logfile "$board")          # re-resolved: the log rolls at midnight
    total=$(wc -l < "$logfile" 2>/dev/null || echo 0)
    # A roll (or a truncation) makes the old offset meaningless. Start again from
    # the top of the new file rather than reading a negative window as silence,
    # which would report a stall the node never had.
    if [ "$total" -lt "$mark" ]; then
      target::say "console log rolled mid-window; re-anchoring at its start"
      mark=0
      continue
    fi
    new=$(tail -n +$(( mark + 1 )) "$logfile")

    # rete#223 — THE DETECTOR'S OWN DUMP IS ASKED FIRST, because it is the most
    # specific thing that can be in this window and it changes what the reader does
    # next. A `oevm: WEDGE` line means the node stopped AND #215 caught it, named the
    # condition and rebooted: the evidence is three lines further down the same log.
    # Reported through the generic fault branch it came out as a bare `rst:0x`, and
    # reported through the stall branch it came out as "no fault, no panic and no
    # reboot" — which on 2026-08-19 was printed eighteen seconds before the dump that
    # refuted it (#223). Neither of those sends the reader to the dump.
    if echo "$new" | grep -E "(tuvm|oevm): WEDGE" >/dev/null; then
      echo "OBSERVE WEDGE: '$scenario' stopped inside the ${window_s}s window and the #215 detector caught it. This is a DETECTED wedge with a dump, not #213's unexplained silence:" >&2
      echo "$new" | grep -E "(tuvm|oevm): WEDGE" | head -2 >&2
      return 1
    fi

    if echo "$new" | grep -Eq "$faults"; then
      echo "OBSERVE FAULT: '$scenario' passed its TAPs and then faulted inside the ${window_s}s window:" >&2
      echo "$new" | grep -E "$faults" | head -3 >&2
      return 1
    fi

    if [ "$progress_re" != none ]; then
      if ! echo "$new" | grep -Eq "$progress_re"; then
        echo "OBSERVE STALL: '$scenario' passed its TAPs and then stopped — no line matching '$progress_re' in ${poll}s, and no fault signature SO FAR. If this node carries the #215 detector, its deadline is 8x the app's own shortest declared period (16 s for the bridge's 2 s cadence) and its dump may still be in flight — read past this line in the console log before treating it as #213's unexplained silence, and raise this scenario's OBSERVE_POLL_S above that deadline if you want the window to wait for it (rete#223)." >&2
        return 1
      fi
    fi

    mark=$total
  done

  # Say only what was actually asserted. Running this on hardware on 2026-08-17
  # printed "still progressing" for both probes that declare `none` — a positive
  # claim about liveness nothing had checked, in the harness built to stop
  # exactly that. The fault half is real for those scenarios; the progress half
  # is not, and a reader six months from now has only this line to go on.
  if [ "$progress_re" = none ]; then
    target::say "observed ${window_s}s past the verdict: no fault (this scenario declares no progress line, so nothing here asserts it stayed alive)"
  else
    target::say "observed ${window_s}s past the verdict: still progressing, no fault"
  fi
  return 0
}
