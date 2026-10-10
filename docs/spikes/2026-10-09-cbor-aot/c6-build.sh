#!/usr/bin/env bash
set +u
source "$HOME/zephyr-esp32/.venv/bin/activate"
export ZEPHYR_BASE="$HOME/zephyr-esp32/zephyr" ZEPHYR_SDK_INSTALL_DIR="$HOME/zephyr-sdk-0.17.4" ZEPHYR_TOOLCHAIN_VARIANT=zephyr OEVM_CBOR_NATIVE=1 ESPTOOL_PORT=/dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_d2e21074c7beed11a5935a4371c9e8b5-if00-port0 CONSOLE_PORT=/dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_d2e21074c7beed11a5935a4371c9e8b5-if00-port0
S="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # the spike dir; apply hooks.patch and copy cbor_native.c into src/ first
cd "$HOME/workspace/rete/nodus/oevm/zephyr"
./run.sh --board esp32c6 --node "$(realpath --relative-to=. $S/CborAB.nsl)" --flash --build-dir build-esp32c6-cborab
# 2026-10-10, the rooting arm: CborRoot.nsl (arms B/C/D timed, F/E the guard and hazard);
# CborRootStress.nsl is the same with 3 samples, built with OEVM_GC_STRESS=1 in the env
# (hooks.patch adds the compile-time force to gc.c and the CMake knob), --build-dir of its own.
