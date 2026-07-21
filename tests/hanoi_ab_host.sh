#!/usr/bin/env bash
# Host sanity for the OEOM native-substitution spike (Phase 1). Builds the image VM
# with OEVM_HANOI_NATIVE, runs HanoiAB, and asserts the native hanoiMove: returns
# 2^n-1 — which also proves the timed native call dispatched to prim_hanoi (a DNU
# or wrong receiver would not return the exact move count). Host timings are NOT the
# deliverable (host clock may not advance); correctness + dispatch fidelity are.
set -euo pipefail
cd "$(dirname "$0")/.."
NSL="$HOME/workspace/nsl-tuvm/tools/ns_compile_oevm.py"
TARGET="${OEVM_TARGET:-posix}"

echo "[1/4] host-ABI layout descriptor"
gcc -std=c99 -Isrc tools/dump_layout.c -o /tmp/dl
/tmp/dl > /tmp/layout.json

echo "[2/4] compile HanoiAB.nsl -> .oimg"
python3 "$NSL" zephyr/HanoiAB.nsl /tmp/hanoi_ab.oimg --format=oimg --layout=/tmp/layout.json

echo "[3/4] build image VM with -DOEVM_HANOI_NATIVE"
gcc -Wall -std=c99 -DOEVM_HANOI_NATIVE -Isrc \
    src/vm.c src/gc.c src/dispatch.c src/platform.c src/space.c src/handles.c \
    src/span.c \
    "src/backend/$TARGET/platform_$TARGET.c" src/serial.c \
    "src/backend/$TARGET/serial_$TARGET.c" src/udp.c "src/backend/$TARGET/udp_$TARGET.c" \
    src/http.c src/http_core.c "src/backend/$TARGET/http_$TARGET.c" \
    "src/backend/$TARGET/pulse_$TARGET.c" "src/backend/$TARGET/timer_$TARGET.c" \
    "src/backend/$TARGET/span_$TARGET.c" \
    "src/backend/$TARGET/sensor_$TARGET.c" \
    src/mirror.c src/hanoi_native.c \
    src/oimg_loader.c src/kernel_boot.c oevm_image_main.c -o /tmp/oevm_hanoi -lm

echo "[4/4] run + assert move counts"
OUT="$(/tmp/oevm_hanoi /tmp/hanoi_ab.oimg)"
echo "--- program output ---"; echo "$OUT"; echo "----------------------"
ok=1
for n in 16 20; do
  want=$(( (1 << n) - 1 ))
  if echo "$OUT" | grep -q "hanoi-moves ${n}: ${want}"; then
    echo "PASS moves n=$n == $want"
  else
    echo "FAIL moves n=$n (want $want)"; ok=0
  fi
done
if [ "$ok" = 1 ]; then echo "HOST SANITY PASS"; else echo "HOST SANITY FAIL"; exit 1; fi
