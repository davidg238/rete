#!/bin/bash
# Usage: bench.sh <test.ns> [runs]   — compile+build -O2, time N runs, report best.
set -e
NS="${1:-../ns-tests/00_perf/fib.ns}"
RUNS="${2:-5}"
cd /home/david/workspace/oevm
TARGET="${OEVM_TARGET:-posix}"   # per-target backend dir: src/backend/$TARGET/
python3 /home/david/workspace/ns-oevm/tools/ns_compile_oevm.py "$NS" src/solidified_image.c >/dev/null
gcc -O2 -DOEVM_APPLIANCE -std=c99 -Isrc src/vm.c src/gc.c src/dispatch.c src/platform.c src/space.c src/handles.c "src/backend/$TARGET/platform_$TARGET.c" src/serial.c "src/backend/$TARGET/serial_$TARGET.c" src/udp.c "src/backend/$TARGET/udp_$TARGET.c" src/http.c "src/backend/$TARGET/http_$TARGET.c" "src/backend/$TARGET/pulse_$TARGET.c" src/mirror.c src/solidified_image.c src/oevm_main.c -o oevm_bench -lm -lpthread
best=99999
for i in $(seq 1 "$RUNS"); do
  t=$( { /usr/bin/time -f "%e" ./oevm_bench >/dev/null; } 2>&1 )
  echo "  run $i: ${t}s"
  awk "BEGIN{exit !($t < $best)}" && best=$t
done
echo "BEST($NS): ${best}s"
