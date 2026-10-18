# invarlint#54 backends-home on the C6: speed A/B of the move (2026-10-18)

**Verdict: no change.** `93d3d3c7` (before) → `61f94e3a` (runner.c, log_udp.c in `src/backend/zephyr`): geomean
AWFY 1.000, STIC 1.000 on c6spare, every row the same across two interleaved passes (`run1/`, `tables.md`).
Gates at `61f94e3a`: nsl 303/0, every aux PASS (invarlint PASS); target 12/12 (not porta-refuse/porta-silent).

**Arena 252 KB, not 256.** At 256 KB `93d3d3c7` no longer links (`sram0_0_seg` over by 2,824 B), so absolute times
are not comparable with the 10-12 tables; the ratio is the measurement.

**Rerun for any change:** `./ab.sh REV_A REV_B [OUT]` on msi with c6spare attached builds both (`c6_build.sh`,
worktree `~/rete-board`), runs them (`run_ab.sh`: custos lends the cable, `c6run.py` in esptool 4 syntax) and
prints `table.py`'s table. ~10 min build, ~20 min board.
