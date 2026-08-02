#!/usr/bin/env bash
# porta space-sync plan 3 — THE HARDWARE GATE. PortaBridgeNodeC6.nsl on a real
# ESP32-C6, over a real Thread mesh, against a real `porta serve` on the border
# router, including a deliberate datagram drop.
#
# Legs 1-3 are plan 2's host gate in the same shape, so a failure of legs 1-3
# here is a HARDWARE or MESH failure and not a protocol one — that separation is
# the whole reason the host gate ran first. Leg 4 is new and is what design §9
# actually asks for.
#
# Assertions live at porta, because porta is the independently written
# implementation: if it says obs/health is there, two codebases agree about one
# wire. The console supplies only what porta cannot see — the node's own misses —
# and leg 4 fails if that number never moves, because an injector that silently
# did nothing must not be scored as a survived drop.
set -euo pipefail
cd "$(dirname "$0")/.."

GW=${GW:-gw85224-01}
GWDIR=${GWDIR:-porta-v2}
NODE=${NODE_ID:-c6-bench}
DROP_EVERY=${DROP_EVERY:-3}
TICKS_PER_LEG=${TICKS_PER_LEG:-10}
CADENCE_MS=2000
CONSOLE=${CONSOLE:-/dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_ac2dce6f529ced118f43fd6be259fb3e-if00-port0}
LOG=${LOG:-/tmp/c6_console.log}
DO_FLASH=${DO_FLASH:-1}
# One of the v1 LAN nodes lost its gateway when v2 took :6969 and now polls this
# port with TFTP read requests once a second, forever. porta answers #err and
# moves on, so it costs nothing but log noise — but a human reading a failure
# dump should not have to wade through it. Filtered from dumps only; never from
# an assertion. Tracked as a porta issue.
NOISY_PEER=${NOISY_PEER:-192.168.0.241}
CONSOLE_PID=""

say()  { echo "  $*"; }
dump() {
  echo "--- last 20 console lines ---" >&2; tail -20 "$LOG" >&2 || true
  echo "--- last 20 gateway lines (excluding $NOISY_PEER) ---" >&2
  ssh "$GW" "grep -v '$NOISY_PEER' ~/$GWDIR/serve.log | tail -20" >&2 || true
}
fail() { echo "FAIL: $*" >&2; dump; exit 1; }
gwporta() { ssh "$GW" "cd ~/$GWDIR && ./porta $* --db space.db"; }
cleanup() {
  [ -n "$CONSOLE_PID" ] && kill "$CONSOLE_PID" 2>/dev/null || true
  ssh "$GW" 'pkill -f "porta serve --db space.db"' 2>/dev/null || true
}
trap cleanup EXIT

# ticks_from_log prints the distinct tick numbers porta has logged, ascending.
# grep -o rather than sed 's/.*tick //': the record's formatting puts punctuation
# after the number, which sort tolerates and arithmetic does not. Every caller
# guards with || true — under `set -euo pipefail` a zero-match grep inside a
# command substitution aborts the script before its own diagnostic can print.
ticks_from_log() {
  gwporta "log $NODE --limit 500" | grep -o 'tick [0-9]\+' | awk '{print $2}' | sort -n -u || true
}

# assert_exactly_once enforces BOTH halves of the claim: no duplicates (rows ==
# distinct) and no gaps (the distinct ticks run contiguously). A radio can lose
# a datagram where loopback cannot, so the second half is not decoration here.
assert_exactly_once() {
  local leg="$1" rows uniq first last ticks
  rows=$(gwporta "log $NODE --limit 500" | grep -c 'tel/log' || true)
  ticks=$(ticks_from_log)
  uniq=$(printf '%s\n' "$ticks" | grep -c . || true)
  say "$leg: tel/log rows=$rows distinct ticks=$uniq"
  [ "$uniq" -ge 2 ] || fail "$leg: fewer than two tel/log entries — the stream never rode"
  [ "$rows" = "$uniq" ] || fail "$leg: a tel/log entry was delivered more than once"
  first=$(printf '%s\n' "$ticks" | head -1)
  last=$(printf '%s\n' "$ticks" | tail -1)
  [ $((last - first + 1)) -eq "$uniq" ] || \
    fail "$leg: a gap in the tick series ($first..$last holds only $uniq) — an entry was lost"
  say "$leg: exactly once, no gaps ($first..$last)"
}

# console_ticks / console_misses read the log the ONE reader writes. Nothing here
# opens the port: a second reader splits the stream and both halves look plausible.
console_ticks()  { grep -c '^tick ' "$LOG" || true; }
console_misses() { grep '^tick ' "$LOG" | tail -1 | grep -o 'misses [0-9]\+' | awk '{print $2}' || true; }

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

# `ssh host 'nohup cmd &'` HANGS: the child inherits the ssh channel's stdio and
# the session will not close while it is held. setsid + </dev/null is what
# actually detaches. Learned the hard way during bring-up.
gw_serve_start() {  # gw_serve_start [extra flags...]
  ssh "$GW" "cd ~/$GWDIR && (setsid nohup ./porta serve --db space.db --udp :6969 --http-port 0 $* >> serve.log 2>&1 < /dev/null &)"
  sleep 2
  ssh "$GW" "grep -q 'serving the space' ~/$GWDIR/serve.log" || fail "the gateway did not start"
}
gw_serve_stop() { ssh "$GW" 'pkill -f "porta serve --db space.db"' 2>/dev/null || true; sleep 1; }

echo "[1/8] preflight"
[ -e "$CONSOLE" ] || fail "no console at $CONSOLE (ls /dev/serial/by-id/)"
ssh -o BatchMode=yes "$GW" true || fail "cannot ssh to $GW without a password"
ssh "$GW" "test -x ~/$GWDIR/porta" || fail "no executable v2 binary at ~/$GWDIR/porta on $GW (Task 5; scp does not preserve the exec bit)"
V1=$(ssh "$GW" 'systemctl is-active porta.service' 2>/dev/null || true)
[ "$V1" = "active" ] && fail "v1 porta.service is still running and owns :6969 (Task 5 Step 2)"
say "console, gateway binary and a free :6969 all present"

echo "[2/8] start the console reader — BEFORE any flash, and held across it"
: > "$LOG"
tests/porta_c6_console.py --port "$CONSOLE" --log "$LOG" --follow >/dev/null 2>&1 &
CONSOLE_PID=$!
sleep 1
kill -0 "$CONSOLE_PID" 2>/dev/null || fail "the console reader died immediately (port busy?)"

echo "[3/8] build and flash the node"
if [ "$DO_FLASH" = 1 ]; then
  # run.sh does not source the west env (zephyr/docs/BOARD_PORTS.md), and a
  # missing env fails silently as `west: command not found`.
  ( set +u
    # shellcheck disable=SC1090
    source ~/zephyr-esp32/.venv/bin/activate
    export ZEPHYR_BASE=~/zephyr-esp32/zephyr \
           ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.17.4 \
           ZEPHYR_TOOLCHAIN_VARIANT=zephyr
    cd zephyr && ./run.sh --board esp32c6 --node PortaBridgeNodeC6.nsl --flash \
      --conf-extra "prj_esp32c6_thread.conf;prj_esp32c6_thread_ftd.conf" ) \
    || fail "the build/flash failed"
else
  say "DO_FLASH=0 — using whatever is already on the board"
fi

# The db is wiped and the gateway started AFTER the flash, not before. Flashing
# resets the board, so from here no previously-running firmware can check in —
# and a pre-flash node WILL repopulate a freshly wiped db if given the chance,
# which is how the first version of this gate scored a stale obs/health from a
# 45-minute-old session as if it were this run's. The node needs ~20-40s to
# attach after the reset, so the gateway is up long before its first check-in
# and there is no unacked window to spend the pending queue on either.
echo "[4/8] wipe the db and start the gateway"
gw_serve_stop
ssh "$GW" "cd ~/$GWDIR && rm -f space.db"
gw_serve_start

echo "[5/8] wait for the node: attach, then READY"
for _ in $(seq 1 120); do grep -q 'PORTA NODE READY' "$LOG" && break; sleep 1; done
grep -q 'PORTA NODE READY' "$LOG" || \
  fail "the node never printed PORTA NODE READY — no role line means the mesh never attached"
say "node is up and attached"

echo "[6/8] legs 1 and 2"
# Leg 2 first: it needs no operator write at all — an idle check-in IS the
# heartbeat, so obs/build arriving is also the liveness proof.
#
# Wait on obs/build, NOT obs/health. obs/build is written once at boot, so its
# arrival proves THIS boot pushed. obs/health is rewritten every tick, so a stale
# copy from an earlier session would satisfy the wait and let the gate race ahead
# of the node it is supposed to be testing — which is exactly what happened the
# first time this ran.
for _ in $(seq 1 90); do gwporta "get $NODE obs" 2>/dev/null | grep -q 'obs/build' && break; sleep 1; done
gwporta "get $NODE obs" | grep -q 'obs/build'  || fail "leg 2: obs/build never reached porta — this boot pushed nothing"
gwporta "get $NODE obs" | grep -q 'esp32c6'    || fail "leg 2: obs/build does not say esp32c6"
for _ in $(seq 1 30); do gwporta "get $NODE obs" 2>/dev/null | grep -q 'obs/health' && break; sleep 1; done
gwporta "get $NODE obs" | grep -q 'obs/health' || fail "leg 2: obs/health never reached porta"
say "leg 2 PASS: obs/health and obs/build are in porta's cell tree, from real silicon"

gwporta "put $NODE goal/name c6" >/dev/null
for _ in $(seq 1 60); do gwporta "get $NODE obs/name" 2>/dev/null | grep -q 'c6' && break; sleep 1; done
gwporta "get $NODE obs/name" | grep -q 'c6' || fail "leg 1: goal/name never converged to obs/name"
say "leg 1 PASS: goal/name converged to obs/name — the diff IS the view"

echo "[7/8] leg 3: exactly once, no injected drop"
wait_ticks "$TICKS_PER_LEG" "leg 3"
assert_exactly_once "leg 3"
say "leg 3 PASS"

echo "[8/8] leg 4: exactly once ACROSS a deliberate drop"
# Restart onto the flag keeping the SAME db, and leave the node running. A fresh
# db would reset porta's high-water to 0 while the node's acked kept climbing,
# breaking contiguity for reasons that have nothing to do with the drop. Reusing
# it also means leg 4 incidentally witnesses that the high-water survives a
# gateway restart; the node sees the restart window as ordinary loss and
# re-pushes, which is the designed behaviour.
MISSES_BEFORE=$(console_misses)
gw_serve_stop
gw_serve_start --test-drop-ack "$DROP_EVERY"
ssh "$GW" "grep -q 'TEST-DROP ACTIVE' ~/$GWDIR/serve.log" || fail "leg 4: the injector did not arm"
wait_ticks "$TICKS_PER_LEG" "leg 4"

DROPS=$(ssh "$GW" "grep -c 'TEST-DROP: ack' ~/$GWDIR/serve.log" || true)
MISSES_AFTER=$(console_misses)
say "leg 4: gateway swallowed $DROPS acks; node misses $MISSES_BEFORE -> $MISSES_AFTER"
[ "${DROPS:-0}" -ge 2 ] || fail "leg 4: the gateway swallowed ${DROPS:-0} acks — the drop never fired"
[ "${MISSES_AFTER:-0}" -gt "${MISSES_BEFORE:-0}" ] || \
  fail "leg 4: the node's misses never moved — it did not notice the drop, so nothing was tested"
assert_exactly_once "leg 4"
say "leg 4 PASS: the node re-pushed rows porta had already applied, and porta deduped them"

echo "PORTA SYNC C6 OK"
