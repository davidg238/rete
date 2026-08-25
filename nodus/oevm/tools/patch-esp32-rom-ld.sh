#!/usr/bin/env bash
# tools/patch-esp32-rom-ld.sh — make an Xtensa ESP32 build use picolibc's
# setjmp/longjmp/ungetc instead of the ROM ones.
#
# WHY: <chip>.rom.newlib-funcs.ld STRONG-assigns these to ROM addresses
# (deliberately — see that file's header: "so that ROM functions are always used
# instead of libc.a"). Under Zephyr + picolibc those ROM functions FAULT:
#   - ROM setjmp faults in vm_protected_fire() on EVERY nsl reaction fire (the
#     same forced-ROM-newlib family as the ROM fflush fault documented in
#     zephyr/prj_esp32.conf / platform_zephyr.c).
#   - ROM ungetc is why picolibc sscanf/vfscanf faulted (oevm #46 hand-rolled
#     http_parse_reqline() to avoid sscanf on the request path, but ungetc could
#     still bite any other formatted-input path).
# Commenting the strong assignments out lets the linker resolve them from
# picolibc's libc.a instead. Confirmed on hardware: HttpServerNode then serves
# /thermal over WiFi with no reaction-fire crash (oevm #46).
#
# fflush is on the same forced list and is deliberately NOT patched here: it is
# already neutralised at the source, because platform_console_flush() is a no-op
# on Zephyr (stdout is _IONBF). See zephyr/prj_esp32.conf's libc note.
#
# PER-CHIP SYMBOL SETS. The forced list is not the same on every part, so each
# chip carries its own. On the ESP32-S2 the ld forces setjmp and longjmp but NOT
# ungetc — so a single shared three-symbol list would make the "already patched"
# verification below hard-fail on the second run of an S2 build (ungetc is
# neither ROM-assigned nor patched-by-us there, because it is simply absent).
# Keep these lists EXPLICIT rather than deriving them from what happens to be in
# the file: the whole point of the verification is to notice when the SDK's
# format drifts, and "absent" and "written differently" must stay
# distinguishable.
#
# This edits the VENDORED west SDK (~/zephyr-esp32), so it is reverted by a
# `west update`/re-fetch — RE-RUN this script after one. run.sh calls it before
# `west build`. Idempotent: re-running on an already-patched (or pristine) tree is
# safe and a no-op. Zero-SDK-touch alternative tracked in oevm #48.
#
# Only Xtensa parts need this. The RISC-V parts (C3/C6) have a different ROM
# export table and no such forced assignments — run.sh's esp32c6 profile sets
# ROM_PATCH=0 and never calls this script.
#
# Usage: tools/patch-esp32-rom-ld.sh [--chip esp32|esp32s2] [/path/to/zephyr-esp32]
#   defaults: --chip esp32, /path = ~/zephyr-esp32
set -euo pipefail

CHIP=esp32
ARGS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --chip) CHIP="$2"; shift 2 ;;
    *)      ARGS+=("$1"); shift ;;
  esac
done

case "$CHIP" in
  esp32)   SYMS=(setjmp longjmp ungetc) ;;
  esp32s2) SYMS=(setjmp longjmp) ;;
  *) echo "patch-esp32-rom-ld: unsupported --chip '$CHIP' (expected esp32 or esp32s2)" >&2
     echo "  RISC-V parts (esp32c3/esp32c6) do not need this patch at all." >&2
     exit 2 ;;
esac

ESP_ROOT="${ARGS[0]:-$HOME/zephyr-esp32}"
LD="$ESP_ROOT/modules/hal/espressif/components/esp_rom/$CHIP/ld/$CHIP.rom.newlib-funcs.ld"

if [ ! -f "$LD" ]; then
  echo "patch-esp32-rom-ld: not found: $LD" >&2
  echo "  (pass the zephyr-esp32 workspace root as a positional arg if it lives elsewhere)" >&2
  exit 1
fi

patched=0
for sym in "${SYMS[@]}"; do
  # Match a live strong assignment at line start: `<sym> = 0x...;`
  if grep -qE "^${sym} = 0x[0-9a-fA-F]+;" "$LD"; then
    # Comment it out, preserving the original for provenance.
    sed -i -E "s|^(${sym} = 0x[0-9a-fA-F]+;)|/* \1  oevm #46: use picolibc, ROM version faults under Zephyr */|" "$LD"
    echo "patch-esp32-rom-ld[$CHIP]: commented ROM ${sym}"
    patched=1
  fi
done

if [ "$patched" = 0 ]; then
  # Nothing matched. Distinguish "already patched" from "SDK format drifted and our
  # regex silently missed it" — otherwise a future SDK writing e.g.
  # `PROVIDE(setjmp = 0x...)` would leave the ROM setjmp fault active while this
  # script cheerfully reports success. Require our commented marker to be present.
  for sym in "${SYMS[@]}"; do
    if ! grep -qE "^/\* ${sym} = 0x[0-9a-fA-F]+;" "$LD"; then
      echo "patch-esp32-rom-ld[$CHIP]: ERROR — ${sym} is neither ROM-assigned nor patched by us." >&2
      echo "  The SDK ld format likely changed; update the regex in this script." >&2
      echo "  File: $LD" >&2
      exit 2
    fi
  done
  echo "patch-esp32-rom-ld[$CHIP]: already patched (${SYMS[*]} use picolibc) — no-op"
fi
