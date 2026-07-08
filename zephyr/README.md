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
