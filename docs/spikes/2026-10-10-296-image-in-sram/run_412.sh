#!/usr/bin/env bash
# run_412.sh UF2 TAG LOGDIR — rete#412's before/after: flash the WeAct (picked by its USB port, since
# both RP2350s answer BOOTSEL with the same label), then three runs of the blob-in-flash arm
# (reboot, reboot, bootsel), leaving the board in BOOTSEL for the next image.
# WEACT_USB (default 1-7) and WEACT_PORT (its CDC by-id path) override the bench's board; RUNS (default 3) the
# number of runs (rete#37 board A/B: 1, since the WeAct repeats to the millisecond).
set -u
UF2="$1"; TAG="$2"; LOGDIR="$3"; HERE="$(cd "$(dirname "$0")" && pwd)"
USBP="${WEACT_USB:-1-7}"; RUNS="${RUNS:-3}"
PORT="${WEACT_PORT:-/dev/serial/by-id/usb-Zephyr_Project_CDC_ACM_serial_backend_371016190D903B10-if00}"
mkdir -p "$LOGDIR"
DEV=""
for i in $(seq 1 120); do
  for b in /sys/block/sd*; do
    readlink -f "$b" | grep -q "/$USBP/" || continue
    p="/dev/$(basename "$b")1"; [ "$(lsblk -rno LABEL "$p" 2>/dev/null)" = RP2350 ] && DEV="$p"
  done
  [ -n "$DEV" ] && break; sleep 1
done
[ -n "$DEV" ] || { echo "[412] no BOOTSEL volume on usb $USBP" >&2; exit 3; }
DEV_OVERRIDE="$DEV" bash "$HERE/flash_uf2.sh" "$UF2" 120 || exit $?
for n in $(seq 1 "$RUNS"); do
  then=reboot; [ "$n" -eq "$RUNS" ] && then=bootsel
  echo "[412] $TAG run $n then=$then"
  echo "python3 '$HERE/spike296_console.py' '$PORT' --arm flash --then $then --log '$LOGDIR/$TAG-flash-$n.log' --timeout 1500 --open-wait 60" | newgrp uucp
  rc=$?; echo "[412] run $n exit $rc"; [ $rc -eq 0 ] || exit $rc
  sleep 3
done
echo "[412] $TAG done"
