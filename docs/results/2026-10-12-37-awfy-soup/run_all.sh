#!/usr/bin/env bash
# rete#37: Are We Fast Yet on the host, nsl against Primordial Soup (README.md beside this file).
# Builds an -O2 image VM, scales awfy.nsl's board-sized inner: counts up to host size, and runs the same
# source on both VMs, sequentially, RUNS times each. Logs and table.md go to OUT (default: beside this file).
# Usage: run_all.sh [RUNS [OUT]]
set -euo pipefail
RUNS=${1:-3}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(mkdir -p "${2:-$HERE}" && cd "${2:-$HERE}" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
OEVM=$REPO/nodus/oevm
W=$(mktemp -d /tmp/awfy-soup-XXXXXX)
mkdir -p "$OUT/logs"

python3 "$REPO/nodus/tests/00_perf/awfy_counts.py" host > "$W/awfy-host.nsl"   # the host profile: counts in one place

# The image VM as the suite builds it (run_tests_oevm.py), plus -O2 and no debug hooks.
B=posix
( cd "$OEVM"
  gcc -std=c99 -Isrc tools/dump_layout.c -o "$W/oevm_dump_layout"
  "$W/oevm_dump_layout" > "$W/oevm_layout.json"
  gcc -O2 -DOEVM_NO_DEBUG_HOOKS -DOEVM_METHOD_PROFILE -Wall -std=c99 -Isrc \
    src/vm.c src/gc.c src/dispatch.c src/platform.c src/space.c src/handles.c src/span.c src/serial.c \
    src/udp.c src/http.c src/http_core.c src/mirror.c src/ota.c \
    src/backend/$B/platform_$B.c src/backend/$B/serial_$B.c src/backend/$B/udp_$B.c \
    src/backend/$B/pulse_$B.c src/backend/$B/timer_$B.c src/backend/$B/span_$B.c src/backend/$B/http_$B.c \
    src/backend/$B/sensor_$B.c src/backend/$B/ota_$B.c \
    src/oimg_loader.c src/kernel_boot.c oevm_image_main.c -o "$W/oevm_img_o2" -lm ) 2> "$OUT/logs/build.log"
python3 "$REPO/nodus/nsl/tools/ns_compile_oevm.py" "$W/awfy-host.nsl" "$W/awfy.oimg" \
  --format=oimg --layout="$W/oevm_layout.json" > /dev/null

{ echo "host: $(hostname)  rete: $(git -C "$REPO" rev-parse --short HEAD)  soup: $(git -C ~/workspace/primordialsoup rev-parse --short HEAD 2>/dev/null || echo '?')"
  echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2-)"
  gcc --version | head -1; } > "$OUT/logs/env.txt"

for i in $(seq 1 "$RUNS"); do
  "$W/oevm_img_o2" "$W/awfy.oimg" > "$OUT/logs/nsl-$i.log" 2>&1
  python3 "$REPO/nodus/tests/00_perf/awfy-soup.py" "$W/soup" "$W/awfy-host.nsl" > "$OUT/logs/soup-$i.log" 2>&1
done
python3 "$HERE/table.py" "$OUT/logs" | tee "$OUT/table.md"
rm -rf "$W"
