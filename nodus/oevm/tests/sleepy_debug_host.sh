#!/usr/bin/env bash
# #342 spike (peer design D9): debug a sleepy node through porta, no cable. A host
# SleepyDebugNode (one check-in per 10 s cadence) against a real `porta serve`, over
# UDP loopback. Three legs:
#   A. a node that sleeps between check-ins with its VM alive (a Thread sleepy end
#      device, or any long-cadence node): a breakpoint written as dbg/req lines waits
#      at porta for the next check-in, the node stops in `work:`, reports the stop on
#      dbg/resp, and is stepped and inspected through porta while held.
#   B. a node whose sleep is a cold boot (deep sleep): the breakpoint lives in the
#      goal/debug/main CELL, which porta replays to a fresh boot (a dbg/req row is not
#      replayed, #262), so the node re-arms it on every wake. Killing the process is
#      the simulated sleep. Runs WITHOUT --manifest: the spike's goal shape (a List of
#      lines) is not the manifest's Debug{action} record.
#   D. profile in the same debug image: a `profile <ms>` row goes to the kernel's
#      Profiler, not the debugger, so the window opens at the next check-in, closes on
#      its own timer while the node sleeps, and never holds the node.
#   C. the control: the same image and the same rows without OEVM_DEBUG. No debugger,
#      so nothing may come back on dbg/resp — a stop reported here would mean leg A's
#      green came from somewhere other than the debugger.
# Not in the suite: standalone, like porta_sync_host.sh (same ports, same lock).
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "${RETE_SUITE_LOCK:-}" != held ]; then
  exec 9>>/tmp/rete-nsl-suite.lock
  if ! flock -n 9; then
    echo "sleepy_debug_host.sh: refused — another run holds /tmp/rete-nsl-suite.lock ($(head -1 /tmp/rete-nsl-suite.lock))" >&2
    exit 1
  fi
  : > /tmp/rete-nsl-suite.lock
  echo "pid $$: sleepy_debug_host.sh" >> /tmp/rete-nsl-suite.lock
  export RETE_SUITE_LOCK=held
fi
command -v go >/dev/null 2>&1 || { echo "SKIP: go is not on PATH"; exit 0; }

ROOT="$(git rev-parse --show-toplevel)"
W="$(mktemp -d /tmp/sleepy_debug.XXXXXX)"
NODE="sleepy-$$"
PORTA_PID=""; NODE_PID=""
trap 'kill ${PORTA_PID:-} ${NODE_PID:-} 2>/dev/null || true' EXIT

echo "[1/6] build porta, the host VM and the node image"
( cd "$ROOT/porta" && go build -o "$W/porta" ./cmd/porta )
gcc -std=c99 -Isrc tools/dump_layout.c -o "$W/dl" && "$W/dl" > "$W/layout.json"
python3 "$ROOT/nodus/nsl/tools/ns_compile_oevm.py" zephyr/SleepyDebugNode.nsl "$W/node.oimg" \
  --format=oimg --layout="$W/layout.json" >/dev/null
T=posix
# -DOEVM_METHOD_PROFILE: without it profileStop is a silent no-op and leg D's window
# would never print (the trap porta_sync_host.sh's leg 8 documents).
gcc -std=c99 -DOEVM_METHOD_PROFILE -Isrc \
    src/vm.c src/gc.c src/dispatch.c src/platform.c src/space.c src/handles.c src/span.c \
    src/backend/$T/platform_$T.c src/serial.c src/backend/$T/serial_$T.c src/udp.c src/backend/$T/udp_$T.c \
    src/http.c src/http_core.c src/backend/$T/http_$T.c src/ota.c src/backend/$T/ota_$T.c \
    src/backend/$T/pulse_$T.c src/backend/$T/timer_$T.c src/backend/$T/span_$T.c \
    src/backend/$T/sensor_$T.c src/mirror.c src/oimg_loader.c src/kernel_boot.c oevm_image_main.c \
    -o "$W/oevm" -lm 2>/dev/null
# The breakpoint is named the way custos names it: the method's blob offset from the
# image map, pc 0. Read from the map, never typed, so a recompile cannot strand it.
OFF=$(python3 -c "import json,sys; m=json.load(open(sys.argv[1]))['methods']; print([k for k,v in m.items() if v['name']=='SleepyDebugNode>>work:'][0])" "$W/node.oimg.map.json")
BRK="dbg:break $OFF 0"; HIT="dbg:paused break $OFF 0"
echo "  work: is blob offset $OFF"

serve() {  # $1 = db, $2.. = extra flags
  local db=$1; shift
  "$W/porta" serve --db "$db" --udp 127.0.0.1:16969 --http-port 0 --sweep-tick 1s "$@" >"$db.log" 2>&1 &
  PORTA_PID=$!
  for _ in $(seq 1 50); do grep -q "serving the space" "$db.log" && return; sleep 0.1; done
  echo "FAIL: porta did not start"; cat "$db.log"; exit 1
}
stop_porta() { kill "$PORTA_PID" 2>/dev/null || true; wait "$PORTA_PID" 2>/dev/null || true; PORTA_PID=""; }
start_node() {  # $1 = log, $2 = "debug" or "std"
  if [ "$2" = debug ]; then
    OEVM_DEBUG=1 OEVM_TIMER_WALLCLOCK=1 OEVM_BOOTID_FILE="$W/bootid" OEVM_NODE_ID="$NODE" \
      "$W/oevm" "$W/node.oimg" </dev/null >>"$1" 2>&1 &
  else
    OEVM_TIMER_WALLCLOCK=1 OEVM_BOOTID_FILE="$W/bootid" OEVM_NODE_ID="$NODE" \
      "$W/oevm" "$W/node.oimg" </dev/null >>"$1" 2>&1 &
  fi
  NODE_PID=$!
  for _ in $(seq 1 50); do grep -q "SLEEPY NODE READY" "$1" && return; sleep 0.1; done
  echo "FAIL: node did not start"; cat "$1"; exit 1
}
stop_node() { kill "$NODE_PID" 2>/dev/null || true; wait "$NODE_PID" 2>/dev/null || true; NODE_PID=""; }
resp_count() {  # $1 = db, $2 = fixed string in a dbg/resp row
  plog "$1" "$NODE" --limit 100000 | grep 'dbg/resp' | grep -cF -- "$2" || true
}
await_resp() {  # $1 = db, $2 = string, $3 = count to exceed, $4 = timeout s; prints ms waited
  local t0 now; t0=$(date +%s%N)
  while [ "$(resp_count "$1" "$2")" -le "$3" ]; do
    now=$(date +%s%N)
    [ $(( (now - t0) / 1000000000 )) -ge "$4" ] && { echo "TIMEOUT"; return; }
    sleep 0.01
  done
  now=$(date +%s%N); echo $(( (now - t0) / 1000000 ))
}
plog() { "$W/porta" log --db "$@" 2>/dev/null; }
req() { "$W/porta" put --db "$1" "$NODE" dbg/req "$2" --kind stream >/dev/null; }

echo "[2/6] leg A: a breakpoint waits at porta for a sleeping node's next check-in"
DB="$W/a.db"; echo 40 > "$W/bootid"
serve "$DB" --manifest "$ROOT/protocol/MANIFEST.cddl"
start_node "$W/a.log" debug
grep -q "debug over space: true" "$W/a.log" || { echo "FAIL: debugOverSpace answered false in a debug image"; exit 1; }
req "$DB" "$BRK"; req "$DB" "dbg:continue"
MS=$(await_resp "$DB" "$HIT" 0 25)
[ "$MS" = TIMEOUT ] && { echo "FAIL: no '$HIT' on dbg/resp within 25 s"; tail -20 "$W/a.log"; exit 1; }
echo "  stopped in work: and reported it through porta, $MS ms after the breakpoint was written (cadence 10000 ms)"
for c in "dbg:backtrace|dbg:frames $OFF@" "dbg:inspect|dbg:stack off=" "dbg:step|dbg:paused step $OFF"; do
  cmd=${c%%|*}; want=${c#*|}
  n=$(resp_count "$DB" "$want"); req "$DB" "$cmd"
  MS=$(await_resp "$DB" "$want" "$n" 10)
  [ "$MS" = TIMEOUT ] && { echo "FAIL: '$cmd' got no '$want' back"; exit 1; }
  echo "  $cmd -> '$want…' in $MS ms (porta put -> node -> porta store)"
done
CPU=$(ps -o pcpu= -p "$NODE_PID" | tr -d ' ')
LIVE=$("$W/porta" get --db "$DB" "$NODE" gw/liveness 2>/dev/null | grep -o '#[a-zA-Z]*$' | tail -1 || true)
echo "  while held: node CPU ${CPU}%, porta judges it ${LIVE:-?}"
req "$DB" "dbg:clear $OFF 0"; req "$DB" "dbg:continue"
n=$(plog "$DB" "$NODE" --limit 100000 | grep -c 'tel/log' || true)
for _ in $(seq 1 250); do
  [ "$(plog "$DB" "$NODE" --limit 100000 | grep -c 'tel/log' || true)" -gt "$n" ] && break; sleep 0.1
done
LAST=$(plog "$DB" "$NODE" --limit 100000 | grep 'tel/log' | head -1)
echo "$LAST" | grep -q "held [1-9]" || { echo "FAIL: no tick after continue, or it counts no held exchange: $LAST"; exit 1; }
HELD=$(echo "$LAST" | grep -o 'held [0-9]*' | awk '{print $2}')
echo "  resumed; $HELD held exchanges were run while stopped"
stop_node; stop_porta

echo "[3/6] leg B: the breakpoint is a goal cell, re-armed on every cold boot"
DB="$W/b.db"; echo 50 > "$W/bootid"
serve "$DB"
HEX=$(python3 -c "import sys; s=sys.argv[1].encode(); print((b'\x81'+(bytes([0x60+len(s)]) if len(s)<24 else b'\x78'+bytes([len(s)]))+s).hex())" "$BRK")
"$W/porta" put --db "$DB" --raw "$NODE" goal/debug/main "$HEX" >/dev/null
for wake in 1 2; do
  n=$(resp_count "$DB" "$HIT")
  start_node "$W/b.log" debug
  MS=$(await_resp "$DB" "$HIT" "$n" 25)
  [ "$MS" = TIMEOUT ] && { echo "FAIL: wake $wake did not stop in work:"; tail -20 "$W/b.log"; exit 1; }
  echo "  wake $wake: stopped in work: $MS ms after boot"
  stop_node   # the simulated deep sleep: the VM, its breakpoint table and its cursor die
done
# Wake 2 is the proof: its VM was new, leg B wrote no dbg/req row (and porta replays
# none to a fresh boot), so only the replayed goal cell can have armed the breakpoint.
echo "  wake 2 re-armed from the goal cell alone: the breakpoint survived the sleep"
stop_porta

echo "[4/6] leg C (control): the same rows to a standard image get no answer"
DB="$W/c.db"; echo 60 > "$W/bootid"
serve "$DB" --manifest "$ROOT/protocol/MANIFEST.cddl"
start_node "$W/c.log" std
grep -q "debug over space: false" "$W/c.log" || { echo "FAIL: debugOverSpace answered true without OEVM_DEBUG"; exit 1; }
req "$DB" "$BRK"; req "$DB" "dbg:continue"
for _ in $(seq 1 250); do
  plog "$DB" "$NODE" --limit 1000 | grep -q 'tel/log' && break; sleep 0.1
done
plog "$DB" "$NODE" --limit 1000 | grep -q 'tel/log' || { echo "FAIL: the control node never ticked"; exit 1; }
sleep 1
[ "$(resp_count "$DB" "dbg:")" = 0 ] || { echo "FAIL: a standard image answered on dbg/resp"; exit 1; }
echo "  ticked, and dbg/resp stayed empty"
stop_node; stop_porta

echo "[5/6] leg D: a profile request to the debug image opens a window and holds nothing"
DB="$W/d.db"; echo 70 > "$W/bootid"
serve "$DB" --manifest "$ROOT/protocol/MANIFEST.cddl"
start_node "$W/d.log" debug
req "$DB" "profile 2000"
for _ in $(seq 1 300); do grep -q '^mprof\[window\]: end' "$W/d.log" && break; sleep 0.1; done
grep -q '^mprof\[window\]: end' "$W/d.log" || { echo "FAIL: no profile window closed within 30 s"; tail -20 "$W/d.log"; exit 1; }
[ "$(resp_count "$DB" "dbg:")" = 0 ] || { echo "FAIL: the profile request reached the debugger (dbg/resp rows)"; exit 1; }
ROWS=$(grep -c '^mprof: ' "$W/d.log" || true)
echo "  window closed with $ROWS rows on the console, and nothing on dbg/resp: the node was never held"
stop_node; stop_porta

echo "  logs in $W"
echo "[6/6] SLEEPY DEBUG OK"
