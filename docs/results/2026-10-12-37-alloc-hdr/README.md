# rete#37 — the one-word arena header: a heap census (2026-10-12)

**Verdict: kept (Opt #17), a memory lever. The smallest arena each AWFY workload verifies in shrinks 12–19 %
on the board layout: the board profile 154 → 136 KB, Storage 350 → 308 KB, Json 430 → 350 KB. Speed level on every
board (1.000 ± 0.002, measured with L5: `../2026-10-12-37-l5/` `run1/`).**

The change (`gc.c` only): `struct HeapHeader` was three words (`next`, `size`, `marked`), 12 B before every arena
object on the boards and 24 on the host. `next` means something only while a block is free, and the sweep walks the
heap by size, so a free block keeps its link in its first payload word (every block has at least one granule) and the
mark is bit 0 of the size (sizes are whole granules). 12 → 4 B per object on the boards, 24 → 8 on the host. Payload
alignment unchanged (4 on the boards, 8 on the host).

## The census

Both measurements build the image VM `-m32` (the boards' 32-bit layout, `dump_layout` under `-m32`) on msi, from
`git archive` of master `a303d2df` and the branch `2b576cea`, and run `nodus/tests/00_perf/awfy.nsl`.

**The smallest arena that verifies** (`minarena.sh`; 2 KB steps, a run counts only if it prints `verified: true`;
logs `logs/minarena-*.txt`). Workloads from `awfy_counts.py`: the board profile, and Storage and Json each alone —
the two rows the board profile drops because their live sets do not fit 256 KB.

| workload | master | one-word header | ÷ |
|---|--:|--:|--:|
| board profile (Bounce … DeltaBlue, no Storage/Json/Mandelbrot/NBody) | 154 KB | 136 KB | 0.883 |
| Storage alone | 350 KB | 308 KB | 0.880 |
| Json alone | 430 KB | 350 KB | 0.814 |

Neither revision runs Storage or Json in the board's 256 KB, so the board profile keeps leaving them out. Json gains
more than the other two; why (a live set of smaller objects, or less fragmentation) is not broken down here.

**Survivors at each collection** (`census.sh`: a throwaway patch prints the objects and header+payload bytes the sweep
keeps; `census_table.py`; logs `logs/census-*`). In a 1 MB arena the AWFY rows rarely collect; where they do, peak live
bytes fall 2–3 % (CD 0.967, Richards 0.981, Mandelbrot 0.986) on 120–330 live objects. In a steady state the live
heap is a ~68 KB floor held by a few large objects, so the per-object saving hardly shows. It shows at the peak, which
is what decides whether a program fits: the minimum-arena table above.

## Gates

msi, `2b576cea` (rebased onto master `a303d2df`): nsl 298/0, every aux gate PASS (footprint, portability-32 and
boot-gc-asan included), PORTA SYNC OK; Go: protocol, porta green, custos red only on #414 (`debug-obs`, red on master
too).

## Reproduce

On msi, from `~/workspace/rete` with both revisions present: `bash minarena.sh REV TAG` (→ `~/census/TAG-min.txt`),
`bash census.sh REV TAG` (`HEAP=` bytes, default 1 MB; → `~/census/TAG.log`); then `python3 census_table.py A.log
B.log`.
