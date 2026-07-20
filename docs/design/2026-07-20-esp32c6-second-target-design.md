# ESP32-C6 as a Second Zephyr Target — Design

**Date:** 2026-07-20
**Status:** approved (design), unimplemented
**Branch:** `feat/esp32c6-target`, cut from `master` after `feat/amg8833-thermal` merges
**Successor rung:** Waveshare 2.13" e-Paper (SSD1680) display seam — deliberately *not* in this design.

---

## Goal

Stand up `esp32c6_devkitc/esp32c6/hpcore` as a **second, peer** ESP32 target for tuvm,
without disturbing the working ESP32-WROOM-32 path. Deliver a defensible measurement of
how much more VM heap the C6 affords, so the decision to promote it to primary rests on
numbers rather than datasheet totals.

**Bench gate:** `TimerNode.nsl` runs on the C6 under Model B, with the max-linkable
`OEVM_HEAP_SIZE` measured on both boards. WiFi is a *stretch* step, quarantined so it
can fail without sinking the rung.

## Motivation — the real argument is the memory map, not the SRAM total

The WROOM-32 caps `OEVM_HEAP_SIZE` at **53248** (52 KB). Not because 520 KB is small,
but because the Xtensa splits SRAM into **separately-addressed IRAM and DRAM banks**, and
the VM arena must live in the ~192 KB DRAM half alongside WiFi's ~40 KB ESP driver heap
and the TCP net-buf pools. `HttpServerNode`'s live high-water is ~52.8 KB — the node runs
at the edge of both DRAM and the VM heap simultaneously.

The ESP32-C6 has **512 KB of unified HP SRAM** — `esp32c6_common.dtsi:98` declares it as a
single region, `reg = <0x40800000 DT_SIZE_K(512)>`, against the Xtensa's separately-addressed
banks. No IRAM/DRAM partition; instruction and data drawn from one pool through a shared
cache. The constraint that has been squeezing this program does not exist in the same form.
That is the thesis this rung tests.

Corollary: **`OEVM_IRAM_HOT` does not apply to the C6, and the build already knows.**
That knob pulls hot code out of flash-XIP into fast IRAM, and it is what removed the
combinator-row layout roulette. `zephyr/CMakeLists.txt:141` already gates it on
`CONFIG_XTENSA`, and lines 144–145 raise a `FATAL_ERROR` if it is forced on for a
non-Xtensa target. **The C6 profile therefore simply does not export it** — no new gating
is needed, and a mistake here fails loudly at configure time rather than silently no-opping.

---

## The C6 is not a drop-in: five board-coupled hazards

These are the concrete places the existing ESP32 path is Xtensa-specific. Each is a
stated decision here so it is not a discovery at bring-up.

### 1. No FPU — confirmed, not assumed

`dts/riscv/espressif/esp32c6/esp32c6_common.dtsi:37` gives the ISA as
**`rv32imac_zicsr`** — no `f`, no `d`. `prj_esp32.conf` sets `CONFIG_FPU=y` with the
comment "ESP32 has an FPU"; on the C6 that comes off and every VM float becomes a
soft-float libcall.

Accepted: this rung optimises for memory, not float throughput.

The float-arena knobs are *predicted* to carry over unchanged — the RISC-V ilp32 ABI keeps
`double` 8-byte aligned, so `struct Float` should remain 16 B and
`OEVM_FLOAT_ARENA_SLAB_CELLS=512` / `BUDGET=4` should still hold. **This is a prediction to
verify, not a fact.** `TimerNode` boxes no floats, so the bench gate will not exercise it;
a deliberate float smoke test is required (see Task 5).

### 2. Console is on the wrong port for the connected cable

The C6-DevKitC-1 exposes two USB ports. As of bring-up only the **native USB-Serial-JTAG**
is enumerating: `303a:1001`, `/dev/ttyACM0`. The CP2102 UART bridge (`/dev/ttyUSB*`) is
absent — that cable is charge-only or unplugged.

But `esp32c6_devkitc_hpcore.dts:20` routes `zephyr,console = &uart0`, i.e. the physical
UART pins feeding the *absent* bridge. Booting as-is prints to a port nobody reads and is
indistinguishable from a hung VM.

**Decision:** the C6 overlay enables the `usb_serial` node
(`esp32c6_common.dtsi:335`, `espressif,esp32-usb-serial`, ships `status = "disabled"`) and
reassigns `zephyr,console`. Kconfig: `CONFIG_SERIAL_ESP32_USB=y`
(`drivers/serial/Kconfig.esp32:15`). Console port default for the profile is
`/dev/ttyACM0`.

### 3. `patch-esp32-rom-ld.sh` is Xtensa-specific and must be board-gated

`tools/patch-esp32-rom-ld.sh` exists because the ESP32's `esp32.rom.newlib-funcs.ld`
force-binds `fflush`/`setjmp`/`longjmp`/`ungetc` to ROM as **strong** symbols (plain
assignment, not `PROVIDE`), and ROM newlib reaches for an ESP-IDF/FreeRTOS TCB reent
pointer that does not exist under Zephyr.

The C6 is RISC-V with an entirely different ROM export table. The patch is meaningless
against a C6 build. **Decision:** gate it to the `esp32` profile, and treat *"does the C6
have its own forced-ROM-newlib problem in a different disguise?"* as an explicit bring-up
question. Expected failure signature if it does: a fault immediately after the first
console output — the exact shape the WROOM-32 gave.

### 4. Flash invocation differs in three ways

| | WROOM-32 | C6 |
|---|---|---|
| `--chip` | `esp32` | `esp32c6` |
| bootloader offset | `0x1000` | **`0x0`** |
| `--flash_size` | `4MB` | `8MB` |

The offset is the dangerous one: RISC-V ESP32 parts changed the convention, and flashing a
C6 image at `0x1000` produces a **silent boot loop**, not an error.

### 5. `layout-esp32.json` is ABI-specific and cannot be reused

Model B's `.oimg` is baked against a struct-layout dump. RV32 field offsets and alignment
differ from Xtensa. The C6 needs its own `layout-esp32c6.json`, generated by running the
`zephyr/layoutdump` stage-0 app **on the C6**.

**This forces task order.** Model B cannot work until layoutdump has booted on the board,
so the first C6 *flash* is the layout dumper, not `TimerNode`. This is the only place where
sequencing is dictated by a hard dependency rather than preference.

---

## Architecture — `run.sh` board profiles (approach A)

`zephyr/run.sh` gains `--board esp32|esp32c6`. Each profile block owns, together:

- `BOARD` target string
- `CONF_FILE`
- `BUILD_DIR`
- esptool invocation: chip, offset, flash size, default port
- the ROM-patch gate (on for `esp32`, off for `esp32c6`)
- the layout JSON path
- the env-knob defaults, including `OEVM_HEAP_SIZE`. The `esp32c6` profile must **not**
  export `OEVM_IRAM_HOT` (see above — `CMakeLists.txt` hard-errors if it is set).

**Rejected alternatives.** A sourced `boards.sh` table (cleaner factoring, but `run.sh` is
deliberately explicit about its wrong-by-defaults — the ladder's lesson is that hiding these
caused the bugs). Moving the seven knobs into Kconfig (most idiomatic Zephyr, kills
export-drift entirely, but it refactors a load-bearing hard-won path while we are trying to
hold it stable — right eventually, wrong now).

**Non-negotiable constraint:** the `esp32` profile reproduces today's values *exactly*,
proven by a byte-identical `zephyr.bin` against a pre-refactor build. The refactor is the
only step that can break working code, so it goes first and proves itself immediately.

### Files

**Create**
- `zephyr/prj_esp32c6.conf` — minimal console config. No `CONFIG_FPU`, no
  `CONFIG_ESP32_USE_UNSUPPORTED_REVISION` (ESP32-silicon-specific), no networking, no
  `CONFIG_SENSOR`/`CONFIG_AMG88XX`. Keeps picolibc, `HEAP_MEM_POOL_SIZE`,
  `MAIN_STACK_SIZE`. Adds `CONFIG_SERIAL_ESP32_USB=y`.
- `zephyr/boards/esp32c6_devkitc_esp32c6_hpcore.overlay` — enables `usb_serial`,
  reassigns `zephyr,console`. **Filename is derived from the board qualifiers** and CMake
  caches overlay discovery (ladder line 704–705): a newly-added overlay needs `-p always`.
- `zephyr/prj_esp32c6_wifi.conf` — stretch layer only.
- `zephyr/layout-esp32c6.json` — **generated, not authored.**

**Modify**
- `zephyr/run.sh` — the `--board` selector and profile blocks.

**Explicitly out of scope:** the AMG8833. It stays on the WROOM-32; the C6 bench is
e-paper-only. The C6 overlay carries no `amg88xx` node.

---

## Measurement — the headline deliverable

**Most of it needs no hardware.** `OEVM_HEAP_SIZE` sizes the static `oevm_heap_store` array
in `gc.c` at build time, so an over-large value fails at **link** with a region-overflow
error. The max-linkable bisect is therefore a pure build loop, runnable on both boards with
no board attached. Only the "…and actually boots" confirmation needs hardware.

The deliverable is a **four-cell table** — {WROOM-32, C6} × {no-WiFi, WiFi} — recording the
max linkable `OEVM_HEAP_SIZE` and the linker's overflow message at the first failing value.

**Why four cells and not one comparison.** The existing 53248 is a *WiFi-loaded* figure.
Quoting it against a no-WiFi C6 number would flatter the C6 by roughly the WiFi driver heap
plus net-buf pools — on the order of 50 KB — and produce a headline that is real but
overstated. The no-WiFi pair is the clean ABI-and-memory-map comparison; the WiFi pair
answers "can the C6 run the demo with room to spare". **Both are reported; neither is quoted
alone.**

Boot-confirmation of the winning C6 value happens at Task 5. A value that links but does not
boot is recorded as such — the reported ceiling is the largest that *boots*.

---

## Task sequence

Ordered by real dependency. Steps 1–3 are hardware-free.

1. **`run.sh` `--board` refactor.** Gate: WROOM-32 `zephyr.bin` byte-identical to a
   pre-refactor build.
2. **`prj_esp32c6.conf` + overlay + C6 build of layoutdump.** Gate: it links.
3. **Heap-ceiling bisect**, build-only, both boards, both WiFi configs. Gate: the four-cell
   table. *Headline deliverable, lands before any bench step.*
4. **Flash layoutdump on C6** (`--chip esp32c6`, offset `0x0`, `/dev/ttyACM0`); capture
   `layout-esp32c6.json`. *First hardware step.*
5. **Model B bake of `TimerNode.nsl`** against the C6 layout; flash; gate on the timer-tick
   log. **Plus a deliberate float smoke test** — `TimerNode` boxes no floats, so soft-float
   and the float-arena knobs are otherwise unexercised.
6. **Stretch: WiFi.** `prj_esp32c6_wifi.conf`, re-bisect with WiFi loaded, attempt
   `ThermalHttpNode` minus the sensor. **Allowed to fail without sinking the rung.**
7. **Close-out.** Promote durable knowledge to `review/program-sequence.md` per the
   CLAUDE.md promotion rule; name the absorbing mainline doc.

---

## Risks, in expected order of biting

1. **A C6-specific forced-ROM-newlib fault.** Surfaces at Task 5 as a fault right after
   first console output. Mitigated by knowing the signature in advance.
2. **WiFi undeclared for the board.** `esp32c6_devkitc_hpcore.yaml` lists `spi`, `i2c`,
   `gpio`, `uart`, `dma`, `netif:openthread` — **not `wifi`**. However the espressif HAL
   ships the full C6 blob set (`libnet80211.a`, `libpp.a`, `libphy.a`, `libcoexist.a`) and
   `drivers/wifi/esp32/Kconfig.esp32:347` lists `SOC_SERIES_ESP32C6` in its `depends on`.
   That `supported:` list drives twister filtering, not capability, so WiFi *should* build —
   but it is unproven here. Quarantined in Task 6 by design.
3. **Soft-float changing float-arena behaviour** enough that carried-over knob values
   misbehave. Surfaces at Task 5's float smoke test, which exists for this reason.
4. **The `run.sh` refactor silently perturbing the WROOM-32 path.** Mitigated by the
   byte-identical gate at Task 1 — the earliest possible detection.

## Success criteria

- WROOM-32 path provably unchanged (byte-identical binary).
- `TimerNode` runs on the C6 under Model B, observed on `/dev/ttyACM0`.
- Float smoke test passes on soft-float.
- Four-cell heap table recorded, with the WiFi/no-WiFi distinction stated wherever the
  headline number is quoted.
- WiFi result recorded as pass **or** as a documented failure — either closes the rung.
