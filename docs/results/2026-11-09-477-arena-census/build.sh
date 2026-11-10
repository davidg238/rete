#!/usr/bin/env bash
# build.sh OUTDIR [extra cflags]: -m32 image VM with the #477 census, from master HEAD.
set -euo pipefail
W=$1; shift; rm -rf "$W"; mkdir -p "$W"
git -C ~/workspace/rete archive HEAD nodus/oevm nodus/nsl nodus/tests | tar -x -C "$W"
ln -sfn ~/workspace/rete/nodus/nsl/grammar/node_modules "$W/nodus/nsl/grammar/node_modules"
[ -n "${KERNEL_FROM:-}" ] && cp "$KERNEL_FROM" "$W/nodus/nsl/lib/kernel.nsl"
G=$W/nodus/oevm/src/gc.c
INC=$(dirname "$0")/census.inc
if [ "${SEG:-}" = 1 ]; then python3 "$(dirname "$0")/seg.py" "$G"; fi
# insert the census include just before gc_allocate's definition
awk -v inc="$INC" '/^OEVM_HOT oop gc_allocate\(oop vtable, size_t size\) \{/{while((getline l < inc)>0) print l} {print}' "$G" > "$G.n" && mv "$G.n" "$G"
sed -i 's/^    size_t need = sizeof(struct HeapHeader) + rounded;/&\n    census_alloc(vtable, need);/' "$G"
sed -i 's/            if (hh_marked(h)) { swept_live +=/            if (hh_marked(h)) { census_live(h); swept_live +=/' "$G"
sed -i 's/^    heap_live_bytes = swept_live;/&\n    census_end(swept_live);/' "$G"
[ "$(grep -c '^ *census_alloc(vtable\|{ census_live(h)\|^ *census_end(swept' "$G")" = 3 ] || { echo "patch did not apply"; exit 1; }
sed -i 's/^OEVM_HOT void gc_collect(void) {/&\n    census_gc_begin();/' "$G"
awk '/^    for \(size_t i = 0; i < FREE_BIN_COUNT; i\+\+\) free_bins\[i\] = NULL;/{n++; if (n==2) print "    census_sweep_begin();"} {print}' "$G" > "$G.n" && mv "$G.n" "$G"
sed -i 's/^    float_sweep();/&\n    census_gc_end();/' "$G"
[ "$(grep -c '^ *census_gc_begin();\|^ *census_sweep_begin();\|^ *census_gc_end();' "$G")" = 3 ] || { echo "timing patch did not apply"; exit 1; }
B=posix
sed -i "s/^    kernel_boot();  /    { extern void census_install(void); census_install(); } kernel_boot();  /" "$W/nodus/oevm/oevm_image_main.c"; grep -q census_install "$W/nodus/oevm/oevm_image_main.c" || { echo main patch failed; exit 1; }
cd "$W/nodus/oevm"
gcc -m32 -std=c99 -Isrc tools/dump_layout.c -o "$W/dump_layout"; "$W/dump_layout" > "$W/layout.json"
gcc -m32 -O2 -std=c99 -Isrc "$@" \
  src/vm.c src/gc.c src/dispatch.c src/platform.c src/space.c src/handles.c src/span.c src/serial.c \
  src/udp.c src/http.c src/http_core.c src/mirror.c src/ota.c \
  src/backend/$B/platform_$B.c src/backend/$B/serial_$B.c src/backend/$B/udp_$B.c \
  src/backend/$B/pulse_$B.c src/backend/$B/timer_$B.c src/backend/$B/span_$B.c src/backend/$B/http_$B.c \
  src/backend/$B/sensor_$B.c src/backend/$B/ota_$B.c \
  src/oimg_loader.c src/kernel_boot.c oevm_image_main.c -o "$W/vm" -lm
echo built "$W/vm"
