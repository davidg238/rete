#!/usr/bin/env bash
# rete#25 host fleet-sim gate (the G10 seed): three runner-wrapped VMs and one
# real `porta serve`, over real UDP loopback, driven through a signed image push
# and its three interesting failures.
#
# What it witnesses, one `ok` per scenario:
#   1. CLEAN DEPLOY. Three nodes on v1 are pushed to v2 the way an agent does it
#      (rete#398): one POST /board/node-N/load each to a custos serving them as
#      porta nodes, which stores, signs and writes the goal through porta's HTTP.
#      Each runner must print staged-ok -> trial -> confirmed and re-exec on the
#      new hash, porta's own tree must show obs/apps/main carrying it, and the
#      agent's /load/last must read Converged — the node saying it, the gateway
#      seeing it and the agent being told it are three claims, not one.
#      Scenarios 2-4 push with porta's CLI, so both fronts stay covered.
#   2. KILL MID-TRANSFER. The VM is SIGKILLed with a partial image in the staging
#      slot. The runner re-execs, the fetch RESUMES from the slot's own length
#      (the slot never shrinks below where the kill left it), and the node still
#      converges. This is the only test of the resume arithmetic that ships.
#   3. BAD SIGNATURE. A record whose 64-byte sig has one flipped byte — legal
#      CBOR, which porta therefore accepts — must be refused by the runner
#      OUTSIDE the image (D4), and the node must keep running the image it had.
#   4. NO CHECK-IN. A valid, correctly signed image that never contacts porta
#      boots fine and confirms nothing. Only the trial watchdog can end it, and
#      the node must land back on its previous image.
#
# TAP-lite with a DECLARED plan: `plan 4` is printed up front, so a run that
# dies half-way is a FAIL and never a short pass. `HOST OTA OK` is printed only
# after all four.
#
# Not wired into run_tests_oevm.py: it builds two Go binaries, three VM images per
# round and runs for minutes. Standalone, like porta_sync_host.sh — and it uses
# the 26969/26970 port pair precisely so it can run while that gate's 16969/16970
# artifacts linger.
set -euo pipefail
cd "$(dirname "$0")/../.."          # nodus/oevm

REPO="$(git rev-parse --show-toplevel)"
PORTA_DIR="${PORTA_DIR:-$REPO/porta}"
NSL="$REPO/nodus/nsl/tools/ns_compile_oevm.py"
TARGET="${OEVM_TARGET:-posix}"

WORK=/tmp/host_ota_gate
DB="$WORK/porta.db"
PORTA="$WORK/porta"
CUSTOS="$WORK/custos"
VM="$WORK/oevm_ota"
EXCH_PORT=26969                     # blocks ride EXCH_PORT+1, chosen by porta
PORTA_HTTP=26968                    # porta's operator HTTP, for custos (rete#398)
CUSTOS_PORT=26967                   # the agent's surface
WATCHDOG=10                         # seconds a trial boot has to confirm

# rete#241: per-RUN node ids, not per-run slots. The nodes answer `platform
# nodeId`; a host VM has no silicon, so this gate hands each VM its id through OEVM_NODE_ID (repeatable
# --env below, applied on every re-exec), so a run beside another run — or
# beside a live host node — cannot write into the same three porta slots the
# way `node-1/2/3` always did. Human-facing messages keep saying node-1/2/3:
# that names the ROLE in this run, which is still true.
NID="ota$$"

echo "plan 4"

# --------------------------------------------------------------------------
# process bookkeeping
# --------------------------------------------------------------------------
PORTA_PID=""
CUSTOS_PID=""
RUNNER_PIDS=()

cleanup() {
  if [ ${#RUNNER_PIDS[@]} -gt 0 ]; then
    kill "${RUNNER_PIDS[@]}" 2>/dev/null || true
    sleep 0.4
    kill -9 "${RUNNER_PIDS[@]}" 2>/dev/null || true
  fi
  # The runners spawn the VMs in their own process groups, so a runner that is
  # already gone leaves no one to reap them. Match on the slot path, which only
  # ever appears in a VM's argv.
  pkill -9 -f "$WORK/node[0-9]/slots/" 2>/dev/null || true
  [ -n "$CUSTOS_PID" ] && kill -9 "$CUSTOS_PID" 2>/dev/null || true
  [ -n "$PORTA_PID" ] && kill -9 "$PORTA_PID" 2>/dev/null || true
  return 0
}
trap cleanup EXIT

STARTED=0     # 1 once this run owns $WORK; before that any logs there are a
              # PREVIOUS run's, and dumping them would answer a preflight
              # failure with evidence from a different afternoon.
dump_logs() {
  [ "$STARTED" = 1 ] || return 0
  for n in 1 2 3; do
    if [ -f "$WORK/runner$n.log" ]; then
      echo "----- runner$n.log (tail 40) -----"
      tail -40 "$WORK/runner$n.log"
    fi
  done
  if [ -f "$WORK/porta.log" ]; then
    echo "----- porta.log (tail 30) -----"
    tail -30 "$WORK/porta.log"
  fi
  if [ -f "$WORK/custos.log" ]; then
    echo "----- custos.log (tail 20) -----"
    tail -20 "$WORK/custos.log"
  fi
  for n in 1 2 3; do
    [ -f "$WORK/node$n/manifest.json" ] && {
      echo "----- node$n manifest -----"; cat "$WORK/node$n/manifest.json"; }
  done
}

fail() {
  echo "FAIL: $*"
  dump_logs
  exit 1
}

# --------------------------------------------------------------------------
# small helpers
# --------------------------------------------------------------------------
# wait_line <file> <start-line> <ere> [tries] -> echoes the absolute line number
# of the first match at or after <start-line>. Everything in this gate is
# ordered ("staged-ok THEN trial THEN confirmed"), and a bare grep cannot say
# that — nor tell this rollout's lines from the previous one's.
wait_line() {
  local f=$1 start=$2 pat=$3 tries=${4:-200} i rel
  for ((i = 0; i < tries; i++)); do
    if [ -f "$f" ]; then
      rel=$(tail -n +"$start" "$f" 2>/dev/null | grep -nE "$pat" | head -1 | cut -d: -f1 || true)
      [ -n "$rel" ] && { echo $((start + rel - 1)); return 0; }
    fi
    sleep 0.2
  done
  return 1
}

# Same match, one shot, no waiting — for "this must NOT have happened".
# grep consumes the stream (no -q) here and in porta_has: under pipefail, -q
# exiting at the first match kills the still-writing left side with SIGPIPE and
# the pipeline adopts 141 — for THIS "must NOT have happened" check that reads
# as "didn't happen", a false green (rete#260).
seen_since() {
  local f=$1 start=$2 pat=$3
  tail -n +"$start" "$f" 2>/dev/null | grep -E "$pat" >/dev/null
}

lines_in() { wc -l < "$1" 2>/dev/null || echo 0; }

# poll_cmd <tries> <sleep> <cmd...>: bounded wait for a command to succeed.
poll_cmd() {
  local tries=$1 nap=$2; shift 2
  local i
  for ((i = 0; i < tries; i++)); do
    "$@" >/dev/null 2>&1 && return 0
    sleep "$nap"
  done
  return 1
}

porta_has() {   # porta_has <node> <prefix> <ere>
  "$PORTA" get --db "$DB" "$1" "$2" 2>/dev/null | grep -E "$3" >/dev/null
}

# "Still checking in", which is a stronger claim than "still has an obs/health
# cell". Counted out of the EVENT LOG and not off the cell's seq: seq is the
# node's own counter, it lives in RAM, and both scenarios below make the node
# reboot — so a cell seq goes BACKWARDS across exactly the event this is trying
# to survive. (porta says so itself: "node-2 restarted … high-water 16 -> 0".)
# The event log is append-only in both directions, which is the run-up property
# porta_sync_host.sh already leans on.
health_rows() {   # rows of obs/health in <node>'s event log
  "$PORTA" log --db "$DB" "$1" --limit 5000 2>/dev/null | grep -c 'obs/health' || true
}

health_advanced() {   # health_advanced <node> <baseline-row-count>
  local n; n=$(health_rows "$1")
  [ -n "$n" ] && [ "$n" -gt "$2" ]
}

inactive_slot() {   # the slot the runner will hand the VM as OEVM_OTA_STAGE
  python3 -c 'import json,sys; print("b" if json.load(open(sys.argv[1]))["active"]=="a" else "a")' \
    "$WORK/node$1/manifest.json"
}

vm_pid() {      # the VM the runner is currently supervising for node $1
  pgrep -f "$WORK/node$1/slots/" | head -1
}

compile_node() {  # compile_node <n> <version> <out.oimg>
  local n=$1 v=$2 out=$3
  local src="$WORK/src-node$n-v$v.nsl"   # a separate `local`: bash expands all of
                                         # one local's words BEFORE assigning any
                                         # of them, so $v would be unset here
  sed -e "s/@LOCAL_PORT@/2697$n/g" -e "s/@VERSION@/$v/g" \
      zephyr/OtaBridgeNode.nsl.tmpl > "$src"
  if grep -qE '@(NODE_ID|LOCAL_PORT|VERSION)@' "$src"; then
    fail "OtaBridgeNode.nsl.tmpl still holds a placeholder after sed — the template gained one this gate does not fill"
  fi
  python3 "$NSL" "$src" "$out" --format=oimg --layout="$WORK/layout.json" >/dev/null
}

# blob_put <file>: store + sign at porta, and capture the three things it prints.
# No --node: this gate writes each goal itself, a bad signature included.
# BP_RAW is the signed App record's hex, reused verbatim — building the App record
# a second time here would be a second implementation of Task 3's shape.
BP_HASH=""; BP_SIG=""; BP_RAW=""
blob_put() {
  local out
  out=$("$PORTA" --db "$DB" blob put "$1")
  BP_HASH=$(printf '%s\n' "$out" | awk '/^hash /{print $2}')
  BP_SIG=$(printf '%s\n' "$out" | awk '/^sig /{print $2}')
  BP_RAW=$(printf '%s\n' "$out" | awk '/^record /{print $2}')
  [ -n "$BP_HASH" ] && [ -n "$BP_SIG" ] && [ -n "$BP_RAW" ] ||
    fail "porta blob put $1 did not print hash/sig/raw:
$out"
}

# --------------------------------------------------------------------------
# [1/9] preflight + porta
# --------------------------------------------------------------------------
[ -d "$PORTA_DIR" ] || fail "no porta at $PORTA_DIR — a broken checkout, not an absent sibling"
[ -f "$NSL" ]       || fail "no nsl compiler at $NSL"
[ -f tools/oevm_runner.py ] || fail "no runner at nodus/oevm/tools/oevm_runner.py"
[ -f zephyr/OtaBridgeNode.nsl.tmpl ] || fail "no node template at nodus/oevm/zephyr/OtaBridgeNode.nsl.tmpl"
[ -f tests/host_ota/never_bridges.nsl ] || fail "no scenario-4 payload at tests/host_ota/never_bridges.nsl"
# The ONE genuine environment skip, exactly as porta_sync_host.sh has it.
if ! command -v go >/dev/null 2>&1; then
  echo "SKIP: go is not on PATH — cannot build the porta counterparty"; exit 0
fi

rm -rf "$WORK"
mkdir -p "$WORK"
STARTED=1

echo "[1/9] build porta and custos"
( cd "$PORTA_DIR" && go build -o "$PORTA" ./cmd/porta )
( cd "$REPO/custos" && go build -o "$CUSTOS" ./cmd/custos )

echo "[2/9] host-ABI layout descriptor"
gcc -std=c99 -Isrc tools/dump_layout.c -o "$WORK/dl"
"$WORK/dl" > "$WORK/layout.json"

echo "[3/9] build the image VM"
# The porta_sync gcc line, minus -DOEVM_METHOD_PROFILE: nothing here drives a
# profile window, and the span cells it would publish are pure batch pressure
# against a rollout that already rides near the 512 B cap (#150).
gcc -Wall -std=c99 -Isrc \
    src/vm.c src/gc.c src/dispatch.c src/platform.c src/space.c src/handles.c \
    src/span.c \
    "src/backend/$TARGET/platform_$TARGET.c" src/serial.c \
    "src/backend/$TARGET/serial_$TARGET.c" src/udp.c "src/backend/$TARGET/udp_$TARGET.c" \
    src/http.c src/http_core.c "src/backend/$TARGET/http_$TARGET.c" \
    src/ota.c "src/backend/$TARGET/ota_$TARGET.c" \
    "src/backend/$TARGET/pulse_$TARGET.c" "src/backend/$TARGET/timer_$TARGET.c" \
    "src/backend/$TARGET/span_$TARGET.c" \
    "src/backend/$TARGET/sensor_$TARGET.c" \
    src/mirror.c \
    src/oimg_loader.c src/kernel_boot.c oevm_image_main.c -o "$VM" -lm

echo "[4/9] key, three v1 images, three node directories"
"$PORTA" --db "$DB" key gen >/dev/null
[ -s "$WORK/porta.pub" ] || fail "porta key gen wrote no porta.pub beside the db"

declare -A V1H V2H
for n in 1 2 3; do
  compile_node "$n" 1 "$WORK/n$n-v1.oimg"
  d="$WORK/node$n"
  mkdir -p "$d/slots" "$d/stage"
  cp "$WORK/n$n-v1.oimg" "$d/slots/a.oimg"
  cp "$WORK/porta.pub" "$d/pub.key"
  blob_put "$WORK/n$n-v1.oimg"
  V1H[$n]=$BP_HASH
  printf '{"active":"a","prev":null,"trial":false,"slots":{"a":{"hash":"%s","sig":"%s"},"b":null}}\n' \
    "$BP_HASH" "$BP_SIG" > "$d/manifest.json"
  echo "  node-$n v1 ${BP_HASH:0:12}… port 2697$n"
done

echo "[5/9] start porta (udp 127.0.0.1:$EXCH_PORT, blocks on $((EXCH_PORT + 1))) and three runners"
"$PORTA" serve --db "$DB" --udp "127.0.0.1:$EXCH_PORT" \
    --http-bind 127.0.0.1 --http-port "$PORTA_HTTP" >"$WORK/porta.log" 2>&1 &
PORTA_PID=$!
for _ in $(seq 1 50); do grep -q "serving blobs on udp" "$WORK/porta.log" && break; sleep 0.1; done
grep -q "serving the space" "$WORK/porta.log" || { cat "$WORK/porta.log"; fail "porta did not start"; }
grep -q "serving blobs on udp 127.0.0.1:$((EXCH_PORT + 1))" "$WORK/porta.log" ||
  { cat "$WORK/porta.log"; fail "porta did not bring up the block plane on $((EXCH_PORT + 1))"; }

# The agent's front: one custos, the three nodes as porta nodes (rete#398).
"$CUSTOS" --bind 127.0.0.1 --port "$CUSTOS_PORT" --porta-http "http://127.0.0.1:$PORTA_HTTP" \
    --porta-node "node-1=$NID-1" --porta-node "node-2=$NID-2" --porta-node "node-3=$NID-3" \
    >"$WORK/custos.log" 2>&1 &
CUSTOS_PID=$!
CUSTOS_URL="http://127.0.0.1:$CUSTOS_PORT"
poll_cmd 50 0.1 curl -sf "$CUSTOS_URL/boards" || { cat "$WORK/custos.log"; fail "custos did not start"; }

for n in 1 2 3; do
  # NEVER pass an OEVM_OTA_* / OEVM_IMAGE_HASH through --env: the runner owns
  # those and --env is applied last, so a well-meant override silently wins.
  python3 tools/oevm_runner.py \
      --node-dir "$WORK/node$n" --vm "$VM" \
      --watchdog-secs "$WATCHDOG" --env OEVM_TIMER_WALLCLOCK=1 \
      --env "OEVM_NODE_ID=$NID-$n" \
      >"$WORK/runner$n.log" 2>&1 &
  RUNNER_PIDS+=($!)
done

for n in 1 2 3; do
  wait_line "$WORK/runner$n.log" 1 "runner: exec a hash=${V1H[$n]}" 150 >/dev/null ||
    fail "node-$n never exec'd its v1 image"
  wait_line "$WORK/runner$n.log" 1 "OTA NODE READY v1" 150 >/dev/null ||
    fail "node-$n's v1 image never printed its banner"
done
echo "  three nodes up on v1"

# --------------------------------------------------------------------------
# [6/9] scenario 1 — clean deploy
# --------------------------------------------------------------------------
echo "[6/9] scenario 1: clean deploy, three nodes v1 -> v2"
for n in 1 2 3; do
  poll_cmd 150 0.2 porta_has "$NID-$n" obs/apps/main "0x${V1H[$n]}" ||
    fail "node-$n never published obs/apps/main at its v1 hash — the running-hash seam is dark"
done
echo "  all three published obs/apps/main at v1"

declare -A MARK
for n in 1 2 3; do MARK[$n]=$(( $(lines_in "$WORK/runner$n.log") + 1 )); done
# json_get <json> <key>: one top-level field of custos's answer (Push nested).
json_get() { python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print(d["Push"][sys.argv[2]])' "$1" "$2"; }
for n in 1 2 3; do
  compile_node "$n" 2 "$WORK/n$n-v2.oimg"
  out=$(curl -sS -f --data-binary "@$WORK/n$n-v2.oimg" "$CUSTOS_URL/board/node-$n/load?name=n$n-v2.oimg") ||
    fail "custos refused node-$n's push: $out"
  V2H[$n]=$(json_get "$out" SHA256)
  want=$(sha256sum "$WORK/n$n-v2.oimg" | cut -d' ' -f1)
  [ "${V2H[$n]}" = "$want" ] || fail "custos answered sha ${V2H[$n]} for node-$n, the image is $want"
  [ "$(json_get "$out" Converged)" = False ] ||
    fail "custos answered Converged before node-$n could have asked: $out"
  echo "  node-$n POST /board/node-$n/load -> goal seq $(json_get "$out" Seq), v2 ${V2H[$n]:0:12}…"
done

for n in 1 2 3; do
  f="$WORK/runner$n.log"
  L=$(wait_line "$f" "${MARK[$n]}" 'runner: staged-ok' 400) ||
    fail "node-$n never staged v2 (no runner: staged-ok)"
  L=$(wait_line "$f" "$((L + 1))" 'runner: trial [ab]' 200) ||
    fail "node-$n staged v2 but never entered a trial boot"
  L=$(wait_line "$f" "$((L + 1))" "runner: exec [ab] hash=${V2H[$n]}" 200) ||
    fail "node-$n's trial boot did not exec the v2 hash"
  L=$(wait_line "$f" "$((L + 1))" 'runner: confirmed [ab]' 200) ||
    fail "node-$n's v2 boot never confirmed — the trial would have been reverted"
  echo "  node-$n staged-ok -> trial -> exec v2 -> confirmed"
done

# The node saying it and the gateway seeing it are two claims. This is the
# second: the whole chain back through the push, or a node could be running v2
# with porta still believing v1.
for n in 1 2 3; do
  poll_cmd 150 0.2 porta_has "$NID-$n" obs/apps/main "0x${V2H[$n]}" ||
    fail "porta's tree never showed node-$n's obs/apps/main at the v2 hash"
done
# The third claim: the agent is told, by the route it pushed through.
converged() { [ "$(json_get "$(curl -sf "$CUSTOS_URL/board/node-$1/load/last")" Converged)" = True ]; }
for n in 1 2 3; do
  poll_cmd 50 0.2 converged "$n" || fail "custos's /board/node-$n/load/last never read Converged"
done
echo "ok 1 - clean deploy converged"

# --------------------------------------------------------------------------
# [7/9] scenario 2 — kill mid-transfer, the fetch resumes
# --------------------------------------------------------------------------
echo "[7/9] scenario 2: SIGKILL node-1 mid-transfer, the fetch must resume"
compile_node 1 3 "$WORK/n1-v3.oimg"
blob_put "$WORK/n1-v3.oimg"
V3H=$BP_HASH
TOTAL=$(stat -c %s "$WORK/n1-v3.oimg")
SLOT="$WORK/node1/slots/$(inactive_slot 1).oimg"
# The kill window is DEEP on purpose: half the image in, and still 20 kB short
# of done. Not an aesthetic choice — the resume evidence is "the slot never fell
# below where the kill left it", and that assertion is only as strong as the gap
# it has to notice. Killing at the first block would leave a ~1.5 kB floor, which
# a from-scratch refetch clears in well under one 50 ms sample: a restart would
# pass. Half an image is a ~2 s climb, which is ~40 samples wide.
KILL_ABOVE=$((TOTAL / 2))
KILL_BELOW=$((TOTAL - 20000))
[ "$KILL_BELOW" -gt "$KILL_ABOVE" ] || fail "v3 image is implausibly small ($TOTAL bytes)"

VMPID=$(vm_pid 1)
[ -n "$VMPID" ] || fail "cannot find node-1's VM process"
# The slot is not empty: it still holds the v1 image this node booted from, and
# ota.c will truncate it (203792 B is not on a 256 B block boundary) before it
# fetches a byte. So "part full" has to mean "changed AND part full", or a stale
# slot that happens to sit below the threshold would trigger an instant kill
# with nothing yet transferred.
PRESZ=$(stat -c %s "$SLOT" 2>/dev/null || echo 0)
M1=$(( $(lines_in "$WORK/runner1.log") + 1 ))
"$PORTA" put --db "$DB" "$NID-1" goal/apps/main --raw "$BP_RAW" >/dev/null
echo "  node-1 goal/apps/main -> v3 ${V3H:0:12}… (staging into ${SLOT##*/}, $TOTAL B, slot was $PRESZ B)"

KILLED_AT=0
MKILL=1
for _ in $(seq 1 1200); do          # 1200 * 0.05 s == 60 s, hard-bounded
  SZ=$(stat -c %s "$SLOT" 2>/dev/null || echo 0)
  if [ "$SZ" -gt "$KILL_ABOVE" ] && [ "$SZ" -lt "$KILL_BELOW" ] && [ "$SZ" != "$PRESZ" ]; then
    kill -9 "$VMPID" 2>/dev/null || true
    KILLED_AT=$SZ
    # Taken at the kill and NOT at the push: the first stage attempt legitimately
    # prints "not on a block boundary" (the slot still held the 203792 B v1
    # image, which is 16 B past a 256 B boundary), so a negative anchored at the
    # push would score that line and always fail.
    MKILL=$(( $(lines_in "$WORK/runner1.log") + 1 ))
    break
  fi
  sleep 0.05
done
[ "$KILLED_AT" -gt 0 ] ||
  fail "never caught node-1's staging slot between $KILL_ABOVE and $KILL_BELOW B — no mid-transfer kill happened"
echo "  killed the VM (pid $VMPID) with $KILLED_AT/$TOTAL B staged"

# The re-exec. Not "the runner survived" — the runner must put the SAME image
# back, because a killed non-trial boot is an ordinary death (G11: reboot IS the
# recovery) and must not be mistaken for a failed rollout.
wait_line "$WORK/runner1.log" "$M1" "runner: exec [ab] hash=${V2H[1]}" 200 >/dev/null ||
  fail "node-1's runner did not re-exec the v2 image after the kill"
echo "  the runner re-exec'd v2"

# THE RESUME. If the fetch restarted from block 0 the slot would drop back below
# where the kill left it; sampling is the only way to see that, because the end
# state of a restart and of a resume are identical.
LOW=$KILLED_AT
DONE=0
for _ in $(seq 1 1200); do
  SZ=$(stat -c %s "$SLOT" 2>/dev/null || echo 0)
  [ "$SZ" -lt "$LOW" ] && LOW=$SZ
  if [ "$SZ" -ge "$TOTAL" ]; then DONE=1; break; fi
  sleep 0.05
done
[ "$DONE" = 1 ] || fail "node-1's staging slot never reached $TOTAL B after the kill (stuck at $SZ)"
[ "$LOW" -ge "$KILLED_AT" ] ||
  fail "node-1 refetched from scratch: the slot fell to $LOW B, below the $KILLED_AT B already on disk"
echo "  the slot never fell below $KILLED_AT B — the fetch resumed"

L=$(wait_line "$WORK/runner1.log" "$M1" 'runner: staged-ok' 400) ||
  fail "node-1 never staged v3 after the resume"
L=$(wait_line "$WORK/runner1.log" "$((L + 1))" "runner: exec [ab] hash=$V3H" 200) ||
  fail "node-1 never exec'd the v3 hash"
wait_line "$WORK/runner1.log" "$((L + 1))" 'runner: confirmed [ab]' 200 >/dev/null ||
  fail "node-1's v3 boot never confirmed"

# RESUME AT THE RIGHT OFFSET, and not merely "resume". The shrink sampling above
# proves the slot was never thrown away; this proves the seam did not decide to
# throw it away and then rebuild it faster than a 50 ms sample. `ota.c` announces
# exactly one did-not-resume path on the console — "slot not on a block boundary,
# restarting" — and every restart path in ota_stage truncates to zero, so the two
# assertions are belt and suspender on the same claim from opposite ends. No
# change to ota.c was needed: the line was already there and already lands in the
# runner's log, interleaved on the same stream.
if seen_since "$WORK/runner1.log" "$MKILL" 'not on a block boundary'; then
  fail "node-1 restarted the fetch after the kill instead of resuming: ota.c reported an off-boundary slot"
fi
echo "  and it resumed at the block boundary the kill left, not from zero"
poll_cmd 150 0.2 porta_has "$NID-1" obs/apps/main "0x$V3H" ||
  fail "porta's tree never showed node-1 at the v3 hash"
echo "ok 2 - kill mid-transfer resumed"

# --------------------------------------------------------------------------
# [8/9] scenario 3 — a bad signature is refused, and the node lives
# --------------------------------------------------------------------------
echo "[8/9] scenario 3: node-2 is pushed a v4 record with a corrupted signature"
compile_node 2 4 "$WORK/n2-v4.oimg"
blob_put "$WORK/n2-v4.oimg"
V4H=$BP_HASH
# Flip the low byte of the signature's SCALAR half (byte 32), not byte 0: byte 0
# is inside the encoded point R, which usually fails to decompress, so the
# rejection would happen before the verification equation was ever evaluated.
# Rejecting a scalar flip requires actually checking [s]B == R + [h]A.
# (tools/test_oevm_runner.py:flip_sig_scalar records the same trap.)
OFF=64                              # 2 hex chars per byte, so byte 32 starts here
BYTE=${BP_SIG:$OFF:2}
BADBYTE=$(printf '%02x' $(( 0x$BYTE ^ 0x01 )))
BADSIG="${BP_SIG:0:$OFF}$BADBYTE${BP_SIG:$((OFF + 2))}"
[ "$BADSIG" != "$BP_SIG" ] && [ ${#BADSIG} -eq ${#BP_SIG} ] || fail "the signature flip did nothing"
# Substituting inside the encoded record leaves every structure byte alone, so
# the result is still legal CBOR carrying a still-64-byte bstr — which is the
# point: porta must ACCEPT it, and the refusal must come from the runner.
BADRAW="${BP_RAW/$BP_SIG/$BADSIG}"
[ "$BADRAW" != "$BP_RAW" ] || fail "the signature hex was not found inside the encoded App record"

HS_BEFORE=$(health_rows "$NID-2")
[ "${HS_BEFORE:-0}" -gt 0 ] || fail "node-2 has no obs/health rows at porta before the bad push"
M2=$(( $(lines_in "$WORK/runner2.log") + 1 ))
"$PORTA" put --db "$DB" "$NID-2" goal/apps/main --raw "$BADRAW" >/dev/null ||
  fail "porta refused the corrupted record — it should be legal CBOR, only the signature is wrong"
echo "  node-2 goal/apps/main -> v4 ${V4H:0:12}… with sig byte 32 flipped (porta accepted it)"

wait_line "$WORK/runner2.log" "$M2" 'runner: staged-reject' 500 >/dev/null ||
  fail "node-2's runner never printed staged-reject — an unsigned image was about to be booted"
L=$(wait_line "$WORK/runner2.log" "$M2" 'runner: staged-reject bad-signature' 5) ||
  fail "node-2 rejected the staged image, but not for its signature: $(tail -5 "$WORK/runner2.log")"
if seen_since "$WORK/runner2.log" "$M2" "runner: exec [ab] hash=$V4H"; then
  fail "node-2 BOOTED the badly signed image — the verify-outside-the-image seat (D4) is not holding"
fi
if seen_since "$WORK/runner2.log" "$M2" 'runner: trial [ab]'; then
  fail "node-2 entered a trial boot for a record whose signature never verified"
fi
wait_line "$WORK/runner2.log" "$((L + 1))" "runner: exec [ab] hash=${V2H[2]}" 200 >/dev/null ||
  fail "node-2 did not go straight back to the image it was already running"
porta_has "$NID-2" obs/apps/main "0x${V2H[2]}" ||
  fail "porta's tree no longer shows node-2 on v2 after the refusal"

# Alive, not merely un-upgraded: every check-in appends another obs/health row to
# porta's event log, so a node that had wedged would leave this count where it was.
poll_cmd 150 0.2 health_advanced "$NID-2" "$HS_BEFORE" ||
  fail "node-2's obs/health stopped arriving after the refusal (still $HS_BEFORE rows in the event log)"
echo "  node-2 still on v2 and still checking in (obs/health rows $HS_BEFORE -> $(health_rows "$NID-2"))"
echo "ok 3 - bad signature refused, node alive"

# --------------------------------------------------------------------------
# [9/9] scenario 4 — an image that never checks in is reverted
# --------------------------------------------------------------------------
echo "[9/9] scenario 4: node-3 is pushed a valid image that never contacts porta"
python3 "$NSL" tests/host_ota/never_bridges.nsl "$WORK/never_bridges.oimg" \
    --format=oimg --layout="$WORK/layout.json" >/dev/null
blob_put "$WORK/never_bridges.oimg"
NBH=$BP_HASH
M3=$(( $(lines_in "$WORK/runner3.log") + 1 ))
"$PORTA" put --db "$DB" "$NID-3" goal/apps/main --raw "$BP_RAW" >/dev/null
echo "  node-3 goal/apps/main -> never_bridges ${NBH:0:12}… (correctly signed)"

L=$(wait_line "$WORK/runner3.log" "$M3" 'runner: staged-ok' 500) ||
  fail "node-3 never staged never_bridges"
L=$(wait_line "$WORK/runner3.log" "$((L + 1))" 'runner: trial [ab]' 200) ||
  fail "node-3 staged never_bridges but never trialled it"
L=$(wait_line "$WORK/runner3.log" "$((L + 1))" "runner: exec [ab] hash=$NBH" 200) ||
  fail "node-3's trial boot did not exec the never_bridges hash"
L=$(wait_line "$WORK/runner3.log" "$((L + 1))" 'SILENT NODE READY' 200) ||
  fail "never_bridges never printed its banner — it must BOOT fine and simply not check in"
# Taken while the silent image is the one running, so it is the last number the
# v2 boot managed to publish. obs/apps/main cannot serve here: never_bridges
# publishes nothing, so porta's copy still reads v2 throughout and an assertion
# on it would pass with the revert deleted.
HS3_BEFORE=$(health_rows "$NID-3")
[ "${HS3_BEFORE:-0}" -gt 0 ] || fail "node-3 has no obs/health rows at porta"
echo "  never_bridges booted; nothing will confirm it"

T0=$SECONDS
L=$(wait_line "$WORK/runner3.log" "$((L + 1))" 'runner: revert [ab]' $((WATCHDOG * 15))) ||
  fail "node-3 was never reverted — the trial watchdog did not fire within $((WATCHDOG * 3)) s"
ELAPSED=$((SECONDS - T0))
# BOUNDED FROM BOTH SIDES, because `runner: revert <slot>` is printed for three
# different causes and this scenario names exactly one of them. The upper bound
# alone would score a never_bridges that CRASHED a second after its banner —
# ~1 s to revert, plan says no-check-in. Only the watchdog takes the full
# window, so the elapsed time IS the discriminator. (-2 s of slack: T0 is taken
# when the banner is first SEEN, up to a poll interval after the child spawned,
# and $SECONDS truncates.)
[ "$ELAPSED" -le $((WATCHDOG * 2)) ] ||
  fail "node-3 took ${ELAPSED}s to revert, more than 2x the ${WATCHDOG}s watchdog"
[ "$ELAPSED" -ge $((WATCHDOG - 2)) ] ||
  fail "node-3 reverted after only ${ELAPSED}s, far short of the ${WATCHDOG}s watchdog — the trial image DIED rather than failing to check in, which is a different scenario"
# The third cause. A trial slot that failed VERIFY reverts before it ever runs,
# and would leave no watchdog window at all; asserting its absence is what makes
# "the watchdog did this" the only reading left.
if seen_since "$WORK/runner3.log" "$M3" 'runner: verify-fail'; then
  fail "node-3's revert followed a verify-fail, not the watchdog — the staged image never got a boot"
fi
if seen_since "$WORK/runner3.log" "$M3" 'runner: confirmed'; then
  fail "node-3 CONFIRMED an image that never checked in"
fi
wait_line "$WORK/runner3.log" "$((L + 1))" "runner: exec [ab] hash=${V2H[3]}" 200 >/dev/null ||
  fail "node-3 did not land back on its previous image after the revert"
poll_cmd 200 0.2 health_advanced "$NID-3" "$HS3_BEFORE" ||
  fail "node-3 came back on v2 but never checked in again (still $HS3_BEFORE obs/health rows)"
echo "  reverted after ${ELAPSED}s, re-exec'd v2 and resumed checking in"
echo "ok 4 - no-check-in reverted"

echo "HOST OTA OK"
