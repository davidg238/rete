#!/usr/bin/env bash
# #163 — build and run the Cbor>>encode: shape bench.
#
# Builds its OWN VM binary and layout under /tmp/cbor-encode-shape/ rather than
# the suite's /tmp/oevm_img and /tmp/oevm_layout.json: the nsl suite has no lock
# (#167) and a bench that clobbered those paths would corrupt a concurrent suite
# run and read as a code regression.
#
# ONE binary carries both arms. The profiler is BUILT IN (-DOEVM_METHOD_PROFILE)
# and OFF at runtime until `platform profileStart`, so the timing rows and the
# activation counts share a code layout — the cross-binary roulette the benchmark
# discipline warns about (and which the #164 spike walked into) cannot happen here.
#
# OEVM_TIMER_WALLCLOCK=1 is REQUIRED: without it the host span clock is the
# virtual clock scaled to microseconds, which advances only via advanceMillis:,
# and every row would read 0 ns.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
OEVM="$REPO/nodus/tuvm"
OUT="${OUT:-/tmp/cbor-encode-shape}"
mkdir -p "$OUT"

cd "$OEVM"

echo "== layout =="
gcc -std=c99 -Isrc tools/dump_layout.c -o "$OUT/dump_layout"
"$OUT/dump_layout" > "$OUT/layout.json"

# The src/*.c list is run_tests_oevm.py's, minus nothing; the BACKEND is globbed
# because src/backend/posix/ is exactly the set of *_backend.h impls and a glob
# cannot fall behind a new seam (the "one new .c file means editing NINE lists"
# tax). src/*.c is NOT globbed: solidified_image.c defines the same static
# Vtable_* symbols as kernel_boot.c and the two cannot link together.
echo "== image VM (with -DOEVM_METHOD_PROFILE) =="
gcc -Wall -std=c99 -Isrc -O2 \
    src/vm.c src/gc.c src/dispatch.c src/platform.c src/space.c src/handles.c \
    src/span.c src/serial.c src/udp.c src/http.c src/http_core.c src/mirror.c \
    src/ota.c \
    src/backend/posix/*.c \
    src/oimg_loader.c src/kernel_boot.c oevm_image_main.c \
    -o "$OUT/oevm_img" -DOEVM_METHOD_PROFILE -lm

echo "== compile CborEncodeShape.nsl =="
python3 "$REPO/nodus/nsl/tools/ns_compile_oevm.py" \
    "$HERE/CborEncodeShape.nsl" "$OUT/CborEncodeShape.oimg" \
    --format=oimg --layout="$OUT/layout.json"

echo "== run =="
OEVM_TIMER_WALLCLOCK=1 "$OUT/oevm_img" "$OUT/CborEncodeShape.oimg" \
    > "$OUT/stdout.txt" 2> "$OUT/stderr.txt" || true

echo "== resolve mprof offsets against the sidecar =="
python3 "$HERE/resolve.py" "$OUT/stdout.txt" "$OUT/stderr.txt" \
    "$OUT/CborEncodeShape.oimg.methods.json" | tee "$OUT/report.txt"

echo
echo "artifacts in $OUT"
