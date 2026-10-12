#!/usr/bin/env bash
# rete#37 heap census: the smallest arena (2 KB steps) in which an AWFY workload verifies, -m32 (board layout).
# Usage: minarena.sh REV TAG   (on msi). Workloads: board profile, Storage alone, Json alone. Out: ~/census/TAG-min.txt
set -euo pipefail
REV=$1; TAG=$2
W=~/census/min-$TAG; rm -rf "$W"; mkdir -p "$W/o"
git -C ~/workspace/rete archive "$REV" nodus/oevm nodus/nsl nodus/tests/00_perf | tar -x -C "$W"
ln -sfn ~/workspace/rete/nodus/nsl/grammar/node_modules "$W/nodus/nsl/grammar/node_modules"
P=$W/nodus/tests/00_perf
python3 - "$P" "$W" <<'EOF'
import sys; sys.path.insert(0, sys.argv[1]); import awfy_counts as a
src = open(sys.argv[1] + "/awfy.nsl").read()
rows = ["Bounce","List","Permute","Storage","Richards","Queens","Sieve","Towers","Mandelbrot","NBody","CD","DeltaBlue","Json"]
only = lambda r: {k: None for k in rows if k != r}
for name, prof in [("board", a.PROFILES["board"]), ("storage", only("Storage")), ("json", only("Json"))]:
    open(f"{sys.argv[2]}/{name}.nsl", "w").write(a.apply(src, prof))
EOF
B=posix
cd "$W/nodus/oevm"
gcc -m32 -std=c99 -Isrc tools/dump_layout.c -o "$W/dump_layout"; "$W/dump_layout" > "$W/layout.json"
CF="-m32 -O2 -DOEVM_NO_DEBUG_HOOKS -std=c99 -Isrc"
for f in src/vm.c src/dispatch.c src/platform.c src/space.c src/handles.c src/span.c src/serial.c \
    src/udp.c src/http.c src/http_core.c src/mirror.c src/ota.c \
    src/backend/$B/platform_$B.c src/backend/$B/serial_$B.c src/backend/$B/udp_$B.c \
    src/backend/$B/pulse_$B.c src/backend/$B/timer_$B.c src/backend/$B/span_$B.c src/backend/$B/http_$B.c \
    src/backend/$B/sensor_$B.c src/backend/$B/ota_$B.c src/oimg_loader.c src/kernel_boot.c oevm_image_main.c; do
  gcc $CF -c "$f" -o "$W/o/$(basename "$f" .c).o" 2>/dev/null
done
for prog in board storage json; do
  python3 "$W/nodus/nsl/tools/ns_compile_oevm.py" "$W/$prog.nsl" "$W/$prog.oimg" --format=oimg --layout="$W/layout.json" > /dev/null
done
fits() {  # $1 KB, $2 prog
  gcc $CF -DOEVM_HEAP_SIZE=$(( $1 * 1024 )) -c src/gc.c -o "$W/gc.o" 2>/dev/null
  gcc -m32 "$W"/o/*.o "$W/gc.o" -o "$W/vm" -lm
  timeout 600 "$W/vm" "$W/$2.oimg" 2>&1 | grep -q '^verified: true'
}
: > ~/census/$TAG-min.txt
for prog in board storage json; do
  lo=16; hi=2048   # lo fails, hi fits (checked)
  fits $hi $prog || { echo "$prog: does not fit ${hi} KB" >> ~/census/$TAG-min.txt; continue; }
  while (( hi - lo > 2 )); do
    mid=$(( (lo + hi) / 2 / 2 * 2 ))
    if fits $mid $prog; then hi=$mid; else lo=$mid; fi
  done
  echo "$prog: ${hi} KB" >> ~/census/$TAG-min.txt
done
cat ~/census/$TAG-min.txt
