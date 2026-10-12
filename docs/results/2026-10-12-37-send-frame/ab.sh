#!/usr/bin/env bash
# rete#37 send/frame levers: AWFY on the host, nsl revision A against revision B, interleaved (README.md beside this
# file). Each revision is built from its own tree (its compiler and its -O2 image VM, since a lever may change both);
# both run the same host-scaled awfy.nsl (the counts of docs/results/2026-10-12-37-awfy-soup/run_all.sh).
# Usage: ab.sh REV_A REV_B RUNS OUT
set -euo pipefail
A=$1 B=$2 RUNS=$3
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
OUT=$(mkdir -p "$4/logs" && cd "$4" && pwd)
W=$(mktemp -d /tmp/awfy-ab-XXXXXX)
trap 'git -C "$REPO" worktree remove --force "$W/a" 2>/dev/null; git -C "$REPO" worktree remove --force "$W/b" 2>/dev/null; rm -rf "$W"' EXIT

sed -E \
  -e "s/(bench: Bounce new +inner: )1\)/\1300)/"       -e "s/(bench: List new +inner: )1\)/\1100)/" \
  -e "s/(bench: Permute new +inner: )1\)/\1200)/"      -e "s/(bench: Storage new +inner: )1\)/\1200)/" \
  -e "s/(bench: Richards new +inner: )1\)/\15)/"       -e "s/(bench: Queens new +inner: )1\)/\1200)/" \
  -e "s/(bench: Sieve new +inner: )1\)/\1300)/"        -e "s/(bench: Towers new +inner: )1\)/\160)/" \
  -e "s/(bench: NBody new +inner: )1\)/\1250000)/"     -e "s/(bench: DeltaBlue new +inner: )100\)/\12000)/" \
  -e "s/(bench: Json new +inner: )1\)/\120)/" \
  "$REPO/nodus/tests/00_perf/awfy.nsl" > "$W/awfy-host.nsl"
[ "$(diff "$REPO/nodus/tests/00_perf/awfy.nsl" "$W/awfy-host.nsl" | grep -c '^>')" = 11 ] \
  || { echo "ab: awfy.nsl's runAll moved; update the counts" >&2; exit 1; }

build() {  # build <tag> <rev>
  local t=$W/$1 P=posix
  git -C "$REPO" worktree add -q --detach "$t" "$2"
  ln -sfn "$REPO/nodus/nsl/grammar/node_modules" "$t/nodus/nsl/grammar/node_modules"
  ( cd "$t/nodus/oevm"
    gcc -std=c99 -Isrc tools/dump_layout.c -o "$t/dump_layout" && "$t/dump_layout" > "$t/layout.json"
    gcc -O2 -DOEVM_NO_DEBUG_HOOKS -DOEVM_METHOD_PROFILE -Wall -std=c99 -Isrc \
      src/vm.c src/gc.c src/dispatch.c src/platform.c src/space.c src/handles.c src/span.c src/serial.c \
      src/udp.c src/http.c src/http_core.c src/mirror.c src/ota.c \
      src/backend/$P/platform_$P.c src/backend/$P/serial_$P.c src/backend/$P/udp_$P.c src/backend/$P/pulse_$P.c \
      src/backend/$P/timer_$P.c src/backend/$P/span_$P.c src/backend/$P/http_$P.c src/backend/$P/sensor_$P.c \
      src/backend/$P/ota_$P.c src/oimg_loader.c src/kernel_boot.c oevm_image_main.c -o "$t/vm" -lm ) 2> "$OUT/logs/build-$1.log"
  python3 "$t/nodus/nsl/tools/ns_compile_oevm.py" "$W/awfy-host.nsl" "$t/awfy.oimg" --format=oimg \
    --layout="$t/layout.json" > /dev/null
  echo "$1: $(git -C "$REPO" rev-parse --short "$2") $(git -C "$REPO" log -1 --format=%s "$2" | cut -c1-90)" >> "$OUT/logs/env.txt"
}
{ echo "host: $(hostname)  cpu:$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2-)"; gcc --version | head -1; } > "$OUT/logs/env.txt"
build a "$A"
build b "$B"
for i in $(seq 1 "$RUNS"); do
  "$W/a/vm" "$W/a/awfy.oimg" > "$OUT/logs/a-$i.log" 2>&1
  "$W/b/vm" "$W/b/awfy.oimg" > "$OUT/logs/b-$i.log" 2>&1
done
python3 "$(dirname "$0")/abtable.py" "$OUT/logs" | tee "$OUT/table.md"
