# rete#37 — STIC on c6spare, Opt #8 and the send levers (Opt #9) (2026-10-11)

The record is `nodus/oevm/docs/optimizations.md` Opt #9. The WeAct and ESP32 companions are
`docs/results/2026-10-11-37-stic/` and `docs/results/2026-10-11-37-stic-esp32/`. From here on, STIC runs on c6spare
only (David, 2026-10-11: the plain ESP32 is retired from #37).

- **Board:** c6spare, ESP32-C6 (RISC-V, 160 MHz) on opti7040 USB, CP2102N `…d2e21074…` at `/dev/ttyUSB1`. Its
  wedge-quiet probe was overwritten (allowed, David 2026-10-11).
- **Images:** Opt #8 = master `7e3c4e59`, levers = `8914831` (the `37-send-levers` tip, merged as `173211b5`). Both were
  built on msi-7d43 by `37-c6-build.sh`: `run.sh --board esp32c6`, node `nodus/tests/00_perf/performance.nsl`, run.sh's
  C6 defaults (128 KB arena, no radio). Model B: the program goes to `program_a` at 0x220000, flashed beside the firmware.
  The hot path runs from flash (XIP) on the C6. `OEVM_HOT` places it in RAM only on Xtensa and the RP2350.
- **Driver:** `c6_stic.py` (esptool 5.5 from a scratch venv; then a reset with the reader already open). The first
  levers boot ran alone to time it (~2 min). The rest ran interleaved: opt8, levers, opt8, levers, opt8
  (`driver.log`, `logs/c6-stic-{opt8,levers}-{1,2,3}.log`). Each image repeats itself to within 1 ms. Table:
  `python3 table.py`.

| test (ms, C6) | Opt #8 ×3 | levers ×3 | levers ÷ Opt #8 |
|---|---:|---:|---:|
| alloc | 14384–14385 | 14418–14419 | 1.00 |
| **arrayWrite** | 67814–67815 | **25285–25286** | **0.37 (2.7×)** |
| **dictionaryWrite** | 17068–17069 | **7490–7491** | **0.44 (2.3×)** |
| floatMath | 23654–23655 | 23665–23666 | 1.00 |
| integerMath | 5285–5286 | 5285–5286 | 1.00 |
| collectionIterate | 13931–13932 | 13931–13932 | 1.00 |
| **collectionWrite** | 36596 | **15994–15995** | **0.44 (2.3×)** |
| stringCompare | 3074–3075 | 3062 | 1.00 |

- The three write rows move as they did on the WeAct (3.0× / 2.6× / 2.3× there, against the spike's `==` build). This
  build compares stored hashes with `=`, and dictionaryWrite comes out at the same 7.5 s.
- The rows with no lever move by at most 0.4 % (stringCompare −0.4 %, alloc +0.2 %). Read that as placement.
