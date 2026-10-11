# WeAct Studio RP2350B Core Board — oevm's out-of-tree board (rete#403, found-during)

Zephyr v4.2 has no port for this board; this directory is one, discovered through the
`BOARD_ROOT` both `zephyr/CMakeLists.txt` and `zephyr/layoutdump/CMakeLists.txt` set. Shape
from the in-tree `pimoroni/pico_plus2` (the one RP2350**B** board Zephyr ships), pins from
`HDK/RP2350B_SCH.pdf` in `~/workspace/WeActStudio.RP2350BCoreBoard` (David's clone of the
vendor repo, adjacent to `rete`, never imported).

**Why this board.** Memory and I/O count: the RP2350B is the 80-pin package — **48 GPIO,
8 ADC channels** (the A has 30 and 4) — on the same 520 KB of flat SRAM and the same M33
at 150 MHz. Several were bought for that. The first one booted the unchanged Pico 2 image
on 2026-10-10 (`docs/results/2026-10-10-rp2350-weact-first-boot.md`), which found a kernel
hang (#402) and the XIP-cache cliff (#403) — neither of them this board's.

## What is on it, and what this port claims

| part | where | in the dts |
|---|---|---|
| blue LED | GP25, active high | `led0` |
| user key | GP23, active low, pulled up | `sw0`, and oevm's `pulsegpio` alias |
| BOOT key + RESET circuit | — | BOOTSEL = hold BOOT, tap RESET (no replug needed) |
| uart0 | GP0 TX / GP1 RX — the pair every RP2 port here uses | `zephyr,console` |
| USB-C | the SoC's own USB | `zephyr_udc0`; `usb-console.overlay` puts the console on it |
| primary QSPI flash | **2/4/8/16 MB variants under one part number** | declared **4 MB** until the chip marking is read |
| second QSPI footprint, back of the board | CS on GP0 (`FLASH2_SS`) | **empty on our boards** — nothing declared |
| ADC0–3 | GP40–43 | `adc_default` |

The LED and key pins come from the schematic nets, not yet from a toggled pin. Everything
not listed (SPI, I2C, PWM, the other 44 GPIO) is the application overlay's to claim, as on
the C6. No program slots (rete#310's `program_a` is the C6's), no NVS, no bootId — this tree
has no flash driver for the RP2350's QMI (`prj_rpi_pico2.conf` says why).

## Build and flash

```
cd ~/workspace/rete/nodus/oevm/zephyr && source ~/zephyr-esp32/.venv/bin/activate && export ZEPHYR_BASE=~/zephyr-esp32/zephyr
EXTRA_DTC_OVERLAY_FILE=$PWD/usb-console.overlay \
  ./run.sh --board weact_rp2350b --conf-extra prj_rpi_pico2_usb.conf --node TimerNode.nsl --flash
```

`prj_rpi_pico2.conf` is the RP2350 Kconfig, shared with the Pico 2 — nothing in it is
board-specific. The USB-CDC console fragment is the bench arrangement until an adapter is on
GP0/GP1; it re-enumerates on every reset, so read it with the port held open (DTR asserted)
and expect to lose the first second of each boot. BOOTSEL enumerates `2e8a:000f` as a USB
disk named `RP2350`; `run.sh --flash` checks for it and says so if the key was not pressed.
With no toolchain on the desk machine: build on the bench machine, `scp` the `.uf2`, copy it
onto the mounted `RP2350` volume.

## Layout

`run.sh` points this profile at `layout-rp2350.json`, the Pico 2's capture. The layout is a
property of the toolchain and the ABI, not the board, and this is the same die family under
the same compiler — but the rule here is *captured from a board, never borrowed*. The capture
(`layoutdump/capture.py` with `--board weact_rp2350b`) is owed; until then the borrowing is
stated in the profile rather than hidden.
