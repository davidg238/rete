# rete#37 — the send/frame levers L1 and L2, on the host (2026-10-12)

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

## Not yet measured

- c6spare (AWFY and blockcost) and the WeAct STIC: the inline decision was made by host gcc and has to hold on
  the board's compiler. The send/frame design names the board as the number that decides.
