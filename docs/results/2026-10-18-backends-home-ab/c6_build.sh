#!/usr/bin/env bash
# c6_build.sh REV TAG OUT — C6 AWFY (board profile) + STIC images for REV: c6-TAG-{awfy,performance}.{bin,oimg}.
# The C6 half of ../2026-10-12-37-send-frame/board_build.sh. env HEAP (default 258048: 256 KB no longer links), TREE.
set -euo pipefail
REV=$1 TAG=$2 OUT=${3:-$HOME/board-ab}
HERE=$(cd "$(dirname "$0")" && pwd)
COUNTS=$HERE/../../../nodus/tests/00_perf/awfy_counts.py
TREE=${TREE:-$HOME/rete-board}
CFLAG=; [ "${COUNT:-}" = 1 ] && { CFLAG=--count; export OEVM_COUNT=1; }
source ~/zephyr-esp32/.venv/bin/activate; export ZEPHYR_BASE=$HOME/zephyr-esp32/zephyr
mkdir -p "$OUT"
[ -d "$TREE" ] || { git -C ~/workspace/rete worktree add -q --detach "$TREE" "$REV"
                    ln -sfn ~/workspace/rete/nodus/nsl/grammar/node_modules "$TREE/nodus/nsl/grammar/node_modules"; }
P=$TREE/nodus/tests/00_perf Z=$TREE/nodus/oevm/zephyr
checkout() { git -C "$TREE" checkout -q -f --detach "$REV"; git -C "$TREE" clean -qfd nodus/tests nodus/oevm/zephyr/src
             python3 "$COUNTS" board $CFLAG "$P/awfy.nsl" > "$P/awfy-board.nsl"; }

checkout
for n in awfy-board performance; do
  ( cd "$Z" && OEVM_HEAP_SIZE=${HEAP:-258048} bash run.sh --board esp32c6 --node ../../tests/00_perf/$n.nsl \
      --build-dir build-ab-c6 --pristine ) > "$OUT/c6-$TAG-$n.build.log" 2>&1 \
    || { echo "c6_build: $n failed (log $OUT/c6-$TAG-$n.build.log)" >&2; exit 1; }
  cp "$Z/build-ab-c6/zephyr/zephyr.bin" "$OUT/c6-$TAG-${n%-board}.bin"; cp "$P/$n.oimg" "$OUT/c6-$TAG-${n%-board}.oimg"
done
echo "c6_build: $TAG ($(git -C "$TREE" rev-parse --short HEAD)) -> $OUT"
