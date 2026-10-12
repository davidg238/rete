#!/usr/bin/env bash
# rete#37 L1/L2 on the boards, round 2 (round 1 showed the WeAct STIC 3 % slower at L1+L2): four revisions,
# base f6f6e472, l1 315d93da, l2a 45bfc754 (gcc chooses the inline), l2 66372f9f (always inlined), built on msi-7d43
# by ~/37sf-build2.sh. WeAct: STIC, run_412.sh, three runs each. c6spare: AWFY without Storage (awfy-board.nsl) and
# STIC, 256 KB arena, two boots each, revisions interleaved, every boot reflashed and capped at 10 min.
# Usage: board_run2.sh IMAGES_DIR PYTHON (PYTHON: one with pyserial, esptool beside it). Run from opti7040.
set -u
S=$1 PY=$2
D=$(cd "$(dirname "$0")" && pwd)
export ESPTOOL=$(dirname "$PY")/esptool
mkdir -p "$D/board2/c6" "$D/board2/weact"
( cd "$D"
  for n in 1 2; do for t in base l1 l2a l2; do for p in awfy-board performance; do
    echo "timeout 600 $PY c6run.py board2/c6/$t-$p-$n.log done 0x0 $S/c6-$t-$p.bin 0x220000 $S/c6-$t-$p.oimg" | newgrp uucp
    echo "[c6] $t $p $n rc $?"
  done; done; done ) > "$D/board2/c6.driver.log" 2>&1 &
( for t in base l1 l2a l2; do
    bash "$D/../../spikes/2026-10-10-296-image-in-sram/run_412.sh" "$S/weact-$t.uf2" "$t" "$D/board2/weact"
  done ) > "$D/board2/weact.driver.log" 2>&1 &
wait
echo "[board2] all done"
