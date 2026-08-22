#!/usr/bin/env bash
# Engine footprint (host proxy for arm-zephyr-eabi rom_report). Sums `text` of the
# engine objects; the .oimg/solidified image object is EXCLUDED (it scales with the
# program, not the VM). Run before and after the deletion; diff the total text.
set -eu
cd "$(dirname "$0")/.." || exit 2

# FOOTPRINT_CONFIG=mcu: real arm-zephyr-eabi cross build (host proxy above/below
# replaced by the true-target figure). arm-zephyr-eabi-gcc ships INSIDE the Zephyr
# SDK and is not on the default PATH, so prefix the call:
#
#   PATH="$HOME/zephyr-sdk-0.17.4/arm-zephyr-eabi/bin:$PATH" \
#     FOOTPRINT_CONFIG=mcu bash tools/footprint.sh
#
# rete#177: this used to say `source ns-zephyr/tools/env.sh`, a path DELETED by the
# #79 consolidation. So the gate SKIPped, and the one instruction it gave for
# un-SKIPping it could not be followed — which is why "no arm-zephyr-eabi
# toolchain" was recorded as expected in CLAUDE.md and in this gate's own baseline
# header for weeks while the compiler sat in the SDK the whole time. A SKIP whose
# remedy is unfollowable is a gate that has been deleted without saying so.
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
  # rete#177: FIND THE COMPILER RATHER THAN REQUIRE AN INCANTATION. arm-zephyr-eabi-gcc
  # lives in the Zephyr SDK and is never on the default PATH, so for weeks this gate
  # SKIPped on a bench that had the compiler installed — and the SKIP was written down
  # as expected, in CLAUDE.md and in this gate's own baseline header. The old hint
  # pointed at `ns-zephyr/tools/env.sh`, deleted by the #79 consolidation, so following
  # it was impossible. A gate that depends on the operator remembering an env incantation
  # is a gate that reports SKIP forever; one that looks in the SDK it already knows about
  # simply runs. ZEPHYR_SDK_INSTALL_DIR wins when set; otherwise probe the standard
  # location, newest SDK first.
  if ! command -v "$XCC" >/dev/null 2>&1; then
    for _sdk in "${ZEPHYR_SDK_INSTALL_DIR:-}" $(ls -d "$HOME"/zephyr-sdk-* 2>/dev/null | sort -Vr); do
      [ -n "$_sdk" ] && [ -x "$_sdk/arm-zephyr-eabi/bin/$XCC" ] && {
        PATH="$_sdk/arm-zephyr-eabi/bin:$PATH"; export PATH; break; }
    done
  fi
  # SKIP (exit 77) only when the cross toolchain is genuinely absent, so the named gate
  # reports SKIP rather than FAIL on a host with no Zephyr SDK at all.
  command -v "$XCC" >/dev/null 2>&1 || {
    echo "footprint(mcu): SKIP — $XCC not on PATH and no Zephyr SDK found in \$ZEPHYR_SDK_INSTALL_DIR or $HOME/zephyr-sdk-*" >&2
    exit 77; }
  MCPU="-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16"
  # Config-D target sizes (DK values from zephyr/README.md; the real Zephyr app build
  # passes the same via ns-zephyr/app/CMakeLists.txt). Without these the arena/caches
  # keep their 16 MB / 4096-entry HOST defaults, reproducing the tuvm#5/A2 host-proxy
  # RAM defect this gate exists to eliminate. data+bss must reflect the TARGET config.
  # rete#226 deleted the selector prim cache, and PRIM_CACHE_SIZE with it; passing
  # it here defined nothing and read as a live target knob.
  TGT_SIZES="-DOEVM_HEAP_SIZE=49152 -DMETHOD_CACHE_SIZE=256"
  $XCC -Os -std=c99 -Isrc $MCPU -D__ZEPHYR__=1 -DOEVM_NO_FILE_IO $TGT_SIZES -c \
    src/vm.c src/gc.c src/dispatch.c src/kernel_boot.c src/platform.c \
    src/serial.c src/udp.c src/http.c src/http_core.c src/mirror.c src/oimg_loader.c src/space.c src/handles.c \
    src/ota.c src/span.c || {
      echo "footprint(mcu): cross-compile failed" >&2; rm -f *.o; exit 1; }
  # TOTAL line from arm-zephyr-eabi-size -t: "  text  data  bss  dec  hex  filename".
  read -r CUR_TEXT CUR_DATA CUR_BSS _ < <(arm-zephyr-eabi-size -t \
    vm.o gc.o dispatch.o kernel_boot.o platform.o serial.o udp.o http.o http_core.o \
    mirror.o oimg_loader.o space.o handles.o ota.o span.o | awk 'END { print $1, $2, $3 }')
  rm -f *.o
  CUR_DATA_BSS=$(( CUR_DATA + CUR_BSS ))
  echo "footprint(mcu): text=$CUR_TEXT data+bss=$CUR_DATA_BSS"

  # This script MEASURES and BANKS. It does not render a verdict (#143).
  # T0.3 is `review/tests/t0-gates/ratchet.sh`, which applies the spec's +2%
  # tolerance (review/test-suites-spec.md §T0.3) to these same figures. This
  # branch used to ratchet internally at ZERO tolerance, so the two gates
  # disagreed: mcu data+bss = 69048 failed here and passed there. One number,
  # one tolerance, one script that decides — this one no longer decides.
  BASELINE="tools/footprint-mcu-baseline.txt"
  if [ "${FOOTPRINT_UPDATE_BASELINE:-0}" = "1" ]; then
    printf 'text %s\ndata_bss %s\n' "$CUR_TEXT" "$CUR_DATA_BSS" > "$BASELINE"
    echo "footprint(mcu): baseline updated -> text=$CUR_TEXT data+bss=$CUR_DATA_BSS"
  fi
  exit 0
fi

CC=${CC:-gcc}; TMP=$(mktemp -d)
# rete#177: THREE TUs were missing from what this config WEIGHS, in two different
# ways, and both ways read as a complete measurement:
#   ota.c, span.c   compiled by neither list — the gap rete#177 item 2 names. They
#                   are 3108 B of arm-zephyr-eabi text between them, and the whole
#                   #25 ota seam sat outside the ratchet that was supposed to bound it.
#   handles.c       compiled by the MCU config and not by this one, so the two
#                   configs disagreed about what "the engine" is.
#   timer_posix.o   COMPILED here and then left out of the `size -t` list below,
#                   which is the worse shape: the TU builds, so nothing errors, and
#                   its text is silently unweighed.
$CC -Os -std=c99 -Isrc -c \
  src/vm.c src/dispatch.c src/gc.c src/kernel_boot.c src/platform.c \
  src/serial.c src/udp.c src/http.c src/http_core.c src/mirror.c src/oimg_loader.c src/space.c \
  src/handles.c src/ota.c src/span.c \
  src/backend/posix/platform_posix.c src/backend/posix/serial_posix.c \
  src/backend/posix/udp_posix.c src/backend/posix/http_posix.c src/backend/posix/timer_posix.c
size -t vm.o dispatch.o gc.o kernel_boot.o platform.o serial.o udp.o http.o http_core.o \
        mirror.o oimg_loader.o space.o handles.o ota.o span.o \
        platform_posix.o serial_posix.o udp_posix.o http_posix.o timer_posix.o
# RAM regression is a gate failure like a text regression (design addendum A2).
# data+bss total from the same `size` output; these are host-proxy builds, so this
# is the host proxy for the MCU static-RAM figure (same caveat as the text total
# above — gc.o's host arena dwarfs the real MCU heap and is not representative).
RAM=$(size -t vm.o dispatch.o gc.o kernel_boot.o platform.o serial.o udp.o http.o http_core.o \
        mirror.o oimg_loader.o space.o handles.o ota.o span.o \
        platform_posix.o serial_posix.o udp_posix.o http_posix.o timer_posix.o \
      | awk 'END { print $2 + $3 }')
echo "data+bss (host proxy for MCU static RAM): $RAM"
rm -f *.o; rmdir "$TMP" 2>/dev/null || true
