# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **Next session starts with:** branch `311-line-in-spike` (pushed, `048f9dc`; do not rebase). #311 is CLOSED. First
  action: merge it into master on David's yes; then the liveness thread's next cheap move, **#272** (P1).
- **Done 2026-10-02 (#311):** the debugger by source line — `nodus/nsl/tools/dbg.py`, `docs/tutorials/04-debugger.md`
  (the `tutorial-debugger` aux gate), proven on `c6bench` over its cable (results doc part 4). Suite 288/0, aux all PASS.
- **Decided — do not re-litigate:** the VM protocol stays ids + offsets; the source translation is the host tool's; the
  `.stmap` names its `entry` (design addendum, in-image-debugger doc). A hooks image IS the debug image. **No SLIP.**
- **Debugger follow-ups, weekly review:** #321 (no breakpoint during next/finish), #322 (step by send — David wants
  into/over/out by send), #323 (call stack + named variables). #321 and #322 are the same code: do them together.
- **Not proven:** why an unpowered DS3231 blocks the bell's boot (#317); why a polled reader hangs the C6.
- **Bench (2026-10-02 22:57 MST):** `c6bench` on the gateway's USB running the #311 debug image
  (`~/rete-bench/c6bench-311-pause-dbg.bin`; rollback `c6bench-118.bin`), collector running. `c6spare` unplugged; the
  bell on its wall supply (its next flash carries the interrupt-driven UART — daylight); no C6 on the desktop.
- **Awaiting David:** merge `311-line-in-spike`?; close #319?; mark buildmap 3 in `review/program-sequence.md`;
  `review/DOCMAP.md` line 56; the three cabot relay items; where #297 goes; #300.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #296–#300, #303, #304, #306–#309, #315–#323.
