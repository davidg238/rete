# rete#37 — the send/frame levers L1 and L2, on the host and three boards (2026-10-12)

**Verdict (boards, below): L1 is kept (Opt #12); L2 is reverted. Its host win did not survive the boards' compiler.**

The levers are from `docs/design/2026-10-12-37-send-frame-cost-design.md` (David: take L1 and L2 first). The
baseline is `docs/results/2026-10-12-37-awfy-soup/` (rete `f6f6e472`, nsl ÷ Soup geomean 1.26).

- **Driver:** `ab.sh REV_A REV_B RUNS OUT`. Each revision is built from its own tree, with its own compiler and its
  own `-O2 -DOEVM_NO_DEBUG_HOOKS` image VM, because L1 changes the image format. Both run the same host-scaled
  `awfy.nsl`, A and B alternating, five runs a side. `abtable.py` reports medians and minimums and refuses an
  unverified log. Lever against lever is nsl against nsl: Soup's own run-to-run spread (±25 %) is wider than any
  lever here.
- **Host:** opti7040, i7-6700, gcc 16.2.1. The machine ran ~15 % faster through this session than in the baseline
  record (List 362 against 435 ms at the same revision), which is why the comparisons interleave rather than reuse
  the baseline's numbers.

## L1 — the access mechanism deleted (`f6f6e472` → `315d93da`, `l1-host/`)

The check never refused: the compiler never set a method's `access_modifier` (dialect row 25). Deleted together:
the check and its two helpers, `trigger_access_violation`, `Symbol_accessViolation`, the `CompiledMethod` field
(28 → 24 B on 32-bit), builtin #14 (registry version 6), the patch format's `access` line, and `test_access.c`.

| row | base | L1 | L1 ÷ base |
|---|--:|--:|--:|
| Bounce | 185 | 181 | 0.978 |
| List | 362 | 335 | 0.925 |
| Permute | 317 | 303 | 0.956 |
| Storage | 428 | 417 | 0.974 |
| Richards | 350 | 340 | 0.971 |
| Queens | 302 | 292 | 0.967 |
| Sieve | 158 | 158 | 1.000 |
| Towers | 298 | 279 | 0.936 |
| Mandelbrot | 1024 | 1043 | 1.019 |
| NBody | 4183 | 3877 | 0.927 |
| CD | 161 | 153 | 0.950 |
| DeltaBlue | 130 | 119 | 0.915 |
| Json | 269 | 269 | 1.000 |

**Geomean 0.963.** The send-heavy rows gain most (DeltaBlue −8.5 %, List −7.5 %, NBody −7.3 %, Towers −6.4 %). The
rows that hardly send are level (Sieve, Json). Mandelbrot +1.9 % is the only row that got worse.

## L2 — one frame push that answers its method (`l2a-host/`, `l2b-host/`, both against L1)

`push_frame` now returns the `CompiledMethod` it resolved, so the six send sites stop calling `closure_entry` a
second time, and `run_interpreter`'s entry frame goes through `push_frame` (its duplicate prologue is deleted).

| row | L1 | L2a (gcc chooses) | L2a ÷ L1 | L2b (always inlined) | L2b ÷ L1 |
|---|--:|--:|--:|--:|--:|
| Bounce | 180 | 198 | 1.100 | 181 | 1.006 |
| List | 334 | 337 | 1.009 | 324 | 0.967 |
| Permute | 298 | 326 | 1.094 | 298 | 0.993 |
| Storage | 417 | 436 | 1.046 | 412 | 0.986 |
| Richards | 338 | 338 | 1.000 | 329 | 0.965 |
| Queens | 292 | 317 | 1.086 | 287 | 0.986 |
| Sieve | 160 | 191 | 1.194 | 165 | 1.051 |
| Towers | 283 | 291 | 1.028 | 264 | 0.920 |
| Mandelbrot | 1058 | 1116 | 1.055 | 1050 | 1.003 |
| NBody | 3851 | 3958 | 1.028 | 3861 | 1.001 |
| CD | 153 | 155 | 1.013 | 150 | 0.974 |
| DeltaBlue | 120 | 122 | 1.017 | 116 | 0.959 |
| Json | 264 | 267 | 1.011 | 258 | 0.974 |
| **geomean** | | | **1.051** | | **0.983** |

- **L2a lost 5 %, and the reason was inlining, not the change itself.** At L1, gcc already inlined `push_frame` at
  all six sites (no `call push_frame` in `vm.o`). The folded prologue made a seventh site, and gcc then
  inlined only three of the seven, so four sends paid a call again. Sieve, which barely sends, lost 19 %:
  the loop's code layout moved under it.
- **L2b forces the inline (`always_inline`).** Against L1, the geomean is 0.983. Towers is −8 %, Richards −3.5 %
  and DeltaBlue −4 %. Sieve is still +5 %. The cost is code: x86-64 `vm.o` text grew from 26,424 to 28,104 B,
  because the prologue's copy is now inlined into `run_interpreter` as well.
- **Host gcc is not the board's gcc.** The C6 and the WeAct build at their own optimisation levels, with
  different inlining heuristics, so the inline decision has to be measured on a board before it is final.

## L1 + L2b against the baseline (`f6f6e472` → `54a0cc9a`, `l1l2b-host/`)

| row | base | L1 + L2b | ÷ base |
|---|--:|--:|--:|
| Bounce | 185 | 182 | 0.984 |
| List | 362 | 321 | 0.887 |
| Permute | 314 | 297 | 0.946 |
| Storage | 430 | 413 | 0.960 |
| Richards | 357 | 326 | 0.913 |
| Queens | 296 | 288 | 0.973 |
| Sieve | 157 | 161 | 1.025 |
| Towers | 301 | 265 | 0.880 |
| Mandelbrot | 1021 | 1036 | 1.015 |
| NBody | 4151 | 3853 | 0.928 |
| CD | 162 | 149 | 0.920 |
| DeltaBlue | 131 | 115 | 0.878 |
| Json | 270 | 261 | 0.967 |

**Geomean 0.943 measured in one A/B** (the product of the two separate runs predicted 0.947). The call rows moved
most: DeltaBlue −12 %, Towers −12 %, List −11 %, Richards −9 %. Two rows got worse: Sieve +2.5 % and Mandelbrot
+1.5 %. Against Soup, these cuts would put the geomean near 1.19 (1.26 × 0.943), with List still about 3.7×.

## Gates (msi-7d43, `37-send-frame` at `54a0cc9a`, L1 + L2b)

- nsl suite: **298 passed, 0 failed**, every aux gate PASS (L1 alone had failed `c-units` on `test_kernel_boot`,
  which still pinned registry version 5; fixed in `54a0cc9a`).
- Footprint ratchet: **MCU text 45,246 B (banked 45,356: −110)**, MCU data+bss 73,596 (banked 73,604), host text
  77,907 (banked 77,801, within the ratchet). On ARM the deleted access code more than pays for the forced inline.
- PORTA SYNC OK. Go: protocol and porta green; custos red only on #414 (`gate`, `debug-obs`), as on master.

## On the boards: L1 holds, L2 does not (2026-10-12, `board4/`)

Host gcc chose L2. The boards' compilers did not agree, so **L2 is reverted on the branch** (`dc34c106`, `0ff44cd7`;
the VM source is byte-identical to L1 again) and **L1 is the lever that lands.**

- **Boards:** c6spare and c6b (two ESP32-C6, QFN40 rev v0.0, 160 MHz) and a WeAct RP2350B (150 MHz, the #296 console
  harness, hot path in SRAM, blob in flash). The two C6 chips agree within 2 ms on every row of every image. A second
  WeAct (usb 1-5) wedged before this run and was left out.
- **Builds:** `board_build.sh` on msi-7d43, one revision at a time (base `f6f6e472`, L1 `315d93da`, L2a `45bfc754`,
  L2b `66372f9f`). Every image has a 256 KB arena: AWFY's board profile needs it, and STIC gets the same arena so that
  each board has one configuration. On the WeAct, the harness's unused SRAM blob buffer is cut from 192 KB to 4 KB to
  make room.
- **Runs:** `board_ab.sh` on opti7040: all boards in parallel, one run per image, revisions interleaved. The whole
  four-revision comparison took about 15 minutes. **AWFY here is `awfy_counts.py`'s board profile**: Storage and Json
  are dropped (their live sets do not fit 256 KB; the C6 aborts out of memory), as are Mandelbrot and NBody
  (soft-float, and AWFY verifies them only at sizes that take minutes or almost nothing). The rows interact through
  the heap, so a profile's numbers compare only with the same profile: base Towers reads 605 ms here and 511 ms
  with Mandelbrot and NBody still in the run.

| board | AWFY: L1 ÷ base | AWFY: L2a ÷ L1 | AWFY: L2b ÷ L1 | STIC: L1 ÷ base | STIC: L2a ÷ L1 | STIC: L2b ÷ L1 |
|---|--:|--:|--:|--:|--:|--:|
| C6 (both chips) | **0.936** | 0.995 | 0.981 | **0.986** | 1.021 | 1.022 |
| WeAct RP2350B | **0.961** | 1.004 | 1.024 | **0.997** | 1.021 | 1.035 |
| host x86-64 (above) | 0.963 | 1.051 | 0.983 | — | — | — |

- **L1 wins everywhere and costs nothing.** On the C6: DeltaBlue −13 %, List −7.4 %, Richards −3.5 %, alloc −6.7 %.
  On the WeAct: List −6.4 %, CD −3.7 %. STIC's loop rows do not move, and stringCompare on the C6 (+3 %) is the only
  row that got worse. MCU text −754 B (44,602 against the bank of 45,356; L1 + L2b was −110).
- **L2 loses on both boards, in both forms.** Each form saves a send's second `closure_entry`. The C6 sends are about
  level (AWFY 0.98–0.995: Richards −5.7 % and CD −7 % at L2b, but DeltaBlue +2.7 %), and the loop rows lose 2 %
  (collectionIterate +12 % at L2b). On the WeAct, L2b makes even the send rows slower (+2.4 %). This is Opt #11's
  lesson again (the arithmetic slow path inlined in the loop): changing the body of `run_interpreter` moves register
  allocation for every case in the loop, and what that costs depends on the compiler. A host win with gcc 16 x86-64
  -O2 says nothing about the boards' gcc 12. **The next send lever should be measured on a board first.**

#### c6spare (c6b identical within 2 ms on every row): AWFY, board profile

| row (ms) | base | L1 | L2a | L2b | L1 ÷ base | L2a ÷ L1 | L2b ÷ L1 |
|---|--:|--:|--:|--:|--:|--:|--:|
| Bounce | 77 | 75 | 75 | 76 | 0.974 | 1.000 | 1.013 |
| List | 394 | 365 | 364 | 348 | 0.926 | 0.997 | 0.953 |
| Permute | 185 | 178 | 179 | 177 | 0.962 | 1.006 | 0.994 |
| Richards | 6949 | 6709 | 6592 | 6328 | 0.965 | 0.983 | 0.943 |
| Queens | 180 | 177 | 179 | 175 | 0.983 | 1.011 | 0.989 |
| Sieve | 72 | 72 | 73 | 73 | 1.000 | 1.014 | 1.014 |
| Towers | 605 | 478 | 477 | 462 | 0.790 | 0.998 | 0.967 |
| CD | 19368 | 18743 | 17970 | 17476 | 0.968 | 0.959 | 0.932 |
| DeltaBlue | 863 | 752 | 743 | 772 | 0.871 | 0.988 | 1.027 |

#### c6spare (c6b identical within 2 ms on every row): STIC

| row (ms) | base | L1 | L2a | L2b | L1 ÷ base | L2a ÷ L1 | L2b ÷ L1 |
|---|--:|--:|--:|--:|--:|--:|--:|
| alloc | 13748 | 12828 | 13138 | 12787 | 0.933 | 1.024 | 0.997 |
| arrayWrite | 17606 | 17605 | 17824 | 18043 | 1.000 | 1.012 | 1.025 |
| dictionaryWrite | 6228 | 5956 | 5898 | 6040 | 0.956 | 0.990 | 1.014 |
| floatMath | 23068 | 23066 | 23286 | 23448 | 1.000 | 1.010 | 1.017 |
| integerMath | 3452 | 3453 | 3685 | 3491 | 1.000 | 1.067 | 1.011 |
| collectionIterate | 10635 | 10635 | 11157 | 11911 | 1.000 | 1.049 | 1.120 |
| collectionWrite | 13592 | 13247 | 13369 | 13186 | 0.975 | 1.009 | 0.995 |
| stringCompare | 2655 | 2736 | 2759 | 2743 | 1.031 | 1.008 | 1.003 |

#### WeAct RP2350B on usb 1-7: AWFY, board profile

| row (ms) | base | L1 | L2a | L2b | L1 ÷ base | L2a ÷ L1 | L2b ÷ L1 |
|---|--:|--:|--:|--:|--:|--:|--:|
| Bounce | 80 | 78 | 79 | 81 | 0.975 | 1.013 | 1.038 |
| List | 424 | 397 | 393 | 397 | 0.936 | 0.990 | 1.000 |
| Permute | 191 | 185 | 187 | 192 | 0.969 | 1.011 | 1.038 |
| Richards | 7605 | 7458 | 7464 | 7552 | 0.981 | 1.001 | 1.013 |
| Queens | 185 | 181 | 183 | 187 | 0.978 | 1.011 | 1.033 |
| Sieve | 69 | 69 | 70 | 74 | 1.000 | 1.014 | 1.072 |
| Towers | 603 | 520 | 519 | 525 | 0.862 | 0.998 | 1.010 |
| CD | 21165 | 20381 | 20360 | 20512 | 0.963 | 0.999 | 1.006 |
| DeltaBlue | 801 | 798 | 798 | 804 | 0.996 | 1.000 | 1.008 |

#### WeAct RP2350B on usb 1-7: STIC

| row (ms) | base | L1 | L2a | L2b | L1 ÷ base | L2a ÷ L1 | L2b ÷ L1 |
|---|--:|--:|--:|--:|--:|--:|--:|
| alloc | 15706 | 15380 | 15788 | 15740 | 0.979 | 1.027 | 1.023 |
| arrayWrite | 17272 | 17332 | 17639 | 18460 | 1.003 | 1.018 | 1.065 |
| dictionaryWrite | 6492 | 6529 | 6613 | 6713 | 1.006 | 1.013 | 1.028 |
| floatMath | 24235 | 24199 | 24286 | 24409 | 0.999 | 1.004 | 1.009 |
| integerMath | 3095 | 3091 | 3220 | 3261 | 0.999 | 1.042 | 1.055 |
| collectionIterate | 13069 | 13197 | 13679 | 13905 | 1.010 | 1.037 | 1.054 |
| collectionWrite | 15487 | 15220 | 15387 | 15553 | 0.983 | 1.011 | 1.022 |
| stringCompare | 2864 | 2852 | 2897 | 2929 | 0.996 | 1.016 | 1.027 |


### Earlier board rounds, kept as evidence

- `board/` (round 1): the first C6 driver bug (`c6run.py` missing), the 128 KB out-of-memory boot
  (`c6/oom-128k-base-awfy.log`), and the WeAct STIC at base and L1 + L2b, three runs each (1.031).
- `board2/` (round 2): every revision on c6spare, two passes, with Storage only dropped (Json faulted at the end, and
  each boot ran into the 10-minute cap). Same verdicts as `board4/`; pass 2 ÷ pass 1 is 0.999 AWFY, 1.000 STIC.
  WeAct STIC at every revision, three runs each.
- `board3/`: replication on c6b and the second WeAct, stopped part-way when the harness was rebuilt (the logs
  there are partial).

## Gates for what lands (msi-7d43, `37-send-frame` at `0ff44cd7`: L1, L2 reverted)

nsl suite **298 passed, 0 failed**, every aux gate PASS. Footprint ratchet: **MCU text 44,602 B (banked 45,356:
−754)**, MCU data+bss 73,596, host text 76,929 (banked 77,801). PORTA SYNC OK. Go: protocol and porta green; custos
red only on #414, as on master.
