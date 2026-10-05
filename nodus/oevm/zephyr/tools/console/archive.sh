#!/usr/bin/env bash
# Archive the ELF that was just flashed, keyed by its build id (#56 D4/§5.3).
# Called from run.sh as part of --flash, never as a separate step: an archive
# you have to remember to update is an archive that is wrong when it matters.
# Usage: archive.sh <build-dir> <board> [<image>.oimg]
#
# The third argument is the Model B image. Given it, the image map beside it
# (<image>.oimg.map.json: offset -> name, file and lines, rete#337) is archived
# into the same build-id directory. This directory is already a build-id-keyed
# symbol store — the ELF is here for exactly the same reason — so the map is one
# file copy into an existing mechanism, not a second one (#165 final review,
# David's ruling). A profile can then be resolved later BY BUILD ID, off a node
# that has since been reflashed.
#
# Model A bakes have no .oimg, so run.sh passes nothing and the map is
# skipped. When the argument IS given, a MISSING map is a hard failure
# naming the file: archiving less than the caller asked for and saying nothing
# is how a symbol store comes to be trusted for a build it does not hold.
set -euo pipefail
BUILD_DIR="$1"; BOARD="$2"; OIMG="${3:-}"
ROOT="${RETE_ELF_ROOT:-$HOME/.local/state/rete/elf}"

ID_TXT="$(find "$BUILD_DIR" -name oevm_build_id.txt -print -quit)"
[ -n "$ID_TXT" ] || { echo "archive: no oevm_build_id.txt under $BUILD_DIR" >&2; exit 1; }
BUILD_ID="$(cat "$ID_TXT")"

DEST="$ROOT/$BUILD_ID"
mkdir -p "$DEST"
cp "$BUILD_DIR/zephyr/zephyr.elf" "$DEST/zephyr.elf"
sha256sum "$BUILD_DIR/zephyr/zephyr.bin" | awk '{print $1}' > "$DEST/zephyr.bin.sha256"

# rete#243: the manifest pin rides the same build-id entry as the ELF, so the
# provision gate can answer "what contract was this build made against" by
# build id alone. Absent pin artifacts = a pre-manifest build dir: visible,
# not fatal, while those still exist.
PIN_TXT="$(find "$BUILD_DIR" -name oevm_manifest_pin.txt -print -quit)"
if [ -n "$PIN_TXT" ]; then
  PIN_DIR="$(dirname "$PIN_TXT")"
  cp "$PIN_DIR/MANIFEST.pinned.cddl" "$DEST/MANIFEST.pinned.cddl"
  sed -n 1p "$PIN_TXT" > "$DEST/manifest.sha256"
  sed -n 's/^source=//p' "$PIN_TXT" > "$DEST/manifest.source"
  echo "[archive] manifest pin: $(cut -c1-12 "$DEST/manifest.sha256")… (source=$(cat "$DEST/manifest.source"))"
else
  echo "[archive] no manifest pin in this build"
fi

if [ -n "$OIMG" ]; then
  MAP="$OIMG.map.json"
  [ -f "$MAP" ] || { echo "archive: missing $MAP (asked to archive $OIMG's map)" >&2; exit 1; }
  cp "$MAP" "$DEST/$(basename "$MAP")"
  echo "[archive] image map: $(basename "$MAP")"
fi

# A human-facing note only. The reader must NEVER consult this automatically:
# if it did, D4's refusal would be a fallback wearing a disguise, and a board
# flashed twice since the fault would hand back a confident wrong answer.
echo "$BUILD_ID" > "$ROOT/last-flashed-$BOARD"
echo "[archive] $BUILD_ID -> $DEST"
