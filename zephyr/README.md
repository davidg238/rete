# tuvm on Zephyr — native_sim bring-up gate (Task 7)

Boots the tuvm VM under Zephyr's `native_sim` (a HOST executable, no hardware
needed) and proves one pulse end-to-end: a simulated GPIO edge posts a value
onto a channel, and the booted node's `whenever:do:` reaction observes it.

This is the **Model A** ("solidified image") boot path: the app is compiled
AHEAD OF TIME into a self-contained `src/solidified_image.c` — a per-program
baked C file with no `.oimg` blob, no loader, no `kernel_boot.c` (that file
is Model B/`.oimg`-only and its static `Vtable_*` definitions would clash
with the ones `solidified_image.c` bakes for this specific program — see
`tools/run_tests_oevm.py`'s `run_c_unit_tests()` docstring in the nsl-tuvm
repo for the identical constraint on the host build).

## Files

- `CMakeLists.txt` — pulls the engine `.c` files from `../src` (this repo)
  plus the zephyr backend (`../src/backend/zephyr/`), and this directory's
  own `src/main.c` + `src/solidified_image.c`.
- `prj.conf` — minimal console + heap/stack sizing + the native_sim
  networking (NSOS) block. Networking is enabled even though the gate node
  itself never grants a `net` capability: `src/dispatch.c`'s `send()` fast
  path unconditionally calls `udp_object()`/`serial_object()` to test
  receiver identity, so `src/udp.c` + `src/backend/zephyr/udp_zephyr.c` must
  link regardless, and that file needs `<zephyr/net/socket.h>` wired up.
- `src/main.c` — the Zephyr entry point: boots the solidified image
  (`init_solidified_vtables()` / `init_reflection_symbols()` /
  `run_solidified_entry_point()`), matching `../src/oevm_main.c` (the host
  Model-A appliance driver) exactly, swapping only the console and the
  in-language millis clock (`k_uptime_get()`, via the `oevm_millis` hook
  `src/vm.c` already reserves for "the Zephyr appliance").
- `PulseNode.ns` — the gate node (source of truth for `src/solidified_image.c`
  — see "Regenerating the image" below).

## The gate node

`PulseNode.ns`'s `main:` registers a `whenever: '' do:` reaction on channel
`sensor/alert`, registers pin 13 to post the String `'done'` on that channel,
fires ONE simulated edge (`gpio fire: 13` — host/sim-only, the on-target
GPIO ISR replaces this in Task 8), then runs the C-owned pump loop:
`[ true ] whileTrue: [ platform pumpEvents. space drain ]`.

Two deliberate deviations from the plan's draft node (both required to
actually compile/run against this VM + compiler, not stylistic — see the
comment header in `PulseNode.ns` for detail):
- `[ ... ] repeat` is not a supported selector in this dialect (grep-confirmed
  against `tools/ns_compile_oevm.py`'s compiler and `src/dispatch.c`'s
  `PRIM_NAMES` — only `whileTrue:`/`whileFalse:`/`timesRepeat:`/`to:do:`/
  `to:by:do:` are). Rewritten as the equivalent `[ true ] whileTrue:`.
- the posted message is the **String** `'done'`, not the **Symbol** `#done`:
  a genuine Symbol's `printString` prints with a leading `#`
  (`src/dispatch.c`'s `PRIM_printString`), which would print `observed #done`
  — not the required `observed done` substring. A String prints as itself.

On native_sim the zephyr pulse backend's `pulse_pump`
(`../src/backend/zephyr/pulse_zephyr.c`) blocks (`K_FOREVER`) once the
one-edge mailbox is drained, so the process prints the observation and then
idles forever — expected; the run is checked under `timeout`, not by process
exit.

## Regenerating the solidified image

`src/solidified_image.c` (and its `.stmap` sidecar) are **generated
artifacts**, committed to `tuvm` only as whatever some OTHER test last left
there — they are NOT the Task 7 gate's own source of truth (`PulseNode.ns`
is). Building this gate CLOBBERS them as a transient build step. Regenerate
from `PulseNode.ns` before every `west build`:

```bash
source /home/david/workspace/ns-zephyr/tools/env.sh
cd /home/david/workspace/nsl-tuvm
python3 tools/ns_compile_oevm.py \
  /home/david/workspace/tuvm/zephyr/PulseNode.ns \
  /home/david/workspace/tuvm/src/solidified_image.c
```

After building (and BEFORE committing anything in `tuvm`), restore the
checked-in `solidified_image.c`/`.stmap` so the working tree stays clean for
other tests that depend on them:

```bash
cd /home/david/workspace/tuvm
git checkout -- src/solidified_image.c src/solidified_image.stmap
```

## Build + run

```bash
source /home/david/workspace/ns-zephyr/tools/env.sh
cd /home/david/workspace/tuvm/zephyr
west build -b native_sim -p always .
timeout 8 ./build/zephyr/zephyr.exe
```

Expected: the build completes, and the run prints a line containing
`observed done` before idling (killed by `timeout`).

# Task 8 — nRF52840-DK acceptance bar (real GPIO ISR)

Boots the SAME VM + pulse seam on real silicon (nRF52840-DK) and proves ONE
pulse driven by a PHYSICAL button edge — no `gpio fire:` simulation. Pressing
**BUTTON1** (P0.11) fires a GPIO edge → `pulse_isr` (IRQ context) →
`gpio_target_fire(13)` → `pulse_post_isr` → the k_msgq mailbox → `pulse_pump`
(thread context) → `space_post_id` → the node's `whenever:` reaction prints
`observed done` on the DK's uart0/VCOM console. **Passed 2026-07-08.**

## Board reality (differs from the original plan)

The plan drafted against the nRF52840 *dongle* (button P1.06, nrfutil DFU). The
hardware on the bench is the *DK* — this Task 8 targets it instead:

| Aspect      | dongle (plan)          | DK (actual)                                   |
|-------------|------------------------|-----------------------------------------------|
| board       | `nrf52840dongle`       | `nrf52840dk/nrf52840`                         |
| button      | P1.06                  | **P0.11** (BUTTON1, via the `pulsegpio` alias) |
| flash       | nrfutil DFU            | `west flash --runner jlink` (on-board J-Link)  |
| console     | USB-CDC                | **uart0 / on-board SEGGER VCOM** (`/dev/ttyACM0`) |
| networking  | native_sim NSOS        | in-kernel loopback UDP (link-only; never used) |

## Files

- `boards/nrf52840dk_nrf52840.overlay` — adds the `pulsegpio` DT alias onto the
  board's existing `button0` (P0.11). Applied automatically for the DK target;
  absent for native_sim, so the ISR guarded on `DT_ALIAS(pulsegpio)` compiles
  out there and the Task 7 build is unchanged.
- `prj_dk.conf` — the on-target Kconfig (selected with `-DCONF_FILE=prj_dk.conf`
  so the native_sim `prj.conf` is untouched): uart0/VCOM console + Zephyr's real
  in-kernel loopback UDP stack (the proven nRF52840 recipe from
  `ns-zephyr/app/prj_dongle_loopback.conf`) — networking is present only so
  `udp_zephyr.c`'s `zsock_*` references link, exactly as on native_sim.
- `PulseNodeDK.ns` — the gate node MINUS the sim-only `gpio fire: 13` line, so
  the one pulse comes from a physical press. Baked into `src/solidified_image.c`.
- `src/main.c` — `wire_pulse_gpio()` binds the button IRQ (guarded on the alias).
- `../src/platform.c` — `gpio_target_fire(int)`, the non-static ISR entry that
  reuses the same host-GPIO slot table the `gpioPin:` prim populates.

## RAM tuning (256 KB part)

The engine's host defaults do not fit the DK's 256 KB SRAM. Passed via env at
build time (the `CMakeLists.txt` env→`-D` passthrough, since NCS sysbuild
swallows bare `-D` app defines).

**Recommended DK config ("D", RAM-optimized — 2026-07-09).** Derived from the
`nsl-tuvm` RAM↔speed knob matrix (`nsl-tests/00_perf/RESULTS.md`, 2026-07-09):

- `VM_STACK_INIT_WORDS=4096` — the VM execution stack is a fixed heap `Stack` object
  allocated INSIDE the arena, and at the 16384 default it is **64 KB on the 32-bit
  DK** — the single largest boot allocation (this is what made 64 KB arenas OOM at
  boot). perfstex's measured peak is 442 words, so 4096 (9× headroom) is safe for
  real recursion and frees ~48 KB of arena at zero speed cost.
- `OEVM_HEAP_SIZE=49152` — 48 KB VM arena. Feasible only *because* the stack shrank
  (the old 128 KB was almost all stack). Sane middle for allocation churn; drop to
  32 KB only for allocation-light nodes.
- `METHOD_CACHE_SIZE=256` / `PRIM_CACHE_SIZE=256` — dispatch caches (default 4096 =
  64 KB + 32 KB), shrunk to 4 KB + 2 KB. Pure caches — smaller only trades a few
  more collisions, never correctness. 512 if a node has diverse dispatch.
- `OEVM_MARK_STACK_SIZE=512` — GC mark worklist (default 1024); overflow only forces
  a rescan (slower GC, never incorrect).

Result: **ROM 25.8 %, RAM 45.1 %** of the nRF52840 (was 80.4 % / 67.5 % pre-matrix),
full perfstex including floatMath, no measurable slowdown. `VM_STACK_INIT_WORDS` and
`OEVM_MARK_STACK_SIZE` are now forwarded by the CMakeLists env→`-D` passthrough
alongside the arena/cache/float knobs. Validated on perfstex; the pulse gate below
boots comfortably at these settings (its arena floor is far under 48 KB with the 4k
stack).

## Build + flash + run

```bash
source /home/david/workspace/ns-zephyr/tools/env.sh
# Recommended DK config "D" (see RAM tuning above + nsl-tests/00_perf/RESULTS.md):
export OEVM_HEAP_SIZE=49152 METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256
export VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512

# 1. bake the DK node into the solidified image (run from the nsl-tuvm repo)
cd /home/david/workspace/nsl-tuvm
python3 tools/ns_compile_oevm.py \
  /home/david/workspace/tuvm/zephyr/PulseNodeDK.ns \
  /home/david/workspace/tuvm/src/solidified_image.c

# 2. build + flash (skip-rebuild so the flash can't rebuild with a restored image)
cd /home/david/workspace/tuvm/zephyr
west build -b nrf52840dk/nrf52840 -p always . -- -DCONF_FILE=prj_dk.conf
west flash --skip-rebuild --runner jlink

# 3. restore the checked-in image BEFORE committing anything in tuvm
cd /home/david/workspace/tuvm
git checkout -- src/solidified_image.c src/solidified_image.stmap
```

Then open the console (`/dev/ttyACM0` @ 115200) and press **BUTTON1**. Expected:
a line `observed done` after the boot banner.

> **J-Link note:** the DK's on-board J-Link OB can enter a firmware-replace loop
> if `JLinkExe` decides its firmware is stale (symptom: no `/dev/ttyACM0`, connect
> times out). Recover by power-cycling the DK (unplug/replug the on-board-debug
> USB). Once healthy it enumerates as `1366:1025 SEGGER J-Link` and `west flash
> --runner jlink` programs it without re-triggering the replace.
