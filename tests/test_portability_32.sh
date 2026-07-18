#!/usr/bin/env bash
# oevm/tests/test_portability_32.sh
# Task 14: Prove a 32-bit (-m32) build serializes + loads + runs a .oimg image.
set -e
# exit 77 (skip convention) when the 32-bit toolchain is absent, so the
# regression gate records SKIP rather than FAIL on hosts without gcc-multilib.
gcc -m32 -E -x c /dev/null >/dev/null 2>&1 || { echo "SKIP: gcc-multilib not installed. Run: sudo apt-get install gcc-multilib"; exit 77; }
cd "$(dirname "$0")/.."
# Task 9a: repointed from the frozen ns-oevm/ns-tests ancestors to the active
# nsl-tuvm compiler + nsl-tests corpus. The ns-oevm compiler still stamps
# registry_version 4; after the Task 9a registry compaction the tuvm firmware is
# v5, so an ns-oevm-built image is rejected on load. nsl-tuvm emits v5 and matches.
NS=../nsl-tuvm
TARGET="${OEVM_TARGET:-posix}"   # per-target backend dir: src/backend/$TARGET/
gcc -m32 -std=c99 -Isrc tools/dump_layout.c -o /tmp/dl32 && /tmp/dl32 > /tmp/layout32.json
python3 -c "import json;d=json.load(open('/tmp/layout32.json'));assert d['oop_size']==4,d['oop_size']"
SRC=../nsl-tests/04_somns_basic/01_method_call.nsl   # same minimal test as Task 10
python3 $NS/tools/ns_compile_oevm.py "$SRC" /tmp/app32.oimg --format=oimg --layout=/tmp/layout32.json
gcc -m32 -Wall -std=c99 -Isrc src/vm.c src/gc.c src/dispatch.c src/platform.c src/space.c src/handles.c "src/backend/$TARGET/platform_$TARGET.c" src/serial.c "src/backend/$TARGET/serial_$TARGET.c" src/udp.c "src/backend/$TARGET/udp_$TARGET.c" src/http.c "src/backend/$TARGET/http_$TARGET.c" "src/backend/$TARGET/pulse_$TARGET.c" "src/backend/$TARGET/timer_$TARGET.c" src/mirror.c src/oimg_loader.c src/kernel_boot.c oevm_image_main.c -o /tmp/oevm_img32 -lm
OUTPUT=$(/tmp/oevm_img32 /tmp/app32.oimg)
echo "$OUTPUT"
[ "$OUTPUT" = "42" ] || { echo "FAIL: expected 42, got $OUTPUT"; exit 1; }
echo "32-bit portability OK"
