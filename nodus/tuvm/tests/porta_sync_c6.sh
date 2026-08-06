#!/usr/bin/env bash
# porta_sync_c6.sh — THE BENCH INTEGRATION TEST OF PORTA. Not a device gate.
#
# What it tests: that porta, on the border router, over a real Thread mesh,
# takes a node's tel/log stream exactly once and contiguously ACROSS A
# DELIBERATE ACK DROP, and that its `seq <= hw` dedupe is WITNESSED firing.
# Those are facts about the far end of the wire, and the node cannot testify to
# any of them — a node that dropped a row is precisely the node that cannot
# report it.
#
# WHAT LEFT, AND WHY (target harness design D1, 2026-08-05). The node-side
# properties this script used to score — obs/health published and acked,
# goal/name converged to obs/name, #streamDropped 0 — are now the
# `bridge-health` scenario, asserted by the workload itself, on the device, with
# no privilege at all:
#
#     nodus/tuvm/tests/target/run.sh bridge-health
#
# A property a node can testify to should not be checked by a script holding an
# ssh key. What remains here is what genuinely needs the key.
#
# THE PRIVILEGE IS THE WHOLE REASON THIS IS NOT A SCENARIO. It restarts the
# gateway with an injector flag, and the node's gateway port is a source
# constant (#118), so it TAKES :6969 for the duration: every node checking in —
# including the ship's bell — sees a ~10 s outage and re-pushes. That is designed
# behaviour and it is also why this is run deliberately, by hand, and never from
# the target suite. It restores the gateway to the exact command line it found.
#
# PRECONDITIONS, all asserted rather than assumed:
#   - a bridge node is already running on the bench C6 (deploy it with the
#     bridge-health scenario above; this script does NOT flash);
#   - the #56 console collector for that board is up. This script never opens
#     the serial port (design D6) — it reads the collector's log, because two
#     readers split the stream and both halves look plausible;
#   - a `porta serve` is already running on the gateway. It is not started from
#     scratch here: there is ONE porta, it is shared, and this test borrows it.
#
# NO DB WIPE (design D4). The old version wiped the store so its assertions
# could demand a series starting at 1. Every assertion here takes a DELTA from a
# baseline sampled before the leg, which is the same assertion at the same
# strength and the only shape that survives contact with a field node. It also
# stopped the wipe from hiding things: the wipe was concealing that a rebooted
# node's cells were being discarded as already-applied, found 2026-08-05 by the
# bridge-health gate and fixed in porta (Store.SyncHighWater).
set -euo pipefail
# Resolved BEFORE the cd, or the source below resolves against the new cwd.
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE/.."

GW=${GW:-gw85224-01}
GWDIR=${GWDIR:-porta-v2}
NODE=${NODE_ID:-c6-bench}
BOARD=${BOARD:-c6bench}
DROP_EVERY=${DROP_EVERY:-3}
TICKS_PER_LEG=${TICKS_PER_LEG:-10}
CADENCE_MS=2000
# One of the v1 LAN nodes lost its gateway when v2 took :6969 and now polls this
# port with TFTP read requests once a second, forever. porta answers #err and
# moves on, so it costs nothing but log noise — but a human reading a failure
# dump should not have to wade through it. Filtered from dumps only; never from
# an assertion.
NOISY_PEER=${NOISY_PEER:-192.168.0.241}

# The collector's log path and its preflight come from the target harness's
# shared layer, so there is ONE place on this bench that knows where a board's
# console lands. A second copy here would eventually disagree with it, and every
# symptom of that reads as "the board went quiet".
# shellcheck source=target/lib/target.sh
source "$HERE/target/lib/target.sh"

say()  { echo "  $*"; }
LOG=""   # resolved per read: the collector rolls its log at midnight
dump() {
  echo "--- last 20 console lines ($BOARD) ---" >&2; tail -20 "$(target::logfile "$BOARD")" >&2 || true
  echo "--- last 20 gateway lines (excluding $NOISY_PEER) ---" >&2
  ssh "$GW" "grep -v '$NOISY_PEER' ~/$GWDIR/serve.log | tail -20" >&2 || true
}
fail() { echo "FAIL: $*" >&2; dump; exit 1; }
gwporta() { ssh "$GW" "cd ~/$GWDIR && ./porta $* --db $GWDB"; }

# ---- the gateway, borrowed and given back --------------------------------
# GW_CMDLINE is the command line found running at preflight, and it is what gets
# restored — including its db, which is NOT assumed to be any particular file.
# The gateway on this bench serves the soak store, and a test that restarted it
# onto a different db would quietly take the fleet's history away.
GW_CMDLINE=""
GWDB=""

# `pkill -f "porta serve --db X"` MATCHES ITS OWN SSH COMMAND LINE and kills the
# shell running it — the remote command dies before its next statement, ssh exits
# 255, and the gateway is left DOWN. It cost two outages on 2026-08-05, and the
# old version of this script carried the same bug with `|| true` swallowing it
# (porta died either way, so it "worked").
#
# TWO rules, and the second is the one that bit twice. The bracket makes the
# pattern text differ from the string it matches, so the ERE cannot match the
# command carrying it — but that only holds if the literal appears NOWHERE ELSE
# in the same command line. `pkill … ; … ./porta serve --db X` self-matches
# through the restart half. So the kill gets its own ssh invocation, always.
gw_serve_stop() { ssh "$GW" 'pkill -f "porta serv[e]"' 2>/dev/null || true; sleep 1; }

# `ssh host 'nohup cmd &'` HANGS: the child inherits the ssh channel's stdio and
# the session will not close while it is held. setsid + </dev/null is what
# actually detaches. Learned the hard way during bring-up.
gw_serve_start() {  # gw_serve_start [extra flags...]
  ssh "$GW" "cd ~/$GWDIR && (setsid nohup $GW_CMDLINE $* >> serve.log 2>&1 < /dev/null &)" \
    || fail "could not reach $GW to start the gateway"
  sleep 3
  ssh "$GW" "pgrep -f 'porta serv[e]' > /dev/null" || fail "the gateway did not come back up"
}

# Restored on EVERY exit path, including a failed assertion: leaving the bench's
# one gateway running with an ack injector armed would silently corrupt every
# later measurement on it, and nothing downstream would say why.
restore_gateway() {
  [ -n "$GW_CMDLINE" ] || return 0
  ssh "$GW" 'pkill -f "porta serv[e]"' 2>/dev/null || true
  sleep 1
  ssh "$GW" "cd ~/$GWDIR && (setsid nohup $GW_CMDLINE >> serve.log 2>&1 < /dev/null &)" || true
  sleep 2
  ssh "$GW" "pgrep -f 'porta serv[e]' > /dev/null" \
    && say "gateway restored: $GW_CMDLINE" \
    || echo "  ! THE GATEWAY IS DOWN and could not be restarted: $GW_CMDLINE" >&2
}
trap restore_gateway EXIT

# gw_drops / gw_skips answer the gateway's CUMULATIVE counts; every leg takes a
# DELTA across them, because serve.log is appended across runs and a running
# total would let this run pass on a previous run's drops even if nothing fired
# here — precisely the false pass leg 2 exists to prevent.
#
# `grep -c` PRINTS the count and EXITS 1 when the count is zero, so the guard has
# to live on the REMOTE side: `ssh ... || echo 0` locally would emit grep's "0"
# AND the fallback "0", and the arithmetic below would die on "0\n0".
gw_count() {  # gw_count <pattern>
  local n
  n=$(ssh "$GW" "grep -c '$1' ~/$GWDIR/serve.log 2>/dev/null || true" | head -1 || true)
  printf '%s\n' "${n:-0}"
}
gw_drops() { gw_count 'TEST-DROP: ack'; }
gw_skips() { gw_count 'skipped .* already-applied'; }

# ---- the console, read from the ONE reader --------------------------------
# The collector stamps every line with an ISO timestamp, so the old `^tick `
# anchor no longer matches and its absence would read as a silent node. Matched
# on the field itself instead.
#
# `|| true` on every one of these: under `set -o pipefail` a zero-match grep
# fails the whole pipeline, which fails the command substitution, which under
# `set -e` kills the script BEFORE the guard that was written to catch the empty
# value can run. A missing number must reach its own diagnostic.
console_field() {  # console_field <name>  -> the last value of `<name> N`
  grep -o "$1 [0-9]\+" "$(target::logfile "$BOARD")" 2>/dev/null | tail -1 | awk '{print $2}' || true
}
console_ticks()   { grep -c ' tick [0-9]' "$(target::logfile "$BOARD")" || true; }
console_misses()  { console_field misses; }
console_dropped() { console_field dropped; }

wait_ticks() {  # wait_ticks <n-more> <what>
  local want budget
  want=$(( $(console_ticks) + $1 ))
  budget=$(( $1 * CADENCE_MS / 1000 + 30 ))
  say "waiting for $1 more ticks ($2), budget ${budget}s"
  for _ in $(seq 1 "$budget"); do
    [ "$(console_ticks)" -ge "$want" ] && return 0
    sleep 1
  done
  fail "$2: the node stopped ticking (have $(console_ticks), want $want)"
}

# ---- exactly-once, FROM A BASELINE ----------------------------------------
# porta_log_ticks answers this node's tel/log tick numbers, NEWEST FIRST, one
# per line. Newest-first is what makes the baseline work without timestamps:
# events are append-only, so the rows added during a leg are exactly the first
# (after - before) of them.
#
# Tick numbers RESTART AT 1 on every node boot, and so do seq values, so no
# filter on the VALUE of either can separate this boot's rows from an earlier
# one's — a previous session that reached tick 1189 would swamp any `> baseline`
# comparison. Row order is the only thing that survives a reboot, which is why
# the baseline is a COUNT.
porta_log_ticks() {
  gwporta "log $NODE --limit 5000" 2>/dev/null | grep 'tel/log' | grep -o 'tick [0-9]\+' | awk '{print $2}' || true
}

# take_baseline sets ROWS_BEFORE and LAST_TICK from ONE snapshot, and exists as a
# function so it cannot be taken any other way. Two fetches let a tel/log push
# land between them: the row is missing from the count but present in the newest
# tick, so the leg then reports "first tick is N, not N+1 — an entry was lost"
# about an entry that was never lost. Cost 2026-08-05: one red leg, and the
# script it replaced already carried this fix and its reasoning.
take_baseline() {
  local snapshot
  snapshot=$(porta_log_ticks)
  ROWS_BEFORE=$(printf '%s\n' "$snapshot" | grep -c . || true)
  LAST_TICK=$(printf '%s\n' "$snapshot" | head -1)
}

# assert_exactly_once_since <leg> <rows-at-baseline> <last-tick-at-baseline>
#
# THREE things, all of which the old wipe-and-start-at-1 version checked, and
# none of which needs a wipe: no duplicates (rows == distinct), no gaps (the
# distinct ticks run contiguously), and — the one the wipe used to buy —
# NOTHING LOST AT THE JOIN: the first new tick is baseline+1. `porta log
# --limit` truncates from the OLDEST end and the bridge's maxPending eviction is
# oldest-first, so contiguity alone never pins the start of the run.
assert_exactly_once_since() {
  local leg="$1" before="$2" last="$3" all now new rows uniq lo hi
  all=$(porta_log_ticks)
  now=$(printf '%s\n' "$all" | grep -c . || true)
  rows=$(( now - before ))
  [ "$rows" -ge 2 ] || fail "$leg: only $rows tel/log row(s) reached porta during this leg — the stream never rode"
  new=$(printf '%s\n' "$all" | head -n "$rows" | sort -n)
  uniq=$(printf '%s\n' "$new" | sort -n -u | grep -c . || true)
  lo=$(printf '%s\n' "$new" | head -1)
  hi=$(printf '%s\n' "$new" | tail -1)
  say "$leg: $rows new tel/log rows, ticks $lo..$hi, $uniq distinct"
  [ "$rows" = "$uniq" ] || fail "$leg: a tel/log entry was delivered more than once ($rows rows, $uniq distinct)"
  [ $((hi - lo + 1)) -eq "$uniq" ] || \
    fail "$leg: a gap in the tick series ($lo..$hi holds only $uniq) — an entry was lost"
  [ "$lo" -eq $((last + 1)) ] || \
    fail "$leg: the leg's first tick is $lo, not $((last + 1)) — an entry was lost at the join with the baseline"
  say "$leg: exactly once from the baseline, no gaps ($lo..$hi)"
}

# The node's OWN eviction count, read from the console rather than from porta:
# a row the bridge evicted before it could ever be pushed is invisible at the
# gateway by construction. The bridge-health workload asserts this for itself at
# boot; it is re-checked here because the injected outage is exactly the load
# that would make it move, and that happens long after the workload's verdict.
assert_no_stream_drops() {
  local leg="$1" d
  d=$(console_dropped)
  [ "${d:-x}" = 0 ] || fail "$leg: the node reports dropped=${d:-<no tick line>} — the bridge evicted pending rows during this leg"
}

echo "[1/5] preflight"
ssh -o BatchMode=yes "$GW" true || fail "cannot ssh to $GW without a password"
ssh "$GW" "test -x ~/$GWDIR/porta" || fail "no executable porta at ~/$GWDIR/porta on $GW (scp does not preserve the exec bit)"
V1=$(ssh "$GW" 'systemctl is-active porta.service' 2>/dev/null || true)
[ "$V1" = "active" ] && fail "v1 porta.service is running and owns :6969"
GW_CMDLINE=$(ssh "$GW" 'for p in $(pgrep -f "porta serv[e]"); do tr "\0" " " < /proc/$p/cmdline; echo; done' | head -1 | sed 's/ *$//')
[ -n "$GW_CMDLINE" ] || fail "no porta is running on $GW. This test borrows the shared gateway rather than starting one, because there is only one and it holds the fleet's history"
GWDB=$(printf '%s\n' "$GW_CMDLINE" | sed -n 's/.*--db \([^ ]*\).*/\1/p')
[ -n "$GWDB" ] || fail "the running gateway names no --db: $GW_CMDLINE"
say "gateway: $GW_CMDLINE"

# Every check FAILS rather than SKIPS, each naming a VERIFIED cause.
target::collector_ok "$BOARD"
[ "$(console_ticks)" -ge 1 ] || \
  fail "no tick lines on $BOARD's console — no bridge node is running. Deploy one: nodus/tuvm/tests/target/run.sh bridge-health"
say "collector up, node ticking (tick $(console_field tick))"

echo "[2/5] baseline"
# Sampled BEFORE anything is touched. Every assertion below is a delta against
# these four numbers; not one of them requires an empty store.
take_baseline
[ -n "$LAST_TICK" ] || fail "porta holds no tel/log row for $NODE — has the node ever checked in?"
DROPS_BEFORE=$(gw_drops)
SKIPS_BEFORE=$(gw_skips)
say "porta holds $ROWS_BEFORE tel/log rows, newest tick $LAST_TICK; gateway drops=$DROPS_BEFORE skips=$SKIPS_BEFORE"

echo "[3/5] leg 1: exactly once, no injected drop"
wait_ticks "$TICKS_PER_LEG" "leg 1"
assert_exactly_once_since "leg 1" "$ROWS_BEFORE" "$LAST_TICK"
assert_no_stream_drops "leg 1"
say "leg 1 PASS"

echo "[4/5] arm the injector"
# The SAME db, deliberately. A fresh one would reset porta's high-water while
# the node's `acked` kept climbing, breaking contiguity for reasons that have
# nothing to do with the drop. Reusing it also means leg 2 incidentally
# witnesses that the high-water survives a gateway restart: the node sees the
# restart window as ordinary loss and re-pushes, which is designed behaviour.
take_baseline
gw_serve_stop
gw_serve_start --test-drop-ack "$DROP_EVERY"
ssh "$GW" "grep -q 'TEST-DROP ACTIVE' ~/$GWDIR/serve.log" || fail "leg 2: the injector did not arm"
# Sampled AFTER the restart, not before: the restart is a 4-6 s outage against a
# 2 s cadence, so it scores a miss on its own regardless of what the injector
# does — proven by a falsification run where misses moved 0->1 with zero acks
# swallowed. Sampling here keeps the restart's own miss out of the delta, so a
# move below reflects the injector and nothing else.
MISSES_BEFORE=$(console_misses)

echo "[5/5] leg 2: exactly once ACROSS a deliberate drop"
wait_ticks "$TICKS_PER_LEG" "leg 2"
DROPS=$(( $(gw_drops) - DROPS_BEFORE ))
SKIPS=$(( $(gw_skips) - SKIPS_BEFORE ))
MISSES_AFTER=$(console_misses)
say "leg 2: gateway swallowed $DROPS acks this run; node misses $MISSES_BEFORE -> $MISSES_AFTER"
[ "${DROPS:-0}" -ge 2 ] || fail "leg 2: the gateway swallowed ${DROPS:-0} acks this run — the drop never fired"
[ "${MISSES_AFTER:-0}" -gt "${MISSES_BEFORE:-0}" ] || \
  fail "leg 2: the node's misses never moved — it did not notice the drop, so nothing was tested"
say "leg 2: gateway logged $SKIPS duplicate-skip line(s) this run"
[ "${SKIPS:-0}" -ge 1 ] || \
  fail "leg 2: the gateway never logged a duplicate-skip — the node re-pushed rows but the seq<=hw dedupe was never observed firing"
assert_exactly_once_since "leg 2" "$ROWS_BEFORE" "$LAST_TICK"
assert_no_stream_drops "leg 2"
say "leg 2 PASS: the node re-pushed rows porta had already applied, and porta deduped them (witnessed: $SKIPS skip line(s) logged)"

echo "PORTA SYNC C6 OK"
