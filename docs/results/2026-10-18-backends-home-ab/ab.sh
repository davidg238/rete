#!/usr/bin/env bash
# ab.sh REV_A REV_B [OUT] — the speed A/B of one change on c6spare: build both, run, table (tables.md in OUT).
set -euo pipefail
A=$1 B=$2 OUT=${3:-$HOME/board-ab/$(date +%F)-$A-$B}
D=$(cd "$(dirname "$0")" && pwd)
bash "$D/c6_build.sh" "$A" a "$OUT/img"; bash "$D/c6_build.sh" "$B" b "$OUT/img"
bash "$D/run_ab.sh" "$OUT/img" "$OUT/run" a b
python3 "$D/table.py" "$OUT/run" a b | tee "$OUT/tables.md"
