#!/usr/bin/env bash
# run_arms.sh UF2 PORT LOGDIR TAG — the #296 cycle on one board already in BOOTSEL:
#   flash UF2 -> boot, arm "flash", run, reboot -> arm "sram", run, reboot -> arm "flash", run, bootsel
# PORT is the board's /dev/serial/by-id/ path (survives the CDC re-enumeration on every reboot).
# Leaves the board in BOOTSEL for the next image. Console transcripts: LOGDIR/TAG-<arm>-<n>.log
set -u
UF2="$1"; PORT="$2"; LOGDIR="$3"; TAG="$4"; HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$LOGDIR"
bash "$HERE/flash_uf2.sh" "$UF2" 120 || exit $?
n=0
for arm in flash sram flash; do
  n=$((n+1))
  then=reboot; [ $n -eq 3 ] && then=bootsel
  echo "[arms] $TAG run $n arm=$arm then=$then"
  echo "python3 '$HERE/spike296_console.py' '$PORT' --arm $arm --then $then --log '$LOGDIR/$TAG-$arm-$n.log' --timeout 1500" | newgrp uucp
  rc=$?
  echo "[arms] run $n exit $rc"
  [ $rc -eq 0 ] || exit $rc
  sleep 3
done
echo "[arms] $TAG done; board should be in BOOTSEL"
