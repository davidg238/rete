#!/usr/bin/env bash
# nodus/oevm/zephyr/flash-bell.sh — flash the ship's bell, deliberately.
#
#   ./flash-bell.sh
#
# WHY THIS EXISTS AT ALL. zephyr/run.sh refuses the bell by design, and the
# target harness refuses it twice (D5's class check and its by-id deny-list).
# Those refusals are right: flashing the bell ends a soak that cannot be
# restarted, it rings for the whole flash, and a FAILED flash parks the C6 in
# the ROM downloader driving no GPIOs, where the ringer sounds until a human
# intervenes (rete#201 — two minutes at 23:01 on 2026-08-11).
#
# The escape hatch run.sh names is "put I_REALLY_MEAN_THE_BELL=1 in front of the
# command you just ran" — which in practice means hand-assembling a long command
# with six exported variables and a device path. That is the part worth removing:
# every one of those pieces is a way to flash the WRONG BOARD, and ttyACM0 is
# c6spare's JTAG node, so an unpinned port detection can silently pick the
# disposable board instead. Encoding the vetted sequence once is strictly safer
# than retyping it, and it makes the dangerous operation reviewable.
#
# WHAT IS DELIBERATE HERE, and must not be "cleaned up":
#
#   * BOTH conf fragments. With only one, the app's own header says it is "the
#     polling node wearing this name" — it builds and runs and is the wrong node.
#   * The CP2102N, not the USB-JTAG. The JTAG path is the one that failed and
#     parked the bell in 2026-08-11; c6spare flashed fine over JTAG minutes
#     earlier, so this is per-board rather than a general rule.
#   * ESPTOOL_PORT *and* CONSOLE_PORT. Pinning only the flash port leaves run.sh
#     detecting the console port itself, which is the identical hazard one line
#     up and went unnoticed while exactly one CP2102N was attached.
#   * OEVM_SOLENOID_CONSOLE is ABSENT (rete#182). This is the unattended board;
#     strike_console reads the console UART, so pulling the cable does not make
#     that input go quiet, it makes it go UNDRIVEN — and a floating RX is noise.
#   * custos lends the cable for the flash and takes it back on EVERY exit path,
#     including failure. It and esptool otherwise split the ROM handshake, and a
#     cable left lent is a bell nobody can see (custos also takes it back at 900 s).
set -euo pipefail
cd "$(dirname "$0")/.."

BELL_BYID=/dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_4e5729ee0172f01199e0049f1045c30f-if00-port0

[ -e "$BELL_BYID" ] || {
  echo "FAIL: the bell's CP2102N ($BELL_BYID) is not attached." >&2
  echo "      Its port=down is the HEALTHY state (wallwart, console over Thread)," >&2
  echo "      so this is a cable question, not a fault to debug." >&2
  exit 1; }

# Identity, stated rather than assumed. The two C6 devkits are otherwise
# identical and the by-id hash is the only thing telling them apart; mistaking
# one for the other cost most of a morning on 2026-08-05.
echo "  bell CP2102N -> $(readlink -f "$BELL_BYID")"

# custos lends the cable for the flash (D16); 900 s is a ceiling, the trap
# takes it back the moment the flash is over.
. tools/bench-identity.sh
trap 'bench::return c6' EXIT
bench::lend c6 900 || { echo "FAIL: custos ($RETE_CUSTOS) would not lend c6's cable." >&2; exit 1; }

( set +u
  # shellcheck disable=SC1091
  source "$HOME/zephyr-esp32/.venv/bin/activate"
  export ZEPHYR_BASE="$HOME/zephyr-esp32/zephyr" \
         ZEPHYR_SDK_INSTALL_DIR="$HOME/zephyr-sdk-0.17.4" \
         ZEPHYR_TOOLCHAIN_VARIANT=zephyr \
         ESPTOOL_PORT="$BELL_BYID" CONSOLE_PORT="$BELL_BYID" \
         OEVM_NTP_SERVER6=fdc5:9109:9afe:97ea:9541:d963:f8d9:8567 \
         I_REALLY_MEAN_THE_BELL=1
  bash zephyr/run.sh --board esp32c6 --node ShipsBellAlarmNodeC6.nsl \
       --conf-extra "prj_esp32c6_thread.conf;prj_esp32c6_rtcalarm.conf" \
       --pristine --flash ) && rc=0 || rc=$?

# `&& rc=0 || rc=$?` rather than a bare `rc=$?`, because `set -e` would abort
# the script the moment the subshell failed — the trap would still restore the
# collector, but the line below would never print and a FAILED flash would say
# nothing at all. The failure path is the one that leaves the ringer sounding,
# so it is the last place that may exit quietly.
#
# Reported explicitly: this script's own exit is the trap's, and a pipeline's
# would be the last stage's — neither says whether the FLASH worked, which is
# the one thing a failed run must not be able to hide.
echo "flash-bell: run.sh exit $rc"
exit $rc
