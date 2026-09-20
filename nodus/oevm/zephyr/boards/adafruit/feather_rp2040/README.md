# Adafruit Feather RP2040 — oevm's out-of-tree board (rete#302 rehearsal)

Zephyr v4.2 has no port for this board; this directory is one, discovered through the
`BOARD_ROOT` both `zephyr/CMakeLists.txt` and `zephyr/layoutdump/CMakeLists.txt` set.
Shape from `adafruit_qt_py_rp2040`, pins from the pico-sdk's `adafruit_feather_rp2040.h`.
It exists to rehearse the RP2 loop while the Pico 2s (the real #302 target) are in the
post, and to prove NVS on RP2 flash — which the RP2350 cannot do in this tree
(`zephyr/prj_rpi_pico2.conf` says why). **It is not the source of #302's number**: an
M0+ at 133 MHz is a different core from the M33 that cabot's M28 gates on.

## Hookup (written 2026-09-19, before the soldering)

The console is **uart0 on the TX/RX pads** at the board's edge — GP0 = TX, GP1 = RX —
the SAME pair the Pico 2 uses, so one adapter wiring serves both boards and one
collector env file names it.

**The cable must be 3.3 V logic.** RP2040 GPIO are 3.3 V and **not 5 V tolerant**; a
5 V TTL cable (FTDI TTL-232R-5V, or anything unlabelled) on the RX pad overdrives the
pin. Only a TTL-232R-3V3 or a CP2102/CH340 board jumpered to 3.3 V goes on.

| cable | Feather pad | why |
|---|---|---|
| TX | **RX** (GP1) | the cable talks into the board's receiver |
| RX | **TX** (GP0) | and listens to its transmitter |
| GND | GND | common reference — without it the UART reads garbage or nothing |
| VCC | *not connected* | the Feather is powered from its own USB; two supplies on one rail is a fault |

Once the cable is in, `ls -l /dev/serial/by-id/` gives its stable name; that path goes in
`~/.config/rete/console-pico2.env` as `RETE_CONSOLE_PORT=` (copy
`zephyr/tools/console/console-c6.env.example`). `run.sh` resolves `CONSOLE_PORT` from
that file for both `--board feather_rp2040` and `--board rpi_pico2`; an explicit
`CONSOLE_PORT=` on the command line still wins.

## Flashing — BOOTSEL/UF2, a physical act

Hold **BOOTSEL**, tap **RESET**, release BOOTSEL. The ROM enumerates as `2e8a:0003`, a USB
disk named **`RPI-RP2`** with `INFO_UF2.TXT` on it. Copy the `.uf2` there; the board resets
into it. `run.sh --flash` checks for `2e8a:0003` first and says so if the button was not
pressed, then runs `west flash --runner uf2`, which finds the mount by that file's
`Board-ID`. If the desktop does not auto-mount removable media the runner reports "No
matching UF2 partitions found"; `udisksctl mount -b /dev/disk/by-label/RPI-RP2` is the fix.

## Bench sequence

Environment first (every shell):

```
cd ~/workspace/rete/nodus/oevm/zephyr && source ~/zephyr-esp32/.venv/bin/activate && \
export ZEPHYR_BASE=~/zephyr-esp32/zephyr ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.17.4 ZEPHYR_TOOLCHAIN_VARIANT=zephyr
```

1. **Layout** (`layout-rp2040.json`; the M33 build needs its own `layout-rp2350.json`
   the same way). Start the reader, THEN put the board in BOOTSEL and drop the file —
   `dump_layout.c` prints once, 3 s after the reset:

   ```
   python3 layoutdump/capture.py <PORT> layout-rp2040.json --seconds 45 --ref layout-esp32c6.json
   ```
   and copy `layoutdump/build-feather/zephyr/zephyr.uf2` (already built) onto `RPI-RP2`.
   The script refuses a truncated or foreign capture and reports whether the result is
   byte-identical to the C6's — expected yes (esp32 = esp32c6 = nrf52840 so far), still
   a measurement. Rebuild the dump if `src/oevm.h` has changed since:
   `west build -b adafruit_feather_rp2040 -p always -d layoutdump/build-feather layoutdump -- -DCONF_FILE=prj_feather_rp2040.conf`

2. **TimerNode with a persistent bootId** — the thing this board is for:

   ```
   bash run.sh --board feather_rp2040 --node TimerNode.nsl --flash
   ```
   Board in BOOTSEL first. Then tap RESET twice more: the console's `bootId` should read
   1 → 2 → 3. That is `oevm/bootid` in NVS on `storage_partition` (the last 64 KB of the
   8 MB flash, carved in the dts) surviving resets — rete#162's seam on RP2 flash.

3. **HanoiNode** for an M0+ activation figure, labelled as such, and `FloatSmokeNode`
   (soft-float here — the M0+ has no FPU). Neither goes on #301.

## What this board taught, to carry to the Pico 2

- `CONFIG_TASK_WDT_MIN_TIMEOUT + HW_FALLBACK_DELAY` must be ≤ 8.39 s here (errata
  RP2040-E1 halves the watchdog ceiling); the Pico 2's 16.78 s takes the C6's 11 s.
- Trial link at the C6's 128 KB arena: RAM 200140 B of 264 KB (74 %) with NVS +
  settings in; the Pico 2 sits at 37 % of 520 KB with neither.
