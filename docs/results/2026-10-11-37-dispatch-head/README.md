# rete#37 — the dispatch head on the WeAct RP2350B (2026-10-11, late)

The record is `nodus/oevm/docs/optimizations.md` Opt #11. It follows Opt #10
(`docs/results/2026-10-11-37-lean-dispatch/`), whose "after" is this record's baseline, reproduced here to the
millisecond.

- **Board and build:** the WeAct RP2350B at 150 MHz, built on msi-7d43 by `~/37h-build.sh`: `rpi_pico2`, `-O2`, USB
  console, `OEVM_SPIKE_296=1`, node `nodus/tests/00_perf/performance.nsl`. The #296 console harness was applied
  uncommitted (msi only), as in every #37 WeAct record. Blob in flash, hot path in SRAM.
- **Driver:** `docs/spikes/2026-10-10-296-image-in-sram/run_412.sh`, three boots per image (`<tag>.driver.log`,
  `logs/`). Every image repeats itself to within 1 ms.
- **Variants.** Each one is a commit on `37-dispatch-head-nofwd` (kept) or `37-dispatch-head` (the forwarding line, dropped):

| tag | what | commit |
|---|---|---|
| h0-base | master `dca8ad1d` (Opt #10) | |
| h1-arith | ADD/SUB/MUL get their own SMI fast paths on the tagged words, the rest goes through an out-of-line `arith_slow`; `JMP_IF_FALSE` tests the two boolean statics before it calls `is_truthy` | `c2c3d7af` |
| h3-decode | h1 + each case decodes the operand fields it reads (`DEST`/`SRC1`/`SRC2`), not all three up front | `f9e26472` |
| h4-loadtime | h3 + the boot prints `oevm: image loaded and verified (N us)` | `32bc23ff` |
| h5-forward | h4 + peephole (C), result forwarding (`i:: i + 1` is one ADD) | `37-dispatch-head` |
| h6-arithlabel | h5, with the slow path inlined at one in-loop label instead of called | `37-dispatch-head` |
| h7-nofwd | h6 without the forwarding | (rebased, not kept) |
| **h8-floatfirst** | h4 + `arith_slow` tests Float with Float first | **`532a2391`** |

| test (ms) | h0 base | h1 | h3 | h5 | h6 | h7 | **h8** | h8 ÷ base |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| alloc | 15946 | 16219 | 15832 | 16192 | 16451 | 16093 | **15828** | 0.993 |
| arrayWrite | 18806 | 19259 | 17299 | 17178 | 17565 | 17692 | **17272** | **0.918** |
| dictionaryWrite | 6707 | 6840 | 6494 | 6654 | 6779 | 6630 | **6501** | 0.969 |
| floatMath | 25302 | 27970 | 27658 | 27618 | 25927 | 26006 | **24234** | 0.958 |
| integerMath | 4073 | 3443 | 3091 | 3020 | 2969 | 3045 | **3095** | **0.760** |
| collectionIterate | 14157 | 14754 | 13057 | 13044 | 13399 | 13411 | **13069** | **0.923** |
| collectionWrite | 15912 | 16197 | 15596 | 15601 | 15907 | 15917 | **15590** | 0.980 |
| stringCompare | 2907 | 2968 | 2857 | 2853 | 2903 | 2915 | **2864** | 0.985 |

## What the data says

- **h8 is better than the baseline on every row: integerMath −24 %, arrayWrite −8 %, collectionIterate −8 %,
  floatMath −4 %, the rest 1–3 %.** The change is to the C only. The compiler and the image are unchanged
  (blob 164,544 B, as in Opt #10).
- **The per-case operand decode is the main win (h1 → h3).** On ARM the dispatch head went from nine instructions to five
  (`ldr`, `uxtb`, `cmp`/`bhi`, `tbh`). GCC 12 keeps the jump table's bound check even with
  `default: __builtin_unreachable()`. Those are the remaining two instructions, and removing them would need computed goto.
- **The arithmetic split alone (h1) won integerMath (−15 %) but cost the other rows 2–4 % and floatMath +10.5 %.**
  The slow path became a `long_call` into SRAM, which every Float operation pays. Inlining it again at one label (h6,
  h7) gave floatMath back, but cost every other row 2–3 %. That is register pressure in a bigger loop function. h8 keeps
  the call and moves the case that matters (Float with Float) to the top of `arith_slow`, ahead of two cross-file
  `oevm_is_integer` calls. floatMath is 12M soft-double operations at ~2 µs each.
- **Result forwarding (h5) was dropped.** It cut integerMath another 2.3 % (8 bytecodes fewer per iteration). In
  exchange it moved alloc and dictionaryWrite by +2 % (placement or real, not separated), and it changes compiled
  output, so the checked-in fixture (`nsl-tools-py`) and custos's pinned offsets would need re-pinning. That cost
  is not worth 2 %.
- **The load-time verifier costs 11.2–11.3 ms a boot** for this 164 KB image on the WeAct (`oimg_load_blob`: header
  plus the method walk, from flash). For scale, the C6's `program sha256` of the same program takes 99 ms
  (`docs/results/2026-10-11-37-stic-c6/logs/`). The S2 (MagTag) number still has to be read on the board, which
  enumerates only in BOOT0+RESET.

## Gates (msi-7d43, `37-dispatch-head-nofwd` at `532a2391`)

- nsl suite: **298 passed, 0 failed** (297 before plus `08_smi_arith_edges`). Every aux gate PASSes
  (portability-32 included) except footprint(T0.3): MCU text 45356 against 44252. It is re-banked with
  attribution in `nodus/oevm/tools/footprint-mcu-baseline.txt`: master's unbanked Opt #10 +860, decode +156,
  Float-first +88. Host text 77801, inside its 2 %.
- `45_udp_bridge` Path B failed in two of the three msi runs because Chrome held UDP 54341 (the test's fixed port)
  as an ephemeral QUIC socket (`ss -uanp`). It passed in the h8 run.
- The verifier on the C6 (c6spare, h8, `docs/results/2026-10-12-37-c6-cross-vm/`): 7.4 ms, beside a 97 ms
  `program sha256` on the same boot.
