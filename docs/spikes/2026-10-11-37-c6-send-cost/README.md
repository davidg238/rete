# rete#37 spike — what a send costs on the board: c6spare, the hot path in flash vs in IRAM (2026-10-11)

> Code on branch `37-c6-send-cost-spike` (master `173211b5`, Opt #9, plus one commit: `OEVM_IRAM_HOT=1` lets the
> esp32c6 put the `OEVM_HOT` functions in IRAM). Not for merging as it stands. David's Q4 of 2026-10-11: measure send
> cost on the board before any design.

**Question.** The send-levers record (`docs/spikes/2026-10-11-37-send-levers/`) left a send at ~1,050 cycles on the
WeAct against ~300 for a whole iteration on the host, and guessed that XIP memory latency was a large share. On a
board, how much of a send is code fetch, and how much is the lookup or the frame?

**Method.** `nodus/tests/00_perf/blockcost.nsl` (10M iterations a row, each row differing from `baseLoop` by one
mechanism), two images of one source (`37-c6-bc.sh`): the C6's own build, where the hot path runs from flash through
the cache (run.sh's default), and `OEVM_IRAM_HOT=1`, where `run_interpreter` and the other `OEVM_HOT` functions are in
IRAM (`run_interpreter` at 0x40803282 against 0x420024a8; run.sh's placement check still prints "FLASH" because it
knows only the Xtensa ranges). ESP32-C6 at 160 MHz (devicetree `clock-frequency` 160000000), interleaved flash/IRAM ×3
(`driver.log` gives each boot's start time). Each image repeats itself to within 1 ms. Table: `python3 table.py`.

| row (ms, C6) | flash ×3 | IRAM ×3 | IRAM ÷ flash |
|---|---:|---:|---:|
| baseLoop | 28535 | 27925 | 0.979 |
| blockValue | 67439–67440 | 67134 | 0.995 |
| methodSend | 72736 | 72298 | 0.994 |
| blockCapture | 74607 | 74865 | 1.003 |
| closureMake | 39036–39037 | 39222 | 1.005 |
| ocDo | 10169–10170 | 10269 | 1.010 |
| ocAt | 122338–122339 | 122888–122889 | 1.004 |

## What it says

1. **Code placement is not the cost on the C6.** Every row is within 2 % between flash and IRAM, in both directions.
   The hot path fits in the cache. The XIP guess does not hold here. (The WeAct's hot path has been in SRAM since #403,
   so it was never the explanation there either.)
2. **A monomorphic send is ~4.4 µs, ~707 cycles** (`methodSend − baseLoop`). That is ~0.66 of the WeAct's ~1,050
   cycles, and in µs the C6 is faster than the WeAct (7 µs) and the plain ESP32 (9 µs).
3. **The frame dominates, not the lookup.** `methodSend − blockValue` (the lookup, `send_resolve`) is ~0.53 µs,
   **~85 cycles**. `blockValue − baseLoop` (push the frame, run the callee's own instructions, return) is ~3.9 µs,
   **~620 cycles**. The host gprof put `send_resolve` at 19 % and `push_frame` at 8 % of a send, but on the board the
   lookup is about an eighth of it.
4. **The per-instruction cost is the likely multiplier.** `baseLoop` alone is 2.85 µs, **~457 cycles an iteration**,
   for an inlined `1 to: N do: [:i | i + 1]`. The image map puts its whole loop, setup included, at pc 3–14, so an
   iteration is at most ~12 instructions and more likely 6–8: **~60–75 cycles an instruction**. A register VM's switch
   dispatch on a 160 MHz RV32 should cost a fraction of that. `push_frame` is about eight stores and a clearing loop,
   and the loop head adds one load and a branch (`g_bp_count`). Neither accounts for 620 cycles, but the 3–5
   instructions a call and return add would, at that rate.

## Not done (the next measurement, before any design)

- **Count the instructions.** Run a build that counts dispatched instructions per row, on the host and on the board
  (one counter, printed by the bench). That turns item 4's estimate into a measured cycles-per-instruction figure, and
  shows how many instructions a send adds.
- **Then split one instruction**, with `rdcycle` (mcycle) around fetch/decode, the `switch` and the handler body of a
  few opcodes (OP_ADD, OP_LE, OP_JMP, OP_RET), in a throwaway build.
- The tunables that would earn a design: cycles/instruction (now ~60–75, estimated) and instructions added per send
  (unknown). If cycles/instruction is the lever, the design is in the dispatch loop (threaded dispatch, fewer
  per-instruction checks, or both), not in `send_resolve`.
