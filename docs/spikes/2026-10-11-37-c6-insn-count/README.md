# rete#37 spike — instructions per bytecode on the C6, and a leaner dispatch loop (2026-10-11)

> Code on branch `37-insn-count-spike` (master `85df1360` plus three spike commits). Not for merging as it stands.
> Follows `docs/spikes/2026-10-11-37-c6-send-cost/` (branch `37-c6-send-cost-spike`), whose "next, before any design"
> was: count the instructions each row dispatches, then split one instruction.

**Question.** The send-cost spike estimated ~60–75 cycles per bytecode on the C6 from a guessed bytecode count, and
left two readings open: the core stalls (memory, branches), or the interpreter simply executes many native
instructions per bytecode. Which is it, and what does the cheapest dispatch change buy?

## Method

- **Bytecodes per row, on the host** (`OEVM_INSN_COUNT`, `src/vm.c`): a per-opcode histogram and a frames-pushed
  counter, printed and reset by `PRIM_millis`. The count depends only on the image, so the host's count is the
  board's. `logs/host-insn-blockcost.log` (blockcost at full scale; the board bench runs a tenth of it).
- **The C6's performance counter** (`OEVM_PERFMON`, ESP32-C6 only): custom CSRs `mpcer` 0x7e0 (event mask), `mpcmr`
  0x7e1 (enable), `mpccr` 0x7e2 (count), as `hal_espressif` `rv_utils.h` names them. A row's `t0 millis` selects the
  next event and zeroes the counter, its `t1 millis` reads it. Events: cycles, instructions, load hazards, loads,
  taken branches (bit 3, jump hazards, read 0 on every row: not implemented on this core, or not that bit).
- **Bench** `nodus/tests/00_perf/blockcost_pm.nsl`: blockcost at a tenth of the iterations (the counter is 32 bits:
  2^32 cycles is 26.8 s at 160 MHz), every row six times in one boot. Seven rows by six events, coprime, so each
  row sees each event once.
- c6spare (`/dev/ttyUSB1`), run.sh's C6 defaults (`-O2`, hot path in flash), `c6_run.py`. Two firmwares on one
  `.oimg` (byte-identical): master + counters (`logs/c6-pm-base.log`), and the same + `OEVM_LEAN_DISPATCH=1`
  (`logs/c6-pm-lean.log`). Table: `python3 pmtable.py logs/c6-pm-base.log logs/c6-pm-lean.log` (`table.md`).

## Results

The clock is **160.0 MHz** (cycles ÷ ms, every row).

| row (C6) | bytecodes | CPI | native inst / bytecode | cycles / bytecode | ms |
|---|--:|--:|--:|--:|--:|
| baseLoop | 9.0M | 1.39 | 36.5 | 50.7 | 2854 |
| blockValue | 14.0M | 1.36 | 58.5 | 79.7 | 6744 |
| methodSend | 13.0M | 1.34 | 67.0 | 89.5 | 7274 |
| blockCapture | 16.0M | 1.33 | 56.8 | 75.6 | 7454 |
| closureMake | 8.0M | 1.32 | 59.7 | 78.8 | 3900 |
| ocDo | 3.5M | 1.34 | 34.6 | 46.5 | 1017 |
| ocAt | 26.4M | 1.38 | 53.8 | 74.1 | 12234 |

1. **The core does not stall.** CPI is 1.32–1.39 on every row and load hazards are ~3 % of cycles. Memory latency
   and the cache are not the cost (the send-cost spike had already ruled out code placement).
2. **The cost is native instructions per bytecode: ~36 for the simple ones.** baseLoop's iteration is 9 bytecodes
   (`LD_CONST`×2, `LD_REG`×2, `ADD`×2, `LE`, `JMP_IF_FALSE`, `JMP`) and 328 RV32 instructions. From the disassembly:
   the dispatch head ~16 per bytecode (fetch, six field extracts, the jump-table bound check, the table load, `jr`,
   the `pc < pc_end` back edge), ~44 % of the row; `LD_CONST` ~33 (two operand bound checks, `is_in_blob` +
   `oimg_resolve`, an out-of-line `oevm_int_demote` call on a constant that is already an SMI); `ADD` ~25 (the
   ADD/SUB/MUL `op ==` chains, the overflow re-tag); `LE` ~20; `JMP_IF_FALSE` ~16 (a call to `is_truthy`);
   `LD_REG` ~12; `JMP` ~5. They sum to ~325.
3. **A send or block round trip is ~500 cycles beyond its bytecodes** (methodSend: +4 bytecodes and one frame over
   baseLoop, +707 cycles, +543 instructions). This agrees with #296's RP2350 model (~41 cycles a bytecode, ~400 a
   round trip; `docs/results/2026-10-10-296-image-in-sram.md` §9).
4. **Every STIC row on the C6 fits the model.** Host counts (`performance.nsl`): arrayWrite 78M bytecodes, no
   frames, 25.3 s = 52 cycles a bytecode. dictionaryWrite is not slow per bytecode: it runs **127 bytecodes and 3.8
   frames per insert** (12.7M and 380k for 100k inserts), where CPython does the insert in C.

### `OEVM_LEAN_DISPATCH` (four changes, measured together)

The jump table loses its bound check (`default: __builtin_unreachable()`); the loop loses `pc < pc_end` (every method
ends in `OP_RET`; the compiler appends one); `LD_CONST`/`LD_REG` lose their operand bound checks; `LD_CONST` tests
`IS_SMI` inline before calling `oevm_int_demote`. All four trust the image, which is a load-time verifier's job.

| row (C6) | cycles / bytecode | CPI | ms | lean ÷ base |
|---|--:|--:|--:|--:|
| baseLoop | 50.7 → 38.6 | 1.39 → 1.21 | 2854 → 2171 | **0.761** |
| blockValue | 79.7 → 68.5 | 1.36 → 1.28 | 6744 → 5750 | 0.853 |
| methodSend | 89.5 → 77.3 | 1.34 → 1.25 | 7274 → 6280 | 0.863 |
| blockCapture | 75.6 → 65.0 | 1.33 → 1.25 | 7454 → 6391 | 0.857 |
| closureMake | 78.8 → 68.6 | 1.32 → 1.23 | 3900 → 3389 | 0.869 |
| ocDo | 46.5 → 37.8 | 1.34 → 1.19 | 1017 → 826 | **0.812** |
| ocAt | 74.1 → 62.6 | 1.38 → 1.27 | 12234 → 10330 | 0.844 |

**12–24 % on every row.** Instructions fell 6–13 %, cycles 13–24 %: the removed compare-and-branches cost more than
their own count (CPI fell too).

## What it says for #37, and for 256 KB against 512 KB

- None of the levers needs RAM. #296 measured the image's placement at 0–5 % on the RP2350 and the send-cost spike
  measured code placement at ≤ 2 % on the C6. The per-bytecode and per-frame costs are instructions and are the
  same on a 256 KB part. No speed reason for a second build appears in the data; what 512 KB would buy is #296 §7's
  list (a writable image, runtime interning, inline caches), a language decision.
- The levers, cheapest first, each measurable on this bench:
  1. **Land the lean dispatch with a load-time verifier** (opcode range, register and constant operands against the
     method's counts, the trailing `OP_RET`) in `oimg_loader.c`, so the checks move rather than vanish.
     `oevm_int_demote` belongs at image build (#296 §7.2).
  2. **The rest of the head**: split `ADD`/`SUB`/`MUL` into their own cases, inline `is_truthy`'s two identity
     tests, decode the operand fields per case rather than all six up front; threaded dispatch only if the
     measurement after (1) still shows the head.
  3. **Fewer bytecodes from the compiler**: an add-immediate, a fused compare-and-branch, the loop constant loaded
     once. baseLoop is 9 bytecodes for what is ~3 operations.
  4. **The frame**, ~500 cycles a round trip.
  5. **dictionaryWrite**: 127 bytecodes an insert. The kernel's Dictionary probe as a primitive, the way CPython
     does it, or fewer bytecodes in the nsl probe.

## Not done

- The nsl suite under `OEVM_LEAN_DISPATCH` (host, opti7040): **297 passed / 0 failed**; aux red only on portability-32
  (no 32-bit headers on opti7040) and footprint SKIP (no cmake): the msi run, with the verifier, belongs to landing.
- `blockcost.nsl` at full scale under lean was not run on the board; the `_pm` bench's ms are a tenth-scale reading.
