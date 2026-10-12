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
case "$BOARD_ID" in esp32|esp32s2|esp32c6|nrf52840dk|nrf52840dongle|rpi_pico2|weact_rp2350b|feather_rp2040) ;; *) echo "--board must be esp32, esp32s2, esp32c6, nrf52840dk, nrf52840dongle, rpi_pico2, weact_rp2350b or feather_rp2040" >&2; exit 2 ;; esac

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
# calls (capture_coredump.py) to the toolchain interpreter,
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

# PM_STATIC_ARG is set by the profiles below and MUST be initialised above them.
# It was initialised *after* them once, on 2026-08-30, which silently emptied what
# the dongle profile had put in it — and that is not a style point: the dropped flag
# let the partition manager place settings_storage at 0xfe000, on top of the dongle's
# Open Bootloader, which is the exact outcome pm_static_dongle.yml exists to prevent.
PM_STATIC_ARG=()

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
    # rete#312 Q2 (David, 2026-10-05): the profiler is in the standard image, so a field
    # node can be profiled without a reflash. On the C6 because a build showed it fits
    # (PortaBridgeNodeC6 + Thread: sram0 +6,176 B, 73.5% -> 75.0%); idle cost +1.3% on
    # Hanoi, the worst case. Other boards stay off until a build of theirs earns it —
    # the ESP32 cannot carry the table (816 B of dram0 spare, #53). 0 turns it off.
    : "${OEVM_METHOD_PROFILE:=1}"
    export OEVM_METHOD_PROFILE
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
    # ONE layout serves both nRF boards: same part, same ABI, same toolchain — the
    # DK/dongle differences are console, trigger pin and flash method, none of which
    # can move a struct offset. Captured on the DONGLE 2026-08-30 (#281 item 2) and
    # byte-identical to layout-esp32c6.json, which is a measured outcome and not a
    # rule: you dump per target because you cannot know it matches until you look.
    LAYOUT_JSON="$ZDIR/layout-nrf52840.json"
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
    LAYOUT_JSON="$ZDIR/layout-nrf52840.json"
    # Dongle builds ONLY (ZEPHYR_LESSONS.md §1): without this the partition manager
    # puts settings_storage — which OpenThread's SETTINGS→NVS mounts at boot — on top
    # of the Open Bootloader's pages at 0xfe000, and the board bricks its own DFU.
    PM_STATIC_ARG=(-DPM_STATIC_YML_FILE=pm_static_dongle.yml)
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
  rpi_pico2)
    # rete#302 — the RP2350 (Raspberry Pi Pico 2), oevm's fourth Zephyr target, cut
    # to the C6 rung's pattern (#57). Built in the C6's west workspace (~/zephyr-esp32)
    # with the arm-zephyr-eabi toolchain the nRF path already uses from the shared SDK:
    # no new toolchain, no new workspace, no sysbuild (so the bare `-D` forms work here
    # the way they do on the C6 — the env route is kept anyway, since it costs nothing).
    # cabot's board move (M28) is gated on this profile's µs/activation number.
    BOARD="rpi_pico2/rp2350a/m33"
    CONF_FILE="prj_rpi_pico2.conf"
    # Captured from a board, never borrowed: the nRF's came out byte-identical to the
    # C6's (#281) and this M33 build uses the nRF's toolchain, so identical is the
    # EXPECTED outcome — and the bake still refuses without this target's own file.
    LAYOUT_JSON="$ZDIR/layout-rp2350.json"
    ROM_PATCH=0
    # No CONSOLE_PORT glob: the Pico 2 has NO on-board USB-UART bridge. Its uart0 (GP0
    # TX / GP1 RX) rides whatever adapter is wired to those pins, and this bench has more
    # than one of every adapter it owns, so guessing between them is the 2026-07-16
    # mistake. The ONE home for instance<->adapter is the collector env file
    # (tools/bench-identity.sh); the instance is `pico2`, and an explicit CONSOLE_PORT
    # still wins. A build without either proceeds — the image then publishes no board
    # name, with the warning below — and --flash needs no console (it goes by USB BOOTSEL), but
    # --capture refuses to read a log it cannot attribute to this board.
    if [ -z "${CONSOLE_PORT:-}" ]; then
      CONSOLE_PORT=$(bench::port_for_board pico2 2>/dev/null || true)
    fi
    # The C6's 131072 (128 KB) arena, because the point of this rung is a number on the
    # SAME footing as the C6's 11.83 µs/activation, and the heap is one of the things
    # that must match for the comparison to hold. It is NOT this part's ceiling: the
    # measured link ceilings (uart0 and USB-CDC console cells) are in
    # docs/results/2026-10-01-rp2350-first-numbers.md. A caller can override toward
    # them:  OEVM_HEAP_SIZE=262144 ...
    : "${OEVM_HEAP_SIZE:=131072}"
    : "${BUILD_DIR:=build-rpi_pico2-$MODEL}"
    ;;
  weact_rp2350b)
    # rete#403 (found-during, 2026-10-10) — the WeAct Studio RP2350B Core Board: the same
    # RP2350 die family as the Pico 2 in the 80-pin package (48 GPIO, 8 ADC), bought for
    # exactly that. Out-of-tree board under zephyr/boards/weact/rp2350b_core/ (BOARD_ROOT
    # is set in CMakeLists.txt); its README is the hookup. Same workspace, same toolchain,
    # same Kconfig as the Pico 2 — prj_rpi_pico2.conf is RP2350-generic.
    BOARD="weact_rp2350b_core/rp2350b/m33"
    CONF_FILE="prj_rpi_pico2.conf"
    # BORROWED, and said so: the Pico 2's capture. The layout is the toolchain's and the
    # ABI's, not the board's, and this is the same die family under the same compiler — but
    # the rule is "captured from a board, never borrowed", so a capture from this board
    # (layoutdump/capture.py) is owed and will replace this line with its own file.
    LAYOUT_JSON="$ZDIR/layout-rp2350.json"
    ROM_PATCH=0
    # Same instance<->adapter rule as the Pico 2: uart0 on GP0/GP1 through whichever adapter
    # the collector env file names; the instance is `weact`. An explicit CONSOLE_PORT wins.
    if [ -z "${CONSOLE_PORT:-}" ]; then
      CONSOLE_PORT=$(bench::port_for_board weact 2>/dev/null || true)
    fi
    : "${OEVM_HEAP_SIZE:=131072}"
    : "${BUILD_DIR:=build-weact_rp2350b-$MODEL}"
    ;;
  feather_rp2040)
    # rete#302's REHEARSAL board (2026-09-19): an Adafruit Feather RP2040 that was on
    # the desk while the Pico 2s were in the post. Out-of-tree board under
    # zephyr/boards/adafruit/feather_rp2040/ (BOARD_ROOT is set in CMakeLists.txt).
    # Same RP2 loop as the Pico 2 — BOOTSEL/UF2, uart0 on GP0/GP1 through an adapter,
    # the layout capture — and the one thing the RP2350 cannot do here: NVS on the
    # RP2 flash. NOT the source of #302's number; prj_feather_rp2040.conf says why.
    BOARD="adafruit_feather_rp2040"
    CONF_FILE="prj_feather_rp2040.conf"
    LAYOUT_JSON="$ZDIR/layout-rp2040.json"
    ROM_PATCH=0
    # Same instance<->adapter rule as the Pico 2, same instance name: the Feather's
    # TX/RX pads are GP0/GP1 too, so it is the SAME adapter wiring moved between two
    # boards, and one env file names it. An explicit CONSOLE_PORT still wins.
    if [ -z "${CONSOLE_PORT:-}" ]; then
      CONSOLE_PORT=$(bench::port_for_board pico2 2>/dev/null || true)
    fi
    # 264 KB SRAM, not 520. The C6's 128 KB arena is kept so a TimerNode here is the
    # same image shape as the Pico 2's; the trial link says whether it fits and by
    # how much — read the RAM line, do not carry this number to the RP2350.
    : "${OEVM_HEAP_SIZE:=131072}"
    : "${BUILD_DIR:=build-feather_rp2040-$MODEL}"
    ;;
esac

# Model B works on nRF as of 2026-08-30 (#281 item 2): layout-nrf52840.json was
# captured from a real layoutdump flash on the dongle `4ec0`. The refusal that stood
# here is DELETED rather than inverted — with a layout present there is nothing left
# to refuse, and Model A on this part is the ~4.9 GB / ~50 min cc1 bake that has
# OOM-killed the workstation (see the bake step below).
case "$BOARD_ID" in nrf52840dk|nrf52840dongle) NRF=1 ;; *) NRF=0 ;; esac

[ -f "$ZDIR/$NODE" ] || { echo "no such node: $ZDIR/$NODE" >&2; exit 2; }
[ -f "$NSL" ]        || { echo "compiler not found: $NSL" >&2; exit 2; }

# The load-bearing knobs. -p always discards the cache, so always export — but the
# CALLER'S value wins and the number here is only the default. Until rete#233 (2026-10-10)
# this line was `METHOD_CACHE_SIZE=256 VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512`,
# an unconditional assignment that silently overrode every recipe that set them: the
# MagTag's METHOD_CACHE_SIZE=128 / VM_STACK_INIT_WORDS=2048 never reached the compiler
# (compile_commands.json said 256 / 4096), so its dram0 accounting credited a 2 KB cut
# that never happened and its arena carried a 16 KB VM stack it was measured as 8 KB
# with. PRIM_CACHE_SIZE went with the prim cache (rete#226) and is no longer read.
export OEVM_HEAP_SIZE METHOD_CACHE_SIZE="${METHOD_CACHE_SIZE:-256}" \
       VM_STACK_INIT_WORDS="${VM_STACK_INIT_WORDS:-4096}" OEVM_MARK_STACK_SIZE="${OEVM_MARK_STACK_SIZE:-512}"
# The float-arena pair is NOT optional, despite every prior doc listing only five.
# gc.c's boxed-Float slab defaults to 4096 cells; struct Float is 16 B on Xtensa
# (8-byte double alignment), so one slab is ~64.5 KB against a ~60 KB libc heap —
# the FIRST float box aborts with "VM Float Arena Error: Out of Memory". A node
# that never boxes a float (TimerNode) never notices, which is why five looked
# sufficient. Values are ns-zephyr's, proven on nRF52840: see that repo's
# docs/ZEPHYR_NODE_GUIDE.md §3 and docs/results.md M3 (perfstex, 2026-06-28) —
# same bug, found and fixed there first. zephyr/CMakeLists.txt already forwards
# both; only the export was missing.
export OEVM_FLOAT_ARENA_SLAB_CELLS="${OEVM_FLOAT_ARENA_SLAB_CELLS:-512}" \
       OEVM_FLOAT_ARENA_SLAB_BUDGET="${OEVM_FLOAT_ARENA_SLAB_BUDGET:-4}"
# OEVM_IRAM_HOT is an ESP32 knob (the Xtensa parts and, since rete#37, the C6):
# zephyr/CMakeLists.txt gates it and FATAL_ERRORs if it is set on any other target. Stated as an
# ALLOWLIST of those parts rather than a list of the boards to unset it for:
# the deny-list shape meant every new non-Xtensa board (esp32c6, now nrf52840dk)
# had to remember to add itself, and the failure mode is a FATAL_ERROR at configure
# time on a board nobody has built yet.
case "$BOARD_ID" in esp32|esp32s2|esp32c6) ;; *) unset OEVM_IRAM_HOT ;; esac

# src/solidified_image.c is TRACKED (the MethodCallTest fixture). A Model A bake
# dirties it; restore on exit so it is never committed by accident. (A Model A
# bake writes no map: the image map is keyed by .oimg offsets, rete#337.)
restore_image() {
  git -C "$ROOT" checkout -- src/solidified_image.c 2>/dev/null || true
}
trap restore_image EXIT

echo "[bake]  $NODE  (model $MODEL)"
BLOB_ARG=()
PROG_OFF=""   # rete#310: set from the build's devicetree when it has program_a
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
# WHICH dataset overlay, and whether this image wants one at all, is a per-board
# fact — so it is decided per board (rete#281, 2026-08-30). It used to be C6-only,
# which meant the nRF profiles silently shipped prj_dongle_thread.conf's PLACEHOLDER
# dataset: the exact "builds, boots and attaches to nothing" failure #165 records
# below, one board family over.
#
# The C6 keys on the RADIO LAYER being requested rather than on the board, because
# there the radio is an optional --conf-extra layer and Kconfig SILENTLY DISCARDS the
# five CONFIG_OPENTHREAD_* symbols without it. On the nRFs the radio is not optional:
# it is in prj_dk_thread.conf / prj_dongle_thread.conf, which is the CONF_FILE. Same
# rule, different answer — credentials follow the radio, so there is ONE condition
# rather than two that can disagree.
case "$BOARD_ID" in
  esp32c6)
    THREAD_LOCAL="$ZDIR/prj_esp32c6_thread.local.conf"
    case ";$CONF_EXTRA;" in
      *prj_esp32c6_thread.conf*) WANTS_THREAD=1 ;;
      *)                         WANTS_THREAD=0 ;;
    esac
    ;;
  nrf52840dk)
    THREAD_LOCAL="$ZDIR/prj_dk_thread_local.conf"
    WANTS_THREAD=1
    ;;
  nrf52840dongle)
    THREAD_LOCAL="$ZDIR/prj_dongle_thread_local.conf"
    WANTS_THREAD=1
    ;;
  *)
    THREAD_LOCAL=""
    WANTS_THREAD=0
    ;;
esac
if [ "$WANTS_THREAD" = 1 ] && [ -n "$THREAD_LOCAL" ] && [ -f "$THREAD_LOCAL" ]; then
  EXTRA_CONF_FILES+=("$(basename "$THREAD_LOCAL")")
  echo "[thread] dataset overlay found -> $THREAD_LOCAL"
elif [ "$WANTS_THREAD" = 1 ]; then
  # A REFUSAL, not a warning (rete#288, 2026-09-11). The placeholder dataset in the
  # tracked .conf is the stock Zephyr/OpenThread sample set, so an image built
  # without the overlay does not attach to NOTHING — it attaches to any stock demo
  # mesh in range, which is the opposite of the intended failure. Until 2026-09-11
  # this branch warned and built; the warning was correct and the image it let
  # through was still wrong.
  echo "[thread] ! NO dataset overlay at $THREAD_LOCAL — refusing to build." >&2
  echo "[thread] ! Without it this image would carry the STOCK SAMPLE dataset and" >&2
  echo "[thread] ! join any Zephyr demo mesh in range, not the porta mesh." >&2
  echo "[thread] ! Read the live one: ssh gw85224-01 docker exec otbr ot-ctl dataset active" >&2
  exit 2
elif [ "$BOARD_ID" = esp32c6 ] && [ -n "$THREAD_LOCAL" ] && [ -f "$THREAD_LOCAL" ]; then
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
# rete#118 — THE BOARD NAME (the node id is read from silicon and needs nothing here).
# It is resolved from the SAME place the console reader resolves it: bench::board_for_port
# over the collector env files, so obs/build.board names the board the bench calls it.
#
# Resolved HERE, before the build, rather than reusing the INSTANCE computed after it:
# that one is for archiving the ELF and runs too late to reach the compiler.
#
# No guess: a board this bench cannot name publishes NO board name (the image bakes "").
BOARD_NAME_ARG=()
if [ -n "${OEVM_BOARD_NAME:-}" ]; then
  # An EXPLICIT name wins over port resolution, because a board that is not on this host
  # has no local port to resolve through (#181: build here, flash and observe on
  # gw85224-01). Announced, and taking precedence rather than filling in.
  BOARD_NAME_ARG=(-DOEVM_BOARD_NAME="$OEVM_BOARD_NAME")
  # STDERR, not stdout: deploy_flash.sh captures run.sh's stdout and echoes it only on
  # failure (rete#236), so this line would be invisible on exactly the runs it describes.
  echo "[board] name '$OEVM_BOARD_NAME' (explicit, overriding port resolution)" >&2
elif [ -n "${CONSOLE_PORT:-}" ] && BOARD_NAME=$(bench::board_for_port "$CONSOLE_PORT" 2>/dev/null); then
  BOARD_NAME_ARG=(-DOEVM_BOARD_NAME="$BOARD_NAME")
  echo "[board] name '$BOARD_NAME' (from the collector env for $CONSOLE_PORT)" >&2
else
  echo "[board] WARNING: no collector env names ${CONSOLE_PORT:-<no console port>} — this image" >&2
  echo "[board] publishes NO board name in obs/build. Name the board." >&2
fi

# SYSBUILD SCOPING, AND IT IS LOAD-BEARING (rete#281, 2026-08-30).
#
# The nRF targets build under sysbuild; the ESP32 targets are single-image. Under
# sysbuild a bare `-DFOO=bar` after `--` lands in the SYSBUILD cache, not the image's,
# so every variable this script passes that Zephyr does not itself forward is silently
# dropped. Zephyr forwards the well-known ones (CONF_FILE, EXTRA_CONF_FILE), which is
# exactly why this was invisible: the config was right, so the build looked right.
#
# What was lost was OEVM_MODEL, OEVM_BLOB and OEVM_BOARD_NAME (then OEVM_NODE_ID) — the
# three that select WHICH PROGRAM the image contains. Every fallback looks like success:
# OEVM_MODEL defaults to "B", OEVM_BLOB to the committed TimerNode.oimg "so it just
# works", and the name to nothing. So `--node PortaBridgeNodeNrf.nsl` baked the right
# .oimg, printed every right line, and built TimerNode with no board name. A dongle flashed that
# way goes dark: no mesh, no console, nothing to see.
#
# `-Dzephyr_OEVM_BLOB=...` (image-scoped, the main image being `zephyr`) WAS TRIED AND
# DOES NOT WORK: measured 2026-08-30, it reaches zephyr_sysbuild_cache.txt and leaves
# the image's own cache untouched. ENV is the mechanism zephyr/CMakeLists.txt already
# uses for five other variables and it adds none — the three below now read env there
# too. Exported inside the subshell only, next to ncs_env's other exports.
export OEVM_MODEL="$MODEL"
[ -n "${OIMG:-}" ] && export OEVM_BLOB="$OIMG"
[ ${#BOARD_NAME_ARG[@]} -gt 0 ] && export OEVM_BOARD_NAME="${BOARD_NAME_ARG[0]#-DOEVM_BOARD_NAME=}"

( ncs_env && cd "$ZDIR" && west build -b "$BOARD" "${PRISTINE_ARG[@]}" -d "$BUILD_DIR" . \
    -- -DCONF_FILE="$CONF_FILE" "${EXTRA_CONF_ARG[@]}" "${PM_STATIC_ARG[@]}" )
BUILD_RC=$?

# WHAT THIS BUILD ACTUALLY CONTAINS, read from the build itself rather than from what
# was asked for. The two disagreed silently and cost a board flash plus an afternoon.
#
# THE MODEL CHECK ALONE IS NOT ENOUGH, and believing it was cost a second cycle: B is
# CMakeLists.txt's default, so a fully dropped variable still yields a Model B link —
# bound to TimerNode.oimg. "model B confirmed" was printed over exactly that. The blob
# path is the check that can actually fail, so it is the one that matters here.
if [ "$BUILD_RC" = 0 ]; then
  if find "$ZDIR/$BUILD_DIR" -name 'solidified_image*.obj' -print -quit | grep -q . ; then
    ACTUAL_MODEL=A
  elif find "$ZDIR/$BUILD_DIR" -name 'oimg_loader*.obj' -print -quit | grep -q . ; then
    ACTUAL_MODEL=B
  else
    ACTUAL_MODEL="?"
  fi
  if [ "$ACTUAL_MODEL" != "$MODEL" ]; then
    echo "[build] FATAL: asked for model $MODEL, the link set says $ACTUAL_MODEL." >&2
    echo "[build] The image does NOT contain $NODE. Under sysbuild this is what a" >&2
    echo "[build] dropped variable looks like; see the SYSBUILD SCOPING note above." >&2
    exit 3
  fi

  if [ "$MODEL" = B ]; then
    # rete#310: a board whose devicetree has program_a links no program,
    # so there is no OEVM_BLOB_FILE to check. Instead, the flash below writes $OIMG
    # at the partition's offset, read from the build's own devicetree (the overlay
    # is the writer; this is a reader, not a copy).
    PROG_REG=$(awk '/program_a: partition@/{f=1} f && /reg = </{gsub(/[<>;]/,""); print $3, $4; exit}' \
                 "$ZDIR/$BUILD_DIR/zephyr/zephyr.dts" 2>/dev/null || true)
    if [ -n "$PROG_REG" ]; then
      read -r PROG_OFF PROG_SIZE <<< "$PROG_REG"
      OIMG_SIZE=$(stat -c %s "$OIMG")
      if [ "$OIMG_SIZE" -gt $((PROG_SIZE)) ]; then
        echo "[build] FATAL: $(basename "$OIMG") is $OIMG_SIZE B; program_a is $((PROG_SIZE)) B." >&2
        exit 3
      fi
      echo "[build] model B, program_a $PROG_OFF ($((PROG_SIZE)) B): $(basename "$OIMG") is written there, not linked"
    else
      # The .incbin'd path, straight out of the generated ninja file — the blob this
      # image carries, not the blob this script baked.
      # `*/build.ninja` exists only under NCS sysbuild (the nRF family). On a C6 the
      # glob matches nothing, grep exits 2 on the missing path, and under
      # `set -euo pipefail` that killed every C6 flash from 151643f (2026-08-30)
      # until 2026-09-17 — silently, after "Successfully created esp32c6 image".
      # The `|| true` keeps the pipeline's verdict with the EMPTY check below.
      ACTUAL_BLOB=$({ grep -ho 'OEVM_BLOB_FILE=[^ ]*' "$ZDIR/$BUILD_DIR"/build.ninja \
                        "$ZDIR/$BUILD_DIR"/*/build.ninja 2>/dev/null || true; } |
                    head -1 | sed 's/^OEVM_BLOB_FILE=//; s/\\\?"//g')
      if [ -z "$ACTUAL_BLOB" ]; then
        echo "[build] FATAL: no OEVM_BLOB_FILE in the generated build — cannot tell which" >&2
        echo "[build] program this image contains, which is the failure this check exists for." >&2
        exit 3
      fi
      if [ "$ACTUAL_BLOB" != "$OIMG" ]; then
        echo "[build] FATAL: the image carries a DIFFERENT program than the one baked." >&2
        echo "[build]   baked:  $OIMG" >&2
        echo "[build]   image:  $ACTUAL_BLOB" >&2
        echo "[build] TimerNode.oimg here means OEVM_BLOB never reached the image (the" >&2
        echo "[build] CMakeLists.txt fallback), and the flash would be silently wrong." >&2
        exit 3
      fi
      echo "[build] model B confirmed, blob $(basename "$ACTUAL_BLOB")"
    fi
  else
    echo "[build] model A confirmed from the link set"
  fi
  # rete#403 — THE XIP CACHE CLIFF. On a part that executes from QSPI flash through a small
  # cache (ESP32 family, RP2 family) the interpreter's speed is a property of where its hot
  # functions LAND, not of their code: 12 KB of unrelated growth moved them and cost 2.1–2.7×
  # on the RP2350, with nothing in the diff to point at (docs/ZEPHYR_LESSONS.md §11). oevm.h
  # OEVM_HOT routes them into RAM; this line says, after every build, whether it did. Read
  # from the ELF with the build's own nm (the compiler path from compile_commands.json).
  HOT_SYM_ADDR=$(CCPATH=$(grep -o -m1 '[^" ]*-gcc' "$ZDIR/$BUILD_DIR/compile_commands.json" | head -1); \
    [ -n "$CCPATH" ] && "${CCPATH%-gcc}-nm" "$ZDIR/$BUILD_DIR/zephyr/zephyr.elf" 2>/dev/null | awk '$3=="run_interpreter"{print $1}')
  case "$BOARD_ID:${HOT_SYM_ADDR:0:4}" in
    rpi_pico2:2000|weact_rp2350b:2000|feather_rp2040:2000|esp32*:4008|esp32*:4037|esp32*:4002|esp32c6:4080|esp32c6:4081|esp32c6:4082|esp32c6:4083|esp32c6:4084|esp32c6:4085|esp32c6:4086|esp32c6:4087)
      echo "[build] hot path: SRAM (run_interpreter @ 0x$HOT_SYM_ADDR)" ;;
    rpi_pico2:*|weact_rp2350b:*|feather_rp2040:*|esp32*:*)
      echo "[build] hot path: FLASH (run_interpreter @ 0x${HOT_SYM_ADDR:-?}) — the XIP cache cliff, ZEPHYR_LESSONS §11; OEVM_HOT is not landing in RAM" >&2 ;;
    *) : ;;   # nRF52840: internal flash, not XIP; nothing to say
  esac

  # WHERE THIS IMAGE PUT ITS SETTINGS PARTITION — a dongle-only check, and it is here
  # because the assertions above would have passed the image that went dark on
  # 2026-08-30. They verify WHICH PROGRAM the image contains; nothing verified where
  # its flash landed, and a correct program at the wrong offsets is still a board you
  # cannot talk to.
  #
  # Without pm_static_dongle.yml the partition manager treats the whole 1 MB as the
  # app's and places settings_storage "before: end" at 0xfe000-0x100000 — the Open
  # Bootloader's MBR-params and bootloader-settings pages. OpenThread selects
  # SETTINGS->NVS, so the node mounts NVS on top of its own bootloader at boot. The
  # static file exists solely to prevent that, so this asserts the OUTCOME rather than
  # trusting that the flag reached the build: the flag was silently emptied once, by an
  # array initialised after the profile that set it, and the flag being passed is not
  # the property anyone cares about.
  if [ "$BOARD_ID" = nrf52840dongle ] && [ -f "$ZDIR/$BUILD_DIR/partitions.yml" ]; then
    SS_ADDR=$(awk '/^settings_storage:/{f=1;next} f&&/^  address:/{print $2;exit} /^[a-z]/{f=0}' \
                "$ZDIR/$BUILD_DIR/partitions.yml")
    if [ -n "$SS_ADDR" ] && [ $((SS_ADDR)) -ge $((0xdc000)) ]; then
      echo "[build] FATAL: settings_storage is at $SS_ADDR, inside the Open Bootloader" >&2
      echo "[build] (0xdc000+). This image would mount NVS over its own bootloader at" >&2
      echo "[build] boot. pm_static_dongle.yml did not reach the build — see PM_STATIC_ARG." >&2
      exit 3
    fi
    echo "[build] settings_storage ${SS_ADDR:-none} — clear of the bootloader"
  fi

  # The board name is baked at CONFIGURE time into oevm_board_name.c, so the generated
  # file is the evidence. An unnamed board is a warning, not a failure: an unset name
  # bakes "" (no obs/build.board), a choice made loudly rather than a default.
  if [ ${#BOARD_NAME_ARG[@]} -gt 0 ]; then
    WANT_NAME="${BOARD_NAME_ARG[0]#-DOEVM_BOARD_NAME=}"
    if ! grep -qs "\"$WANT_NAME\"" "$ZDIR/$BUILD_DIR"/*/oevm_board_name.c \
                                   "$ZDIR/$BUILD_DIR"/oevm_board_name.c 2>/dev/null; then
      echo "[build] FATAL: board name '$WANT_NAME' is not in the generated oevm_board_name.c." >&2
      echo "[build] The image would publish a different name than the board it is for." >&2
      exit 3
    fi
    echo "[build] board name $WANT_NAME confirmed in the generated source"
  fi
fi
[ "$BUILD_RC" = 0 ] || exit "$BUILD_RC"

if [ "$DO_FLASH" = 1 ]; then
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
    # NOT `west flash --runner nrfutil`. That runner hunts for device traits over a
    # debugger and answers `FATAL ERROR: Unable to find a board` at a dongle sitting in
    # DFU mode two inches away — measured 2026-08-30, and it is the same J-Link-shaped
    # assumption fbb2e56 removed from the CONF_FILE and the board target but left here.
    #
    # The real path is the Open Bootloader's: package the hex, then push it over the
    # bootloader's own serial port. `pkg generate` lives in nrf5sdk-tools, which is NOT
    # in the NCS bundle and cannot be added to it (the bundle's nrfutil home is locked
    # on purpose), so it comes from a separate NRFUTIL_HOME — #48, zero SDK touch.
    NRFUTIL_RETE="$HOME/.nrfutil-rete/bin/nrfutil"
    if [ ! -x "$NRFUTIL_RETE" ]; then
      echo "[flash] no nrfutil at $NRFUTIL_RETE — install nrf5sdk-tools into a" >&2
      echo "[flash] separate NRFUTIL_HOME: NRFUTIL_HOME=~/.nrfutil-rete nrfutil install nrf5sdk-tools" >&2
      exit 2
    fi
    DFU_PORT=$(ls /dev/serial/by-id/*Open_DFU_Bootloader* 2>/dev/null | head -1)
    if [ -z "$DFU_PORT" ]; then
      echo "[flash] 1915:521f is on USB but no Open_DFU_Bootloader serial port appeared." >&2
      exit 2
    fi
    DFU_ZIP="$ZDIR/$BUILD_DIR/dfu-app.zip"
    rm -f "$DFU_ZIP"
    echo "[flash] nrfutil pkg generate -> $DFU_ZIP"
    NRFUTIL_HOME="$HOME/.nrfutil-rete" "$NRFUTIL_RETE" nrf5sdk-tools pkg generate \
      --hw-version 52 --sd-req 0x00 --application-version 1 \
      --application "$ZDIR/$BUILD_DIR/merged.hex" "$DFU_ZIP" >/dev/null || exit 2
    echo "[flash] nrfutil dfu usb-serial -> $(readlink -f "$DFU_PORT")"
    NRFUTIL_HOME="$HOME/.nrfutil-rete" "$NRFUTIL_RETE" nrf5sdk-tools dfu usb-serial \
      -pkg "$DFU_ZIP" -p "$(readlink -f "$DFU_PORT")" || exit 2
  elif [ "$BOARD_ID" = rpi_pico2 ] || [ "$BOARD_ID" = weact_rp2350b ] || [ "$BOARD_ID" = feather_rp2040 ]; then
    # UF2 over BOOTSEL mass storage — the bench path (#302 Task 4). Like the dongle's
    # DFU it is a PHYSICAL act: hold BOOTSEL while plugging the board in (the Feather:
    # hold BOOTSEL, tap RESET) and the ROM enumerates as a USB disk with an
    # INFO_UF2.TXT on it — 2e8a:000f / volume RP2350 for the Pico 2, 2e8a:0003 /
    # volume RPI-RP2 for the RP2040. Checked here rather than left to the runner, whose
    # own failure ("No matching UF2 partitions found") reads like a missing package
    # rather than an unpressed button. SWD via `openocd` + the debug probe is the
    # second path and cabot's OTA path (#233); not wired until a probe is on the desk.
    case "$BOARD_ID" in rpi_pico2|weact_rp2350b) ROM_USB=2e8a:000f ROM_VOL=RP2350 ;; *) ROM_USB=2e8a:0003 ROM_VOL=RPI-RP2 ;; esac
    if ! lsusb 2>/dev/null | grep -q "$ROM_USB"; then
      echo "[flash] the board is NOT in BOOTSEL mode (no $ROM_USB on USB)." >&2
      echo "[flash] Hold BOOTSEL while plugging the board in, then re-run with --flash." >&2
      echo "[flash] Its uart0 console adapter is a separate device and is unaffected." >&2
      exit 2
    fi
    # The runner finds the mount by INFO_UF2.TXT's Board-ID (board.cmake pins it), so
    # a desktop that does not auto-mount removable media fails there with the message
    # above. `udisksctl mount -b /dev/disk/by-label/$ROM_VOL` is the hand fix.
    echo "[flash] west flash --runner uf2 -d $BUILD_DIR"
    ( cd "$ZDIR" && west flash --skip-rebuild --runner uf2 -d "$BUILD_DIR" )
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
  PROG_ARGS=()
  [ -n "${PROG_OFF:-}" ] && PROG_ARGS=("$PROG_OFF" "$OIMG")
  # rete#154: when the flash port IS a cable custos collects (the esp32/esp32s2
  # shared UART, or a C6 flashed over its CP2102N as the target harness and
  # flash-bell.sh do), esptool and custos would split the ROM handshake:
  #   A serial exception error occurred: device reports readiness to read but
  #   returned no data (device disconnected or multiple access on port?)
  # So custos lends the cable for exactly the esptool call, here where the flash
  # happens, and no caller does it for us. The C6's default flash port is its
  # USB-JTAG node, which no bench file names, so the common path lends nothing.
  # 180 s is a ceiling: it gives the cable back if this script is killed mid-flash.
  # The boot esptool triggers lands while the cable is lent and is not logged; a
  # caller that anchors on a boot (the harness) resets the board again after.
  LENT=""
  if FLASH_BOARD=$(bench::board_for_port "$ESPTOOL_PORT") \
     && [ "$(bench::collector_state "$FLASH_BOARD")" != unserved ]; then
    bench::lend "$FLASH_BOARD" 180 \
      || { echo "[flash] custos ($RETE_CUSTOS) would not lend $FLASH_BOARD's cable" >&2; exit 1; }
    LENT=$FLASH_BOARD
    echo "[flash] custos lent $LENT's cable for the flash"
  fi
  rc=0
  "$ESPTOOL" --chip "$ESPTOOL_CHIP" --port "$ESPTOOL_PORT" --baud 921600 write_flash \
    --flash_mode dio --flash_freq 40m --flash_size "$ESPTOOL_FLASH_SIZE" \
    "$ESPTOOL_OFFSET" "$ZDIR/$BUILD_DIR/zephyr/zephyr.bin" "${PROG_ARGS[@]}" || rc=$?
  if [ -n "$LENT" ]; then
    bench::return "$LENT" || echo "[flash] WARNING: custos did not take $LENT's cable back; it will at 180 s" >&2
    for _ in $(seq 20); do
      [ "$(bench::collector_state "$LENT")" = lent ] || break
      sleep 1
    done
  fi
  [ "$rc" = 0 ] || exit "$rc"
  fi
  # Part of flashing, not a step to remember afterwards (#56 §5.3). Keyed by the
  # build id the firmware itself prints, read from oevm_build_id.txt so host and
  # device cannot disagree about the string.
  if [ "$BOARD_ID" = esp32c6 ]; then
    # ${OIMG:-} is set only by a Model B bake (above); archive.sh then also
    # takes the .oimg.map.json image map into the same build-id
    # directory, so a profile taken today can be resolved after the board has
    # been reflashed twice.
    # WHICH BOARD DID THIS ACTUALLY GO TO? rete#170: this used to be the literal
    # `c6`, so every C6 flash archived under the ship's bell and checked the
    # bell's env file whichever board was flashed. Resolved instead from the
    # console port, through the same env files the collector itself reads.
    if INSTANCE=$(bench::board_for_port "$CONSOLE_PORT"); then
      "$ZDIR/archive.sh" "$ZDIR/$BUILD_DIR" "$INSTANCE" "${OIMG:-}"

      # The old check compared this port against `console-c6.env` and so was
      # wrong on every board but one. Comparing it against the instance we just
      # resolved FROM that file would be worse — tautological, a warning that can
      # never fire. What can still be wrong, and is what the operator actually
      # wants to know, is whether that instance's collector will capture the boot.
      case "$(bench::collector_state "$INSTANCE")" in
        active) : ;;
        lent)
          # This script returned its own lend above, so a lent cable now is some
          # other process's: the boot just triggered is not being logged.
          echo "[console] WARNING: $INSTANCE's cable is lent to something else, so the" >&2
          echo "[console] boot you just triggered is not being logged." >&2 ;;
        unserved)
          echo "[console] WARNING: custos ($RETE_CUSTOS) does not serve $INSTANCE." >&2
          echo "[console] The boot you just triggered is not being logged." >&2 ;;
        no-log)
          echo "[console] WARNING: custos serves $INSTANCE but has no log for today." >&2 ;;
        port-down)
          if [ "$INSTANCE" = c6 ]; then
            echo "[console] note: custos reports c6's cable down, which is NORMAL for the" >&2
            echo "[console] ship's bell — its console goes over Thread, not this cable." >&2
          else
            echo "[console] WARNING: custos reports $INSTANCE's cable down — it is serving" >&2
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
      "$ZDIR/archive.sh" "$ZDIR/$BUILD_DIR" unknown "${OIMG:-}"
      INSTANCE=unknown
    fi
    echo "[console] $INSTANCE $CONSOLE_PORT"
  fi
fi

if [ "${CAPTURE:-0}" != 0 ]; then
  if [ "$BOARD_ID" = rpi_pico2 ] || [ "$BOARD_ID" = weact_rp2350b ] || [ "$BOARD_ID" = feather_rp2040 ]; then
    # Same D6 shape as the C6 below — the collector owns the port — but the board is
    # NAMED: custos serves every board, so the read must say which. Resolved from the console port through the env files, and it
    # refuses rather than reads the wrong board's log when nothing names the port.
    if ! PICO_INSTANCE=$(bench::board_for_port "${CONSOLE_PORT:-}"); then
      echo "[watch] no collector env names '${CONSOLE_PORT:-<unset>}' — set CONSOLE_PORT" >&2
      echo "[watch] or write ~/.config/rete/console-pico2.env and add --board pico2 to custos." >&2
      exit 2
    fi
    echo "[watch] last ${CAPTURE}s from the collector log for $PICO_INSTANCE"
    curl -sf "$RETE_CUSTOS/board/$PICO_INSTANCE/log?since=-${CAPTURE}s" | jq -r '.[] | "\(.When) \(.Text)"'
  elif [ "$BOARD_ID" = esp32c6 ]; then
    # D6: the collector owns the port, so --capture is a READ of what it already
    # has. This gains rather than loses — it shows the boot you just triggered
    # even if it fired before you asked, which is the exact failure that lost
    # the first AMG8833 panic.
    echo "[watch] last ${CAPTURE}s from the collector log"
    # Named, as the pico branch is: the collector serves every board, and the
    # board flashed is the one the console port resolves to.
    if ! C6_INSTANCE=$(bench::board_for_port "${CONSOLE_PORT:-}"); then
      echo "[watch] no bench file names '${CONSOLE_PORT:-<unset>}' — set CONSOLE_PORT" >&2
      exit 2
    fi
    curl -sf "$RETE_CUSTOS/board/$C6_INSTANCE/log?since=-${CAPTURE}s" | jq -r '.[] | "\(.When) \(.Text)"'
  else
    # esp32 / esp32s2 keep the direct serial read, including its DTR/RTS reset
    # pulse (capture_coredump.py:9), which is a no-op on the C6 but real here —
    # on the esp32 that same CP210x port is both flash port and console. They
    # get a collector when they get a design (#56 §10).
    echo "[watch] ${CAPTURE}s"
    python3 "$ZDIR/capture_coredump.py" "$CAPTURE"
  fi
fi
