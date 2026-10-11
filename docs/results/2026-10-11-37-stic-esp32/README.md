# rete#37 — STIC on the plain ESP32, before and after the inlined iterators (2026-10-11)

The record is `nodus/oevm/docs/optimizations.md` Opt #8; the published column is `~/workspaceToit/stic/README.md`
(`rete/nsl (2026-10)`, msi). The WeAct companion is `docs/results/2026-10-11-37-stic/`.

- **Board:** ESP32-D0WDQ6 rev 1.0 (WROOM-32, 240 MHz, unicore), CP2104 on opti7040 USB. It is the E4 board's
  silicon (`nodus/oevm/zephyr/docs/results.md` E4, 2026-07-16).
- **Images:** before = master `0d773612` (before the merge), after = master `7e3c4e59`. Both were built on msi-7d43 by
  `37-esp32-build.sh` (copied here): `run.sh --board esp32`, node `nodus/tests/00_perf/performance.nsl`,
  E4's knobs (`OEVM_HEAP_SIZE=98304`, caches 256, `VM_STACK_INIT_WORDS=4096`, mark stack 512), WiFi off
  (`37-esp32-nowifi.conf`, `CONFIG_REBOOT=y` included, because WiFi had been selecting it), and **float slab 128 cells**.
  The build-ids read `-dirty` because of the layout file and the no-WiFi fragment placed in the tree.
- **Two things had to be fixed first.** `layout-esp32.json` predated #226 and refused every ESP32 bake; it was
  recaptured from this board by layoutdump (master `cec0f49a`, byte-identical to the C6's). E4's 512-cell float slab
  (8 KB) aborted in floatMath (`VM Float Arena Error: Out of Memory`, `logs/first-attempt/`). Today's image leaves
  ~3 KB of libc heap where July's left ~28 KB, so the slab is 128 cells (2 KB).
- **Driver:** `esp32_stic.py` (esptool from a scratch venv, then a reset with the reader already open), interleaved
  after/before × 3 (`driver.log`, `logs/esp32-stic-{after,before}-{1,2,3}.log`). Every boot repeats its image to the
  millisecond. Table: `python3 table.py`.

| test (ms, ESP32) | before ×3 | after ×3 | after ÷ before |
|---|---:|---:|---:|
| alloc | 19288 | 19289 | 1.00 |
| arrayWrite | 93815 | 93773 | 1.00 |
| dictionaryWrite | 25310 | 25490 | 1.01 |
| floatMath | 44149 | 44150 | 1.00 |
| integerMath | 5348 | 5347 | 1.00 |
| **collectionIterate** | **122586** | **22198** | **0.18 (5.5×)** |
| collectionWrite | 51233 | 50618 | 0.99 |
| stringCompare | 4159–4160 | 4159 | 1.00 |
| hanoi8 | 2 | 3 | (1 ms resolution) |

- **collectionIterate: 122.6 → 22.2 s**, 6.1 → 1.1 µs an element. Against the published ESP32 column: Toit 12.7,
  CircuitPython 29.2, uLisp 42.5. rete is second, and was last by 12× in July (532.6).
- **dictionaryWrite +0.7 %, deterministic, where the WeAct gained 7 %.** Its one site (`keys do: [:k | d at: k put: o]`)
  removes ten block activations per dictionary, ~90 µs of 2,550. Not explained. Placement on the ESP32's flash cache
  is the leading suspect: the rows with no site move by up to 1 % (collectionWrite −1.2 %).
- **Why array, dictionary and collection write are slow:** each kernel collection operation is a full send, about 9 µs
  on this part. arrayWrite is 1 send per store (9.4 µs), collectionWrite ~50 per collection, and dictionaryWrite ~300
  per dictionary (send counts read from the kernel source, not profiled).
