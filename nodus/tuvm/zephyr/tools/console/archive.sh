#!/usr/bin/env bash
# Archive the ELF that was just flashed, keyed by its build id (#56 D4/§5.3).
# Called from run.sh as part of --flash, never as a separate step: an archive
# you have to remember to update is an archive that is wrong when it matters.
# Usage: archive.sh <build-dir> <board>
set -euo pipefail
BUILD_DIR="$1"; BOARD="$2"
ROOT="${RETE_ELF_ROOT:-$HOME/.local/state/rete/elf}"

ID_TXT="$(find "$BUILD_DIR" -name tuvm_build_id.txt -print -quit)"
[ -n "$ID_TXT" ] || { echo "archive: no tuvm_build_id.txt under $BUILD_DIR" >&2; exit 1; }
BUILD_ID="$(cat "$ID_TXT")"

DEST="$ROOT/$BUILD_ID"
mkdir -p "$DEST"
cp "$BUILD_DIR/zephyr/zephyr.elf" "$DEST/zephyr.elf"
sha256sum "$BUILD_DIR/zephyr/zephyr.bin" | awk '{print $1}' > "$DEST/zephyr.bin.sha256"

# A human-facing note only. The reader must NEVER consult this automatically:
# if it did, D4's refusal would be a fallback wearing a disguise, and a board
# flashed twice since the fault would hand back a confident wrong answer.
echo "$BUILD_ID" > "$ROOT/last-flashed-$BOARD"
echo "[archive] $BUILD_ID -> $DEST"
