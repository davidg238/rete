# ESP32-C6 as a Second Zephyr Target — Design

**Date:** 2026-07-20
**Status:** approved (design), unimplemented
**Branch:** `feat/esp32c6-target`, cut from `master` after `feat/amg8833-thermal` merges
**Tracking issue:** **#57** (evaluate C6 as the dev-loop target). Motivated by **#53**
(dram0 at 99.58%, 816 B spare) and unlocks the debug leg of **#56** (agent-watchable
on-device observability).
**Successor rung:** Waveshare 2.13" e-Paper (SSD1680) display seam — deliberately *not* in this design.

---

## Goal

Stand up `esp32c6_devkitc/esp32c6/hpcore` as a **second, peer** ESP32 target for tuvm,
without disturbing the working ESP32-WROOM-32 path. Deliver a defensible measurement of
how much more VM heap the C6 affords, so the decision to promote it to primary rests on
numbers rather than datasheet totals.

**Bench gate:** `TimerNode.nsl` runs on the C6 under Model B, with the max-linkable
`OEVM_HEAP_SIZE` measured on both boards. Thread/IPv6 is the C6's radio path; WiFi is out
of scope on the C6 (see below).

## Motivation — the real argument is the memory map, not the SRAM total

The WROOM-32 caps `OEVM_HEAP_SIZE` at **53248** (52 KB). Not because 520 KB is small,
but because the Xtensa splits SRAM into **separately-addressed IRAM and DRAM banks**, and
the VM arena must live in the ~192 KB DRAM half alongside WiFi's ~40 KB ESP driver heap
and the TCP net-buf pools. `HttpServerNode`'s live high-water is ~52.8 KB — the node runs
at the edge of both DRAM and the VM heap simultaneously.

The ESP32-C6 has **unified HP SRAM** — `esp32c6_common.dtsi:96-98` declares the `sramhp`
node as a single 512 KB region, `reg = <0x40800000 DT_SIZE_K(512)>`, against the Xtensa's
separately-addressed banks. No IRAM/DRAM partition; instruction and data drawn from one pool
through a shared cache. The constraint that has been squeezing this program does not exist
in the same form. That is the thesis this rung tests.

**Corrected at Task 2 (measured, not predicted):** the *linker's* usable region is
`sram0_0_seg` at **437512 B (~427 KB)**, not the DTS node's 512 KB — `soc/espressif/esp32c6/memory.h`
and `default.ld:65` subtract the icache and bootloader reservations. An earlier revision of
this document claimed 512 KB of usable SRAM; that was the datasheet figure, not the linkable
one. ~427 KB is still more than double the WROOM-32's 192 KB `dram0_0_seg`, so the thesis
holds — but the honest number is 427, and the four-cell table is what actually settles it.

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

### 2. Two ports, used the right way round — console on UART, flash on native USB

Both C6 ports enumerate: **`/dev/ttyUSB0`** (CP210x bridge, `10c4:ea60`) and
**`/dev/ttyACM0`** (native USB-Serial-JTAG, `303a:1001`). Both can flash.

The useful split is the non-obvious one, per **#57**:

- The **native USB port re-enumerates when the target resets** — the host device node
  disappears and comes back, breaking any continuous reader across every reset.
- The **UART bridge stays enumerated across target resets**, because the bridge chip never
  reboots.

**Decision:** **console on the UART bridge** (an always-on log that survives reset —
this is what makes an agent-watchable serial log possible, the debug leg of #56);
**flashing and JTAG on the native USB port.**

This means the board's default routing is already correct —
`esp32c6_devkitc_hpcore.dts:20` gives `zephyr,console = &uart0` — and **no console overlay
is needed.** The `usb_serial` node (`esp32c6_common.dtsi:335`,
`espressif,esp32-usb-serial`, ships `status = "disabled"`) and
`CONFIG_SERIAL_ESP32_USB` (`drivers/serial/Kconfig.esp32:15`) stay unused for console;
they are noted here only so a future reader knows the native-USB console path exists and
was rejected deliberately, not overlooked.

### 2b. Port pinning — a re-armed footgun

`run.sh` hard-pins `ESPTOOL_PORT=/dev/ttyUSB0` for the WROOM-32, after auto-detect
clobbered the jag board on 2026-07-16. **The C6's CP210x now claims `/dev/ttyUSB0` too.**
With both boards attached, enumeration order decides which one gets flashed — the same bug,
re-armed.

**Decision:** board profiles pin by stable path under `/dev/serial/by-id/` rather than by
`ttyUSB0`/`ttyACM0`, which are allocation-order names. Overridable by `ESPTOOL_PORT` as
today.

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

### 5. `layout-*.json` must be generated per target — but the RV32 layout turned out identical

Model B's `.oimg` is baked against a struct-layout dump. The C6 needs its own
`layout-esp32c6.json`, generated by running the `zephyr/layoutdump` stage-0 app **on the
C6**.

**This forces task order.** Model B cannot work until layoutdump has booted on the board,
so the first C6 *flash* is the layout dumper, not `TimerNode`. This is the only place where
sequencing is dictated by a hard dependency rather than preference.

**Corrected at Task 4 — the stated justification was wrong.** This section originally
claimed "RV32 field offsets and alignment differ from Xtensa, so `layout-esp32.json` cannot
be reused." **That is false for these structs.** The measured C6 dump is byte-identical to
the Xtensa one — every struct size and every field offset matches, `struct Float` included
(16 B on both, `value` at offset 8). Verified as a real result rather than a bad capture:
esptool's chip probe confirmed the part, the build used `-mabi=ilp32 -march=rv32imac`, and
`tools/dump_layout.c` computes everything through `sizeof`/`offsetof` with nothing
hardcoded. None of these structs contains a field whose natural alignment diverges between
Xtensa and RV32 ilp32.

The dump was still the right thing to do — the identity was unknowable without measuring,
and Model B requires the file to exist — but the *reason given* was an assumption presented
as fact. The durable lesson is narrower than the original claim: **generate a layout per
target because you cannot know it matches until you look**, not because it necessarily
differs. A future target with different alignment rules (an ABI where `double` is 4-byte
aligned, or a 64-bit part) would genuinely differ; RV32 ilp32 does not.

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
- `zephyr/prj_esp32c6.conf` — console + **minimal net stack, no radio**. No `CONFIG_FPU`,
  no `CONFIG_ESP32_USE_UNSUPPORTED_REVISION` (ESP32-silicon-specific), no
  `CONFIG_SENSOR`/`CONFIG_AMG88XX`. Keeps picolibc, `HEAP_MEM_POOL_SIZE`, `MAIN_STACK_SIZE`.
  Console needs no overlay — the board DTS already routes `zephyr,console = &uart0`.
- `zephyr/prj_esp32c6_thread.conf` — OpenThread / IPv6 layer. **The supported radio path.**
- `zephyr/boards/esp32c6_devkitc_esp32c6_hpcore.overlay` — enables the `ieee802154` node
  (`esp32c6_common.dtsi:85` ships it `status = "disabled"`). **Filename is derived from the
  board qualifiers** and CMake caches overlay discovery (ladder line 704–705): a
  newly-added overlay needs `-p always`.
- `zephyr/layout-esp32c6.json` — **generated, not authored.**

**WiFi is out of scope on the C6** (decision, 2026-07-20). The board yaml declares
`netif:openthread` and does *not* declare `wifi`; Thread is the supported path and is
sufficient for this program's networking. Dropping WiFi removes the rung's only
allowed-to-fail task. Revisit only if a future rung needs an IP uplink the border router
cannot provide.

**Modify**
- `zephyr/run.sh` — the `--board` selector and profile blocks.

**Explicitly out of scope:** the AMG8833. It stays on the WROOM-32; the C6 bench is
e-paper-only. The C6 overlay carries no `amg88xx` node.

**Note on the thermal rung's Task 9** (phase-2 threshold trigger): #53 records that
switching off `CONFIG_AMG88XX_TRIGGER_NONE` adds a driver thread with its own stack, and
**that may not fit in 816 B of spare DRAM.** Task 9 is therefore not a blocker for merging
the thermal branch, and is a natural candidate to land *on the C6* once this rung proves
the headroom.

---

## Measurement — the headline deliverable

**Most of it needs no hardware.** `OEVM_HEAP_SIZE` sizes the static `oevm_heap_store` array
in `gc.c` at build time, so an over-large value fails at **link** with a region-overflow
error. The max-linkable bisect is therefore a pure build loop, runnable on both boards with
no board attached. Only the "…and actually boots" confirmation needs hardware.

The deliverable is a **four-cell table** recording the max linkable `OEVM_HEAP_SIZE` and the
linker's overflow message at the first failing value:

| | no radio (net stack only) | with radio |
|---|---|---|
| **WROOM-32** | measure | 53248 (WiFi, already documented) |
| **C6** | measure — *the clean memory-map comparison* | measure (Thread/IPv6) |

**Every cell carries a net stack.** `prj_esp32c6.conf` cannot omit networking: `src/udp.c`
and the Zephyr UDP/HTTP backends are in `target_sources` unconditionally, because
`dispatch.c`'s `send()` fast path probes `udp_object()` on every send. So "no radio" means
net stack without a radio driver, not without sockets. Discovered at Task 2.

**Why four cells and not one comparison.** The existing 53248 is a *radio-loaded* figure.
Quoting it against a no-radio C6 number would flatter the C6 by roughly the WiFi driver heap
plus net-buf pools — on the order of 50 KB — and produce a headline that is real but
overstated. The no-radio pair is the clean ABI-and-memory-map comparison; the with-radio
pair answers "can the C6 run a real workload with room to spare". **Both are reported;
neither is quoted alone.** Note the with-radio cells are not like-for-like — WiFi and
OpenThread have different footprints — so that column compares each board against its own
usable radio, not against each other.

Boot-confirmation of the winning C6 value happens at Task 5. A value that links but does not
boot is recorded as such — the reported ceiling is the largest that *boots*.

---

## Task sequence

Ordered by real dependency. Steps 1–3 are hardware-free.

1. **`run.sh` `--board` refactor.** Gate: WROOM-32 `zephyr.bin` byte-identical to a
   pre-refactor build.
2. **`prj_esp32c6.conf` + overlay + C6 build of layoutdump.** Gate: it links.
3. **Heap-ceiling bisect**, build-only, both boards, both radio configs. Gate: the four-cell
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
2. **OpenThread on RISC-V is unproven in this tree.** The `ieee802154_esp32.c` driver
   exists, `esp32c6_common.dtsi:85` has the node, and the board yaml declares
   `netif:openthread` — but none of that is a green link. Surfaces at Task 6. Also note
   OpenThread is not RAM-cheap (`prj_dk_thread.conf` reserves a 6240 B thread stack plus
   net-buf pools), so the Thread cell will not be the C6's best-case heap number.
3. **Soft-float changing float-arena behaviour** enough that carried-over knob values
   misbehave. Surfaces at Task 5's float smoke test, which exists for this reason.
4. **The `run.sh` refactor silently perturbing the WROOM-32 path.** Mitigated by the
   byte-identical gate at Task 1 — the earliest possible detection.

## Success criteria

- WROOM-32 path provably unchanged (byte-identical binary).
- `TimerNode` runs on the C6 under Model B, observed on `/dev/ttyACM0`.
- Float smoke test passes on soft-float.
- Four-cell heap table recorded, with the radio/no-radio distinction stated wherever the
  headline number is quoted.
- Thread/IPv6 builds and its heap cost is measured.
