#!/usr/bin/env bash
# zephyr/run.sh — the ESP32 dev loop: bake -> build -> flash -> capture.
# Owns every step that has a wrong-by-default: cwd, the seven env exports
# (see below — five looked sufficient for a long time and were not), the
# per-board profile (target, Kconfig, build dir, layout JSON, esptool triple,
# flash port), the compiler path, the build dir, and restoring the tracked
# solidified_image.c.
set -euo pipefail

MODEL=B NODE=TimerNode.nsl DO_FLASH=0 CAPTURE=0 PRISTINE=0 BUILD_DIR="" BOARD_ID=esp32c6 CONF_EXTRA=""
# Which chip's ROM ld the setjmp/longjmp patch targets. Only meaningful when a
# profile sets ROM_PATCH=1; the esp32 profile leaves this default.
ROM_PATCH_CHIP=esp32
while [ $# -gt 0 ]; do
  case "$1" in
    --model)      MODEL="$2"; shift 2 ;;
    --node)       NODE="$2"; shift 2 ;;
    --flash)      DO_FLASH=1; shift ;;
    --capture)    CAPTURE="$2"; shift 2 ;;
    --pristine)   PRISTINE=1; shift ;;
    --build-dir)  BUILD_DIR="$2"; shift 2 ;;
    --board)      BOARD_ID="$2"; shift 2 ;;
    --conf-extra) CONF_EXTRA="$2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
case "$MODEL" in A|B) ;; *) echo "--model must be A or B" >&2; exit 2 ;; esac
case "$BOARD_ID" in esp32|esp32s2|esp32c6) ;; *) echo "--board must be esp32, esp32s2 or esp32c6" >&2; exit 2 ;; esac

ZDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # zephyr/ — the app dir
ROOT="$(cd "$ZDIR/.." && pwd)"                          # repo root
NSL="$HOME/workspace/nsl-tuvm/tools/ns_compile_oevm.py" # NOT ~/workspace/ns-oevm (stale)
ESPTOOL="$HOME/.espressif/python_env/idf5.4_py3.12_env/bin/esptool.py"

# ---- board profiles -------------------------------------------------------
# Each profile owns EVERY board-coupled wrong-by-default together: the board
# target, Kconfig, build dir, layout JSON, the esptool triple, the port, and
# whether the Xtensa ROM linker patch applies. Adding a board means adding a
# block here and nothing else.
#
# Ports are pinned by /dev/serial/by-id/ rather than ttyUSB0/ttyACM0: those are
# allocation-order names, and the C6's CP210x now claims /dev/ttyUSB0 — the very
# node this script pinned for the WROOM-32 after esptool auto-detect clobbered the
# jag board on 2026-07-16.
#
# By-id globbing is NOT itself a fix, because it is not unique: the bench also
# has a second, identical WROOM-32E (the "jag"/Toit board) with the same
# CP210x bridge, and the C6 devkit has its own CP210x too. `ls ... | head -1`
# over a glob with more than one match picks arbitrarily (by serial-hash sort
# order) — exactly the failure that clobbered the jag board on 2026-07-16, just
# one glob level up. pick_port() below closes that: zero matches falls back to
# the profile's hardcoded default, exactly one match is used, and more than
# one match ABORTS with every match named rather than guessing. An explicit
# ESPTOOL_PORT always wins and skips detection (and the ambiguity check)
# entirely.
pick_port() {
  local pattern="$1" fallback="$2" matches n
  if [ -n "${ESPTOOL_PORT:-}" ]; then
    return 0   # explicit override always wins; no detection, no check
  fi
  matches="$(ls $pattern 2>/dev/null || true)"
  n=0; [ -n "$matches" ] && n=$(printf '%s\n' "$matches" | wc -l)
  if [ "$n" -gt 1 ]; then
    echo "ERROR: more than one device matches '$pattern':" >&2
    printf '%s\n' "$matches" | sed 's/^/  /' >&2
    echo "Refusing to guess which one to flash — head -1 would pick arbitrarily" >&2
    echo "and could overwrite the wrong board (see the 2026-07-16 incident above)." >&2
    echo "Set ESPTOOL_PORT explicitly to the intended device and retry." >&2
    exit 2
  elif [ "$n" -eq 1 ]; then
    ESPTOOL_PORT="$matches"
  else
    ESPTOOL_PORT="$fallback"
  fi
}
case "$BOARD_ID" in
  esp32)
    BOARD="esp32_devkitc/esp32/procpu"
    CONF_FILE="prj_esp32.conf"
    LAYOUT_JSON="$ZDIR/layout-esp32.json"
    ESPTOOL_CHIP="esp32"
    ESPTOOL_OFFSET="0x1000"
    ESPTOOL_FLASH_SIZE="4MB"
    ROM_PATCH=1
    # OEVM_HEAP_SIZE default 53248 (52 KB): prj_esp32.conf now unconditionally pulls in
    # WiFi (#46) whose ESP driver heap (~40 KB) + the TCP net-buf pool leave no room for
    # the old 64 KB VM arena on this 192 KB DRAM part — at 65536 the image overflows
    # DRAM at link. 53248 is the most the DRAM allows AND about the most the HttpServerNode
    # needs (live-heap high-water ~52.8 KB observed via /stats — the node runs at the edge
    # of both DRAM and VM heap; see review/model-capability-findings.md). A caller can
    # override (e.g. a heavier compute node on a future non-WiFi esp32 config):
    #   OEVM_HEAP_SIZE=65536 bash run.sh --node SomeNode.nsl
    # 53248 -> 49152 (#65). Adding CONFIG_NET_TCP_KEEPALIVE — the reaper that breaks
    # the #65 deadlock — overflowed dram0 by 112 B, because #53's ceiling had already
    # been reached. The 4 KB comes from the VM arena rather than from the net-buffer
    # pools deliberately: the pools are what the wedge exhausts, so shrinking them to
    # pay for the fix would trade the cure for the disease.
    # Justified by measurement, not guessed: /stats on the running node reported live
    # bytes 21,860 against the 53,248 arena, so 49,152 keeps well over 2x headroom.
    # Heap high-water reads as the arena ceiling no matter what — the GC is
    # collect-when-full — so high-water is NOT evidence the node needs 52 KB.
    : "${OEVM_HEAP_SIZE:=49152}"
    : "${BUILD_DIR:=build-esp32-$MODEL}"
    pick_port '/dev/serial/by-id/*CP210*' /dev/ttyUSB0
    ;;
  esp32s2)
    # Adafruit MagTag (ESP32-S2-WROVER). NOT a MagTag board target — there is no
    # MagTag board in Zephyr and none is needed to boot: esp32s2_saola.dts:9
    # includes <espressif/esp32s2/esp32s2_wrover_n4r2.dtsi>, which IS the
    # MagTag's module (WROVER, 4 MB flash, 2 MB PSRAM). Board IO that actually
    # differs (buttons, NeoPixels, the IL0373 e-paper) arrives later as
    # boards/esp32s2_saola_esp32s2.overlay. See prj_esp32s2.conf's header.
    BOARD="esp32s2_saola/esp32s2"
    CONF_FILE="prj_esp32s2.conf"
    LAYOUT_JSON="$ZDIR/layout-esp32s2.json"
    ESPTOOL_CHIP="esp32s2"
    # Xtensa parts keep the bootloader at 0x1000 (the C6's 0x0 is a RISC-V
    # change). Same offset as the WROOM-32.
    ESPTOOL_OFFSET="0x1000"
    ESPTOOL_FLASH_SIZE="4MB"
    # The S2 ROM ld forces setjmp/longjmp to ROM exactly like the WROOM-32's
    # does, so the patch is load-bearing here too — but the S2 symbol set is
    # setjmp/longjmp only (no ungetc), hence --chip below.
    ROM_PATCH=1
    ROM_PATCH_CHIP="esp32s2"
    # MEASURED 2026-07-23 (tuvm#88), no longer provisional: the LINK ceiling is
    #   tools/heap-ceiling.sh --board esp32s2 --node MagTagDisplayNode.nsl
    #     -> max-linkable-OEVM_HEAP_SIZE=94208 (92 KB)
    #        98304 overflows dram0_0_seg by 2912 B
    # measured with the e-paper driver, CFB and the display seam's shadow grid
    # all linked in — i.e. the display rung's own footprint is already paid for
    # in that number. The default stays at the conservative 65536 because a
    # value that LINKS is not a value that BOOTS, and only 65536 has been run on
    # hardware; the ~28 KB between them is headroom a heavier node may claim
    # explicitly (OEVM_HEAP_SIZE=... bash run.sh ...) and then boot-verify.
    # Real cost is ~67.5 KB (the GC bitmap adds OEVM_HEAP_SIZE/32 outside the
    # arena).
    # Note the 2 MB of PSRAM is NOT in play here: CONFIG_ESP_SPIRAM is off, and
    # enabling it would not enlarge this arena anyway — oevm_heap_store (gc.c) is
    # a static array, whereas ESP_SPIRAM selects SHARED_MULTI_HEAP and hands out
    # PSRAM as a separate runtime heap region. Using it means changing how the
    # arena is obtained, not flipping a Kconfig.
    : "${OEVM_HEAP_SIZE:=65536}"
    : "${BUILD_DIR:=build-esp32s2-$MODEL}"
    # Flash on the MagTag's NATIVE USB, in ROM download mode. Observed on the
    # bench 2026-07-22: holding BOOT0 while clicking RESET enumerates
    #   303a:0002 Espressif ESP32-S2  ->  usb-Espressif_ESP32-S2_0-if00
    # so the glob below is the ROM bootloader's own id, NOT the board's. In its
    # normal application mode this same port enumerates as 239a:80e6
    # 'usb-Adafruit_MagTag_<serial>-if00' — a DIFFERENT string that is absent
    # exactly when you want to flash, which is why '*MagTag*' would be wrong.
    # Scoped to ESP32-S2 rather than the C6 profile's bare '*Espressif*' so two
    # Espressif parts in download mode at once stay distinguishable.
    #
    # Entering download mode is MANUAL and unavoidable: the soldered UART Debug
    # header breaks out TX/RX only (no DTR/RTS), and this native port belongs to
    # the ROM, so esptool has no reset line to drive either way. Hold BOOT0,
    # click RESET, release BOOT0 — then flash.
    #
    # The console is the OTHER port: the UART header on GPIO43/44 via a PL2303
    # (usb-Prolific_...-if00-port0). It is a separate device from this one, so a
    # console handle can be held across a flash. It does NOT collide with the
    # esp32 profile's '*CP210*' glob — different bridge chip entirely.
    pick_port '/dev/serial/by-id/*Espressif_ESP32-S2*' /dev/ttyACM0
    ;;
  esp32c6)
    BOARD="esp32c6_devkitc/esp32c6/hpcore"
    CONF_FILE="prj_esp32c6.conf"
    LAYOUT_JSON="$ZDIR/layout-esp32c6.json"
    ESPTOOL_CHIP="esp32c6"
    # RISC-V ESP32 parts moved the bootloader to 0x0. At 0x1000 the C6 boot-loops
    # SILENTLY — no error, no console output. This offset is load-bearing.
    ESPTOOL_OFFSET="0x0"
    ESPTOOL_FLASH_SIZE="8MB"
    # patch-esp32-rom-ld.sh rewrites esp32.rom.newlib-funcs.ld, an Xtensa artifact.
    # The C6 has a different ROM export table; the patch is meaningless here.
    ROM_PATCH=0
    # C6 default 131072 (128 KB), chosen 2026-07-20 (#57) once the C6 was made
    # primary. NOT the measured boot ceiling (319488 — 100% of the sramhp region,
    # 8 B spare) and not the 262144 the promotion compared against: a default wants
    # margin, not the edge. 128 KB clears every workload measured to date (deltablue
    # 112 KB, HttpServerNode ~52.8 KB high-water) AND boots in BOTH radio configs
    # (with-radio affords 262144, no-radio 319488), so a node that later layers
    # Thread does not have to re-pick the heap. Real cost is ~135 KB (the GC bitmap
    # adds OEVM_HEAP_SIZE/32 outside the arena). A caller can still override up
    # toward the ceiling for a heavier compute node:  OEVM_HEAP_SIZE=262144 ...
    : "${OEVM_HEAP_SIZE:=131072}"
    : "${BUILD_DIR:=build-esp32c6-$MODEL}"
    # Flash on the NATIVE USB port (303a:1001). Console lives on the UART bridge,
    # which stays enumerated across target resets — see #57.
    pick_port '/dev/serial/by-id/*Espressif*' /dev/ttyACM0
    ;;
esac

[ -f "$ZDIR/$NODE" ] || { echo "no such node: $ZDIR/$NODE" >&2; exit 2; }
[ -f "$NSL" ]        || { echo "compiler not found: $NSL" >&2; exit 2; }

# The seven load-bearing knobs. -p always discards the cache, so always export.
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
# OEVM_IRAM_HOT is Xtensa-only: zephyr/CMakeLists.txt:141-145 gates it on
# CONFIG_XTENSA and FATAL_ERRORs if it is set on a non-Xtensa target. Never
# export it for esp32c6 — unset any inherited value.
[ "$BOARD_ID" = esp32c6 ] && unset OEVM_IRAM_HOT

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
  LAYOUT="$LAYOUT_JSON"
  [ -f "$LAYOUT" ] || { echo "missing $LAYOUT — run the layoutdump app (stage 0) for $BOARD_ID" >&2; exit 2; }
  OIMG="$ZDIR/${NODE%.nsl}.oimg"
  python3 "$NSL" "$ZDIR/$NODE" "$OIMG" --format=oimg --layout="$LAYOUT"
  echo "[blob]  $(stat -c %s "$OIMG") bytes -> .incbin"
  BLOB_ARG=(-DTUVM_BLOB="$OIMG")
fi

PRISTINE_ARG=(); [ "$PRISTINE" = 1 ] && PRISTINE_ARG=(-p always)
# WiFi credentials overlay (#46): git-ignored, generated once with the real
# SSID/PSK (never tracked). Appended only when present so a checkout without
# it (or a future non-WiFi board) builds unaffected. The overlay is ESP32-specific
# today; scope it so a C6 build ignores it.
WIFI_CONF="$ZDIR/prj_esp32_wifi.local.conf"
EXTRA_CONF_FILES=()
if [ "$BOARD_ID" = esp32 ] && [ -f "$WIFI_CONF" ]; then
  EXTRA_CONF_FILES+=(prj_esp32_wifi.local.conf)
  echo "[wifi]  overlay found -> $WIFI_CONF"
fi
# --conf-extra (Task 6): a generic caller-supplied EXTRA_CONF_FILE, e.g. the
# C6's prj_esp32c6_thread.conf radio layer. Forwarded the same way as the WiFi
# overlay above, and stacked with it (semicolon-joined) if both are present —
# Zephyr's EXTRA_CONF_FILE accepts a list. Not board-scoped: any board may pass it.
if [ -n "$CONF_EXTRA" ]; then
  EXTRA_CONF_FILES+=("$CONF_EXTRA")
  echo "[extra] conf overlay -> $CONF_EXTRA"
fi
# C6 Thread dataset overlay (#59): gitignored real porta dataset, auto-layered like
# the WiFi overlay. Appended LAST — Zephyr applies EXTRA_CONF_FILE fragments in order
# and LATER wins, so this must follow --conf-extra prj_esp32c6_thread.conf for its 5
# real dataset fields to override that file's placeholders (proven by .config check).
THREAD_LOCAL="$ZDIR/prj_esp32c6_thread.local.conf"
if [ "$BOARD_ID" = esp32c6 ] && [ -f "$THREAD_LOCAL" ]; then
  EXTRA_CONF_FILES+=(prj_esp32c6_thread.local.conf)
  echo "[thread] dataset overlay found -> $THREAD_LOCAL"
fi
EXTRA_CONF_ARG=()
if [ ${#EXTRA_CONF_FILES[@]} -gt 0 ]; then
  IFS=';'
  EXTRA_CONF_ARG=(-DEXTRA_CONF_FILE="${EXTRA_CONF_FILES[*]}")
  unset IFS
fi
# ROM setjmp/longjmp/ungetc fault under Zephyr+picolibc (same forced-ROM-newlib
# family as fflush); patch the vendored SDK ld to use picolibc's. Idempotent; a
# west re-fetch reverts the SDK, so re-apply every build. Zero-SDK-touch: tuvm #48.
# The forced-symbol set differs per part (the S2 forces setjmp/longjmp but not
# ungetc), so the chip is passed explicitly; the script keeps a per-chip list.
if [ "$ROM_PATCH" = 1 ]; then
  "$ROOT/tools/patch-esp32-rom-ld.sh" --chip "$ROM_PATCH_CHIP"
fi

echo "[build] -b $BOARD -d $BUILD_DIR ${PRISTINE_ARG[*]:-(incremental)}"
( cd "$ZDIR" && west build -b "$BOARD" "${PRISTINE_ARG[@]}" -d "$BUILD_DIR" . \
    -- -DCONF_FILE="$CONF_FILE" -DTUVM_MODEL="$MODEL" "${BLOB_ARG[@]}" \
       "${EXTRA_CONF_ARG[@]}" )

if [ "$DO_FLASH" = 1 ]; then
  echo "[flash] esptool $ESPTOOL_CHIP @ $ESPTOOL_OFFSET -> $ESPTOOL_PORT"
  # NOT `west flash` — that would rebuild.
  "$ESPTOOL" --chip "$ESPTOOL_CHIP" --port "$ESPTOOL_PORT" --baud 921600 write_flash \
    --flash_mode dio --flash_freq 40m --flash_size "$ESPTOOL_FLASH_SIZE" \
    "$ESPTOOL_OFFSET" "$ZDIR/$BUILD_DIR/zephyr/zephyr.bin"
fi

if [ "${CAPTURE:-0}" != 0 ]; then
  echo "[watch] ${CAPTURE}s"
  python3 "$ZDIR/capture_coredump.py" "$CAPTURE"
fi
