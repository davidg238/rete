# tuvm on Zephyr — board ports

How tuvm/nsl-zephyr runs the **same VM + baked image** on **multiple nRF hardware
targets with different IO**, via **per-board builds**. This is the design doc; the
*why* behind every Kconfig choice lives in [`ZEPHYR_LESSONS.md`](./ZEPHYR_LESSONS.md).

## Principle: one core, per-board IO

Everything above the board line is **shared and board-agnostic**:

- the VM (`vm.c`/`gc.c`/`dispatch.c`/…), baked per-program into `src/solidified_image.c`;
- the Zephyr backends `udp_zephyr.c` / `serial_zephyr.c` / `pulse_zephyr.c` (all `zsock_*`/msgq based, no board assumptions);
- the OpenThread bring-up `thread_bringup()` in `src/main.c` (`openthread_run()` + `net_if_up()` + attach-wait);
- the pulse-GPIO seam (`wire_pulse_gpio()` in `src/main.c`, bound to the `pulsegpio` DT alias).

Only **four things vary per board**, and each has a single, well-defined seam:

| Axis | Seam | DK | Dongle |
|---|---|---|---|
| **Console** | `zephyr,console` in the board DTS/overlay | uart0/VCOM (board default) | SoC USB-CDC (overlay) |
| **USB settle** | a `k_sleep` in `main()` gated on USB-CDC console | none | **3 s, required** |
| **Trigger IO** | the `pulsegpio` DT alias → a button pin | BUTTON1 P0.11 | SW1 **P1.06** |
| **Flash method** | runner | `west flash -r jlink` | nRF Util DFU (`-r nrfutil`) |

Everything else (OT MTD, dataset, heap/stack sizes, partial-erase, MANUAL_START,
`net_if_up`) is **common** and lives in a shared config fragment.

## File layout (per board)

```
zephyr/
  prj_dk_thread.conf              # DK: uart0 console + common OT block   [committed, placeholder dataset]
  prj_dk_thread_local.conf        # DK: real dataset overlay              [gitignored]
  prj_dongle_thread.conf          # Dongle: USB-CDC console + picolibc + USB recipe + common OT  [committed]
  prj_dongle_thread_local.conf    # Dongle: real dataset overlay          [gitignored]
  boards/
    nrf52840dk_nrf52840.overlay   # pulsegpio alias -> BUTTON1 P0.11
    nrf52840dongle_nrf52840.overlay  # pulsegpio alias -> SW1 P1.06; console on usb cdc-acm
  src/main.c                      # shared entry: setvbuf, GC hooks, wire_pulse_gpio,
                                  #   [USB-CDC-gated 3s settle], thread_bringup(), run VM
```

Build:
```
west build -b nrf52840dk/nrf52840     . -- -DCONF_FILE=prj_dk_thread.conf     -DEXTRA_CONF_FILE=prj_dk_thread_local.conf      # DK
west build -b nrf52840dongle/nrf52840 . -- -DCONF_FILE=prj_dongle_thread.conf -DEXTRA_CONF_FILE=prj_dongle_thread_local.conf  # dongle
```

To keep the common OT block DRY, factor it into `prj_thread_common.conf` and list
it in each board's `CONF_FILE` (`-DCONF_FILE="prj_thread_common.conf;prj_dk_thread.conf"`).
(Rung-7 shipped the DK conf standalone; the dongle port is the moment to extract
the common fragment.)

## The USB-settle gate (board-conditional in one place)

The 3 s settle must run on USB-CDC-console boards and must **not** run on the DK
(uart0). Gate it on the console being USB-CDC rather than per-board `#ifdef`, so a
future USB board inherits it automatically:

```c
/* in main(), before thread_bringup() */
#if DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_console), zephyr_cdc_acm_uart)
    k_sleep(K_SECONDS(3));   /* USB-CDC enumerate before OT/console; see ZEPHYR_LESSONS §1 */
#endif
```

## Board matrix

- **nRF52840-DK** — ✅ **DONE** (rung-7 `BRIDGE OK`). The reference target. Console
  uart0/VCOM (immune to Issue A), trigger BUTTON1 P0.11, J-Link flash. Use the DK
  for all Thread/debug work.
- **nRF52840 Dongle** — ✅ **PORTED + ATTACHED, and HEADLESS.** Same SoC/256 KB RAM, so the
  footprint fits; DFU-flashed, joins the porta mesh as an MTD child. But it consoles over
  the SoC USB-CDC and inherits the **unsolved Issue A** (USB-CDC dies under radio — see
  [`ZEPHYR_LESSONS.md`](./ZEPHYR_LESSONS.md) §1; picolibc did not beat it).

  **Issue A is now routed around, not blocking.** Rung 8 removed the two things that made a
  console necessary:
  - **The trigger.** A real `k_timer` backend for the `Timers` seam
    (`src/backend/zephyr/timer_zephyr.c`) drives a **15 s autonomous cadence**, so the node
    needs no BUTTON1 press — which was never practical on a stick in a USB port anyway.
  - **The proof.** `ThreadTimerNode.ns` is **self-gating**: it sends again only after the
    previous reflection decoded value-equal, so the gateway's *outbound-only* echo log
    witnesses full round-trip closure (**N lines = N−1 proven round-trips**), and a broken
    inbound leg stops the train dead. Calibrated on the DK, where console and train were
    shown to tick in lockstep. See `zephyr/README.md` "Rung 8".

  **Remaining (needs hands):** DFU-flash `ThreadTimerNode` and confirm the train. DFU mode =
  hold RESET while inserting (enumerates `1915:521f`), then
  `west flash --skip-rebuild --runner nrfutil`. Verify from the gateway: a child row in
  `ot-ctl child table`, and ≥4 `udp-echo6` lines ~15 s apart.

  Port checklist (done):
  1. `prj_dongle_thread.conf` = common OT block **+ picolibc** (`PICOLIBC=y` /
     `PICOLIBC_IO_FLOAT=y`; drop the newlib lines) **+** USB-CDC recipe
     (`USBD_CDC_ACM_WORKQUEUE=y`, `UART_LINE_CTRL=y`, CDC-ACM console) **+**
     settings partition placed outside the DFU-erase region.
  2. dongle overlay: `zephyr,console` → cdc-acm-uart; `pulsegpio` alias → SW1 P1.06.
  3. the DT-gated 3 s settle (above) activates automatically.
  4. flash via DFU (`west flash -r nrfutil`, or the `nrfutil pkg generate` + `dfu
     usb-serial` flow); dongle enters DFU by holding RESET on insert (`1915:521f`).
  5. **read the console with pyserial `dtr=True`, not `cat`** (DTR must be asserted).
  6. **Verification without the console:** the node's *function* (attach +
     round-trip) does not need the console. Even if Issue A silences it, confirm the
     round-trip from the **gateway** side: `ot-ctl child table` (attached) + a fresh
     `udp-echo6` log line `[fdc5:…]:<port>` (outbound reached the echo). `BRIDGE OK`
     on the node console is a bonus, gated on Issue A being beaten (picolibc is the
     bet).
- ~~Thingy:52~~ — **not a target.** It is nRF52832 (64 KB RAM); OT MTD + mbedTLS + a
  ~48 KB VM arena will not fit. (Dropped 2026-07-11.)

## Adding a new board (generic recipe)

1. Confirm the SoC RAM budget vs [`ZEPHYR_LESSONS.md`](./ZEPHYR_LESSONS.md) §5
   (OT+mbedTLS+arena needs ≳150 KB; sub-256 KB parts likely won't fit an OT node).
2. Copy `prj_dk_thread.conf` → `prj_<board>_thread.conf`; keep the common OT block,
   adjust console/libc/USB per the board's console type (§1/§6 of the lessons doc).
3. Add `boards/<board>.overlay`: `pulsegpio` alias → the board's button pin; set
   `zephyr,console` if not the board default.
4. If the console is SoC USB-CDC: the DT-gated settle activates; use DFU flashing;
   read with DTR asserted; expect Issue A until proven otherwise.
5. Bring-up order to debug a silent board: `RESET_ON_FATAL_ERROR=n` + SWD/RTT first,
   then walk §2/§4 of the lessons doc (stack size, NET_CONFIG, partial-erase).
