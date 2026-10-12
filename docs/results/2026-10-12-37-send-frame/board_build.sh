#!/usr/bin/env bash
# rete#37 board A/B, the build half: run on msi-7d43 (the Zephyr workspace). For REV, tagged TAG, builds into OUT:
#   c6-TAG-awfy.{bin,oimg}, c6-TAG-performance.{bin,oimg}   esp32c6, 256 KB arena (AWFY's board profile needs it)
#   weact-TAG-awfy.uf2, weact-TAG-performance.uf2           rpi_pico2 with the #296 console harness (HARNESS, a patch
#                                                           kept uncommitted), 256 KB arena, its unused SRAM blob
#                                                           buffer cut to 4 KB so the arena fits (arm=flash only)
# AWFY is nodus/tests/00_perf/awfy_counts.py's board profile from THIS script's tree, applied to REV's awfy.nsl, so
# every revision runs the same rows. TREE is a worktree this script owns (checked out -f, cleaned).
# Usage: board_build.sh REV TAG [OUT]   (env: TREE ~/rete-board, HARNESS ~/stic-harness.patch)
#   env COUNT=1: the counting build (OEVM_COUNT, send/frame design §M) and AWFY with awfy_counts.py --count, so each
#                row prints a `count:` line; its times are not timings. REV must carry OEVM_COUNT.
set -euo pipefail
REV=$1 TAG=$2 OUT=${3:-$HOME/board-ab}
HERE=$(cd "$(dirname "$0")" && pwd)
COUNTS=$HERE/../../../nodus/tests/00_perf/awfy_counts.py
TREE=${TREE:-$HOME/rete-board} HARNESS=${HARNESS:-$HOME/stic-harness.patch}
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
  ( cd "$Z" && OEVM_HEAP_SIZE=262144 bash run.sh --board esp32c6 --node ../../tests/00_perf/$n.nsl \
      --build-dir build-ab-c6 --pristine ) > "$OUT/c6-$TAG-$n.build.log" 2>&1 \
    || { echo "board_build: c6 $n failed (log $OUT/c6-$TAG-$n.build.log)" >&2; exit 1; }
  cp "$Z/build-ab-c6/zephyr/zephyr.bin" "$OUT/c6-$TAG-${n%-board}.bin"; cp "$P/$n.oimg" "$OUT/c6-$TAG-${n%-board}.oimg"
done

checkout
git -C "$TREE" apply "$HARNESS"
sed -i 's|#define OEVM_BLOB_SRAM_CAP (192u \* 1024u)|#define OEVM_BLOB_SRAM_CAP (4u * 1024u)|' "$Z/src/main.c"
grep -q 'BLOB_SRAM_CAP (4u' "$Z/src/main.c" || { echo "board_build: the harness's SRAM cap moved" >&2; exit 1; }
for n in awfy-board performance; do
  ( cd "$Z" && env OEVM_HEAP_SIZE=262144 OEVM_SPIKE_296=1 EXTRA_DTC_OVERLAY_FILE="$Z/usb-console.overlay" \
      bash run.sh --board rpi_pico2 --node ../../tests/00_perf/$n.nsl --conf-extra prj_rpi_pico2_usb.conf \
      --build-dir build-ab-weact --pristine ) > "$OUT/weact-$TAG-$n.build.log" 2>&1 \
    || { echo "board_build: weact $n failed (log $OUT/weact-$TAG-$n.build.log)" >&2; exit 1; }
  cp "$Z/build-ab-weact/zephyr/zephyr.uf2" "$OUT/weact-$TAG-${n%-board}.uf2"
done
echo "board_build: $TAG ($(git -C "$TREE" rev-parse --short HEAD)) -> $OUT"
