#!/usr/bin/env bash
# #297 spike — build one host VM, bake CborPrimArms.nsl (Path B), run it.
# Own paths under $OUT so a concurrent nsl suite is not clobbered (#167).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
OUT="${OUT:-/tmp/cbor-prim-arms}"
mkdir -p "$OUT"
cd "$REPO/nodus/oevm"
gcc -std=c99 -Isrc tools/dump_layout.c -o "$OUT/dump_layout"
"$OUT/dump_layout" > "$OUT/layout.json"
gcc -Wall -std=gnu99 -Isrc -O2 \
    src/vm.c src/gc.c src/dispatch.c src/platform.c src/space.c src/handles.c \
    src/span.c src/serial.c src/udp.c src/http.c src/http_core.c src/mirror.c \
    src/ota.c src/backend/posix/*.c \
    src/oimg_loader.c src/kernel_boot.c oevm_image_main.c \
    -o "$OUT/oevm_img" -DOEVM_SPAN_SLOTS=64 ${EXTRA_CFLAGS:-} -lm
python3 "$REPO/nodus/nsl/tools/ns_compile_oevm.py" \
    "$HERE/CborPrimArms.nsl" "$OUT/CborPrimArms.oimg" \
    --format=oimg --layout="$OUT/layout.json"
OEVM_TIMER_WALLCLOCK=1 "$OUT/oevm_img" "$OUT/CborPrimArms.oimg"
