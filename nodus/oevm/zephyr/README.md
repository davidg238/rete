# oevm on Zephyr — native_sim bring-up gate (Task 7)

Boots the oevm VM under Zephyr's `native_sim` (a HOST executable, no hardware
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
- `PulseNode.nsl` — the gate node (source of truth for `src/solidified_image.c`
  — see "Regenerating the image" below).

## The gate node

`PulseNode.nsl`'s `main:` registers a `whenever: '' do:` reaction on channel
`sensor/alert`, registers pin 13 to post the String `'done'` on that channel,
fires ONE simulated edge (`gpio fire: 13` — host/sim-only, the on-target
GPIO ISR replaces this in Task 8), then runs the C-owned pump loop:
`[ true ] whileTrue: [ platform pumpEvents. space drain ]`.

Two deliberate deviations from the plan's draft node (both required to
actually compile/run against this VM + compiler, not stylistic — see the
comment header in `PulseNode.nsl` for detail):
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

`src/solidified_image.c` is a **generated
artifact**, committed to `oevm` only as whatever some OTHER test last left
there — they are NOT the Task 7 gate's own source of truth (`PulseNode.nsl`
is). Building this gate CLOBBERS them as a transient build step. Regenerate
from `PulseNode.nsl` before every `west build`:

```bash
source /home/david/workspace/ns-zephyr/tools/env.sh
cd /home/david/workspace/nsl-tuvm
python3 tools/ns_compile_oevm.py \
  /home/david/workspace/tuvm/zephyr/PulseNode.nsl \
  /home/david/workspace/tuvm/src/solidified_image.c
```

After building (and BEFORE committing anything in `oevm`), restore the
checked-in `solidified_image.c` so the working tree stays clean for
other tests that depend on them:

```bash
cd /home/david/workspace/tuvm
git checkout -- src/solidified_image.c
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
- `PulseNodeDK.nsl` — the gate node MINUS the sim-only `gpio fire: 13` line, so
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
# Config "D" is BAKED as the default for every nRF52840 build (CMakeLists.txt,
# 2026-07-12) — no exports needed. It is load-bearing, not tuning: a 52840 build
# without VM_STACK_INIT_WORDS=4096 allocates a 64 KB VM stack inside the 48 KB
# arena and dies in a SILENT boot OOM (this shipped a dead dongle through rungs
# 7-8; see docs/BOARD_PORTS.md). Export any knob only to EXPERIMENT — env still
# overrides the baked defaults:
#   export OEVM_HEAP_SIZE=49152 METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256
#   export VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512

# 1. bake the DK node into the solidified image (run from the nsl-tuvm repo)
cd /home/david/workspace/nsl-tuvm
python3 tools/ns_compile_oevm.py \
  /home/david/workspace/tuvm/zephyr/PulseNodeDK.nsl \
  /home/david/workspace/tuvm/src/solidified_image.c

# 2. build + flash (skip-rebuild so the flash can't rebuild with a restored image)
cd /home/david/workspace/tuvm/zephyr
west build -b nrf52840dk/nrf52840 -p always . -- -DCONF_FILE=prj_dk.conf
west flash --skip-rebuild --runner jlink

# 3. restore the checked-in image BEFORE committing anything in oevm
cd /home/david/workspace/tuvm
git checkout -- src/solidified_image.c
```

Then open the console (`/dev/ttyACM0` @ 115200) and press **BUTTON1**. Expected:
a line `observed done` after the boot banner.

> **J-Link note:** the DK's on-board J-Link OB can enter a firmware-replace loop
> if `JLinkExe` decides its firmware is stale (symptom: no `/dev/ttyACM0`, connect
> times out). Recover by power-cycling the DK (unplug/replug the on-board-debug
> USB). Once healthy it enumerates as `1366:1025 SEGGER J-Link` and `west flash
> --runner jlink` programs it without re-triggering the replace.

# Rung 5 — native_sim UDP round-trip gate (NetNode.nsl)

Proves the Zephyr UDP backend (`../src/backend/zephyr/udp_zephyr.c`) functionally
round-trips a datagram on `native_sim` — previously it only *linked* (the pulse
gate's `prj.conf` already turns on NSOS networking because `dispatch.c`'s `send()`
fast path unconditionally probes `udp_object()`). The baked node `NetNode.nsl`
declares the `net:` capability, binds a receiver, sends to loopback (v4 + v6),
polls the non-blocking `receive`, checks the payload, and prints `UDP OK`.

Model A; reuses this directory's `main.c` / `CMakeLists.txt` / `prj.conf`
UNCHANGED — the pulse-GPIO block compiles out on native_sim, and `main()` already
handles a `main:` that returns. (Payload migrated to an immutable `Bytes` in Layer 2
— see the Rung 6 gate below.) Real OpenThread on the DK is Layer 3.

## Build + run

```bash
source /home/david/workspace/ns-zephyr/tools/env.sh
# bake NetNode.nsl into the solidified image (from the nsl-tuvm repo)
cd /home/david/workspace/nsl-tuvm
python3 tools/ns_compile_oevm.py \
  /home/david/workspace/tuvm/zephyr/NetNode.nsl \
  /home/david/workspace/tuvm/src/solidified_image.c
cd /home/david/workspace/tuvm/zephyr
west build -b native_sim -p always .
# NOTE: the current NCS/native_sim runner nests the executable one level deeper
# than the older pulse recipe above — it is build/zephyr/zephyr/zephyr.exe.
timeout 8 ./build/zephyr/zephyr/zephyr.exe   # expect a line: UDP OK
# restore the generated image before committing anything in oevm
cd /home/david/workspace/tuvm
git checkout -- src/solidified_image.c
```

Expected: build completes, the run prints `UDP OK` before `timeout` kills it —
i.e. `udp_zephyr.c`'s `zsock_socket`/`zsock_bind`/`zsock_sendto`/`zsock_recvfrom`
actually moved a datagram over Zephyr's NSOS loopback.

# Rung 6 — native_sim NetBridge round-trip gate (NetBridgeNode.nsl)

Proves the Layer-2 reactive channel↔UDP bridge (`NetBridge` in
`nsl-tuvm/lib/kernel.nsl`) functionally round-trips a CBOR `Record` on `native_sim`.
The baked node `NetBridgeNode.nsl` wires two `NetBridge`s A↔B in one `Space`, posts a
`Record` to A's `out` channel, and observes it CBOR-decoded on B's `in` channel,
printing `BRIDGE OK`. This exercises the whole Layer-2 stack on the target-shaped
backend: `Cbor encode`/`decode` (the leaves link on native_sim), the immutable-`Bytes`
payload, `Socket` send/receive over `udp_zephyr.c`, and `Space>>drain`. Model A;
reuses this directory's `main.c` / `CMakeLists.txt` / `prj.conf` UNCHANGED (no
`CONFIG_NET_LOOPBACK` needed — NSOS loopback delivered as-is).

## Build + run

```bash
source /home/david/workspace/ns-zephyr/tools/env.sh
# bake NetBridgeNode.nsl into the solidified image (from the nsl-tuvm repo)
cd /home/david/workspace/nsl-tuvm
python3 tools/ns_compile_oevm.py \
  /home/david/workspace/tuvm/zephyr/NetBridgeNode.nsl \
  /home/david/workspace/tuvm/src/solidified_image.c
cd /home/david/workspace/tuvm/zephyr
west build -b native_sim -p always .
timeout 8 ./build/zephyr/zephyr/zephyr.exe   # expect a line: BRIDGE OK
# restore the generated image before committing anything in oevm
cd /home/david/workspace/tuvm
git checkout -- src/solidified_image.c
```

---

# Rung 7 — real OpenThread round-trip gate (ThreadEchoNodeDK.nsl)

Proves the Layer-2 `NetBridge` round-trips a CBOR `Record` over a **real 802.15.4 /
OpenThread mesh**. The DK is an **MTD child** of the porta OTBR (`gw85224-01`); a
BUTTON1 press posts a `Record` whose outbound reaction CBOR-encodes and sends it over
the radio to the gateway's `udp-echo6` server (:7777), which reflects it byte-verbatim;
the node receives it, CBOR-decodes it value-equal, and its `whenever:` reaction prints
`BRIDGE OK` on uart0/VCOM.

No VM / bridge / CBOR / facade source changed this rung — it is radio + mesh bring-up
only. Model A (baked image); **MTD, never FTD**. This is a **bench-verified manual gate**
(live silicon + live gateway + radio), not a `west build` CI gate.

## Build + flash + run

```bash
source /home/david/workspace/ns-zephyr/tools/env.sh
export OEVM_HEAP_SIZE=49152 METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256
export VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512

# bake the Thread node into the solidified image (from the nsl-tuvm repo)
cd /home/david/workspace/nsl-tuvm
python3 tools/ns_compile_oevm.py \
  /home/david/workspace/tuvm/zephyr/ThreadEchoNodeDK.nsl \
  /home/david/workspace/tuvm/src/solidified_image.c

cd /home/david/workspace/tuvm/zephyr
west build -b nrf52840dk/nrf52840 -p always . -- \
  -DCONF_FILE=prj_dk_thread.conf -DEXTRA_CONF_FILE=prj_dk_thread_local.conf
west flash --skip-rebuild --runner jlink

# restore the generated image BEFORE committing anything in oevm
cd /home/david/workspace/tuvm
git checkout -- src/solidified_image.c

# console /dev/ttyACM0 @115200: wait for `thread: attached role=2` (and confirm the
# child row with `ssh david@gw85224-01 'docker exec otbr ot-ctl child table'`),
# then press BUTTON1  ->  expect:  BRIDGE OK
```

The **gitignored** `prj_dk_thread_local.conf` (copied from
`ns-zephyr/app/prj_thread_local.conf`) supplies the real dataset — the Thread network
key is a secret and never enters a committed file. Without that overlay the node still
builds, but will not join the mesh.

## Two things this rung learned the hard way

1. **Target the gateway's ML-EID, not its OMR.** An MTD child never SLAAC-configures an
   OMR address, so it has no route (and no source address) for the OMR prefix, and
   `zsock_sendto` to the gateway's OMR fails silently. Send to the gateway's **ML-EID** —
   the same mesh-local /64 the node itself lives in.

2. **OT attached ≠ sockets work.** NCS v3.2.4's `openthread_run()` enables OT (the node
   attaches, and `ot-ctl ping` works — that is OT-internal ICMP) but never `net_if_up()`s
   the Zephyr socket iface; with `CONFIG_NET_CONFIG_SETTINGS=n` nothing else does either.
   The symptom is `zsock_sendto` returning **`ENETDOWN` (errno 115) while the node is
   attached and pingable**. The fix is `CONFIG_OPENTHREAD_MANUAL_START=y` plus
   `thread_bringup()` in `src/main.c` = `openthread_run()` **+ `net_if_up()`** + attach-wait.

See `docs/ZEPHYR_LESSONS.md` for the full list, and `docs/BOARD_PORTS.md` for how the
same image runs on other boards (the nRF52840 dongle is ported but **headless** — Issue A).

---

# Rung 8 — autonomous timer round-trip (TimerNode.nsl / ThreadTimerNode.nsl)

Gives the nsl `Timers` seam a **real on-target backend** (`src/backend/zephyr/timer_zephyr.c`,
a Zephyr `k_timer`) and uses it to run the rung-7 round-trip on a **15 s autonomous
cadence** — **no button press**. That is what lets the node run on the nRF52840 **dongle**,
which is headless (Issue A).

The `k_timer` expiry ISR posts through the **existing** pulse mailbox
(`pulse_post_isr` → `k_msgq` → `pulse_pump` → `space_post_id`) onto the reserved
`sys/timer` channel — the identical path the host virtual clock posts on, and the one
`pulse_backend.h` already sanctioned ("A GPIO/timer ISR calls `pulse_post_isr`"). Since the
tick lands in the pulse mailbox, `platform pumpEvents` (which blocks `K_FOREVER` there)
**idles the CPU** between ticks and yields to the OpenThread thread.

## Gate 1 — native_sim: the timer backend, with zero hardware

    source /home/david/workspace/ns-zephyr/tools/env.sh
    cd /home/david/workspace/nsl-tuvm
    python3 tools/ns_compile_oevm.py \
      /home/david/workspace/tuvm/zephyr/TimerNode.nsl \
      /home/david/workspace/tuvm/src/solidified_image.c
    cd /home/david/workspace/tuvm/zephyr
    west build -b native_sim -p always .
    timeout 10 ./build/zephyr/zephyr/zephyr.exe    # expect: TIMER OK  (3 ticks @200ms)
    cd /home/david/workspace/tuvm && git checkout -- src/solidified_image.c

## Gate 2 — the DK: BRIDGE OK every 15 s, in lockstep with the gateway

Same build/flash recipe as Rung 7, but bake `ThreadTimerNode.nsl` instead. **No button
press** — just watch. Capture both ends at once:

    timeout 80 cat /dev/ttyACM0 &
    ssh david@gw85224-01 'timeout 80 journalctl -u udp-echo6.service -f -o short-iso'

Verified 2026-07-11: five `BRIDGE OK` prints at 15 s intervals, each matching a gateway
`echo 32B <-> [fdc5:...]:54360` line **to the second**.

## The self-gating train — how a HEADLESS board proves its inbound leg

The gateway's echo log only witnesses the **outbound** leg (it logs datagrams it
*receives*). So `ThreadTimerNode` makes the next send **conditional on the previous
reflection having decoded value-equal**: the `awaiting` flag is cleared *only* by a good
reflection, and the timer reaction sends *only* when it is clear.

Therefore a steady 15 s train of gateway echo lines proves the **full round-trip** is
closing every cycle — **N lines = N−1 proven round-trips**. A broken inbound leg (lost
datagram, decode mismatch, an `ENETDOWN`-class regression) leaves `awaiting` stuck true:
the node never sends again and the train **stops dead after one line**. A broken inbound
leg cannot fake a train.

`awaiting` is load-bearing, not decorative: without it, a timed-out wait would simply send
again on the next tick and produce a healthy-looking train while the inbound leg was
broken — the exact false pass this design exists to prevent.

**Deliberate:** a single lost datagram wedges the node forever. For a *gate* node that is a
feature (failure must be loud, never a silent pass). It is not how a production sensor node
would behave.

The DK run above is what **calibrates** this gate: the console and the train were shown to
tick together, so on the dongle — where there is no console — the train alone is trusted.

---

# ESP32 dev loop — `run.sh --board {esp32,esp32c6}`

`run.sh` owns the whole bake→build→(flash)→(capture) loop for both ESP32 targets.
`--board` selects a profile that owns every board-coupled wrong-by-default together (the
board target, Kconfig, build dir, layout JSON, the esptool triple, the serial port, and
whether the Xtensa ROM-linker patch applies) so adding a board only means adding one
profile block, not touching the rest of the script.

```bash
cd ~/workspace/tuvm
bash zephyr/run.sh --board esp32c6 --model B --node TimerNode.nsl --pristine --flash
```

`--board esp32c6` (**default** since 2026-07-20, #57 — the C6 is the primary target)
targets the C6 (`esp32c6_devkitc/esp32c6/hpcore`, RISC-V `rv32imac_zicsr` — no FPU,
soft-float only — flash offset `0x0`; `0x1000` boot-loops the C6 silently, no error;
heap default 131072). `--board esp32` targets the WROOM-32, now the supported *secondary*
board (`esp32_devkitc/esp32/procpu`, Xtensa, flash offset `0x1000`, ROM-newlib patch
applied, heap default 53248). `--pristine` is
required on the first build after adding or changing an overlay — CMake caches overlay
discovery, so a stale cache silently keeps the old (or no) overlay rather than erroring.

## Which port does what (C6 only — the WROOM-32 has a single USB port)

The C6 DevKitC-1 exposes two USB ports, and the useful split is the non-obvious one:

- **Native USB (Espressif JTAG/serial, `303a:1001`) — flashing.** This port
  **re-enumerates when the target resets**, which breaks any continuous reader across a
  reset. `run.sh` defaults `ESPTOOL_PORT` to the first `/dev/serial/by-id/*Espressif*`
  device.
- **UART bridge (Silicon Labs CP210x) — console.** The bridge chip never reboots, so this
  port **stays enumerated across target resets** and carries an always-on log — open it
  with a serial reader *before* the first flash and leave it running through every
  subsequent flash.

Both scripts pin ports by `/dev/serial/by-id/*` glob rather than `ttyUSB0`/`ttyACM0`
(allocation-order names that have previously collided with an unrelated board on the same
bench — see #57); `ESPTOOL_PORT` always overrides the default.

## The CMakeLists have a gate now, and it needs no board

`nodus/oevm/tests/cmake_gate.sh` parses every tracked CMake listfile in the repo and is run
by the nsl suite as the `cmake-parse` aux gate. It takes about 12 ms a file: no toolchain,
no west venv, no `ZEPHYR_BASE`, no board.

It exists because rete#226 deleted both `PRIM_CACHE_SIZE` blocks from this directory's
`CMakeLists.txt` — each `if(...)` and its body, but not its `endif()` — and the file stopped
parsing (`Flow control statements are not properly nested`). It stayed broken for a day under
a suite reading **262/0 with every aux gate PASS and PORTA SYNC OK**, because a CMakeLists is
code that only a target build compiles and a target build needs hardware.

How it parses without running anything: `cmake -P` parses a whole listfile before executing
any of it, so each file is checked wrapped in `if(FALSE)` / `endif()` — the body is parsed and
never executed, and the nesting is preserved exactly (an extra `endif()` orphans the wrapper's;
a missing one orphans the last). A canary with a deliberate orphan `endif()` runs first and
must be reported broken, so a check that has gone blind FAILS instead of clearing everything.

**What it does not catch, said positively:** an unknown Kconfig symbol, a `target_sources`
naming a file that does not exist, a wrong board, or an option whose `depends on` is unmet —
which Zephyr discards silently (see BOARD_PORTS.md's nRF52840 note). Those need a real
configure with the west venv and the SDK, and today the first place they are caught is a
target build.

## Where the layout JSON comes from

Model B (`.oimg`) bakes struct offsets ahead of time, so it needs an ABI-accurate
`layout-$BOARD.json` for whichever target it's building — `zephyr/layout-esp32.json`
(Xtensa), `zephyr/layout-esp32c6.json` (RV32 ilp32) and `zephyr/layout-nrf52840.json`
(Cortex-M4F, arm-zephyr-eabi — captured 2026-08-30, rete#281) are checked in, generated,
not hand-written. **ONE nRF file serves both the DK and the dongle**: same part, same
ABI, same toolchain, and the differences `run.sh` encodes between those boards (console,
trigger pin, flash method) are Kconfig and devicetree, none of which can move a struct
offset. Each comes from flashing the `zephyr/layoutdump/` app (which computes every
struct size/offset via `sizeof`/`offsetof` against the real `struct` definitions in
`src/oevm.h`, compiled for that target) onto real hardware and capturing its console
output. A new architecture needs its own layoutdump run before Model B works on it at all
— `run.sh` fails loudly (`missing .../layout-$BOARD.json — run the layoutdump app`) rather
than silently reusing another target's file. For the three targets measured so far the
captured layouts are byte-identical (verified independently of the capture itself — see
the ladder's C6 entry), Cortex-M4F included — a coincidence of this particular struct set
having no field type whose natural alignment diverges between these 32-bit ABIs, not a
reason to assume a future target's layout will match without generating it.
**`layout-esp32s2.json` is the one exception, and it is labelled (rete#388):** the S2's
own capture predated rete#226 and differed from the C6's only in `Symbol`, the struct #226
changed, so since 2026-10-09 it is a copy of the C6 capture — derived, not captured — until
the MagTag is recaptured over its UART header (`layoutdump/prj_esp32s2.conf`).
**One hand edit to all five files, 2026-10-12 (rete#37 L1):** `CompiledMethod.access_modifier`, its last field,
was deleted, so its size went from 28 to 24 and no other offset moved. This was checked rather than assumed: `_Static_assert`s on
`sizeof` and every `offsetof` of the edited struct, against `src/oevm.h`, passed under the SDK's RV32 (ilp32), ARM
(Cortex-M33) and both Xtensa compilers. A recapture should reproduce the files byte for byte.

**Capturing on a board whose console is its own USB (the dongle) takes three things that
a UART target does not need**, all of them in `layoutdump/prj_dongle.conf` and
`layoutdump/src/console_prep.c`, and each one was learned from a capture that came back
empty or corrupt (rete#281):

1. `CONFIG_BOOT_DELAY` — `dump_layout.c` prints once from `main()` and returns, and the
   console does not exist until USB enumerates after the DFU reset. Without the delay the
   dump is emitted into a console with no host, seconds before a reader can attach.
2. `setvbuf(stdout, NULL, _IONBF, 0)` — `main()` returning never flushes, so picolibc
   holds the whole dump. It goes in a `SYS_INIT` **beside** `tools/dump_layout.c`, never
   inside it: that file is shared with the host gate, which regenerates the host layout on
   every nsl suite run.
3. `hw-flow-control` on the cdc-acm node — without it a full ring buffer **discards**
   (`usbd_cdc_acm.c:1004`), which truncates the JSON mid-object and reads like a corrupt
   dump rather than a dropped one. **Do not copy this to a node image**: a blocking
   `poll_out` on an unattended dongle wedges the VM (rete#286).

## C6 `OEVM_HEAP_SIZE` — no recommended default yet

`run.sh`'s `esp32c6` profile ships `OEVM_HEAP_SIZE=53248` — the WROOM-32's own value,
carried over as an explicitly provisional placeholder, not a measured or recommended C6
number.

`tools/heap-ceiling.sh` has measured the C6's max-*linkable* `OEVM_HEAP_SIZE` (see
`review/program-sequence.md` and `docs/results/2026-07-20-c6-heap-ceiling.md`):

- **319488** — no radio (net stack only).
- **262144** — with the OpenThread/IPv6 radio layer (`prj_esp32c6_thread.conf`).

**Neither ceiling is a recommended default.** A ceiling is the largest value that still
*links*; it says nothing about what a given workload actually needs, and both numbers cut
it close: 319488 links `sram0_0_seg` at 100.00% occupancy with 8 bytes spare. The VM's GC
is collect-when-full (`src/gc.c`), so the heap's high-water mark pegs to whatever arena
size you give it regardless of a workload's real live set — running at the ceiling leaves
no slack for anything else in `sram0_0_seg` (kernel, net stack, stacks, other bss/data) to
grow even slightly. Choosing a real default requires measuring a workload's actual live
heap, the same way `prj_esp32.conf`'s 53248 was justified by the WROOM-32's HttpServerNode
`/stats` high-water figure — not by taking the link ceiling itself.
