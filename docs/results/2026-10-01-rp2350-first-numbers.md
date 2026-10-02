# RP2350 first numbers — #302 Task 5

Result doc of record for rete#302 (`rpi_pico2`, oevm's fourth Zephyr target). Raw console logs:
[`2026-09-21-rp2350-first-numbers/`](./2026-09-21-rp2350-first-numbers/).

## Headline

**RP2350 = 11.13 µs/activation; C6 = 11.83.** Ratio 0.941 — the RP2350 is 5.9 % faster per
activation. cabot's M28 gate (≤ 11.83 µs/activation, the C6's) is **met**.

Arithmetic: 2918.8 ms / 262143 = 11.134 µs; C6 3101 ms / 262143 = 11.829 µs
(`2026-08-06-method-call-counter-spike.md`, "Cost on the C6").

Scope: the gate was three boots of the one workload the C6's 11.83 came from. The other STIC
rows named in #302 Task 5 were **not run** on this rung.

## Method

- Board: Waveshare RP2350-Zero (RP2350A, Cortex-M33 @ 150 MHz, 2 MB flash, native USB-C), built
  as Zephyr board `rpi_pico2/rp2350a/m33` unchanged. Zephyr v4.2, `~/zephyr-esp32` workspace,
  arm-zephyr-eabi from the shared SDK — no new toolchain or workspace.
- Console: the SoC's own USB-CDC (`prj_rpi_pico2_usb.conf` + `usb-console.overlay`), because no
  adapter is soldered to GP0/GP1.
- Workload: `nodus/oevm/zephyr/MethodProfileNodeC6.nsl`, Hanoi depth 18, 20 runs × 262143
  activations, Model B, 128 KB arena (`OEVM_HEAP_SIZE=131072`), `-Os`, profiling counter not
  enabled. One binary, never reflashed between boots: build-id
  `porta-v1-final-1259-gcc235eb-dirty-20260921T210314`.

## The boots

| Boot | Date | Runs (ms) | Mean (ms) | µs/activation | Log |
|---|---|---|---|---|---|
| 1 | 2026-09-21 | 2918–2919 (4 × 2918, 16 × 2919) | 2918.8 | 11.134 | `hanoi-run1-console.log` |
| 2 | 2026-10-01 | 2918–2919 (18 × 2918, 2 × 2919) | 2918.1 | 11.131 | `hanoi-run2-console.log` |
| 3 | 2026-10-01 | 2918–2919 (4 × 2918, 16 × 2919) | 2918.8 | 11.134 | `hanoi-run3-console.log` |

Means recomputed from the logs. Scatter is 1 ms within a boot and 0.7 ms across the three boots.
Boot 2's log was read from the CDC console's retained buffer after the run; its banner lines
were outside the kept tail.

## Layout

`nodus/oevm/zephyr/layout-rp2350.json` was captured from the board (`layoutdump/capture.py`) and
is byte-identical to `layout-esp32c6.json` (commit `7c6aca7`). Expected (the nRF's was identical too, #281), and still this target's own file, since `run.sh` refuses to bake without it.

## Heap ceiling

`tools/heap-ceiling.sh --board rpi_pico2`, TimerNode.nsl, grain 4096, range [16384, 524288],
run 2026-10-01. Linker region `RAM` = 532480 B (520 KB). A **link** ceiling, as on the C6
(`2026-07-20-c6-heap-ceiling.md`); "links" is not "boots".

| Cell | Console | max-linkable `OEVM_HEAP_SIZE` | Failing probes (region `RAM` overflowed by, B) | boots? |
|---|---|---|---|---|
| A | uart0 (`prj_rpi_pico2.conf`) | **450560** | 524288: 71948; 458752: 4364; 454656: 140 | not boot-tested: no uart adapter on this bench |
| B | USB-CDC (`--conf-extra prj_rpi_pico2_usb.conf` + `usb-console.overlay`) | **442368** | 524288: 82580; 458752: 14996; 450560: 6548; 446464: 2324 | **yes** — `TimerNode` flashed at 442368 (RAM 530580/532480 B), printed `TIMER OK` (2026-10-01, `timernode-ceiling-442368-console.log`). Boots and runs the timer path; it does not prove the arena is usable to the last cell. |

For comparison, the C6 no-radio cell is 319488.

**Sanity check** (same method as the C6 doc: the GC bitmap is a separate static array of H/32
bytes, so `region + overflow − H − H/32` must be constant across failing probes). Re-derived
from the overflow figures above:

- Cell A: **63756 B** at all three probes (524288, 458752, 454656).
- Cell B: **74388 B** at all four probes (524288, 458752, 450560, 446464).

Constant to the byte, so the bisect measured capacity only. The fixed non-arena footprint is
63.8 KB (uart console) and 74.4 KB (USB console); the USB console stack costs 74388 − 63756 =
**10632 B** of RAM.

Link at the cell-B ceiling: RAM 530580 B / 532480 B = 99.64 %; FLASH 296544 B (7.07 % of the
dts's 4 MB; the board has 2 MB).

## FloatSmokeNode (#302 hazard 1)

The RP2350 has an FPv5 **single-precision** unit; the VM's Float is a C double, so every double
op is still a libcall. A float row for this part on the STIC table needs "FPv5-SP, double in
software" on it. Image built 2026-10-01: FLASH 296120 B, RAM 209556 B (39.35 %). On the board:
**pass** — `float-smoke sum=167167` then `float-smoke ok`, the node's exact `expect:` lines (2026-10-01, `floatsmoke-console.log`).

## Not delivered on this rung

(From the #302 session comments, 2026-09-19.)

- **No NVS / settings partition / bootId (bootId = 0).** Zephyr v4.2 has no RP2350 flash driver:
  `flash_rpi_pico.c` is RP2040-only (it includes pico-sdk `hardware/structs/ssi.h`; the RP2350
  uses the QMI). Earning it back is a QMI driver, not an overlay.
- **No entropy driver.** `CONFIG_TEST_RANDOM_GENERATOR=y` is the non-cryptographic degenerate
  case for a board that sends no packet; a real TRNG comes before any radio or token here.
- Second core unconfigured (single-core by default). SWD/openocd flash path not exercised
  (UF2/BOOTSEL only). The uart0 console (GP0/GP1) not exercised.
- The Feather RP2040 rehearsal board was built (`c4714a3`) but **never run on the bench**; no
  M0+ figure exists and none is claimed.
- The other STIC rows (see Headline scope).

## Reproduction

Builds need the west env (`west` is not on the global PATH):

```
source ~/zephyr-esp32/.venv/bin/activate && export ZEPHYR_BASE=~/zephyr-esp32/zephyr
cd ~/workspace/rete/nodus/oevm/zephyr
# USB-console node image + flash (BOOTSEL; form from prj_rpi_pico2_usb.conf's header)
EXTRA_DTC_OVERLAY_FILE=$PWD/usb-console.overlay \
  ./run.sh --board rpi_pico2 --conf-extra prj_rpi_pico2_usb.conf --node MethodProfileNodeC6.nsl --flash
# read the port with pyserial dtr=True, not cat
# heap-ceiling cells (no hardware needed)
cd ..; bash tools/heap-ceiling.sh --board rpi_pico2 --node TimerNode.nsl                       # cell A
EXTRA_DTC_OVERLAY_FILE=$PWD/zephyr/usb-console.overlay \
  bash tools/heap-ceiling.sh --board rpi_pico2 --node TimerNode.nsl --conf-extra prj_rpi_pico2_usb.conf   # cell B
```

Cell B's `--conf-extra` path form and `--lo/--hi` flags are as in `heap-ceiling.sh`'s usage
(range here was `--lo 16384 --hi 524288`).
