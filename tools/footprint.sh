#!/usr/bin/env bash
# Engine footprint (host proxy for arm-zephyr-eabi rom_report). Sums `text` of the
# engine objects; the .oimg/solidified image object is EXCLUDED (it scales with the
# program, not the VM). Run before and after the deletion; diff the total text.
set -eu
cd "$(dirname "$0")/.." || exit 2

# FOOTPRINT_CONFIG=mcu: real arm-zephyr-eabi cross build (host proxy above/below
# replaced by the true-target figure). Requires the NCS SDK on PATH — the caller
# must `source ns-zephyr/tools/env.sh` before invoking this script; arm-zephyr-eabi-gcc
# is not on the default PATH otherwise.
#
# Engine core ONLY. Every src/backend/* file (posix AND zephyr) is EXCLUDED here,
# including src/backend/zephyr/{platform,serial,udp,pulse}_zephyr.c — those TUs
# `#include <zephyr/...>` and only compile inside a full Zephyr/west build; that
# backend seam is measured there instead (Task 7), not by this standalone cross-compile.
#
# -D__ZEPHYR__=1 and -DOEVM_NO_FILE_IO mirror the two compile definitions the real
# Zephyr app build applies (ns-zephyr/app/CMakeLists.txt sets OEVM_NO_FILE_IO
# explicitly; __ZEPHYR__=1 is defined for every Zephyr build by Zephyr's own
# top-level CMakeLists.txt). Without them this standalone compile fails outright:
# vm.c's default_millis() needs __ZEPHYR__ to compile out the POSIX
# clock_gettime/CLOCK_MONOTONIC path (no POSIX timers on bare-metal), and
# oimg_loader.c needs OEVM_NO_FILE_IO to compile out its <sys/mman.h> POSIX
# file-mapping path (no mmap on bare-metal either). Both guards already exist in
# the source for exactly this purpose; this branch just supplies the same defines
# the real target build supplies.
if [ "${FOOTPRINT_CONFIG:-host}" = "mcu" ]; then
  XCC=${XCC:-arm-zephyr-eabi-gcc}
  MCPU="-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16"
  $XCC -Os -std=c99 -Isrc $MCPU -D__ZEPHYR__=1 -DOEVM_NO_FILE_IO -c \
    src/vm.c src/gc.c src/dispatch.c src/kernel_boot.c src/platform.c \
    src/serial.c src/udp.c src/mirror.c src/oimg_loader.c src/space.c src/handles.c || {
      echo "mcu build failed — is the NCS SDK on PATH? (source ns-zephyr/tools/env.sh)" >&2
      exit 2; }
  arm-zephyr-eabi-size -t vm.o gc.o dispatch.o kernel_boot.o platform.o serial.o \
        udp.o mirror.o oimg_loader.o space.o handles.o
  rm -f *.o
  exit 0
fi

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
