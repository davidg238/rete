#!/usr/bin/env bash
# run_mpy.sh UF2 LOGDIR — flash MicroPython onto the RP2350 in BOOTSEL, run hanoi/blockcost/richards
# over its REPL, log, then machine.bootloader() so the board is back in BOOTSEL.
set -u
UF2="$1"; LOGDIR="$2"; HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$LOGDIR"
bash "$HERE/flash_uf2.sh" "$UF2" 120 || exit $?
for i in $(seq 1 40); do PORT=$(ls /dev/serial/by-id/*MicroPython* 2>/dev/null | head -1); [ -n "$PORT" ] && break; sleep 1; done
[ -n "${PORT:-}" ] || { echo "no MicroPython CDC port appeared" >&2; exit 3; }
echo "[mpy] port $PORT"; sleep 2
for s in hanoi blockcost richards; do
  last=""; [ "$s" = richards ] && last="--bootloader"
  echo "python3 '$HERE/mpy_run.py' '$PORT' '$HERE/mpy/$s.py' --log '$LOGDIR/mpy-$s.log' --timeout 1500 $last" | newgrp uucp
  echo "[mpy] $s exit $?"
done
echo "[mpy] done; board should be in BOOTSEL"
