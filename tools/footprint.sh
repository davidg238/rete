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
  # SKIP (exit 77) when the cross toolchain is absent, so the named gate reports
  # SKIP rather than FAIL on hosts without the NCS SDK sourced.
  command -v "$XCC" >/dev/null 2>&1 || {
    echo "footprint(mcu): SKIP — $XCC not on PATH (source ns-zephyr/tools/env.sh)" >&2
    exit 77; }
  MCPU="-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16"
  # Config-D target sizes (DK values from zephyr/README.md; the real Zephyr app build
  # passes the same via ns-zephyr/app/CMakeLists.txt). Without these the arena/caches
  # keep their 16 MB / 4096-entry HOST defaults, reproducing the tuvm#5/A2 host-proxy
  # RAM defect this gate exists to eliminate. data+bss must reflect the TARGET config.
  TGT_SIZES="-DOEVM_HEAP_SIZE=49152 -DMETHOD_CACHE_SIZE=256 -DPRIM_CACHE_SIZE=256"
  $XCC -Os -std=c99 -Isrc $MCPU -D__ZEPHYR__=1 -DOEVM_NO_FILE_IO $TGT_SIZES -c \
    src/vm.c src/gc.c src/dispatch.c src/kernel_boot.c src/platform.c \
    src/serial.c src/udp.c src/http.c src/mirror.c src/oimg_loader.c src/space.c src/handles.c || {
      echo "footprint(mcu): cross-compile failed" >&2; rm -f *.o; exit 1; }
  # TOTAL line from arm-zephyr-eabi-size -t: "  text  data  bss  dec  hex  filename".
  read -r CUR_TEXT CUR_DATA CUR_BSS _ < <(arm-zephyr-eabi-size -t \
    vm.o gc.o dispatch.o kernel_boot.o platform.o serial.o udp.o http.o \
    mirror.o oimg_loader.o space.o handles.o | awk 'END { print $1, $2, $3 }')
  rm -f *.o
  CUR_DATA_BSS=$(( CUR_DATA + CUR_BSS ))
  echo "footprint(mcu): text=$CUR_TEXT data+bss=$CUR_DATA_BSS"

  BASELINE="tools/footprint-mcu-baseline.txt"
  if [ "${FOOTPRINT_UPDATE_BASELINE:-0}" = "1" ]; then
    printf 'text %s\ndata_bss %s\n' "$CUR_TEXT" "$CUR_DATA_BSS" > "$BASELINE"
    echo "footprint(mcu): baseline updated -> text=$CUR_TEXT data+bss=$CUR_DATA_BSS"
    exit 0
  fi
  if [ ! -f "$BASELINE" ]; then
    echo "footprint(mcu): FAIL — no baseline; run FOOTPRINT_UPDATE_BASELINE=1 first" >&2
    exit 1; fi
  BASE_TEXT=$(awk '/^text /{print $2}' "$BASELINE")
  BASE_DATA_BSS=$(awk '/^data_bss /{print $2}' "$BASELINE")
  # Corrupt/incomplete baseline: a missing text or data_bss line yields an empty
  # string, which would make the integer -gt tests error (skipped under set -e as an
  # `if` condition) and fall through to a false PASS. Fail loudly instead.
  if [ -z "$BASE_TEXT" ] || [ -z "$BASE_DATA_BSS" ]; then
    echo "footprint(mcu): FAIL — corrupt baseline $BASELINE (missing text/data_bss line); run FOOTPRINT_UPDATE_BASELINE=1 to rebase" >&2
    exit 1; fi
  RC=0
  if [ "$CUR_TEXT" -gt "$BASE_TEXT" ]; then
    echo "footprint(mcu): FAIL text $CUR_TEXT > baseline $BASE_TEXT" >&2; RC=1; fi
  if [ "$CUR_DATA_BSS" -gt "$BASE_DATA_BSS" ]; then
    echo "footprint(mcu): FAIL data+bss $CUR_DATA_BSS > baseline $BASE_DATA_BSS" >&2; RC=1; fi
  [ "$RC" -eq 0 ] && echo "footprint(mcu): PASS (text<=$BASE_TEXT data+bss<=$BASE_DATA_BSS)"
  exit "$RC"
fi

CC=${CC:-gcc}; TMP=$(mktemp -d)
$CC -Os -std=c99 -Isrc -c \
  src/vm.c src/dispatch.c src/gc.c src/kernel_boot.c src/platform.c \
  src/serial.c src/udp.c src/http.c src/mirror.c src/oimg_loader.c src/space.c \
  src/backend/posix/platform_posix.c src/backend/posix/serial_posix.c \
  src/backend/posix/udp_posix.c src/backend/posix/http_posix.c src/backend/posix/timer_posix.c
size -t vm.o dispatch.o gc.o kernel_boot.o platform.o serial.o udp.o http.o \
        mirror.o oimg_loader.o space.o platform_posix.o serial_posix.o udp_posix.o http_posix.o
# RAM regression is a gate failure like a text regression (design addendum A2).
# data+bss total from the same `size` output; these are host-proxy builds, so this
# is the host proxy for the MCU static-RAM figure (same caveat as the text total
# above — gc.o's host arena dwarfs the real MCU heap and is not representative).
RAM=$(size -t vm.o dispatch.o gc.o kernel_boot.o platform.o serial.o udp.o http.o \
        mirror.o oimg_loader.o space.o platform_posix.o serial_posix.o udp_posix.o http_posix.o \
      | awk 'END { print $2 + $3 }')
echo "data+bss (host proxy for MCU static RAM): $RAM"
rm -f *.o; rmdir "$TMP" 2>/dev/null || true
