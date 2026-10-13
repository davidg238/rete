# STIC `performance.nsl` on the plain ESP32 at master `38a5500f` — 2026-10-12

*Found-during rete#233 (an evening side trip once the network was reflashed). The 2026-10-11 baseline on the same
silicon is `docs/results/2026-10-11-37-stic-esp32/` (rete#37, the "after" image `7e3c4e59`).*

- **Board:** ESP32-D0WDQ6 rev 1.0 (WROOM-32), CP2104 `01558C55` on opti7040 USB, MAC `30:ae:a4:1a:62:08`.
- **Image:** `porta-v1-final-1686-g38a5500f-20261012T185102`, built on msi by `build.sh`: #37's recipe and knobs
  (96 KB arena, caches 256, VM stack 4096, mark stack 512, float slab 128 cells, WiFi off via #37's
  `37-esp32-nowifi.conf`). It differs from #37 in one input: the **committed** `layout-esp32.json`. #37's script copied
  `~/layout-esp32.json` from msi's home, which predates #37 L6 and #372 and would bake a wrong image today.
  dram0 193,112 / 196,608 (98.2 %); `[build] hot path: SRAM (run_interpreter @ 0x40082be8)`.
- **Driver:** #37's `esp32_stic.py` with the port set to this board's by-id path. Three runs, identical to the
  millisecond (`logs/wroom32-run{1,2,3}.log`).

| test (ms) | 2026-10-11 `7e3c4e59` | 2026-10-12 `38a5500f` | ratio (10-12 ÷ 10-11) |
|---|---:|---:|---:|
| alloc | 19,289 | **25,224** | **1.31** |
| arrayWrite | 93,773 | 21,839 | 0.23 |
| dictionaryWrite | 25,490 | 6,359 | 0.25 |
| floatMath | 44,150 | 38,944 | 0.88 |
| integerMath | 5,347 | 2,800 | 0.52 |
| collectionIterate | 22,198 | 17,289 | 0.78 |
| collectionWrite | 50,618 | 14,492 | 0.29 |
| stringCompare | 4,159 | 2,543 | 0.61 |
| hanoi8 | 3 | 2 | (1 ms resolution) |
| **sum of timed rows** | **265,024** | **129,490** | **0.49** |

- **The collection-send rows are each ~4× faster.** That is #37's later work (Opt #17–#19) on the cost of a kernel
  collection send, which #37 measured at ~9 µs on this part.
- **alloc is 31 % slower, and only here.** On c6spare the same row went 14.4 s (2026-10-11) → 10.9 s (master before
  #372) → 11.3 s (#372). Cause not investigated: rete#428.
