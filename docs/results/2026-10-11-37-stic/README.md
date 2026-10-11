# rete#37 — STIC (`performance.nsl`) on the WeAct, before and after the inlined iterators (2026-10-11)

The record is `nodus/oevm/docs/optimizations.md` Opt #8; the design is
`docs/design/2026-10-11-37-do-inline-design.md`.

- **Before:** master at `0d773612` plus the #296 WeAct console harness (`main.c`, `CMakeLists.txt` from
  `37-do-inline-spike`; build-id `…ga77bee0…`). **After:** branch `37-do-inline` at `aa34ae1a` plus the same
  harness (build-id `…ga40c6a4…`). Both built on msi-7d43 by `~/37-stic-build.sh`: `rpi_pico2`, `-O2`, USB console,
  node `nodus/tests/00_perf/performance.nsl` (identical to `~/workspaceToit/stic/performance.nsl` but for one
  comment path). Blob 162,912 → 163,744 B (+832).
- Driver: `docs/spikes/2026-10-10-296-image-in-sram/run_412.sh`, three boots each, blob in flash
  (`before.driver.log`, `after.driver.log`, `logs/weact-stic-{before,after}-flash-{1,2,3}.log`). Table:
  `python3 table.py`.

| test (ms, WeAct RP2350B @ 150 MHz) | before ×3 | after ×3 | after ÷ before |
|---|---:|---:|---:|
| alloc | 18023–18024 | 18118–18119 | 1.01 |
| arrayWrite | 67654 | 68474 | 1.01 |
| dictionaryWrite | 19420–19422 | **18066** | **0.93** |
| floatMath | 26274–26275 | 25916 | 0.99 |
| integerMath | 4651–4652 | 4804 | 1.03 |
| **collectionIterate** | **100755** | **15918–15919** | **0.16 (6.3×)** |
| collectionWrite | 38705–38706 | 38586 | 1.00 |
| stringCompare | 3014 | 3071 | 1.02 |
| hanoi8 | 2–3 | 2–3 | |

- **collectionIterate: 100.8 → 15.9 s.** It is 100,000 × ten `oc do: [:e | e]` over 20 elements (20M elements):
  5.0 → 0.80 µs an element.
- **dictionaryWrite −7 %:** its `keys do: [:k | d at: k put: o]` is a literal `do:` over an Array; the `at:put:`
  sends remain.
- The rows with no iterator call site in them move by −1 % … +3 % (integerMath +3 %, arrayWrite +1 %, floatMath
  −1 %): code placement, as every #37 and #296 record has seen. Read within ±3 % as noise.
- **Not the published STIC column.** `~/workspaceToit/stic/README.md`'s rete column is a 240 MHz ESP32 (E4,
  2026-07-16), and no plain ESP32 is on the bench. For scale only, and across chips: the published ESP32
  collection iterate is Toit 12.7 s, CircuitPython 29.2 s, uLisp 42.5 s, and this WeAct is now 15.9 s.
