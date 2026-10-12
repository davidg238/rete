# rete#372 on c6spare: what method visibility costs on a board (2026-10-12)

Board-first rule (FOCUS: a loop-body lever is measured on a board first). Images built on msi with
`docs/results/2026-10-12-37-send-frame/board_build.sh` (C6, 256 KB arena), run here with `board_ab.sh`
(`C6S="c6spare=/dev/ttyUSB1"`), one run per image; c6spare repeats to 0.1 % (base run 2 / run 1: AWFY 1.001,
STIC 1.000). `base` = master `8a3ffe4`; `vis` = `2ac280c` (phase B); `vis3` = `006eded` (super and private sends
cached, frameless shapes answered there).

| ratio to base | AWFY geomean | STIC geomean | worst rows |
|---|---|---|---|
| `vis` (run1/) | 1.034 | 1.011 | Permute 1.085, Queens 1.070, Towers 1.060 (recursive private methods, uncached) |
| `vis3` (run2/) | **1.018** | **1.010** | Permute 1.042, Towers 1.037, alloc 1.036, integerMath 1.035 |

Firmware text: `vis` −88 B, `vis3` −4 B against base. The AWFY image is +504 B (its explicit markings).

What is left, read from the rows: integerMath sends nothing visibility touches (only LargeInteger arithmetic
falling back from the inline ops), so its +3.5 % is the send path itself — `send_resolve`'s new `rule`
argument and the send handlers' new layout in the loop (the L2 lesson: code moved in the loop costs on a
board). Permute and Towers keep ~4 %: a private send is one more dispatched instruction per call (the
vtable constant) than the `OP_SEND` it replaced.
