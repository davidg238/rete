# MagTag DAC bell — design

**Date:** 2026-07-23
**Issue:** tuvm#94 (rescoped), closing out tuvm#86's "not done here"
**Board:** Adafruit MagTag (ESP32-S2-WROVER), board target `esp32s2_saola/esp32s2`
**Branch:** `feat/magtag-esp32s2-port`

## What changed about #94

#94 was filed as *the ship's-bell clock*: WiFi/NTP + DAC + e-paper converging on one
board, with the arena arithmetic as its gating risk. **That scope is withdrawn.** The
clock face is dropped; what is wanted later is a *notification appliance* (an external
alert lights the RGB LED, writes a line, and a button press acknowledges it), which
needs no timekeeping at all.

So this rung is **sound only**, and the entire arena question is moot — nothing competes
for RAM or CPU. The findings that the arithmetic produced are recorded below anyway,
because they were expensive to get and they will be wanted when the appliance rung comes
round.

## Findings preserved from the withdrawn scope

**1. `CONFIG_ESP_WIFI_HEAP_SPIRAM` reclaims 21,504 B of dram0.**
`zephyr/drivers/wifi/esp32/Kconfig.esp32:24-27` — `HEAP_MEM_POOL_ADD_SIZE_ESP_WIFI`
is `40960 if ESP_WIFI_HEAP_SYSTEM` but `19456 if ESP_WIFI_HEAP_SPIRAM`. This is #85's
"reclaim path", except it is a first-class Kconfig choice rather than the bespoke work
#85 assumed: `ESP_SPIRAM` is available for `SOC_SERIES_ESP32S2`, `select`s
`SHARED_MULTI_HEAP`, and `ESP_SPIRAM_SIZE` defaults straight from the devicetree, where
`esp32s2_wrover_n4r2.dtsi:15` **already declares the MagTag's 2 MB**. Plausibly two
Kconfig lines, with `ESP_SPIRAM_MEMTEST=y` (default) giving a free boot-time gate.
Unverified on hardware.

**2. WiFi's cost is static, paid at link time.** Associating or not makes no difference;
"sync at boot then disconnect" reclaims nothing.

**3. Estimated combined ceiling without PSRAM was ~25 KB** (display ceiling 94208 from
#88, minus #85's WiFi cost, minus the 8192 that #88's `HEAP_MEM_POOL_SIZE` raise took
after #85 was measured). Never verified — the scope was withdrawn first.

**4. The 180 s refresh guidance is real and applies to mono panels.** Adafruit's own
`il0373_flexible_2.13_monochrome.py:48` ends with `time.sleep(180)`. It is a convention,
not a hardware interlock — the driver exposes `refresh_time` as a settable minimum
(`il0373_2.9_grayscale.py:38` uses `refresh_time=1`). **A one-row update does not escape
it today**, because #88's `cfb_framebuffer_finalize_area` still drives the OTP waveform —
which is why gate 2 of #88 flashed black-then-white. A partial *area* under a full-refresh
waveform is the same electrical drive over fewer rows: smaller, not gentler. Only a real
partial LUT in the empty `partial { }` node changes that. Recorded on tuvm#89.

## The rung

A bell struck on the MagTag's onboard buzzer via the **DAC**, not LEDC.

### Why not PWM — and what PWM already achieved

PWM is not a failure to be fixed. #86 proved the whole audio path with LEDC: three
ascending tones (E6/G6/C7) heard on the bench. What LEDC cannot do is exactly what a bell
needs:

- **No low fundamental.** `SOC_LEDC_TIMER_BIT_WIDTH` is 14 on the S2, so the driver must
  fit `clock/freq` in the counter; ~1 kHz is the floor (500 Hz → `-EINVAL`).
- **No amplitude control**, so no decay envelope.

### Why the cosine generator, not sample streaming

Zephyr's DAC API cannot stream. `drivers/dac/dac_esp32.c:28` — `dac_write_value()` →
`dac_output_voltage()`, one 8-bit DC level per call; the whole `dac_driver_api` is
`channel_setup` + `write_value`. There is no buffer, no DMA, no continuous mode. So #86's
"stream unsigned 8-bit PCM" is not an API call. Three routes exist:

| Route | Cost | Verdict |
|---|---|---|
| **Hardware cosine generator** | 0 RAM, 0 CPU, no new driver | **Chosen for gates 1-2** |
| CPU-timed per-sample writes | blocks a thread; fixed-point (S2 has no FPU) | deferred to gate 3+ if needed |
| DAC DMA (`dac_digi_*`) | PCM buffer, SPI3/DMA wiring, deprecated API | not now |

The cosine generator is verified present for this part: `dac_cw_generator_config/enable`
live in `components/driver/deprecated/dac_common_legacy.c`, which
`hal/espressif/zephyr/esp32s2/CMakeLists.txt:209-215` compiles under `CONFIG_DAC_ESP32`;
the S2 low-level layer is complete at `components/hal/esp32s2/include/hal/dac_ll.h:110-193`
(enable, `set_freq`, `set_atten`, `set_phase`, `set_dc_offset`). Range is **130 Hz-55 kHz**
with 4 amplitude steps (`DAC_CW_SCALE_1/2/4/8`) — i.e. it reaches well below LEDC's floor
and has the amplitude control LEDC lacks.

Its limit is that it is **one pure oscillator**: a decaying sine, not a bell's inharmonic
partials. That is accepted for now, deliberately — see Gate 2.

## Gates

Each gate is one build, one flash, one listen. Flashing is manual (BOOT + RESET) and the
console is the soldered UART header on `/dev/ttyUSB0` (a PL2303 cable; the native USB does
not enumerate under Zephyr).

**Gate 1 — does the DAC drive the amp, and what is audible?**
Enable `dac@3f408800`; add `CONFIG_DAC` / `CONFIG_DAC_ESP32`. Replace the boot chime body
with a **frequency sweep** at full scale: 220 / 440 / 880 / 1760 Hz, ~1 s each. A sweep
rather than a single tone because the open risk is that a sine is *quieter* than a square
wave on this tiny PAM8302-driven buzzer (a square carries far more harmonic energy), and
one frequency cannot distinguish "the DAC is silent" from "this buzzer will not reproduce
220 Hz". This is the same move that found LEDC's floor in #86.

**Gate 1 RESULT — PASSED on hardware 2026-07-23.** All four tones audible:

```
E: DAC clock setup failed (-5)
bell: dac_output_enable rc=0
bell:  220 Hz rc=0      bell:  440 Hz rc=0
bell:  880 Hz rc=0      bell: 1760 Hz rc=0
bell: swept
```

Two findings, one of them the opposite of what was predicted:

1. **220 Hz and 440 Hz are audible** — both below LEDC's floor on this part (#86
   measured 500 Hz → `-EINVAL`). The DAC reaches a register PWM cannot. That is the
   whole justification for the rung and it is now evidence.
2. **1760 Hz sounded BAD; the low tones sounded fine.** The stated risk was the
   reverse — that a sine would be too weak for a tiny buzzer where a square wave's
   harmonic energy carried. Wrong: the usable range is the BOTTOM of the range. This
   is why the gate swept instead of playing one note.

**Gate 1 also found an upstream Zephyr bug (see below), which cost one build.**

**Gate 2 — a strike, not a tone.**
Add the 4-step decay (`SCALE_1 → 2 → 4 → 8`, then disable) and the ship's-bell pair
phrasing: *ding-ding … ding-ding … ding* for an odd count. **Then listen and judge.** If a
single decaying sine does not read as a bell, that is the decision point for fixed-point
additive synthesis (~6 inharmonic partials, per-partial decay, ~22 kHz) — affordable now
that nothing else contends for the CPU, but not worth building before the cheap path has
been heard.

**Gate 3 — the nsl seam.**
Promote to a prim so nsl strikes the bell rather than boot-time C, closing #86's "not done
here". Witness: **button press → N strikes**, self-contained and needing no time source,
using the buttons already proven in #82/#84.

Gates 1-2 stay boot-time C in `magtag_chime()` deliberately: a silent result then has
exactly one suspect. Only gate 3 introduces a seam.

## Constraints carried forward

- **The amp needs ~50 ms to settle.** GPIO16 (`SPEAKER_ENABLE`, HIGH = un-muted) → wait →
  then drive. Driving in the same breath is silent, and that silence reads exactly like a
  driver fault. Cost three builds in #86.
- **Notes must be long.** 160 ms returned `rc=0` and was inaudible; 500 ms was loud.
  `rc=0` is not evidence of a physical effect.
- **GPIO17 is both `LEDC_CH0_GPIO17` and `DAC_1`.** Two peripherals driving one pin is the
  same race already documented in this overlay for i2c0-vs-SPI-CS on GPIO8, with the same
  signature: `rc=0`, silence, looks like a driver fault. Avoided here because `CONFIG_PWM`
  was **never committed** — `5d7fdd8` touched only the overlay and `main.c`, so the LEDC
  chime is inert in the tree and its pinctrl route is not applied. If PWM is ever enabled
  alongside the DAC, disable `&ledc0` first.
- **Pitch is uncalibrated.** `dac_ll_cw_set_freq(freq, rtc8m_freq)` clocks the cosine
  generator from the internal RTC 8 MHz RC oscillator. Irrelevant for a bell; it would
  matter if this seam were reused for anything pitched.

## Upstream Zephyr bug: `dac_esp32.c` treats `-EALREADY` as fatal

Found in gate 1, **confirmed on hardware** (`E: DAC clock setup failed (-5)` = `-EIO`).

`drivers/clock_control/clock_control_esp32.c:502-513` returns **`-EALREADY`** when a
peripheral's module clock is *already on* — a success condition:

```c
if (status == CLOCK_CONTROL_STATUS_ON && !reset_reason_is_cpu_reset()) {
        return -EALREADY;
}
```

`drivers/dac/dac_esp32.c:71` treats any non-zero as fatal:

```c
if (clock_control_on(cfg->clock_dev, (clock_control_subsys_t)cfg->clock_subsys) != 0) {
        LOG_ERR("DAC clock setup failed (%d)", -EIO);
        return -EIO;
}
```

So `dac_esp32_init` fails, the device never becomes ready, and `device_is_ready()` is the
only symptom. **`drivers/adc/adc_esp32.c:668` calls the same function for the same clock
module and discards the return value entirely** — on the S2 both DAC and ADC hang off
`ESP32_PERIPH_SARADC_MODULE`, so the ADC driver already tolerates exactly what the DAC
driver dies on. That asymmetry is the strongest evidence it is a bug rather than a design
choice. One-line fix: accept `-EALREADY`.

**Not patched locally.** `~/zephyr-esp32/zephyr` is on branch `tuvm-c6-radio-fix` and must
not accrete casual patches. tuvm routes around it instead by taking the pin enable from
`dac_output_enable()` — which costs nothing, because the tone has to come from the legacy
API regardless, so this *removes* a dependency rather than adding a workaround.
`CONFIG_DAC_ESP32` is still required: it is what compiles `dac_common_legacy.c`.

**Second-order lesson, already promoted into `prj_esp32s2.conf`:** this build has
`CONFIG_PRINTK` but not `CONFIG_LOG`, so every driver `LOG_ERR` compiles to nothing. The
failure was invisible until `CONFIG_LOG_MODE_MINIMAL` was switched on (~1 KB, no backend
thread). A silent failure is not a missing failure — the same family as #86's "`rc=0` is
not evidence of a physical effect".

## Open risks

- Whether `driver/dac.h` is on the app's include path — a build detail, discovered by
  building.
- Whether the buzzer reproduces a low fundamental audibly. If gate 1 is disappointing the
  answer is probably a higher fundamental, not abandoning the DAC.

## Promotion

On close-out the durable knowledge lands in `review/program-sequence.md` (the rung) and
`zephyr/docs/BOARD_PORTS.md` (the MagTag's capabilities). The preserved findings above go
to tuvm#85 (PSRAM WiFi heap) and tuvm#89 (the 180 s / partial-LUT answer).
