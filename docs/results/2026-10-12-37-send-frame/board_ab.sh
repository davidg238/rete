#!/usr/bin/env bash
# rete#37 board A/B, the run half: run on opti7040, with the images board_build.sh made (copied into IMAGES).
# Every board runs every TAG's AWFY (board profile) and STIC once, the TAGs interleaved; boards run in parallel.
# One run per image is enough: both boards repeat to within 0.1 % (c6spare pass 2 / pass 1: 0.999 AWFY, 1.000 STIC;
# two WeAct chips agree to the millisecond). A C6 boot that faults ends at once (c6run.py exit 3).
# Usage: board_ab.sh IMAGES OUT PYTHON TAG...   (PYTHON: pyserial, esptool beside it)
#   env C6S    C6 consoles, e.g. "c6spare=/dev/ttyUSB1 c6b=/dev/ttyUSB0"
#       WEACTS WeAct USB ports, e.g. "1-5 1-7" (each found in BOOTSEL, or parked by the harness)
set -u
S=$1 OUT=$2 PY=$3; shift 3; TAGS=$*
D=$(cd "$(dirname "$0")" && pwd)
R412=$D/../../spikes/2026-10-10-296-image-in-sram/run_412.sh
export ESPTOOL=$(dirname "$PY")/esptool
mkdir -p "$OUT"
for c in ${C6S:-}; do
  name=${c%%=*} port=${c#*=}; mkdir -p "$OUT/$name"
  ( for t in $TAGS; do for p in awfy performance; do
      echo "C6_PORT=$port C6_TIMEOUT=600 $PY $D/c6run.py $OUT/$name/$t-$p.log done 0x0 $S/c6-$t-$p.bin 0x220000 $S/c6-$t-$p.oimg" | newgrp uucp
      echo "[$name] $t $p rc $?"
    done; done ) > "$OUT/$name.driver.log" 2>&1 &
done
for u in ${WEACTS:-}; do
  name=weact-$u port=/dev/serial/by-path/pci-0000:00:14.0-usb-0:${u#1-}:1.0; mkdir -p "$OUT/$name"
  ( echo "python3 $D/weact_bootsel.py $u $port $OUT/$name/bootsel.log" | newgrp uucp || { echo "[$name] not in BOOTSEL"; exit 1; }
    for t in $TAGS; do for p in awfy performance; do
      RUNS=1 WEACT_USB=$u WEACT_PORT=$port bash "$R412" "$S/weact-$t-$p.uf2" "$t-$p" "$OUT/$name"
      echo "[$name] $t $p rc $?"
    done; done ) > "$OUT/$name.driver.log" 2>&1 &
done
wait
echo "[board_ab] all done"
