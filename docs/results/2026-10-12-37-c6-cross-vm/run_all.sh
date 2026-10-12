#!/usr/bin/env bash
# run_all.sh — rete#37 cross-VM summary on c6spare: nsl (rete 37-dispatch-head-nofwd), Toit v2.0.0-alpha.194,
# MicroPython v1.29.0; STIC, Richards, DeltaBlue. Run under `newgrp uucp`. S = the scratch dir holding the images.
set -u
S=/tmp/claude-1000/-home-david-workspace-rete/c5dabc79-5acf-4ea5-88fb-f294bc120f3b/scratchpad/c6sum; D=/home/david/workspace/rete/docs/results/2026-10-12-37-c6-cross-vm; PY=$S/venv/bin/python; export ESPTOOL=$S/venv/bin/esptool
cd $D
echo "[c6] nsl"
$PY c6run.py logs/nsl-perf-1.log done 0x0 $S/c6-nsl-perf.bin 0x220000 $S/c6-nsl-perf.oimg; echo rc $?
$PY c6run.py logs/nsl-perf-2.log done; echo rc $?
$PY c6run.py logs/nsl-perf-3.log done; echo rc $?
for p in richards deltablue; do $PY c6run.py logs/nsl-$p.log done 0x0 $S/c6-nsl-$p.bin 0x220000 $S/c6-nsl-$p.oimg; echo rc $?; done
echo "[c6] toit"
for p in performance richards deltablue; do
  m=done; [ $p = performance ] && m=hanoi
  $S/venv/bin/esptool --chip esp32c6 --port /dev/ttyUSB1 erase-flash > logs/toit-$p.erase 2>&1
  $S/toit/bin/toit tool firmware -e $S/tt-$p.envelope flash --port /dev/ttyUSB1 --baud 921600 > logs/toit-$p.flash 2>&1; echo flash rc $?
  $PY c6run.py logs/toit-$p.log $m; echo rc $?
done
echo "[c6] micropython"
$S/venv/bin/esptool --chip esp32c6 --port /dev/ttyUSB1 erase-flash > logs/mpy.erase 2>&1
$S/venv/bin/esptool --chip esp32c6 --port /dev/ttyUSB1 --baud 921600 write-flash 0x0 $S/ESP32_GENERIC_C6-20260824-v1.29.0.bin > logs/mpy.flash 2>&1; echo flash rc $?
sleep 5
for p in performance richards deltablue; do
  timeout 3600 $S/venv/bin/mpremote connect /dev/ttyUSB1 run $S/mpy-$p.py > logs/mpy-$p.log 2>&1; echo rc $?
done
echo "[c6] all done"
