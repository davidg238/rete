#!/usr/bin/env bash
# flash_uf2.sh UF2 [WAIT_S] — wait for an RP2350 BOOTSEL volume to appear on this host, mount it
# through udisks (no root), copy the UF2, and wait for the board to re-enumerate as Zephyr CDC.
# A board in BOOTSEL shows as a removable FAT volume labelled RP2350 (lsblk LABEL column).
set -u
UF2="$1"; WAIT="${2:-120}"
[ -f "$UF2" ] || { echo "no such uf2: $UF2" >&2; exit 2; }
t0=$(date +%s)
while :; do
  DEV="${DEV_OVERRIDE:-$(lsblk -rno NAME,LABEL 2>/dev/null | awk '$2=="RP2350"{print "/dev/"$1; exit}')}"
  [ -n "$DEV" ] && break
  [ $(( $(date +%s) - t0 )) -ge "$WAIT" ] && { echo "no RP2350 BOOTSEL volume within ${WAIT}s" >&2; exit 3; }
  sleep 1
done
echo "[flash] BOOTSEL volume $DEV"
MP=$(lsblk -rno MOUNTPOINT "$DEV")
if [ -z "$MP" ]; then
  OUT=$(udisksctl mount -b "$DEV" 2>&1) || { echo "[flash] mount failed: $OUT" >&2; exit 4; }
  MP=$(lsblk -rno MOUNTPOINT "$DEV")
fi
echo "[flash] mounted at $MP; copying $(basename "$UF2") ($(stat -c %s "$UF2") B)"
cp "$UF2" "$MP/" && sync
echo "[flash] copied; waiting for the volume to go away"
for i in $(seq 1 30); do lsblk -rno NAME "$DEV" >/dev/null 2>&1 || break; sleep 1; done
echo "[flash] done"
