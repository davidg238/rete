# rete#37 — nsl, Toit and MicroPython on the ESP32-C6 (2026-10-12)

One board (c6spare, ESP32-C6, RISC-V at 160 MHz, `/dev/ttyUSB1`) and one session, all three VMs, with the
harness's own sources (`~/workspaceToit/stic`: `performance`, `richards`, `deltablue`). Driver: `run_all.sh`
(`c6run.py` for flashing and the console; mpremote for MicroPython). Logs: `logs/`, `driver.log`.

- **nsl:** rete `37-dispatch-head-nofwd` `532a2391` (Opt #11), built on msi-7d43 with `run.sh --board esp32c6`
  (C6 defaults: 128 KB arena, no radio, Model B, hot path in flash/XIP). STIC runs once per boot, three boots,
  identical to the millisecond. Richards and DeltaBlue each run three times in one boot.
- **Toit:** v2.0.0-alpha.194, envelope `firmware-esp32c6` with the benchmark installed as a boot container
  (`toit tool firmware … container install`). STIC is the mean of three in-process runs (Toit's own `performance.toit`).
- **MicroPython:** v1.29.0 `ESP32_GENERIC_C6-20260824`. `performance.py` and the others unchanged, except that
  `time.monotonic()` → `time.ticks_ms()/1000`. STIC is the mean of three in-process runs.

| test (s) | nsl | Toit | MicroPython | nsl ÷ Toit |
|---|--:|--:|--:|--:|
| alloc | 13.9 | 28.0 | 13.2 | 0.50 |
| array write | 17.6 | 25.9 | 41.5 | 0.68 |
| dictionary write | 6.2 | 5.5 | 0.98 | 1.13 |
| float math | 23.1 | 78.7 | 49.0 | 0.29 |
| integer math | 3.5 | 13.8 | 17.6 | 0.25 |
| collection iterate | 10.6 | 17.1 | 35.3 | 0.62 |
| collection write | 13.7 | 12.1 | 6.4 | 1.13 |
| string compare | 2.7 | 1.9 | 4.0 | 1.40 |
| Richards (ms per run) | 6989–7047 | 1177–1179 | 7471 | 5.9 |
| DeltaBlue (ms per run) | 542–548 | 76–78 | 326–359 | 7.1 |

All three VMs verify Richards and DeltaBlue (`verified: true`).

**Update, 2026-10-12 (night): nsl after Opt #12–#18** (rete `e2fa47e3`, the VM code of master `d9c4fdae`; same board,
same C6 defaults, hot path in SRAM since Opt #14; `logs-opt18/`). Toit and MicroPython were not rerun.

| test | nsl (`532a2391`) | nsl (`e2fa47e3`) | Toit | MicroPython |
|---|--:|--:|--:|--:|
| alloc (s) | 13.9 | 10.91 | 28.0 | 13.19 |
| array write (s) | 17.6 | 17.96 | 25.9 | 41.50 |
| dictionary write (s) | 6.2 | 5.31 | 5.5 | 0.98 |
| float math (s) | 23.1 | 23.31 | 78.7 | 48.97 |
| integer math (s) | 3.5 | 3.57 | 13.8 | 17.61 |
| collection iterate (s) | 10.6 | 10.93 | 17.1 | 35.26 |
| collection write (s) | 13.7 | 11.84 | 12.1 | 6.42 |
| string compare (s) | 2.7 | 2.15 | 1.9 | 3.96 |
| Richards (ms per run) | 6989–7047 | 5079–5081 | 1177–1179 | 7471 |
| DeltaBlue (ms per run) | 542–548 | 335–337 | 76–78 | 326–359 |

Toit's lead: Richards 4.3× (from 5.9×), DeltaBlue 4.4× (from 7.1×). nsl is now ahead of MicroPython on Richards and
level on DeltaBlue.

- **STIC:** nsl leads on alloc, array write, float math, integer math and collection iterate. Toit leads on
  dictionary and collection write and on string compare. MicroPython leads on dictionary and collection write
  (built-in dict and list are C).
- **Richards and DeltaBlue, which are send-heavy and object-heavy:** Toit is 6–7× faster than nsl. nsl roughly
  ties MicroPython on Richards and is 1.6× slower on DeltaBlue. This is the frame cost (#37 list item 4, ~500
  cycles a send round trip). It is the gap that is left.
- nsl's boot prints its image load and verification at 7.4–8.2 ms, beside the 97 ms `program sha256`.
