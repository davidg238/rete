# rete#37 M — the counting build: is the frame Toit's lead? (2026-10-12)

**Verdict: the send-and-frame is the largest single cost on the call-heavy rows (about 520–730 C6 cycles per frame,
61–72 % of Richards, Towers, DeltaBlue and List), so the levers that cut it (L3, L4, L5) are the right next work. But it
is not the whole of Toit's 5.9× on Richards: with frames free, Richards would still take ~2.3 s against Toit's 1.18 s,
because the base instruction costs ~36 cycles. The implicit-send linear method scan is under 1 % everywhere; not a lever.**

Design: `docs/design/2026-10-12-37-send-frame-cost-design.md` §M, step 1. Step 2 (the `rdcycle` split of the
~640 cycles across `send_resolve`, `push_frame` and the return) is still to do.

## The build

`OEVM_COUNT=1` (host `-DOEVM_COUNT`, Zephyr env `OEVM_COUNT=1`; never shipped) adds seven 64-bit counters (oevm.h
`struct OevmCount`): instructions dispatched, explicit sends, implicit sends, bindings compared by a linear method scan
(`OP_SEND_IMPLICIT`'s lexical walk, `lookup`, `lookup_defining_vtable`), frames pushed, block fast-path invokes, and
`num_registers` summed over frames. The window is the existing `profileStart`/`profileStop` prims, so no prim id is
consumed and a production build is unchanged. `awfy_counts.py PROFILE --count` brackets each row's timed block with
them, which prints a `count:` line before each row line. `board_build.sh` takes `COUNT=1`.

## How the numbers were made

- **Counts:** the host counting build (rete `12e50b19` + this change) running AWFY's **board** profile
  (`board-count.log`). The counts depend only on the image and the VM's semantics, not on the platform, so the host
  stands in for the board here; a count build on c6spare would confirm it and was not run.
- **Times:** c6spare at L1 (Opt #12, master), `../2026-10-12-37-send-frame/board4/c6spare/l1-awfy.log`; c6b agrees
  to the millisecond. 160 MHz.
- **Attribution** (`frame_cost.py board-count.log <time log>`): Sieve pushes 9 frames in 318 k instructions, so its
  36.2 cycles per instruction is the base. A row's cycles above base × instructions are charged to its frames, which
  therefore also carry the primitive sends (a third of Richards' sends end in a primitive, not a frame).

| row | ms | instructions | sends | implicit | frames | block invokes | insn/frame | regs/frame | cycles/insn | cycles/frame | frame share |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Bounce | 75 | 220627 | 6311 | 1427 | 7035 | 101 | 31.4 | 13.0 | 54.4 | 569 | 33 % |
| List | 365 | 444627 | 83592 | 0 | 80746 | 1 | 5.5 | 3.8 | 131.3 | 524 | 72 % |
| Permute | 178 | 497266 | 27408 | 1 | 18746 | 1 | 26.5 | 14.4 | 57.3 | 558 | 37 % |
| Richards | 6709 | 10212828 | 1652089 | 9 | 1103063 | 1 | 9.3 | 5.9 | 105.1 | 638 | 66 % |
| Queens | 177 | 441570 | 30866 | 40 | 12205 | 1 | 36.2 | 12.7 | 64.1 | 1009 | 43 % |
| Sieve | 72 | 317794 | 11 | 1 | 9 | 1 | 35310.4 | 8.6 | 36.2 | — | — |
| Towers | 478 | 830049 | 98131 | 0 | 89809 | 1 | 9.2 | 6.4 | 92.1 | 517 | 61 % |
| CD | 18743 | 19504249 | 2953101 | 376066 | 2100603 | 10680 | 9.3 | 6.9 | 153.8 | 1091 | 76 % |
| DeltaBlue | 752 | 1059912 | 134456 | 919 | 112793 | 17139 | 9.4 | 5.8 | 113.5 | 726 | 68 % |

## Reading it

- **A frame costs 520–730 cycles** on the six rows whose non-call work is plain (Bounce, List, Permute, Richards,
  Towers, DeltaBlue). The 10-11 spike's ~707 for a monomorphic send, before Opt #10 and #11, sits at the top of that
  range. Queens and CD read higher because their non-call instructions are dearer than Sieve's (CD is float-heavy,
  soft-float on the C6), and the method charges that to the frames; they are upper bounds.
- **Frames are dense:** one every ~9 instructions on Richards, Towers, CD and DeltaBlue, one every 5.5 on List.
- **Registers per frame are small (4–7 on the call-heavy rows)**, so the NULL fill and the argument copy in
  `push_frame` are a few stores each; a Lua-style overlapping frame (design: "not now") is not indicated.
- **The base instruction is the other half.** 36 cycles per instruction even on Sieve. Richards' 10.2 M
  instructions at 36 cycles is 2.3 s on their own, about twice Toit's whole run, so closing the gap needs the
  dispatch cost cut as well as the frame. Lever candidates there are the queue's item 5 (fused compare-branch,
  add-immediate: fewer instructions) and where the loop runs from on the C6 (flash XIP versus RAM, as #403 decided
  for the RP2350); neither is measured yet.
- **The linear method scans are negligible.** `OP_SEND_IMPLICIT` walks the lexical chain's bindings linearly and
  then calls the uncached `lookup`, but implicit sends are rare in AWFY (CD's 376 k is the most) and CD's 3.9 M
  compares are under 1 % of its 3 G cycles. Not a lever on these benchmarks; it would be on code that sends to
  `self` implicitly in a hot loop through a deep class.

## Files

- `board-count.log`: the counting run of the board profile (host, `vm-count`).
- `host-count.log`, `host-times/`: the host profile, counting run and three timing runs (opti7040, `-O2`). A
  least-squares fit over the host rows did not separate the costs (per-instruction cost varies too much between rows);
  the host's Richards is ~70 % above Sieve's base, consistent with the board.
- `frame_cost.py`: the table above, from any count log and time log.
