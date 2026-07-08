#!/usr/bin/env bash
# Task 10 end-to-end: compile a known program to .oimg, mmap-load and run it
# with the standalone image VM, assert it prints the same value as Model A (42).
set -e
cd "$(dirname "$0")/.."
NS=../ns-oevm
TARGET="${OEVM_TARGET:-posix}"   # per-target backend dir: src/backend/$TARGET/

# 1. host-ABI layout descriptor
gcc -std=c99 -Isrc tools/dump_layout.c -o /tmp/dl
/tmp/dl > /tmp/layout.json

# 2. compile the program to a .oimg blob
SRC=../ns-tests/04_somns_basic/01_method_call.ns
python3 "$NS/tools/ns_compile_oevm.py" "$SRC" /tmp/app.oimg --format=oimg --layout=/tmp/layout.json

# 3. build the standalone image VM
gcc -Wall -std=c99 -Isrc src/vm.c src/gc.c src/dispatch.c src/platform.c src/space.c src/handles.c "src/backend/$TARGET/platform_$TARGET.c" src/serial.c "src/backend/$TARGET/serial_$TARGET.c" src/udp.c "src/backend/$TARGET/udp_$TARGET.c" "src/backend/$TARGET/pulse_$TARGET.c" src/mirror.c src/oimg_loader.c \
    src/kernel_boot.c oevm_image_main.c -o /tmp/oevm_img -lm

# 4. run and assert
OUT="$(/tmp/oevm_img /tmp/app.oimg)"
echo "image-VM output: [$OUT]"
if echo "$OUT" | grep -qx '42'; then
    echo "PASS: image VM printed 42"
    exit 0
else
    echo "FAIL: expected 42, got [$OUT]"
    exit 1
fi
