# rete#37 — Are We Fast Yet on the host: nsl against Primordial Soup (2026-10-12)

The baseline for the send/frame work (`docs/design/2026-10-12-37-send-frame-cost-design.md`, L1 + L2): every
attempt there is measured against this table, on the host first and then on c6spare.

- **Source:** `nodus/tests/00_perf/awfy.nsl` at rete `33cb0e16` (the port, its departures and how Soup gets the same
  source: `nodus/tests/00_perf/AWFY.md`). Havlak is not in it (host-only, not merged).
- **Host:** opti7040, i7-6700 at 3.4 GHz, gcc 16.2.1 (`logs/env.txt`). Sequential, nothing else running.
- **nsl:** the image VM as the suite builds it, plus `-O2 -DOEVM_NO_DEBUG_HOOKS` (the suite's own `oevm_img` is
  unoptimised).
- **Soup:** Primordial Soup `9ac43be`, ReleaseHost, driven by `nodus/tests/00_perf/awfy-soup.py`.
- **Driver:** `run_all.sh [RUNS]` (three runs here). It scales the board-sized `inner:` counts in `runAll` up to
  host size (Bounce 300, List 100, Permute 200, Storage 200, Richards 5, Queens 200, Sieve 300, Towers 60, NBody
  250000, DeltaBlue 2000, Json 20; Mandelbrot 500 and CD 10 unchanged, as sizes AWFY verifies), builds, runs both
  VMs, and writes `table.md` with `table.py`. Every log ends `verified: true`, and `table.py` refuses one that does not.

| row | nsl ms (median) | nsl runs | Soup ms (median) | Soup runs | nsl ÷ Soup |
|---|--:|--:|--:|--:|--:|
| List | 435 | 404/435/436 | 105 | 104/110/105 | 4.14 |
| Storage | 603 | 619/603/592 | 309 | 309/344/231 | 1.95 |
| Json | 348 | 348/385/309 | 197 | 192/197/204 | 1.77 |
| Towers | 367 | 367/377/362 | 210 | 209/231/210 | 1.75 |
| DeltaBlue | 198 | 198/222/193 | 115 | 82/115/117 | 1.72 |
| NBody | 5334 | 5298/5708/5334 | 3651 | 3651/4480/3471 | 1.46 |
| Permute | 377 | 359/379/377 | 275 | 275/321/254 | 1.37 |
| Queens | 370 | 374/370/356 | 294 | 294/323/286 | 1.26 |
| Richards | 453 | 453/454/433 | 477 | 477/511/317 | 0.95 |
| Sieve | 192 | 192/201/190 | 221 | 221/246/219 | 0.87 |
| Mandelbrot | 1446 | 1446/1523/1439 | 2294 | 2294/2747/2079 | 0.63 |
| Bounce | 226 | 226/226/224 | 367 | 314/422/367 | 0.62 |
| CD | 218 | 208/231/218 | 391 | 391/383/451 | 0.56 |

**Geomean nsl ÷ Soup over 13 rows: 1.26** (the session's first single run read 1.31; `RESULTS.md`'s older figure
was ~1.9).

## What the data says

- **nsl is slower on the call-heavy and allocation-heavy rows.** List (4.1×) is recursion over linked cells: a send
  and a frame per element and almost nothing else. Towers, Permute and Queens (1.3–1.8×) are recursion depth. Storage
  (2.0×) is allocation. These are the rows L1 and L2 should move, and GC performance after them.
- **nsl is faster where the work is inside the frame:** arithmetic (Mandelbrot 0.63×, Bounce 0.62×), array loops
  (Sieve 0.87×), and CD (0.56×). Richards is level.
- **Soup's runs vary more than nsl's**, up to ±25 % (Richards 317–511, DeltaBlue 82–117). nsl stays within ~10 %. The
  median of three is the number to compare. A change smaller than ~10 % on one row needs more runs before it counts.
- This is host x86-64, not a board. It ranks the levers. The board number (c6spare) is the one that decides.
