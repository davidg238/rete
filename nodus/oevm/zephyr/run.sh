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
case "$BOARD_ID" in esp32|esp32s2|esp32c6|nrf52840dk|nrf52840dongle) ;; *) echo "--board must be esp32, esp32s2, esp32c6, nrf52840dk or nrf52840dongle" >&2; exit 2 ;; esac

ZDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # zephyr/ — the app dir
ROOT="$(cd "$ZDIR/.." && pwd)"                          # nodus/oevm — the VM root, not the repo root
NSL="$(git -C "$ZDIR" rev-parse --show-toplevel)/nodus/nsl/tools/ns_compile_oevm.py" # NOT ~/workspace/ns-oevm (stale)

# The nRF port lives in a SEPARATE west workspace (~/ncs) with its own bundled
# toolchain AND ITS OWN PYTHON. The ESP32 profiles inherit an ambient environment;
# this one has to construct it, because the two workspaces cannot both be sourced
# into one shell. Values are read from the toolchain's own environment.json rather
# than remembered — that file is what `nrfutil toolchain-manager env` prints.
#
# CALLED ONLY INSIDE THE SUBSHELLS THAT RUN west, never exported globally: it sets
# PYTHONHOME/PYTHONPATH, and leaking those would send this script's OWN later python
# calls (tools/console/logread.py, capture_coredump.py) to the toolchain interpreter,
# which has none of their dependencies. That failure would appear at --capture time,
# long after the build it came from.
ncs_env() {
  case "$BOARD_ID" in nrf52840dk|nrf52840dongle) ;; *) return 0 ;; esac
  local tc
  tc=$(ls -d "$HOME"/ncs/toolchains/*/ 2>/dev/null | head -1)
  tc="${tc%/}"
  if [ -z "$tc" ] || [ ! -x "$tc/usr/local/bin/west" ]; then
    echo "no NCS toolchain under ~/ncs/toolchains — cannot build for $BOARD_ID" >&2
    return 1
  fi
  export PATH="$tc/bin:$tc/usr/bin:$tc/usr/local/bin:$tc/opt/bin:$tc/nrfutil/bin:$tc/opt/zephyr-sdk/arm-zephyr-eabi/bin:$PATH"
  export LD_LIBRARY_PATH="$tc/lib:$tc/lib/x86_64-linux-gnu:$tc/usr/local/lib:${LD_LIBRARY_PATH:-}"
  export PYTHONHOME="$tc/usr/local"
  export PYTHONPATH="$tc/usr/local/lib/python3.12:$tc/usr/local/lib/python3.12/site-packages"
  export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
  export ZEPHYR_SDK_INSTALL_DIR="$tc/opt/zephyr-sdk"
  export ZEPHYR_BASE="$HOME/ncs/zephyr"
  export NRFUTIL_HOME="$tc/nrfutil/home"
}
# Who-is-who on this bench: the instance<->adapter mapping and the bell's
# identities, shared with the target harness so there is ONE home for both
# (rete#185, rete#170). Policy stays here, at each call site.
. "$ROOT/tools/bench-identity.sh"
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
# bell_interlock — refuse a flash aimed at the ship's bell (rete#185).
#
# Called from pick_port, i.e. the moment the port is decided and BEFORE anything
# else looks at the board. That ordering is the lesson target.sh already learned
# by falsifying itself: with an identity check downstream, a deploy aimed at the
# bell gets refused by some *other* check whose message is about something else,
# and the operator acts on the wrong instruction.
#
# pick_port's zero/one/many discipline is about AMBIGUITY, not identity, so on
# its own it protects the bell only as a side effect of a second board being
# plugged in:
#
#   many  -> exits 2.                      safe
#   one   -> uses it silently.             UNSAFE when the one is the bell
#   zero  -> falls back to the hardcoded default.
#
# The danger window is "the bell is attached and the other board is not", and it
# is opened by the OTHER board failing — precisely when nobody is thinking about
# the bell. It was live for about five hours on 2026-08-09 when c6spare's
# USB-JTAG cable died: one 303a:1001 on the bus, and it was the bell's.
bell_interlock() {
  [ "$DO_FLASH" = 1 ] || return 0
  bell::is_bell_port "${ESPTOOL_PORT:-}" || return 0
  if [ "${I_REALLY_MEAN_THE_BELL:-0}" = 1 ]; then
    echo "[bell] I_REALLY_MEAN_THE_BELL=1 — flashing THE SHIP'S BELL on $ESPTOOL_PORT" >&2
    echo "[bell] This ends the current soak. It cannot be restarted from here." >&2
    return 0
  fi
  echo "ERROR: $ESPTOOL_PORT is the ship's bell." >&2
  echo "Flashing it ends a multi-day soak that cannot be restarted." >&2
  echo >&2
  echo "If you are trying to flash a disposable board, plug it in and retry —" >&2
  echo "this fired because the bell was the only match, not because you asked" >&2
  echo "for it. If you genuinely mean the bell, say so and it will proceed —" >&2
  echo "put I_REALLY_MEAN_THE_BELL=1 in front of the command you just ran." >&2
  exit 2
}

pick_port() {
  local pattern="$1" fallback="$2" matches n
  # An explicit ESPTOOL_PORT still wins and still skips detection — but it no
  # longer skips the bell interlock at the bottom. rete#185's live case was an
  # explicit pin, which used to be the one way past every check there was.
  if [ -n "${ESPTOOL_PORT:-}" ]; then
    bell_interlock
    return 0
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
  bell_interlock
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
    # the ROM, so esptool has no reset line to drive. Hold BOOT0, click RESET,
    # release BOOT0 — then flash.
    #
    # LEAVING download mode is NOT manual, contrary to what this comment and
    # flash-magtag.sh both claimed until 2026-07-23. esptool does not need a
    # reset line: ESP32S2ROM.hard_reset() detects USB-OTG and, when the
    # strapping allows, arms the RTC watchdog and lets it fire
    # (esptool/targets/esp32s2.py:291-314). Observed on the bench — esptool
    # printed 'Hard resetting with a watchdog...' and the app booted with no
    # hand on the board. So a flash-and-witness loop needs a human ONCE, to
    # enter download mode, not twice.
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
    # The console is a DIFFERENT physical port from the flash port (#57, #56 §3):
    # flash over the native USB JTAG bridge above, console over the CP2102N, which
    # stays enumerated across target resets. Naming it here is what lets an
    # always-on collector and --flash coexist. Same zero/one/many discipline as
    # pick_port, for the same reason: this bench has more than one CP210x device,
    # and guessing between them is what clobbered the jag board on 2026-07-16.
    if [ -z "${CONSOLE_PORT:-}" ]; then
      cmatches=$(ls /dev/serial/by-id/*CP2102N* 2>/dev/null || true)
      cn=$(printf '%s' "$cmatches" | grep -c . || true)
      if [ "$cn" -gt 1 ]; then
        echo "more than one CP2102N console candidate:" >&2
        printf '%s\n' "$cmatches" >&2
        echo "Set CONSOLE_PORT explicitly and retry." >&2
        exit 2
      elif [ "$cn" -eq 1 ]; then
        CONSOLE_PORT="$cmatches"
      else
        CONSOLE_PORT=/dev/ttyUSB0
      fi
    fi
    ;;
  nrf52840dk)
    # rete#281. The nRF port predates the space protocol: three DKs have echoed to
    # gw85224-01 since 2026-07-11 and none of it reaches porta. This profile is what
    # lets PortaBridgeNodeDK.nsl be built the same way every other board's node is,
    # instead of by a hand-typed west line in a README (which is how it was, and the
    # README's paths are pre-consolidation and no longer resolve).
    BOARD="nrf52840dk/nrf52840"
    CONF_FILE="prj_dk_thread.conf"
    # There is no layout-nrf52840.json: this port predates Model B (#281 item 2), so
    # Model A only. Empty here and refused explicitly below — NOT left to fail inside
    # the bake with a message about running layoutdump on a board nobody has flashed.
    LAYOUT_JSON=""
    ROM_PATCH=0
    # 114688 (112 KB) is MEASURED, not chosen. At the C6's 131072 this image does not
    # LINK: `region RAM overflowed by 8676 bytes`. At 114688 it fits with 8220 B spare
    # (RAM 253924/262144 = 96.86%, FLASH 41.76%). The DK cannot spend the C6's 128 KB
    # because OT/mbedTLS/the 802.15.4 driver and the 32 KB system heap are static here
    # too, and on this part VM_STACK_INIT_WORDS (4096 words = 16 KB) sits INSIDE the
    # arena. Re-measure if any of those move; do not carry this number to another node.
    : "${OEVM_HEAP_SIZE:=114688}"
    : "${BUILD_DIR:=build-nrf52840dk-$MODEL}"
    # ONE cable, unlike the C6: the DK's on-board SEGGER J-Link carries both the flash
    # channel (west flash --runner jlink) and the console VCOM. Same zero/one/many
    # discipline as the ESP32 profiles, for the same reason — this bench has had more
    # than one of a given adapter, and guessing between them is what clobbered a board.
    if [ -z "${CONSOLE_PORT:-}" ]; then
      cmatches=$(ls /dev/serial/by-id/*SEGGER* 2>/dev/null || true)
      cn=$(printf '%s' "$cmatches" | grep -c . || true)
      if [ "$cn" -gt 1 ]; then
        echo "more than one SEGGER console candidate:" >&2
        printf '%s\n' "$cmatches" >&2
        echo "Set CONSOLE_PORT explicitly and retry." >&2
        exit 2
      elif [ "$cn" -eq 1 ]; then
        CONSOLE_PORT="$cmatches"
      else
        CONSOLE_PORT=/dev/ttyACM0
      fi
    fi
    ;;
  nrf52840dongle)
    # The PCA10059 dongle. SAME SoC and same 256 KB as the DK, and the same node
    # source — the differences are all Kconfig/devicetree (docs/BOARD_PORTS.md):
    # console on the SoC USB-CDC because the dongle HAS NO on-board debugger, SW1 on
    # P1.06 instead of BUTTON1 on P0.11, picolibc instead of newlib, and DFU instead
    # of J-Link. This profile exists because the boards on the wallwarts turned out
    # to be dongles, not DKs (rete#281, 2026-08-30) — the soak has been running on
    # them since 2026-07-11 and every doc that named the hardware said "DK".
    BOARD="nrf52840dongle/nrf52840"
    CONF_FILE="prj_dongle_thread.conf"
    LAYOUT_JSON=""
    ROM_PATCH=0
    # MEASURED for THIS config, and it is not the DK's number even though the part is
    # identical: 114688 links at RAM 257948/262144 = 98.40%, FLASH 42.39%. That is
    # 4196 B of spare RAM — against the DK's 8220 B — because the USB device stack and
    # picolibc cost what uart0 and newlib did not. FOUR KILOBYTES IS THE WHOLE MARGIN:
    # the next Kconfig line anyone adds here is as likely as not to fail the link, and
    # it will fail with `region RAM overflowed`, not with anything about this comment.
    : "${OEVM_HEAP_SIZE:=114688}"
    : "${BUILD_DIR:=build-nrf52840dongle-$MODEL}"
    # The dongle presents the APPLICATION's USB-CDC (2fe3:0004 "Zephyr Project CDC ACM
    # serial backend"), not a debugger — so there is no *SEGGER* to glob for, and the
    # console port only exists while firmware that opens one is running. In DFU mode it
    # re-enumerates as 1915:521f and has no console at all.
    if [ -z "${CONSOLE_PORT:-}" ]; then
      cmatches=$(ls /dev/serial/by-id/*Zephyr*CDC_ACM* 2>/dev/null || true)
      cn=$(printf '%s' "$cmatches" | grep -c . || true)
      if [ "$cn" -gt 1 ]; then
        echo "more than one Zephyr CDC-ACM console candidate:" >&2
        printf '%s\n' "$cmatches" >&2
        echo "Set CONSOLE_PORT explicitly and retry." >&2
        exit 2
      elif [ "$cn" -eq 1 ]; then
        CONSOLE_PORT="$cmatches"
      else
        CONSOLE_PORT=/dev/ttyACM0
      fi
    fi
    ;;
esac

# Model B needs a captured object layout and the nRF has none (#281 item 2). Refused
# by NAME here so the reason is the real one.
case "$BOARD_ID" in nrf52840dk|nrf52840dongle) NRF=1 ;; *) NRF=0 ;; esac
if [ "$NRF" = 1 ] && [ "$MODEL" != A ]; then
  echo "--board $BOARD_ID supports --model A only: there is no layout-nrf52840.json" >&2
  echo "(rete#281 item 2 — a layoutdump run on a DK is owed before Model B works here)" >&2
  exit 2
fi

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
# CONFIG_XTENSA and FATAL_ERRORs if it is set on a non-Xtensa target. Stated as an
# ALLOWLIST of the Xtensa parts rather than a list of the boards to unset it for:
# the deny-list shape meant every new non-Xtensa board (esp32c6, now nrf52840dk)
# had to remember to add itself, and the failure mode is a FATAL_ERROR at configure
# time on a board nobody has built yet.
case "$BOARD_ID" in esp32|esp32s2) ;; *) unset OEVM_IRAM_HOT ;; esac

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
  BLOB_ARG=(-DOEVM_BLOB="$OIMG")
fi

PRISTINE_ARG=(); [ "$PRISTINE" = 1 ] && PRISTINE_ARG=(-p always)
# WiFi credentials overlay (#46): git-ignored, generated once with the real
# SSID/PSK (never tracked). Appended only when present so a checkout without
# it (or a future non-WiFi board) builds unaffected. Scope it so a C6 build ignores it.
# NOT esp32-specific despite the filename: it carries CONFIG_OEVM_WIFI_SSID/_PSK,
# which zephyr/src/main.c reads on any board, so the MagTag (esp32s2) takes the same
# credentials rather than a second git-ignored copy to keep in sync (tuvm#94 stage 4).
WIFI_CONF="$ZDIR/prj_esp32_wifi.local.conf"
EXTRA_CONF_FILES=()
if { [ "$BOARD_ID" = esp32 ] || [ "$BOARD_ID" = esp32s2 ]; } && [ -f "$WIFI_CONF" ]; then
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
# C6 Thread dataset overlay (#59): gitignored real porta dataset. Appended LAST —
# Zephyr applies EXTRA_CONF_FILE fragments in order and LATER wins, so this must
# follow --conf-extra prj_esp32c6_thread.conf for its 5 real dataset fields to
# override that file's placeholders (proven by .config check).
#
# Keyed on the RADIO LAYER being requested, not on the board (#165, 2026-08-06).
# It used to key on `BOARD_ID = esp32c6` alone, so any C6 build printed "dataset
# overlay found" — including one built with no --conf-extra at all, which has no
# NET_L2_OPENTHREAD, which makes Kconfig silently discard all five
# CONFIG_OPENTHREAD_* symbols the overlay sets. The build then looked configured
# for Thread and shipped an image with no radio: PortaBridgeNodeC6 failed its UDP
# bind at boot and sat dead for over an hour while the log said the credentials
# were in. Credentials follow the radio, so there is ONE condition rather than
# two independent ones that can disagree.
THREAD_LOCAL="$ZDIR/prj_esp32c6_thread.local.conf"
case ";$CONF_EXTRA;" in
  *prj_esp32c6_thread.conf*) WANTS_THREAD=1 ;;
  *)                         WANTS_THREAD=0 ;;
esac
if [ "$WANTS_THREAD" = 1 ] && [ -f "$THREAD_LOCAL" ]; then
  EXTRA_CONF_FILES+=(prj_esp32c6_thread.local.conf)
  echo "[thread] dataset overlay found -> $THREAD_LOCAL"
elif [ "$WANTS_THREAD" = 1 ]; then
  # Loud, because the mirror failure is just as quiet: the node builds, boots and
  # attaches to nothing, using the placeholder dataset committed in the .conf.
  echo "[thread] ! NO dataset overlay at $THREAD_LOCAL — this image gets the" >&2
  echo "[thread] ! PLACEHOLDER dataset and will NOT join the porta mesh." >&2
  echo "[thread] ! Read the live one: ssh gw85224-01 docker exec otbr ot-ctl dataset active" >&2
elif [ "$BOARD_ID" = esp32c6 ] && [ -f "$THREAD_LOCAL" ]; then
  echo "[thread] radio layer not requested — dataset overlay NOT applied. This" >&2
  echo "[thread] image has no OpenThread. Add:" >&2
  echo "[thread]   --conf-extra \"prj_esp32c6_thread.conf;prj_esp32c6_thread_ftd.conf\"" >&2
fi
EXTRA_CONF_ARG=()
if [ ${#EXTRA_CONF_FILES[@]} -gt 0 ]; then
  IFS=';'
  EXTRA_CONF_ARG=(-DEXTRA_CONF_FILE="${EXTRA_CONF_FILES[*]}")
  unset IFS
fi
# ROM setjmp/longjmp/ungetc fault under Zephyr+picolibc (same forced-ROM-newlib
# family as fflush); patch the vendored SDK ld to use picolibc's. Idempotent; a
# west re-fetch reverts the SDK, so re-apply every build. Zero-SDK-touch: oevm #48.
# The forced-symbol set differs per part (the S2 forces setjmp/longjmp but not
# ungetc), so the chip is passed explicitly; the script keeps a per-chip list.
if [ "$ROM_PATCH" = 1 ]; then
  "$ROOT/tools/patch-esp32-rom-ld.sh" --chip "$ROM_PATCH_CHIP"
fi

echo "[build] -b $BOARD -d $BUILD_DIR ${PRISTINE_ARG[*]:-(incremental)}"
# rete#186 — THE NODE ID IS THE BOARD, and it is resolved from the SAME place the
# console reader resolves it: bench::board_for_port over the collector env files. That
# is the point. `nodeId` used to be a source constant (`'c6-bench'`), so whichever
# devkit carried the bridge wrote into the c6-bench space — 36,153 events from at least
# two boards and two different apps, with nothing in the store saying which.
#
# Resolved HERE, before the build, rather than reusing the INSTANCE computed after it:
# that one is for archiving the ELF and runs too late to reach the compiler.
#
# Fails to "unnamed" rather than guessing. A board this bench cannot name must not
# inherit a name, because inheriting one is the bug.
NODE_ID_ARG=()
if [ -n "${OEVM_NODE_ID:-}" ]; then
  # rete#186/#181: an EXPLICIT id wins over port resolution, because a board that is
  # not on this host has no local port to resolve through. That is the remote-bench
  # case (#181: build here, flash and observe on gw85224-01) and it is the only way
  # to build an image for a board this workstation cannot see.
  #
  # Deliberately NOT a silent fallback: it is announced, and it takes precedence
  # rather than filling in, so "I said c6bench and it built c6spare" cannot happen.
  NODE_ID_ARG=(-DOEVM_NODE_ID="$OEVM_NODE_ID")
  # STDERR, not stdout: deploy_flash.sh captures run.sh's stdout and echoes it only on
  # failure (rete#236), so this line would be invisible on exactly the runs it describes.
  # Which porta space a run writes into is not an optional detail — rete#186 is what it
  # costs to not know.
  echo "[node] id '$OEVM_NODE_ID' (explicit, overriding port resolution)" >&2
elif [ -n "${CONSOLE_PORT:-}" ] && NODE_ID=$(bench::board_for_port "$CONSOLE_PORT" 2>/dev/null); then
  NODE_ID_ARG=(-DOEVM_NODE_ID="$NODE_ID")
  echo "[node] id '$NODE_ID' (from the collector env for $CONSOLE_PORT)" >&2
else
  echo "[node] WARNING: no collector env names $CONSOLE_PORT — falling back to this" >&2
  echo "[node] HOST's tailscale name (see cmake/gen_node_id.cmake). The node will write" >&2
  echo "[node] to a space named for the machine that built it, not for a board — which" >&2
  echo "[node] separates hosts but NOT two unnamed boards on this one. Name the board." >&2
fi

( ncs_env && cd "$ZDIR" && west build -b "$BOARD" "${PRISTINE_ARG[@]}" -d "$BUILD_DIR" . \
    -- -DCONF_FILE="$CONF_FILE" -DOEVM_MODEL="$MODEL" "${BLOB_ARG[@]}" \
       "${EXTRA_CONF_ARG[@]}" "${NODE_ID_ARG[@]}" )

if [ "$DO_FLASH" = 1 ]; then
  # KNOWN RACE, not yet closed here (rete#154, closed as recorded rather than
  # fixed — this comment is the record). The #56 collector holds the CP2102N
  # open permanently and a tty has no exclusive open, so on the boards where the
  # flash and console channels are the SAME adapter, esptool and collect.py both
  # read it and the ROM handshake bytes are split between them. Symptom:
  #
  #   A serial exception error occurred: device reports readiness to read but
  #   returned no data (device disconnected or multiple access on port?)
  #
  # Intermittent, which is why it went unnoticed — whether a flash wins depends
  # on where the collector is in its read loop. The C6 is NOT exposed (it flashes
  # over the USB-JTAG node and consoles over the CP2102N, two physical channels),
  # so this bites the esp32 and esp32s2 profiles above.
  #
  # Already closed for the target harness: target::collector_pause stops the
  # collector for the deploy window, and target::reset_board then drives RTS
  # afterwards so the boot banner is still captured (esptool resets the board on
  # completion, i.e. while there is no reader). The fix here is to move that pause
  # into this script and have the harness stop doing it. rete#170 unblocked it by
  # giving this script the real instance name (below); what remains is moving the
  # stop/start and the second reset over, and deleting the harness's copy.
  if [ "$BOARD_ID" = nrf52840dongle ]; then
    # DFU, because the dongle has no debugger. It only accepts a flash while in DFU
    # mode, which is a PHYSICAL act: hold RESET while inserting, and it re-enumerates
    # as 1915:521f. Checked here rather than left to `west flash` — nrfutil's own
    # failure for "not in DFU mode" is a timeout, which reads like a broken cable.
    # The recipe is docs/BOARD_PORTS.md's; this is the executable copy of it.
    if ! lsusb 2>/dev/null | grep -q 1915:521f; then
      echo "[flash] the dongle is NOT in DFU mode (no 1915:521f on USB)." >&2
      echo "[flash] Hold the dongle's RESET button while inserting it, then re-run" >&2
      echo "[flash] with --flash. Its app console (2fe3:0004) disappears in DFU mode," >&2
      echo "[flash] so seeing no console port is expected and not a fault." >&2
      exit 2
    fi
    echo "[flash] west flash --runner nrfutil -d $BUILD_DIR (DFU)"
    ( ncs_env && cd "$ZDIR" && west flash --skip-rebuild --runner nrfutil -d "$BUILD_DIR" )
  elif [ "$BOARD_ID" = nrf52840dk ]; then
    # The DK has no ROM serial loader in this path: it flashes over its on-board
    # SEGGER J-Link. --skip-rebuild for the SAME reason the esptool branch does not
    # call `west flash` bare — a rebuild here would run under a different resolved
    # environment than the one that produced the artifact just built.
    echo "[flash] west flash --runner jlink -d $BUILD_DIR"
    ( ncs_env && cd "$ZDIR" && west flash --skip-rebuild --runner jlink -d "$BUILD_DIR" )
  else
  echo "[flash] esptool $ESPTOOL_CHIP @ $ESPTOOL_OFFSET -> $ESPTOOL_PORT"
  # NOT `west flash` — that would rebuild.
  "$ESPTOOL" --chip "$ESPTOOL_CHIP" --port "$ESPTOOL_PORT" --baud 921600 write_flash \
    --flash_mode dio --flash_freq 40m --flash_size "$ESPTOOL_FLASH_SIZE" \
    "$ESPTOOL_OFFSET" "$ZDIR/$BUILD_DIR/zephyr/zephyr.bin"
  fi
  # Part of flashing, not a step to remember afterwards (#56 §5.3). Keyed by the
  # build id the firmware itself prints, read from oevm_build_id.txt so host and
  # device cannot disagree about the string.
  if [ "$BOARD_ID" = esp32c6 ]; then
    # ${OIMG:-} is set only by a Model B bake (above); archive.sh then also
    # takes the .oimg.methods.json and .stmap sidecars into the same build-id
    # directory, so a profile taken today can be resolved after the board has
    # been reflashed twice.
    # WHICH BOARD DID THIS ACTUALLY GO TO? rete#170: this used to be the literal
    # `c6`, so every C6 flash archived under the ship's bell and checked the
    # bell's env file whichever board was flashed. Resolved instead from the
    # console port, through the same env files the collector itself reads.
    if INSTANCE=$(bench::board_for_port "$CONSOLE_PORT"); then
      "$ZDIR/tools/console/archive.sh" "$ZDIR/$BUILD_DIR" "$INSTANCE" "${OIMG:-}"

      # The old check compared this port against `console-c6.env` and so was
      # wrong on every board but one. Comparing it against the instance we just
      # resolved FROM that file would be worse — tautological, a warning that can
      # never fire. What can still be wrong, and is what the operator actually
      # wants to know, is whether that instance's collector will capture the boot.
      case "$(bench::collector_state "$INSTANCE")" in
        active) : ;;
        inactive)
          # rete#237: the same fact means two different things, and only the CALLER
          # knows which. On the hand path nobody resets the board again, so an
          # uncollected boot is the operator's problem and this is a real warning.
          # Under the target harness the collector was stopped on purpose moments
          # ago (esptool and the collector otherwise split the ROM handshake), and
          # deploy_flash resumes it and resets the board again — so the run's anchor
          # boot IS captured and the warning is noise. A warning that fires on every
          # run and always means nothing is training to ignore warnings, which is the
          # failure rete#236 had just cost a board cycle to fix.
          #
          # The board name must match: target::collector_pause exports the instance
          # IT stopped, and $INSTANCE is resolved here independently from the console
          # port. If they disagree, some OTHER board's collector is down and that is
          # worth shouting about, so this fails closed.
          if [ "${TARGET_PAUSED_BOARD:-}" = "$INSTANCE" ]; then
            echo "[console] note: rete-console@$INSTANCE was stopped by the caller for this" >&2
            echo "[console] flash, so this boot is not logged — the caller resets the board" >&2
            echo "[console] again with the collector back up, and that boot is the anchor." >&2
          else
            echo "[console] WARNING: rete-console@$INSTANCE is not running." >&2
            echo "[console] The boot you just triggered is not being logged." >&2
          fi ;;
        no-log)
          echo "[console] WARNING: rete-console@$INSTANCE is running but has no log for today." >&2 ;;
        port-down)
          if [ "$INSTANCE" = c6 ]; then
            echo "[console] note: rete-console@c6 reports port=down, which is NORMAL for the" >&2
            echo "[console] ship's bell — its console goes over Thread, not this cable." >&2
          else
            echo "[console] WARNING: rete-console@$INSTANCE reports port=down — it is running" >&2
            echo "[console] and reading nothing, so the boot you just triggered went nowhere." >&2
          fi ;;
      esac

      # The one cross-board pairing this script can still detect. The flash port
      # is the USB-JTAG node and no env file names it, so a general "did I flash
      # one board and read another's console" check is not available here — but
      # the case that costs a multi-day soak is, because the bell's JTAG identity
      # is known. Exactly one of the two ports being the bell's means the pair
      # straddles two boards.
      if bell::is_bell_port "$ESPTOOL_PORT" && ! bell::is_bell_port "$CONSOLE_PORT"; then
        echo "[console] WARNING: flashed the BELL's JTAG but the console resolves to '$INSTANCE'." >&2
      elif ! bell::is_bell_port "$ESPTOOL_PORT" && bell::is_bell_port "$CONSOLE_PORT"; then
        echo "[console] WARNING: console is the BELL's, but the flash went somewhere else." >&2
      fi
    else
      # Refusing to guess is the whole point: archiving under a name that is not
      # this board is what rete#170 was, and `last-flashed-<board>` is the
      # human-facing "what is on this board" note. The build-id directory is
      # written either way, so nothing is lost by declining to name it.
      echo "[console] WARNING: no collector instance owns $CONSOLE_PORT" >&2
      echo "[console] (or more than one claims it), so this flash cannot be attributed" >&2
      echo "[console] to a board. Archiving under 'unknown'; last-flashed-* is unchanged." >&2
      "$ZDIR/tools/console/archive.sh" "$ZDIR/$BUILD_DIR" unknown "${OIMG:-}"
      INSTANCE=unknown
    fi
    echo "[console] $INSTANCE $CONSOLE_PORT"
  fi
fi

if [ "${CAPTURE:-0}" != 0 ]; then
  if [ "$BOARD_ID" = esp32c6 ]; then
    # D6: the collector owns the port, so --capture is a READ of what it already
    # has. This gains rather than loses — it shows the boot you just triggered
    # even if it fired before you asked, which is the exact failure that lost
    # the first AMG8833 panic.
    echo "[watch] last ${CAPTURE}s from the collector log"
    # `--` is load-bearing: without it argparse reads -180s as an option flag.
    python3 "$ZDIR/tools/console/logread.py" since -- "-${CAPTURE}s"
  else
    # esp32 / esp32s2 keep the direct serial read, including its DTR/RTS reset
    # pulse (capture_coredump.py:9), which is a no-op on the C6 but real here —
    # on the esp32 that same CP210x port is both flash port and console. They
    # get a collector when they get a design (#56 §10).
    echo "[watch] ${CAPTURE}s"
    python3 "$ZDIR/capture_coredump.py" "$CAPTURE"
  fi
fi
