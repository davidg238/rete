# oevm on Zephyr — board ports

How oevm/nsl-zephyr runs the **same VM + baked image** on **multiple hardware
targets with different IO**, via **per-board builds** — nRF52840 (Thread/mesh) and
Espressif (ESP32 / ESP32-C6 / ESP32-S2) families. This is the design doc; the
*why* behind every Kconfig choice lives in [`ZEPHYR_LESSONS.md`](./ZEPHYR_LESSONS.md),
and the per-rung Espressif port narrative in `review/program-sequence.md`.

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

Build — the config-D RAM knobs are LOAD-BEARING on every 256 KB nRF52840 target, not
DK-only tuning: without `VM_STACK_INIT_WORDS=4096` the VM boots by allocating its default
16384-word (64 KB) execution stack inside the 49152-byte arena — a guaranteed OOM whose
handler used to print only to stderr (NOT wired to the Zephyr console) and abort(), i.e.
the board died silently while OpenThread kept it attached to the mesh. That exact omission
is what kept the dongle dead through rungs 7–8 (fixed 2026-07-12). TWO belt-and-braces
fixes since: **config-D is now BAKED into zephyr/CMakeLists.txt as the default for every
`CONFIG_SOC_NRF52840` build** (env vars still override, for experiments), and the OOM
message now also goes through `platform_console_write` so it can never be invisible.
The dongle additionally needs `PM_STATIC_YML_FILE` or the partition manager mounts
settings NVS on the Open Bootloader's flash pages, which kills USB enumeration:
```
west build -b nrf52840dk/nrf52840     --build-dir build-dk     . -- -DCONF_FILE=prj_dk_thread.conf     -DEXTRA_CONF_FILE=prj_dk_thread_local.conf      # DK
west build -b nrf52840dongle/nrf52840 --build-dir build-dongle . -- -DCONF_FILE=prj_dongle_thread.conf -DEXTRA_CONF_FILE=prj_dongle_thread_local.conf \
  -DPM_STATIC_YML_FILE=pm_static_dongle.yml                                                                                                            # dongle
```
**Prefer `zephyr/run.sh --board nrf52840dongle` to the hand-typed line above** (rete#281,
2026-08-30). It owns every board-coupled default here, and it now ASSERTS the three
things this section tells you to check by hand — because a check nobody runs is not a
check, and each of these had already failed silently:

- `settings_storage` below `0xdc000`. The hand-check below was in this document the whole
  time and was still skipped, and the resulting image mounted NVS over the Open
  Bootloader and took the board dark — **no USB at all, app or DFU**. The bootloader
  survived and DFU recovered it, but nothing on the console said so, because a dongle
  has no console until its app publishes one.
- the **blob path** actually `.incbin`'d, not just the model. Model B is the CMakeLists
  default, so a dropped `OEVM_BLOB` still links Model B — bound to `TimerNode.oimg`.
- the node id, read back from the generated `oevm_node_id.c`.

It also flashes the dongle correctly: `west flash --runner nrfutil` hunts for debugger
traits and answers `Unable to find a board` at a dongle in DFU. The working path is
`nrfutil nrf5sdk-tools pkg generate` + `dfu usb-serial`, under the separate
`NRFUTIL_HOME=~/.nrfutil-rete` that holds `nrf5sdk-tools` (the NCS bundle's home is
locked; #48, zero SDK touch).

If you do build by hand, sanity-check `<build-dir>/partitions.yml` (settings at 0xd8000,
`open_bootloader_gap` present) and `compile_commands.json` for the five `-D` knobs.
Note that a bare `-D` after `--` reaches the SYSBUILD cache and **not** the image, which
is why `OEVM_MODEL` / `OEVM_BLOB` / `OEVM_NODE_ID` travel by environment variable now —
`-Dzephyr_OEVM_BLOB=` (image-scoped) was tried and does not work either.
**Where a running dongle lives is part of the port (rete#281, 2026-09-19): never in the
working host's USB hub.** `4ec0` ran `PortaBridgeNodeNrf` for 17.4 days in the desk hub
(`1-1.3`, beside a 5 Gbps SuperSpeed link) and could attach to the mesh **only while the
workstation was asleep or off** — eight windows matching `journalctl --list-boots` /
`PM: suspend` to the second, deaf within seconds of every resume, so every bench
observation of "does not attach" was made inside the failure. On a wall USB charger it
attached in under a minute with the host awake. Flash on a motherboard port (DFU needs the
data link), then move it to a charger or a port with no USB 3 neighbour; the console is lost
by that, and it is lossy there anyway (#286) — the gateway store is the instrument. Conducted
(the data link) vs radiated (the hub's 2.4 GHz noise at the antenna) is deliberately not
separated.

For a silently-dead board, stack `prj_dongle_diag.conf` as an extra conf
(`-DEXTRA_CONF_FILE="prj_dongle_thread_local.conf;prj_dongle_diag.conf"`) — minimal-mode
logging makes ARM fault dumps print via printk instead of dying invisibly.

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
  - **The proof.** `ThreadTimerNode.nsl` is **self-gating**: it sends again only after the
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

### Espressif targets

The nRF matrix above is Thread/mesh-first. The Espressif boards are a different axis —
capacity, WiFi, and display — and their build seams differ (a separate west workspace at
`~/zephyr-esp32`, the `xtensa-espressif_*`/`riscv64` SDK toolchains, esptool flashing, a ROM
`setjmp`/`longjmp` ld patch). **`zephyr/run.sh` owns the whole ESP32 dev loop** (bake → build
→ flash → capture) per `--board`; it does **not** source the west env, so a caller must:
`source ~/zephyr-esp32/.venv/bin/activate && export ZEPHYR_BASE=~/zephyr-esp32/zephyr
ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.17.4 ZEPHYR_TOOLCHAIN_VARIANT=zephyr` first (a missing
env fails silently as `west: command not found`). Full per-rung detail lives in the ladder.

- **ESP32 (WROOM-32)** — ✅ node runs; the perf/latency witness board (#61 spans, #72 profiler).
  Console on a CP210x UART (`/dev/ttyUSB*` — **identify by `esptool read_mac`, never trust the
  number**). Flash offset `0x1000`. Details in the ladder (job-latency + node-profiler rungs).
- **ESP32-C6** — ✅ Thread second target (#59). RISC-V, flash offset `0x0`, a separate CP2102N
  console (`ttyUSB0`) that survives resets, native-USB flash (`ttyACM0`). The C6 radio needed
  four Zephyr 802.15.4 driver fixes (branch `oevm-c6-radio-fix`; one submitted upstream as
  zephyr#114163). Details in the ladder (C6 second-target + radio-blocker rungs).
- **Adafruit MagTag (ESP32-S2-WROVER)** — ✅ **VM boots, four buttons + chording, red LED + level
  read, WiFi, and the onboard buzzer all proven on hardware 2026-07-23** (#60, #82, #84, #85, #86).
  A capacity/display board, **not** perf or mesh:
  no FPU (soft-float — no float figure belongs in the STIC table without "soft-float" on the row),
  and PSRAM is present but not in the VM arena. Specifics:
  - **No board port was written.** `esp32s2_saola/esp32s2` already targets the MagTag's module
    (`esp32s2_saola.dts:9` includes `esp32s2_wrover_n4r2.dtsi`); only board IO needed the overlay
    `boards/esp32s2_saola_esp32s2.overlay` (buttons 11/12/14/15, red LED 13, NeoPixel power gate 21,
    speaker-enable 16 + LEDC channel on 17). Check for an existing board target on the same module
    before porting one.
  - **WiFi works, and costs half the VM arena (#85).** No new code — `CONFIG_WIFI_ESP32` builds for
    the S2 and the existing `backend/zephyr` net stack + `wifi_bringup()` + #72's
    `NET_IPV4_MAPPING_TO_IPV6` carry associate → DHCPv4 → VM. But WiFi's ~40 KB system heap
    overflows dram0 by 32,704 B at the default 64 KB arena; at a **32 KB arena it fits at dram0
    99.51%** (1,088 B spare — the #53 knife-edge again). So WiFi and a large working set do not
    coexist here without PSRAM.
  - **The onboard buzzer works (#86).** SPEAKER = GPIO17 (also DAC_1/A0) via LEDC; SPEAKER_SHUTDOWN
    = GPIO16, HIGH = un-muted. **The amp needs ~50 ms to settle after enable or it is silent** —
    the single biggest time sink of that rung. `rc=0` from `pwm_set` is *not* evidence of audio
    (160 ms notes were inaudible; 500 ms is loud). LEDC bottoms out near 1 kHz, so a real bell
    wants the DAC on the same pin, not PWM.
  - **The DAC strikes a bell (#94), at a pitch LEDC cannot reach.** Swept on hardware: 220/440/
    880/1760 Hz all audible, **440 chosen** — and 440 is *below* the LEDC floor above, so this is
    a tone the PWM path could never have produced. 1760 Hz sounded bad: **the usable range is the
    BOTTOM**, the opposite of what a square-wave-harmonics argument predicts for a tiny buzzer.
    A four-step amplitude ramp (`DAC_CW_SCALE_1/2/4/8`) reads as a *strike*, so no per-sample
    synthesis is needed — the bell costs **zero RAM and zero CPU**.
    - **Zephyr's DAC API cannot generate a tone.** `dac_esp32.c` is `channel_setup` +
      `write_value` — one 8-bit DC level per call, no buffer, no DMA. The tone comes from the
      legacy IDF `dac_cw_generator_*` (130 Hz–55 kHz, 4 amplitude steps), compiled by
      `CONFIG_DAC_ESP32` and on the global include path.
    - **`dac_esp32_init` FAILS on this part (#95)** — it treats `clock_control_on`'s `-EALREADY`
      ("clock already on", a success) as fatal, while `adc_esp32.c` discards that same call's
      return for the same clock module. Take the pin enable from `dac_output_enable()` instead.
    - **GPIO17 is both `LEDC_CH0_GPIO17` and `DAC_1`.** Do not enable `CONFIG_PWM` and the DAC
      together without disabling `&ledc0` — two peripherals on one pin, claimed through different
      register families (pinctrl vs RTC IO), failing as `rc=0` and silence. Same shape as the
      i2c0-vs-e-paper-CS race on GPIO8.
    - **Drive the envelope with `dac_ll_cw_set_atten()`, never `dac_cw_generator_config()`
      (#94 stage 1).** `config()` is the legacy driver's only public route to the amplitude field,
      but it also rewrites the frequency divider, re-stamps phase, and re-measures the uncalibrated
      RTC-8M oscillator — so calling it per envelope step puts three phase discontinuities inside
      one tone, heard on the bench as an echo. Configure **once** per strike, then step
      `dac_ll_cw_set_atten()` (`hal/dac_ll.h`, on the global include path), which touches only
      `SENS.sar_dac_ctrl2.dac_scale1`. Going under the driver skips its mutex — fine here, where
      this TU is the only DAC user and the strike runs on the VM's single cooperative thread.
      Verdict after the fix: **"largely gone", not gone.** Two mechanisms remain and are inherent
      to a 4-step hardware envelope — each -6 dB step is an instantaneous amplitude jump, and the
      strike ends by disabling the generator at 1/8 scale rather than at zero.
  - **nsl drives the bell, via prim 129 `bellStrike:` (#94 stage 1).** `bell_bringup()` enables the
    pin at boot and rings nothing; `bell_target_strike(n)` un-mutes the amp, waits the 50 ms
    settle, strikes, and re-mutes — **per strike, not once at boot**, which is what makes a repeat
    work. The 1-8 range check lives in nsl (`class Bell`), *not* in C, because it is the
    ship's-bell convention rather than a hardware limit; so this seam has two status tags where
    the display seam has three.
  - **Driver failures are SILENT here: these builds have `CONFIG_PRINTK` but not `CONFIG_LOG`,**
    so every driver `LOG_ERR` compiles to nothing. #95 cost a build to that. When a driver
    misbehaves for no stated reason, add `CONFIG_LOG=y` + `CONFIG_LOG_MODE_MINIMAL=y` (~1 KB
    dram0, no backend thread) and re-flash before debugging anything else.
  - **PSRAM: 2 MB, links but boot unproven (#85, #94).** `prj_esp32s2_psram.conf` — nothing
    bespoke: `ESP_SPIRAM` is first-class for this SoC series, `select`s `SHARED_MULTI_HEAP`, and
    the size derives from `esp32s2_wrover_n4r2.dtsi`. Adds a 1 MB `ext_ram_seg`, costs 2,544 B of
    dram0. It matters because **`ESP_WIFI_HEAP_SPIRAM` reclaims 21,504 B** of dram0
    (`HEAP_MEM_POOL_ADD_SIZE_ESP_WIFI` 40960 → 19456). **But that is only half the gap:**
    WiFi's *full* static cost here is ~73 KB, back-computed from #85's measured overflow rather
    than its quoted heap figure, so the arena must also drop from 64 KB to ~32–40 KB.
  - **WiFi + SNTP arena: MEASURED, and the arena has a HARD FLOOR the estimates missed (#94
    stage 4, 2026-07-24).** The shipped value is **`OEVM_HEAP_SIZE=20480`**, which links at
    **96.64% dram0 (~7.4 KB spare)** and boots at `heap 18/20K`. The estimate above (~32–40 KB)
    and #94's planned **16384 are both wrong, in opposite directions** — and 16384 is wrong
    *structurally*, not marginally: with `VM_STACK_INIT_WORDS=4096` the VM allocates a
    4096 × 4 B = **16,400 B** execution stack inside the arena, so a **16,384 B arena cannot
    hold the VM's own stack** and dies at boot with
    `Out of Memory (requested 16400, used 172 of 16384)` before any node code runs.
    **This is the nRF52840 stack-inside-the-arena OOM above (line ~47) one level down** — that
    entry records the failure at the *default* 16384-word stack; this one says the *reduced*
    4096-word stack has a floor too. The general rule, worth applying to any new target:
    **the arena must exceed `VM_STACK_INIT_WORDS × oop_size` by the node's whole working set**,
    and a figure that merely LINKS says nothing about that — 16384 was only ever established as
    a number that links.
    Measured on a host `-m32` build with the device knob values, 20 simulated ticks each:
    16384/4096w **fails at boot**, 18432/4096w fails mid-run, 20480/4096w survives (gc=60),
    24576/4096w survives (gc=15); 16384 survives at a **2048**-word stack (gc=15) or 1024
    (gc=9). So the stack is a genuine second lever — but it is **fixed-capacity, never grows**
    (`src/vm.c:398`) and overflow is a bare `abort()` no node can catch, so shrinking it trades
    a boot failure for a crash on the first deep call. Left on the shelf, deliberately.
    Recipe for pre-flighting any arena WITHOUT the board (this is what turned a certain wasted
    flash into a table): build `dump_layout` **`-m32`** → regenerate the layout (`oop_size: 4`)
    → recompile the node `.oimg` against **that** layout → build the image VM `-m32` with
    `-DOEVM_HEAP_SIZE=<candidate> -DMETHOD_CACHE_SIZE=256 -DPRIM_CACHE_SIZE=256
    -DVM_STACK_INIT_WORDS=4096 -DOEVM_MARK_STACK_SIZE=512` → run under simulated time.
    **Skipping the 32-bit layout is silent**: the VM dies with
    `Message 'OIMG' not understood by receiver` and nothing points at the layout.
    The 64-bit host has wider objects and is the HARDER case, so a fit there implies a fit on
    device — the inference runs one way only.
  - **Out-of-tree, invisible to git:** the Zephyr SDK needs `xtensa-espressif_esp32s2_zephyr-elf`
    installed (`~/zephyr-sdk-0.17.4/setup.sh -t xtensa-espressif_esp32s2_zephyr-elf`); a fresh clone
    cannot build this target without it.
  - **ENTERING download mode is manual; LEAVING it is not.** Hold BOOT0, click RESET — there is no
    reset line for esptool to drive (the soldered Debug header breaks out TX/RX only; the native
    USB belongs to the ROM). But esptool does not need one to get *out*: `ESP32S2ROM.hard_reset()`
    detects USB-OTG and arms the RTC watchdog instead (`esptool/targets/esp32s2.py:291-314`),
    printing `Hard resetting with a watchdog...`. Observed 2026-07-23 — the app booted with no
    hand on the board. **So the flash loop needs a human once, not twice**, and `run.sh`/
    `flash-magtag.sh` said otherwise until that date. Fall back to a manual RESET click only if
    that line is absent (esptool's strapping check can decline) or the console stays silent.
    In download mode the port is `303a:0002` / `usb-Espressif_ESP32-S2_0-if00`; once
    Zephyr is flashed the native USB stops enumerating entirely, so the soldered UART header
    (a separate USB-serial dongle, a PL2303 at `/dev/ttyUSB0`) is the board's *only* console.
    **Opening that header does NOT reboot the board** — it breaks out TX/RX only, so there is no
    DTR/RTS for an auto-reset circuit to drive. (An earlier revision of this entry said the
    opposite; that is the WROOM-32's rule, and it cannot apply to a cable with no reset line. See
    `prj_esp32s2.conf`'s header for the full argument.) Consequence worth exploiting: **a console
    handle can be held across a flash**, so the boot banner is not lost the way it is on the other
    ESP32s. Use pyserial and hold exactly ONE handle — two readers on the port split the byte
    stream and each silently loses lines, which looks like missing output rather than contention.
    `run.sh --board esp32s2 --flash` scopes esptool to the S2 ROM id.
  - **No battery-backed RTC — the clock does not survive a power cut (#94).** The MagTag has no
    dedicated RTC chip and no coin-cell/VBAT backup domain; the "RTC" in the overlay is the
    ESP32-S2's internal RTC-IO domain (deep-sleep-capable GPIOs, and the DAC through `rtc_io_hal`),
    not timekeeping. The internal RTC timer DOES keep time across **deep sleep** on battery
    (relevant to #83), but a full power loss resets it. Consequence: stages 1-3 of the bell clock
    need continuous power (USB or LiPo) or they reboot to 00:00 and lose the time. This is a
    first-class argument for stage 4 (NTP): with WiFi a power loss is a non-event — reboot ->
    free-run from 00:00 -> NTP re-sync in seconds -> correct, so permanent power stops being a
    requirement.
  - **`OEVM_HEAP_SIZE=65536` is provisional** (dram0 at ~81.8% of 217 KB), not a measured ceiling.
  - `flash 0x1000` (Xtensa keeps the ESP32 offset); ROM `setjmp`/`longjmp` patch applies with an
    S2-specific symbol set (`tools/patch-esp32-rom-ld.sh --chip esp32s2`).
  - **Deep sleep needs the patched Zephyr tree, or the image dies at the boot handoff (#94
    stage 5, 2026-07-25).** Zephyr's simple-boot segment walk (`soc/espressif/common/loader.c`
    `map_rom_segments`) classified any segment loading into RTC IRAM (0x40070000 — the wake stub
    `CONFIG_POWEROFF` links) or RTC slow mem (0x50000000, `RTC_DATA_ATTR`) as end-of-image, broke
    early, and skipped the IROM/DROM flash-offset fix-up — the MMU then mapped stale linker
    offsets and boot died with no output right after `libc heap size`, but ONLY on images where
    elf2image had moved the segments (a big `.oimg` incbin does; the minimal deep_sleep sample
    doesn't, which made it look like a config interaction). Fixed in `~/zephyr-esp32/zephyr`
    commit `bc47c6fab9c` (branch `oevm-c6-radio-fix`; upstream candidate) — **a west re-fetch of
    that tree loses the fix and POWEROFF images stop booting again.** Verification without
    hardware: `docs/spikes/2026-07-25-esp32-loader-walk-sim.py` replays the walk over a built
    `zephyr.bin`.
  - **Constants crossing the nsl↔C seam must fit a 32-bit SMI: |v| < 2^30.** The RTC magic
    0x94BE11C0 round-tripped on the host's 61-bit SMI and stored garbage through `rtcPut:` on the
    device — every host test green, every device wake a cold boot. Now 0x14BE11C0 = 348000704.
    The detector for this whole class is re-running the relevant harness against a `-m32` host VM
    (same `-m32 dump_layout` → recompile-the-.oimg recipe as the arena sweep above); the 64-bit
    suite is structurally blind to it.
  - **A reflash is always a cold start.** The BOOT0+RESET chord reads as a POWERON-class reset
    (`rst:0x1`) and RTC slow memory does not survive it, so after any flash the deep-sleep clock
    free-runs from 00:00 and converges at its first watch-end NTP sync (≤4 h). A 1-bell first
    strike after a reflash is the design working, not a retention bug.
  - **Display bringup must be LAZY on a deep-sleep board — the uc81xx driver wipes the glass
    from TWO places at init time.** First the CFB-level push-clear (fixed: `cfb_framebuffer_clear
    (dev, false)`); but the deeper one is `display_blanking_off()`, which uc81xx implements as a
    **full panel refresh of controller RAM that `controller_init` just cleared to white** — so
    any boot that runs bringup flashes the glass white regardless of the CFB fix (witnessed
    2026-07-25 as a full white-black-white "rewrite" seconds before each strike). Fix: bringup
    runs inside `display_target_available()` (which every display prim dispatch gates on), so
    wakes that never paint never touch the panel; the driver's own device init only resets the
    controller and e-paper holds its image unpowered. The watch-end stats then persist across
    strike wakes, photographed. (Same lazy-seam treatment queued for WiFi: tuvm#99.)

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
