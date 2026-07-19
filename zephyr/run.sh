#!/usr/bin/env bash
# zephyr/run.sh — the ESP32 dev loop: bake -> build -> flash -> capture.
# Owns every step that has a wrong-by-default: cwd, the five env exports, the
# compiler path, the build dir, and restoring the tracked solidified_image.c.
set -euo pipefail

MODEL=B NODE=TimerNode.nsl DO_FLASH=0 CAPTURE=0 PRISTINE=0 BUILD_DIR=""
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
# Pin the flash port. esptool with no --port auto-detects the FIRST ESP32 it
# finds — which clobbered the /dev/ttyUSB1 jag/Toit board on 2026-07-16. The
# tuvm devkit is /dev/ttyUSB0 (prj_esp32.conf console). Override with
# ESPTOOL_PORT=... when the enumeration differs.
ESPTOOL_PORT="${ESPTOOL_PORT:-/dev/ttyUSB0}"

[ -f "$ZDIR/$NODE" ] || { echo "no such node: $ZDIR/$NODE" >&2; exit 2; }
[ -f "$NSL" ]        || { echo "compiler not found: $NSL" >&2; exit 2; }

# The seven load-bearing knobs. -p always discards the cache, so always export.
# OEVM_HEAP_SIZE default 53248 (52 KB): prj_esp32.conf now unconditionally pulls in
# WiFi (#46) whose ESP driver heap (~40 KB) + the TCP net-buf pool leave no room for
# the old 64 KB VM arena on this 192 KB DRAM part — at 65536 the image overflows
# DRAM at link. 53248 is the most the DRAM allows AND about the most the HttpServerNode
# needs (live-heap high-water ~52.8 KB observed via /stats — the node runs at the edge
# of both DRAM and VM heap; see review/model-capability-findings.md). A caller can
# override (e.g. a heavier compute node on a future non-WiFi esp32 config):
#   OEVM_HEAP_SIZE=65536 bash run.sh --node SomeNode.nsl
: "${OEVM_HEAP_SIZE:=53248}"
export OEVM_HEAP_SIZE METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256 \
       VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512
# The float-arena pair is NOT optional, despite every prior doc listing only five.
# gc.c's boxed-Float slab defaults to 4096 cells; struct Float is 16 B on Xtensa
# (8-byte double alignment), so one slab is ~64.5 KB against a ~60 KB libc heap —
# the FIRST float box aborts with "VM Float Arena Error: Out of Memory". A node
# that never boxes a float (TimerNode) never notices, which is why five looked
# sufficient. Values are ns-zephyr's, proven on nRF52840: see that repo's
# docs/ZEPHYR_NODE_GUIDE.md §3 and docs/results.md M3 (perfstex, 2026-06-28) —
# same bug, found and fixed there first. zephyr/CMakeLists.txt already forwards
# both; only the export was missing.
export OEVM_FLOAT_ARENA_SLAB_CELLS=512 OEVM_FLOAT_ARENA_SLAB_BUDGET=4

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
BLOB_ARG=()
if [ "$MODEL" = A ]; then
  # Bakes the program into src/solidified_image.c. Beware: cc1 on the result
  # needs ~4.9 GB RSS and ~50 min, and HAS OOM-killed the desktop (2026-07-16).
  # Model B exists to delete this step.
  python3 "$NSL" "$ZDIR/$NODE" "$ROOT/src/solidified_image.c"
else
  LAYOUT="$ZDIR/layout-esp32.json"
  [ -f "$LAYOUT" ] || { echo "missing $LAYOUT — run the layoutdump app (stage 0)" >&2; exit 2; }
  OIMG="$ZDIR/${NODE%.nsl}.oimg"
  python3 "$NSL" "$ZDIR/$NODE" "$OIMG" --format=oimg --layout="$LAYOUT"
  echo "[blob]  $(stat -c %s "$OIMG") bytes -> .incbin"
  BLOB_ARG=(-DTUVM_BLOB="$OIMG")
fi

PRISTINE_ARG=(); [ "$PRISTINE" = 1 ] && PRISTINE_ARG=(-p always)
# WiFi credentials overlay (#46): git-ignored, generated once with the real
# SSID/PSK (never tracked). Appended only when present so a checkout without
# it (or a future non-WiFi board) builds unaffected.
WIFI_CONF="$ZDIR/prj_esp32_wifi.local.conf"
EXTRA_CONF_ARG=()
if [ -f "$WIFI_CONF" ]; then
  EXTRA_CONF_ARG=(-DEXTRA_CONF_FILE=prj_esp32_wifi.local.conf)
  echo "[wifi]  overlay found -> $WIFI_CONF"
fi
# ROM setjmp/longjmp/ungetc fault under Zephyr+picolibc (same forced-ROM-newlib
# family as fflush); patch the vendored SDK ld to use picolibc's. Idempotent; a
# west re-fetch reverts the SDK, so re-apply every build. Zero-SDK-touch: tuvm #48.
"$ROOT/tools/patch-esp32-rom-ld.sh"

echo "[build] -b $BOARD -d $BUILD_DIR ${PRISTINE_ARG[*]:-(incremental)}"
( cd "$ZDIR" && west build -b "$BOARD" "${PRISTINE_ARG[@]}" -d "$BUILD_DIR" . \
    -- -DCONF_FILE=prj_esp32.conf -DTUVM_MODEL="$MODEL" "${BLOB_ARG[@]}" \
       "${EXTRA_CONF_ARG[@]}" )

if [ "$DO_FLASH" = 1 ]; then
  echo "[flash] esptool 0x1000"
  # NOT `west flash` — that would rebuild.
  "$ESPTOOL" --chip esp32 --port "$ESPTOOL_PORT" --baud 921600 write_flash \
    --flash_mode dio --flash_freq 40m --flash_size 4MB 0x1000 \
    "$ZDIR/$BUILD_DIR/zephyr/zephyr.bin"
fi

if [ "${CAPTURE:-0}" != 0 ]; then
  echo "[watch] ${CAPTURE}s"
  python3 "$ZDIR/capture_coredump.py" "$CAPTURE"
fi
