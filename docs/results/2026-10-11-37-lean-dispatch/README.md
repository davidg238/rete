# rete#37 — the lean dispatch loop on the WeAct RP2350B (2026-10-11)

The record is `nodus/oevm/docs/optimizations.md` Opt #10; the C6 spike that earned it is
`docs/spikes/2026-10-11-37-c6-insn-count/` (branch `37-insn-count-spike`).

- **Before:** master `2670924` (Opt #9 and the S2 settings merge). **After:** `37-lean-dispatch` `956555c`
  (format 11, the verifier, the loop without per-instruction checks). Both with the #296 WeAct console harness
  cherry-picked uncommitted (`faa30d4`, msi only), built on msi-7d43: `rpi_pico2`, `-O2`, USB console, node
  `nodus/tests/00_perf/performance.nsl`. Blob 164,536 → 164,544 B (+8, the header's method descriptor);
  FLASH 347,768 → 348,580 B (+812, the verifier); RAM 435,940 → 435,804.
- Driver: `docs/spikes/2026-10-10-296-image-in-sram/run_412.sh`, three boots each, blob in flash
  (`before.driver.log`, `after.driver.log`, `logs/`). Every row is identical across boots to within 1 ms.

| test (ms, WeAct RP2350B @ 150 MHz) | before ×3 | after ×3 | after ÷ before |
|---|--:|--:|--:|
| alloc | 16765 | 15946 | 0.951 |
| arrayWrite | 22867–22868 | **18806** | **0.822** |
| dictionaryWrite | 7392–7393 | 6706–6707 | 0.907 |
| floatMath | 25928–25929 | 25301–25302 | 0.976 |
| integerMath | 4804 | **4073–4074** | **0.848** |
| collectionIterate | 15773–15774 | 14156–14157 | 0.897 |
| collectionWrite | 17354 | 15912–15913 | 0.917 |
| stringCompare | 3128–3129 | 2907–2908 | 0.929 |
| hanoi8 | 2 | 1–2 | |

- **Every row moves, 2.4–17.8 %.** The interpreter-bound rows move most (integerMath −15 %, arrayWrite −18 %);
  floatMath, whose time is in the float primitives and the arena, least (−2.4 %). The C6 spike's blockcost rows
  moved 12–24 %; STIC's rows carry more primitive time per bytecode, so the smaller spread is expected.
- **The ±3 % placement noise every #37 record has seen does not explain this:** the direction is uniform across
  eight rows, and five of them move by more than 7 %.
