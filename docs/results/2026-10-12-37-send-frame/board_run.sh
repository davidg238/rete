#!/usr/bin/env bash
# rete#37 L1/L2 on the boards: base (f6f6e472) against L1+L2 (66372f9f), built on msi-7d43 by ~/37sf-build.sh
# (c6: run.sh --board esp32c6, awfy.nsl and performance.nsl; WeAct: rpi_pico2 STIC with the #296 console harness).
# c6spare: AWFY (board counts) and STIC, three boots each, base and l2 interleaved, every boot reflashed (c6run.py).
# WeAct RP2350B: STIC through run_412.sh, three runs per image, in parallel with the C6. Run from opti7040.
# Usage: board_run.sh IMAGES_DIR PYTHON [c6|weact|both] (PYTHON: one with pyserial, esptool beside it)
set -u
S=$1 PY=$2 WHICH=${3:-both}
D=$(cd "$(dirname "$0")" && pwd)
export ESPTOOL=$(dirname "$PY")/esptool
mkdir -p "$D/board/c6" "$D/board/weact"
[ "$WHICH" = weact ] || ( cd "$D"
  for n in 1 2 3; do for t in base l2; do for p in awfy performance; do
    echo "$PY c6run.py board/c6/$t-$p-$n.log done 0x0 $S/c6-$t-$p.bin 0x220000 $S/c6-$t-$p.oimg" | newgrp uucp
    echo "[c6] $t $p $n rc $?"
  done; done; done ) > "$D/board/c6.driver.log" 2>&1 &
[ "$WHICH" = c6 ] || ( for t in base l2; do
    bash "$D/../../spikes/2026-10-10-296-image-in-sram/run_412.sh" "$S/weact-$t.uf2" "$t" "$D/board/weact"
  done ) > "$D/board/weact.driver.log" 2>&1 &
wait
echo "[board] all done"
