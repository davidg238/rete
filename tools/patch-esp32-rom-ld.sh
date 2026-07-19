#!/usr/bin/env bash
# tools/patch-esp32-rom-ld.sh — make the ESP32 build use picolibc's
# setjmp/longjmp/ungetc instead of the ROM ones.
#
# WHY: esp32.rom.newlib-funcs.ld STRONG-assigns setjmp/longjmp/ungetc to ROM
# addresses (deliberately — see that file's header: "so that ROM functions are
# always used instead of libc.a"). Under Zephyr + picolibc those ROM functions
# FAULT:
#   - ROM setjmp faults in vm_protected_fire() on EVERY nsl reaction fire (the
#     same forced-ROM-newlib family as the ROM fflush fault documented in
#     zephyr/prj_esp32.conf / platform_zephyr.c).
#   - ROM ungetc is why picolibc sscanf/vfscanf faulted (tuvm #46 hand-rolled
#     http_parse_reqline() to avoid sscanf on the request path, but ungetc could
#     still bite any other formatted-input path).
# Commenting these three strong assignments out lets the linker resolve them from
# picolibc's libc.a instead. Confirmed on hardware: HttpServerNode then serves
# /thermal over WiFi with no reaction-fire crash (tuvm #46).
#
# This edits the VENDORED west SDK (~/zephyr-esp32), so it is reverted by a
# `west update`/re-fetch — RE-RUN this script after one. run.sh calls it before
# `west build`. Idempotent: re-running on an already-patched (or pristine) tree is
# safe and a no-op. Zero-SDK-touch alternative tracked in tuvm #48.
#
# Usage: tools/patch-esp32-rom-ld.sh [/path/to/zephyr-esp32]   (default ~/zephyr-esp32)
set -euo pipefail

ESP_ROOT="${1:-$HOME/zephyr-esp32}"
LD="$ESP_ROOT/modules/hal/espressif/components/esp_rom/esp32/ld/esp32.rom.newlib-funcs.ld"

if [ ! -f "$LD" ]; then
  echo "patch-esp32-rom-ld: not found: $LD" >&2
  echo "  (pass the zephyr-esp32 workspace root as \$1 if it lives elsewhere)" >&2
  exit 1
fi

patched=0
for sym in setjmp longjmp ungetc; do
  # Match a live strong assignment at line start: `<sym> = 0x...;`
  if grep -qE "^${sym} = 0x[0-9a-fA-F]+;" "$LD"; then
    # Comment it out, preserving the original for provenance.
    sed -i -E "s|^(${sym} = 0x[0-9a-fA-F]+;)|/* \1  tuvm #46: use picolibc, ROM version faults under Zephyr */|" "$LD"
    echo "patch-esp32-rom-ld: commented ROM ${sym}"
    patched=1
  fi
done

if [ "$patched" = 0 ]; then
  echo "patch-esp32-rom-ld: already patched (setjmp/longjmp/ungetc not ROM-assigned) — no-op"
fi
