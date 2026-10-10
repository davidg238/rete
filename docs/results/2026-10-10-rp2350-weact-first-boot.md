# RP2350 (WeAct RP2350B) first boot — a hang and a 2× that were both master's, not the board's

Result doc of record for rete#402 (the polled-console hang) and rete#403 (the hot path in SRAM).
Raw console logs: [`2026-10-10-rp2350-weact-first-boot/`](./2026-10-10-rp2350-weact-first-boot/).
Companion to [`2026-10-01-rp2350-first-numbers.md`](./2026-10-01-rp2350-first-numbers.md), whose 11.13
µs/activation this doc re-reads as "measured from flash on a layout that happened to fit".

## Headline

- **A never-used WeAct Studio RP2350B Core Board boots the unchanged `rpi_pico2/rp2350a/m33` image** and
  runs the VM. Flash reads verified byte-exact, stdout path verified, GPIO map differs (unused here).
- **Every polled-console board had been dead since 2026-10-05/06 (#312/#364):** the kernel's Profiler spun
  in `drainConsole` before `main:` because the consoleLine prim answered the VM's `false` singleton and the
  kernel compared it by identity to the image's `false`. One line in `platform.c` (answer nil) fixes it;
  proven on the WeAct board and on the Waveshare RP2350-Zero. **#402.**
- **With the hang fixed, master ran 2.1–2.7× slower than the 2026-09-21 binary on the same silicon,** and
  every suspect but one was measured out: it is XIP-cache placement. Putting the four `OEVM_HOT` functions
  (8.4 KB) in SRAM through Zephyr's `.ramfunc` gives **11.87 µs/activation** on the WeAct board. **#403.**

## Method

- Boards, both on `opti7040`'s USB: the WeAct RP2350B (bootloader `2e8a:000f` serial `542338F2DFFE536A`,
  Zephyr CDC serial `371016190D903B10`, `/dev/ttyACM1`); the Waveshare RP2350-Zero of the 2026-10-01 doc
  (Zephyr CDC serial `4530454D38330003`, `/dev/ttyACM0`).
- Builds on `msi-7d43` (the only machine with the Zephyr workspace: `~/zephyr-esp32`, David's fork
  `tuvm-c6-radio-fix` = v4.2.0-12-gbc47c6fab9c, SDK 0.17.4) in a detached worktree at master `357640f`
  (`~/rete-master`), `run.sh --board rpi_pico2 --conf-extra prj_rpi_pico2_usb.conf` with
  `usb-console.overlay`, `-Os`, Model B, 128 KB arena unless stated. UF2s copied to `opti7040` over
  Tailscale SSH and dropped on the BOOTSEL volume; console read with the port held open (`newgrp uucp`).
- Workload: `MethodProfileNodeC6.nsl`, Hanoi depth 18, 262143 activations per run — the 2026-10-01 workload.
- Host control: the same node through the suite's image VM (`run_tests_oevm._build_pathb_once`) at
  master and at `cc235eb` (the 2026-09-21 commit), `-O2` and `-Os`.

## The hang (#402)

TimerNode and MethodProfileNodeC6 both booted to `oevm: observer armed` and printed nothing more. Five
diagnostic images, each one BOOTSEL:

| Image | Showed |
|---|---|
| boot markers | `kernel_boot`, blob load and `oimg_bootstrap` entry all reached; silence after |
| PC sampler (preemptive reporter) | reporter never ran → main spins, never blocks |
| PC sampler (coop reporter) + blob sum | sums `0035bf18 0035bf18` = host → flash reads good; PCs spread over `run_interpreter`, `closure_entry`, `arena_try_alloc`, `cm_const_at`… → a healthy interpreter running nsl |
| stdout probes + console-write trace | `printf`/`fwrite`/`fputs` reach the console; **zero** `platform_console_write` calls → `main:` never entered |
| `current_frame` walk | blob offsets 18128 `Kernel_ReadStream>>upTo:`, 17904 `Kernel_ReadStream>>next`, 9488 `Kernel_Collection>>asArray` |

Only boot-time caller: `Profiler>>install:` → `drainConsole` (kernel.nsl, #312 Q1, in every standard
image since #364). On a USB-CDC console `RL_IS_CDC` picks the polled branch of `platform_zephyr.c`;
`platform_console_arm` answers 0; `PRIM_pf_consoleLine` answered `false_symbol`. `first == false` in nsl
is an identity compare against the image's own `false` literal — a distinct object (`is_truthy`'s
comment) — so it failed, `request: false` parsed the Symbol as a line, and
`[ line:: kernel consoleLine. line == nil ] whileFalse:` never saw nil. The C6's UART takes the
interrupt-driven branch and arms, which is why c6spare never showed it; the host likewise.

Fix: the prim answers nil when it cannot arm. First Hanoi line printed on both boards.

## The 2× (#403)

| Board, build | ms/run | µs/activation | log |
|---|---|---|---|
| Zero, 2026-09-21 binary `cc235eb` | 2918 | 11.13 | 2026-10-01 doc |
| Zero, master fixed | 8013–8014 (3 runs) | 30.57 | `zero-fix-console.log` |
| WeAct, master fixed | 6106–6107 (4 runs) | 23.30 | `pico2-fix-console.log` |
| WeAct, master fixed, **256 KB arena** | 6107 | 23.30 | `pico2-fix-256k-console.log` |
| **WeAct, master fixed, hot path in SRAM** | **3111–3112 (8 runs)** | **11.87** | `pico2-fix-ram-console.log` |
| Host, master vs `cc235eb`, -O2 | 17.9 vs 18.1 | — | — |
| Host, master vs `cc235eb`, -Os | 29.7 vs 31.5 | — | — |

Ruled out in order: the board (the Zero is slower still); the arena (identical at 256 KB, so not GC
pressure); the toolchain and Kconfig (the 2026-09-21 build directory is still on `msi-7d43`:
`.config` and the `vm.c` compile command diff empty against today's); the C code (every hot function
byte-identical in size — `run_interpreter` 2600, `send_resolve` 4868, `gc_allocate` 204 …; `.text`
296,012 → 308,352 B, the growth is `channels` 4864, the OTA runner and stack); the interpreter itself (host
flat). What moved is every hot function's address, by 12 KB of new code ahead of it, in a part that fetches
code from QSPI flash through a 16 KB XIP cache. The two boards' flash parts set the penalty (2.1× vs 2.7×).

`oevm.h` already routes `OEVM_HOT` into instruction RAM on the ESP32 (`OEVM_IRAM_HOT`). The same marker
into `.ramfunc` (Zephyr copies it to SRAM at boot; `CONFIG_ARCH_HAS_RAMFUNC_SUPPORT=y`) puts
`run_interpreter`, `send_resolve`, `push_frame`, `is_truthy` at `0x20000000`–`0x200020f8` (8,440 B).
RAM 217,444 → 225,884 B of 532,480; FLASH 308,936 → 309,624 B. `OEVM_RAM_HOT=0` opts out.

## Not done / found-during

- The `-O2` variant (`CONFIG_SPEED_OPTIMIZATIONS=y`, the C6's default since #329) was built, not flashed.
- `platform_dispatch` (5,288 B, the prims) stays in flash; a further gain is plausible, unmeasured.
- A `weact_rp2350b` board definition (RP2350B, 48 GPIO, primary flash only — the second flash pad is empty).
- `rg "== false|== true" nodus/nsl/lib` — are there other identity compares against prim-answered booleans?
- The STIC row for the RP2350 should name the placement ("hot path in SRAM") beside "FPv5-SP, double in software".
- `opti7040` has no Zephyr toolchain, no `west`, no pyserial; it flashes UF2s and reads consoles only. The
  machine map is in this doc's Method, not in memory.
