#!/usr/bin/env bash
# zephyr/flash-magtag.sh — one-shot MagTag (ESP32-S2) flash for a hardware gate.
#
# Folds the sequence that run.sh leaves to the operator on this board, because the
# MagTag has two traps run.sh cannot automate:
#   1. The native USB belongs to the ROM, so entering download mode is MANUAL
#      (hold BOOT0, click RESET, release BOOT0) and esptool has no reset line.
#   2. The flash port (native USB, *Espressif_ESP32-S2*) and the console port
#      (the PL2303 UART header, ttyUSB0) are DIFFERENT devices.
#
# What this script does: source the west env, wait for download mode, flash the
# node, then (optionally) hold the console open so you can press the chord.
#
#   ./flash-magtag.sh                       # flash MagTagChordNode.nsl, open console
#   ./flash-magtag.sh MagTagButtonsNode.nsl # regression node
#   ./flash-magtag.sh MagTagChordNode.nsl --no-console
set -euo pipefail

ZDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NODE="${1:-MagTagChordNode.nsl}"
OPEN_CONSOLE=1
[ "${2:-}" = "--no-console" ] && OPEN_CONSOLE=0

FLASH_GLOB='/dev/serial/by-id/*Espressif_ESP32-S2*'
CONSOLE_GLOB='/dev/serial/by-id/*Prolific*'   # PL2303 UART header, TX/RX only

# ---- west env (run.sh does NOT source it) ---------------------------------
# shellcheck disable=SC1090,SC1091
source "$HOME/zephyr-esp32/.venv/bin/activate"
# shellcheck disable=SC1090,SC1091
source "$HOME/zephyr-esp32/zephyr/zephyr-env.sh"
export ZEPHYR_TOOLCHAIN_VARIANT="${ZEPHYR_TOOLCHAIN_VARIANT:-zephyr}"

# ---- 1. wait for ROM download mode ----------------------------------------
if ! ls $FLASH_GLOB >/dev/null 2>&1; then
  echo ">> Put the MagTag in ROM download mode:"
  echo "     hold BOOT0, click RESET, release BOOT0"
  printf '   waiting for %s ' "$FLASH_GLOB"
  for _ in $(seq 1 60); do
    ls $FLASH_GLOB >/dev/null 2>&1 && { echo " ok"; break; }
    printf '.'; sleep 1
  done
  ls $FLASH_GLOB >/dev/null 2>&1 || { echo; echo "!! download-mode port never appeared" >&2; exit 1; }
fi

# ---- 2. flash -------------------------------------------------------------
echo ">> flashing $NODE"
"$ZDIR/run.sh" --board esp32s2 --model B --node "$NODE" --flash

# ---- 3. boot the app ------------------------------------------------------
# esptool RESETS THIS BOARD ITSELF. Corrected on the bench 2026-07-23: the
# earlier claim here ("esptool cannot reset the native port") was wrong.
# ESP32S2ROM.hard_reset() detects USB-OTG and arms the RTC watchdog instead of
# driving a reset line it does not have — watch for 'Hard resetting with a
# watchdog...' in esptool's output above. Only fall back to a manual click if
# that line is absent (the strapping check in esptool/targets/esp32s2.py:301
# can decline) or the console stays silent.
echo
echo ">> flash done. esptool should have watchdog-reset the board into the app;"
echo "   click RESET (no BOOT0) only if the console shows no boot banner."

# ---- 4. console -----------------------------------------------------------
if [ "$OPEN_CONSOLE" = 1 ]; then
  CONSOLE="$(ls $CONSOLE_GLOB 2>/dev/null | head -1 || true)"
  CONSOLE="${CONSOLE:-/dev/ttyUSB0}"
  echo ">> opening console on $CONSOLE (Ctrl-] to quit)"
  case "$NODE" in
    MagTagChordNode.nsl)
      echo "   gate: hold A tap B -> 'held=AB'; hold A+C tap D -> 'held=ACD'; tap B -> 'held=B'"
      echo "   (the old 'up held=' lines are gone by design: level is pull, not push)"
      ;;
  esac
  exec python3 -m serial.tools.miniterm "$CONSOLE" 115200
fi
