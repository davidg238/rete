# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **Next session starts with:** branch `272-progress-verdict` (pushed). Focus **#272** (P1). First action: David reviews
  `porta/docs/superpowers/plans/2026-10-03-272-stalled-verdict.md`; on his yes, execute it **natively** from Task 1
  (the store's mark columns + `LatestSample`), then run all three gates in Task 3.
- **Done 2026-10-03:** `311-line-in-spike` merged to master (`7666b4f`). The liveness model gained the rate rule
  (`12347f3`; `sound-rate.cfg` green over 38.4M states, 13/13 configs match). Plan `c4663d0`; design addendum D3.2.
- **Decided — do not re-litigate:** `#stalled` = arriving but behind the declared `telCad`, judged mark→latest `wak`, with
  one period of slack. An empty window keeps the verdict AND the mark; one sample = `#unknown`; the bound is
  `bud + SweepTick + telCad` (D3.2, model README). No `boot_id` join (#272 body). The VM debug protocol stays ids + offsets. **No SLIP.**
- **Debugger follow-ups, weekly review:** #321 + #322 (same code, do together), #323.
- **Not proven:** why an unpowered DS3231 blocks the bell's boot (#317); why a polled reader hangs the C6.
- **Bench (2026-10-02 22:57 MST, unchanged):** `c6bench` on the gateway's USB running the #311 debug image
  (`~/rete-bench/c6bench-311-pause-dbg.bin`; rollback `c6bench-118.bin`), collector running. `c6spare` unplugged; the
  bell on its wall supply (its next flash carries the interrupt-driven UART — daylight).
- **Awaiting David:** the #272 plan + native execution?; close #319?; mark buildmap 3 in `review/program-sequence.md`;
  `review/DOCMAP.md` line 56; the three cabot relay items; where #297 goes; #300.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #296–#300, #303, #304, #306–#309, #315–#323.
