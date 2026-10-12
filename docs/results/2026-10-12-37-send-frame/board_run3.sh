#!/usr/bin/env bash
# rete#37 L1/L2 on the boards, round 3: replication on second chips, three lanes in parallel (images from msi-7d43,
# ~/37sf-build2.sh plus the WeAct AWFY build below). Revisions: base f6f6e472, l1 315d93da, l2a 45bfc754, l2 66372f9f.
#  - c6b (second ESP32-C6, CP2102N ac2dce6f…, /dev/ttyUSB0): AWFY without Storage + STIC, 256 KB arena, one pass.
#  - WeAct on usb 1-7: STIC, three runs per revision (run_412.sh).
#  - WeAct on usb 1-5: AWFY without Storage and Json (awfy-weact.nsl), 256 KB arena, the harness's unused SRAM blob
#    buffer cut from 192 KB to 4 KB so it links; three runs per revision.
# Usage: board_run3.sh IMAGES_DIR PYTHON (PYTHON: one with pyserial, esptool beside it). Run from opti7040.
set -u
S=$1 PY=$2
D=$(cd "$(dirname "$0")" && pwd)
R412=$D/../../spikes/2026-10-10-296-image-in-sram/run_412.sh
export ESPTOOL=$(dirname "$PY")/esptool
mkdir -p "$D/board3/c6b" "$D/board3/weact7" "$D/board3/weact5-awfy"
( cd "$D"
  for t in base l1 l2a l2; do for p in awfy-board performance; do
    echo "C6_PORT=/dev/ttyUSB0 timeout 600 $PY c6run.py board3/c6b/$t-$p-1.log done 0x0 $S/c6-$t-$p.bin 0x220000 $S/c6-$t-$p.oimg" | newgrp uucp
    echo "[c6b] $t $p rc $?"
  done; done ) > "$D/board3/c6b.driver.log" 2>&1 &
( for t in base l1 l2a l2; do
    WEACT_USB=1-7 WEACT_PORT=/dev/serial/by-path/pci-0000:00:14.0-usb-0:7:1.0 bash "$R412" "$S/weact-$t.uf2" "$t" "$D/board3/weact7"
  done ) > "$D/board3/weact7.driver.log" 2>&1 &
( for t in base l1 l2a l2; do
    WEACT_USB=1-5 WEACT_PORT=/dev/serial/by-path/pci-0000:00:14.0-usb-0:5:1.0 bash "$R412" "$S/weact-awfy256-$t.uf2" "$t" "$D/board3/weact5-awfy"
  done ) > "$D/board3/weact5-awfy.driver.log" 2>&1 &
wait
echo "[board3] all done"
