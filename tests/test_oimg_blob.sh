#!/usr/bin/env bash
# Gate: oimg_load_blob (fread, no mmap) produces identical output to oimg_load
# (mmap) for the same .oimg. Proves the S0 extraction is byte-acquisition-only.
set -euo pipefail
cd "$(dirname "$0")/.."          # oevm repo root

PROG="${1:-../ns-tests/01_expressions/01_addition.ns}"
OIMG=/tmp/test_oimg_blob.oimg
python3 ../ns-oevm/tools/ns_compile_oevm.py "$PROG" "$OIMG" \
  --format=oimg --layout=/tmp/oevm_layout.json

BACKEND="src/backend/posix"
COMMON="src/vm.c src/gc.c src/dispatch.c src/platform.c src/space.c src/handles.c src/serial.c src/udp.c \
src/mirror.c $BACKEND/platform_posix.c $BACKEND/serial_posix.c $BACKEND/udp_posix.c $BACKEND/pulse_posix.c \
src/oimg_loader.c src/kernel_boot.c"

# Canonical mmap path.
gcc -std=c99 -Isrc $COMMON oevm_image_main.c -o /tmp/oevm_img_mmap -lm
# Blob path: a fread-based main exercising oimg_load_blob.
cat > /tmp/oevm_img_blob_main.c <<'EOF'
#include "oevm.h"
#include <stdio.h>
#include <stdlib.h>
int  oimg_load_blob(const uint8_t *base, size_t size);
oop  oimg_root(void);
oop  oimg_bootstrap(oop entry_vtable);
extern void kernel_boot(void);
extern void gc_set_stack_base(void *p);
int main(int argc, char **argv) {
    int anchor; gc_set_stack_base(&anchor);
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb"); if (!f) return 1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) return 1;
    fclose(f);
    kernel_boot();
    if (oimg_load_blob(buf, (size_t)sz) != 0) { fprintf(stderr, "load_blob failed\n"); return 1; }
    oimg_bootstrap(oimg_root());
    return 0;
}
EOF
gcc -std=c99 -Isrc $COMMON /tmp/oevm_img_blob_main.c -o /tmp/oevm_img_blob -lm

OUT_MMAP=$(/tmp/oevm_img_mmap "$OIMG")
OUT_BLOB=$(/tmp/oevm_img_blob "$OIMG")
if [ "$OUT_MMAP" != "$OUT_BLOB" ]; then
  echo "FAIL: mmap='$OUT_MMAP' blob='$OUT_BLOB'"; exit 1
fi
echo "PASS: oimg_load_blob matches oimg_load ('$OUT_BLOB')"
