#!/usr/bin/env bash
# rete#37 heap census: survivors (objects, header+payload bytes) at every GC, for one revision, -m32 (board layout).
# Usage: census.sh REV TAG   (run on msi from ~/workspace/rete). Out: ~/census/TAG.log
set -euo pipefail
REV=$1; TAG=$2
W=~/census/$TAG; rm -rf "$W"; mkdir -p "$W"
git -C ~/workspace/rete archive "$REV" nodus/oevm nodus/nsl nodus/tests/00_perf | tar -x -C "$W"
ln -sfn ~/workspace/rete/nodus/nsl/grammar/node_modules "$W/nodus/nsl/grammar/node_modules"
G=$W/nodus/oevm/src/gc.c
sed -i '1i #include <stdio.h>' "$G"
sed -i 's/size_t swept_live = 0;/size_t swept_live = 0; size_t census_n = 0;/' "$G"
sed -i 's/{ swept_live +=/{ census_n++; swept_live +=/' "$G"
sed -i 's/^    heap_live_bytes = swept_live;/    heap_live_bytes = swept_live; fprintf(stdout, "CENSUS %zu %zu\\n", census_n, swept_live); fflush(stdout);/' "$G"
grep -c 'census_n' "$G" | grep -qx 3 || { echo "patch did not apply"; exit 1; }
B=posix
( cd "$W/nodus/oevm"
  gcc -m32 -std=c99 -Isrc tools/dump_layout.c -o "$W/dump_layout"
  "$W/dump_layout" > "$W/layout.json"
  gcc -m32 -O2 -DOEVM_HEAP_SIZE=${HEAP:-1048576} -DOEVM_NO_DEBUG_HOOKS -std=c99 -Isrc \
    src/vm.c src/gc.c src/dispatch.c src/platform.c src/space.c src/handles.c src/span.c src/serial.c \
    src/udp.c src/http.c src/http_core.c src/mirror.c src/ota.c \
    src/backend/$B/platform_$B.c src/backend/$B/serial_$B.c src/backend/$B/udp_$B.c \
    src/backend/$B/pulse_$B.c src/backend/$B/timer_$B.c src/backend/$B/span_$B.c src/backend/$B/http_$B.c \
    src/backend/$B/sensor_$B.c src/backend/$B/ota_$B.c \
    src/oimg_loader.c src/kernel_boot.c oevm_image_main.c -o "$W/vm" -lm )
python3 "$W/nodus/nsl/tools/ns_compile_oevm.py" "$W/nodus/tests/00_perf/awfy.nsl" "$W/awfy.oimg" \
  --format=oimg --layout="$W/layout.json" > /dev/null
stdbuf -o0 "$W/vm" "$W/awfy.oimg" > ~/census/$TAG.log 2>&1
tail -3 ~/census/$TAG.log
