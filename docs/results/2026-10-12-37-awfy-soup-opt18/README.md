# rete#37 — Are We Fast Yet on the host after Opt #12–#18 (2026-10-12, night)

The morning baseline's driver (`../2026-10-12-37-awfy-soup/`, its README has the method), rerun unchanged at master
`13d5e6ef` (Opt #18; L4(b) not in it). Same host (opti7040), Soup `9ac43be`, three runs each, sequential.

**Geomean nsl ÷ Soup: 1.01 (morning: 1.26).** nsl is ahead on 6 of 13 rows; Soup's lead is the allocation-heavy rows,
List 2.67 and Storage 1.65 (morning 4.14 and 1.95).

`table.md`; logs `logs/`. Compare ratios, not milliseconds, across the two sessions: Soup's own code did not change and
it ran 15–30 % faster than in the morning (List 105 → 89 ms, Richards 477 → 334), so the host was in a faster state and
part of nsl's raw drop is that.
