#!/usr/bin/env bash
# zephyr/run.sh — the ESP32 dev loop: bake -> build -> flash -> capture.
# Owns every step that has a wrong-by-default: cwd, the five env exports, the
# compiler path, the build dir, and restoring the tracked solidified_image.c.
set -euo pipefail

MODEL=A NODE=TimerNode.ns DO_FLASH=0 CAPTURE=0 PRISTINE=0 BUILD_DIR=""
while [ $# -gt 0 ]; do
  case "$1" in
    --model)     MODEL="$2"; shift 2 ;;
    --node)      NODE="$2"; shift 2 ;;
    --flash)     DO_FLASH=1; shift ;;
    --capture)   CAPTURE="$2"; shift 2 ;;
    --pristine)  PRISTINE=1; shift ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
case "$MODEL" in A|B) ;; *) echo "--model must be A or B" >&2; exit 2 ;; esac

ZDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # zephyr/ — the app dir
ROOT="$(cd "$ZDIR/.." && pwd)"                          # repo root
NSL="$HOME/workspace/nsl-tuvm/tools/ns_compile_oevm.py" # NOT ~/workspace/ns-oevm (stale)
: "${BUILD_DIR:=build-esp32-$MODEL}"
BOARD="esp32_devkitc/esp32/procpu"
ESPTOOL="$HOME/.espressif/python_env/idf5.4_py3.12_env/bin/esptool.py"

[ -f "$ZDIR/$NODE" ] || { echo "no such node: $ZDIR/$NODE" >&2; exit 2; }
[ -f "$NSL" ]        || { echo "compiler not found: $NSL" >&2; exit 2; }

# The five load-bearing knobs. -p always discards the cache, so always export.
export OEVM_HEAP_SIZE=65536 METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256 \
       VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512

# src/solidified_image.c is TRACKED (the MethodCallTest fixture). A Model A bake
# dirties it; restore on exit so it is never committed by accident. The compiler
# also writes a .stmap sidecar beside the image (ns_compile_oevm.py emits
# os.path.splitext(out_c)[0] + ".stmap"), and THAT IS TRACKED TOO — restoring only
# the .c leaves the source map dirty and silently mismatched with the fixture.
restore_image() {
  git -C "$ROOT" checkout -- src/solidified_image.c src/solidified_image.stmap 2>/dev/null || true
}
trap restore_image EXIT

echo "[bake]  $NODE  (model $MODEL)"
if [ "$MODEL" = A ]; then
  python3 "$NSL" "$ZDIR/$NODE" "$ROOT/src/solidified_image.c"
else
  echo "[bake]  model B not wired yet (Task 4)" >&2; exit 3
fi

PRISTINE_ARG=(); [ "$PRISTINE" = 1 ] && PRISTINE_ARG=(-p always)
echo "[build] -b $BOARD -d $BUILD_DIR ${PRISTINE_ARG[*]:-(incremental)}"
( cd "$ZDIR" && west build -b "$BOARD" "${PRISTINE_ARG[@]}" -d "$BUILD_DIR" . \
    -- -DCONF_FILE=prj_esp32.conf -DTUVM_MODEL="$MODEL" )

if [ "$DO_FLASH" = 1 ]; then
  echo "[flash] esptool 0x1000"
  # NOT `west flash` — that would rebuild.
  "$ESPTOOL" --chip esp32 --baud 921600 write_flash --flash_mode dio \
    --flash_freq 40m --flash_size 4MB 0x1000 "$ZDIR/$BUILD_DIR/zephyr/zephyr.bin"
fi

if [ "${CAPTURE:-0}" != 0 ]; then
  echo "[watch] ${CAPTURE}s"
  python3 "$ZDIR/capture_coredump.py" "$CAPTURE"
fi
