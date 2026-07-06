#!/usr/bin/env bash
# Engine footprint (host proxy for arm-zephyr-eabi rom_report). Sums `text` of the
# engine objects; the .oimg/solidified image object is EXCLUDED (it scales with the
# program, not the VM). Run before and after the deletion; diff the total text.
set -eu
cd "$(dirname "$0")/.." || exit 2
CC=${CC:-gcc}; TMP=$(mktemp -d)
$CC -Os -std=c99 -Isrc -c \
  src/vm.c src/dispatch.c src/gc.c src/kernel_boot.c src/platform.c \
  src/serial.c src/udp.c src/mirror.c src/oimg_loader.c src/space.c \
  src/backend/posix/platform_posix.c src/backend/posix/serial_posix.c \
  src/backend/posix/udp_posix.c
size -t vm.o dispatch.o gc.o kernel_boot.o platform.o serial.o udp.o \
        mirror.o oimg_loader.o space.o platform_posix.o serial_posix.o udp_posix.o
# RAM regression is a gate failure like a text regression (design addendum A2).
# data+bss total from the same `size` output; these are host-proxy builds, so this
# is the host proxy for the MCU static-RAM figure (same caveat as the text total
# above — gc.o's host arena dwarfs the real MCU heap and is not representative).
RAM=$(size -t vm.o dispatch.o gc.o kernel_boot.o platform.o serial.o udp.o \
        mirror.o oimg_loader.o space.o platform_posix.o serial_posix.o udp_posix.o \
      | awk 'END { print $2 + $3 }')
echo "data+bss (host proxy for MCU static RAM): $RAM"
rm -f *.o; rmdir "$TMP" 2>/dev/null || true
