#!/usr/bin/env bash
# rete#199 host reproduction, CORRECTED — at the C6's ACTUAL memory configuration.
#
# Supersedes repro199.sh, whose result was retracted: it set OEVM_HEAP_SIZE as an
# environment variable, but that is a compile-time macro (gc.c:18-19), so the node
# ran with the 16 MB host default and "it survived" measured nothing.
#
# Two things had to change, and the second is the one #179 was hiding:
#   1. the arena is a -D, not an env var;
#   2. the VM is built -m32.  A 64-bit host cannot host the device's arena at all,
#      because VM_STACK_INIT_WORDS=16384 x 8 B = 131072 = the whole C6 arena — that
#      is the "boot-path allocation asking for the whole arena" of #179, and it is
#      not a bug, it is a word-size coincidence.  At -m32 with the device's own
#      knobs (VM_STACK_INIT_WORDS=4096) the arena is byte-for-byte and
#      object-for-object what the C6 runs.
#
# CONTROL: the same node against the MATCHING gateway. Without it, "the arena
# grows" is a reading of one run and not a finding.
set -uo pipefail
# ROOT is DERIVED, not hard-coded. The point of a fix gate is to run against the
# tree carrying the fix; a script that silently rebuilt the main checkout would
# report the wrong tree's behaviour and read exactly like a passing gate.
ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
TUVM=$ROOT/nodus/tuvm
# Artifacts land OUTSIDE the repo: this script builds a throwaway git worktree, and
# putting that under docs/ leaves the tree dirty in a way that reads as source.
OUT=${OUT:-$(mktemp -d "${TMPDIR:-/tmp}/repro199.XXXXXX")}
NSL=$ROOT/nodus/nsl/tools/ns_compile_oevm.py
HEAP=${HEAP:-131072}
STACKW=${STACKW:-4096}
RUN_S=${RUN_S:-900}
LEGS=${LEGS:-AB}          # A = mismatched (the fix gate), B = matched control
# The commit alone would LIE whenever the fix under test is still uncommitted, which
# is the normal case for a fix gate — say so explicitly rather than printing a hash
# that names a tree this run is not measuring.
DIRTY=$(git -C "$ROOT" status --porcelain -- nodus porta protocol | head -c 1)
echo "== tree under test: $ROOT ($(git -C "$ROOT" rev-parse --short HEAD)${DIRTY:+ + UNCOMMITTED CHANGES})"
[ -n "$DIRTY" ] && git -C "$ROOT" status --short -- nodus porta protocol | sed 's/^/==   /'
echo "== artifacts:       $OUT"

echo "== [1] build the MISMATCHED gateway (porta @ e52a86e, pre-#197 labels)"
WT=$OUT/porta-old-wt
git -C "$ROOT" worktree remove --force "$WT" 2>/dev/null
rm -rf "$WT"
git -C "$ROOT" worktree add -f "$WT" e52a86e >/dev/null 2>&1 || { echo "worktree failed"; exit 1; }
( cd "$WT/porta" && go build -o "$OUT/porta-old" ./cmd/porta ) || { echo "old porta build failed"; exit 1; }
echo "   porta-old built at $(git -C "$WT" rev-parse --short HEAD)"

echo "== [2] build the MATCHING gateway (porta @ HEAD) for the control"
( cd "$ROOT/porta" && go build -o "$OUT/porta-new" ./cmd/porta ) || exit 1

echo "== [3] node image + VM from HEAD, 32-bit, at the device's knobs"
cd "$TUVM"
gcc -m32 -w -std=c99 -Isrc tools/dump_layout.c -o "$OUT/dl32" && "$OUT/dl32" > "$OUT/layout32.json" || exit 1
python3 -c "import json;d=json.load(open('$OUT/layout32.json'));assert d['oop_size']==4,d" || exit 1
python3 "$NSL" zephyr/PortaBridgeNode.nsl "$OUT/node32.oimg" --format=oimg --layout="$OUT/layout32.json" >/dev/null || exit 1
gcc -m32 -w -std=c99 -Isrc -O2 \
    src/vm.c src/gc.c src/dispatch.c src/platform.c src/space.c src/handles.c src/span.c \
    src/backend/posix/platform_posix.c src/serial.c src/backend/posix/serial_posix.c \
    src/udp.c src/backend/posix/udp_posix.c \
    src/http.c src/http_core.c src/backend/posix/http_posix.c \
    src/ota.c src/backend/posix/ota_posix.c \
    src/backend/posix/pulse_posix.c src/backend/posix/timer_posix.c \
    src/backend/posix/span_posix.c src/backend/posix/sensor_posix.c \
    src/mirror.c src/oimg_loader.c src/kernel_boot.c oevm_image_main.c \
    -o "$OUT/oevm199_32" \
    -DOEVM_HEAP_SIZE=$HEAP -DVM_STACK_INIT_WORDS=$STACKW \
    -DOEVM_MARK_STACK_SIZE=512 -DMETHOD_CACHE_SIZE=256 -DPRIM_CACHE_SIZE=256 -lm || exit 1
echo "   oevm199_32: arena=$HEAP stack=${STACKW}w ($((STACKW*4)) B) oop=4"

# ---- one leg -------------------------------------------------------------
# $1 = label, $2 = porta binary, $3 = udp port, $4 = db.
# Port 16969 for BOTH legs and not a parameter in any real sense: the node
# addresses its gateway by a SOURCE CONSTANT (#118, PortaBridgeNode>>gwPort),
# so the legs run sequentially on one port rather than side by side.
leg() {
  local label=$1 bin=$2 port=$3 db=$4
  local log=$OUT/m32-$label.log serve=$OUT/m32-serve-$label.log
  rm -f "$db" "$db-wal" "$db-shm" "$OUT/bootid-$label" "$log" "$serve"
  "$bin" serve --db "$db" --udp 127.0.0.1:$port --http-port 0 >"$serve" 2>&1 &
  local pp=$!
  for _ in $(seq 1 80); do grep -q "serving the space" "$serve" && break; sleep 0.1; done
  if ! grep -q "serving the space" "$serve"; then
    echo "   $label: porta did not start"; kill $pp 2>/dev/null; return 1; fi

  local t0=$SECONDS
  OEVM_TIMER_WALLCLOCK=1 TUVM_BOOTID_FILE="$OUT/bootid-$label" \
    "$OUT/oevm199_32" "$OUT/node32.oimg" >"$log" 2>&1 &
  local np=$!
  local waited=0
  while [ $waited -lt "$RUN_S" ]; do
    kill -0 $np 2>/dev/null || break
    sleep 2; waited=$((waited+2))
  done
  local died=0; kill -0 $np 2>/dev/null || died=1
  local dur=$((SECONDS - t0))
  kill $np $pp 2>/dev/null; wait $np 2>/dev/null; wait $pp 2>/dev/null

  # The host twin prints no per-tick line (that was repro199.sh's "ticks=0"
  # artifact), so the exchange count is read off the GATEWAY, which sees every
  # datagram whether it accepts it or refuses it.
  local rej=$(grep -c "rejecting datagram" "$serve")
  local rx=$(grep -cE "rejecting datagram|exchange|pull|push" "$serve")
  echo "   $label: node_died=$died  after=${dur}s  refused_datagrams=$rej"
  # Frame-size progression: the backlog's only externally visible trace, until
  # pushFrame's 512 B trim caps it and the growth moves entirely into the arena.
  grep -o "([0-9]* bytes)" "$serve" | grep -o "[0-9]*" | awk '
    NR==1{f=$1;c=1;next}
    $1!=f{printf "%s B x%d -> ",f,c; f=$1; c=1; next}
    {c++} END{if(NR)printf "%s B x%d\n",f,c}' | tail -c 400
  echo
  grep -m1 "Out of Memory" "$log" && echo "   ^^ $label OOMed"
  return 0
}

case "$LEGS" in
  *A*) echo "== [4] leg A: MISMATCHED gateway (every frame refused)"
       leg mismatch "$OUT/porta-old" 16969 "$OUT/m32a.db" ;;
esac
case "$LEGS" in
  *B*) echo "== [5] leg B: CONTROL, matched gateway"
       leg control  "$OUT/porta-new" 16969 "$OUT/m32b.db" ;;
esac

git -C "$ROOT" worktree remove --force "$WT" 2>/dev/null
echo "== done; logs in $OUT/m32-*.log"
