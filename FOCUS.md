# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **Next session starts with:** branch `272-progress-verdict` (pushed). Focus **#272** (P1). First action: execute
  `porta/docs/superpowers/plans/2026-10-03-272-count-on-the-pull.md` **natively**, Tasks 1–4 (Task 1: `wakes` in the
  kernel bridge, `#wak` in `pullFrame`), then STOP before Task 5 (board gate, reflash, gateway deploy, the C6 bell).
- **Done 2026-10-03:** the D3.2 plan's Tasks 1–3 built and gated green (`4513b81`, `4b6820b`, `614f501`), then the
  whole-branch review refuted its premise; the model reproduces it (`474bfa0`); design D3.3 + the new plan (`c3681e6`).
- **Decided — do not re-litigate (design D3.3):** the tick count rides the **pull** (`#wak`, required; "no wire change"
  is withdrawn). porta reads ONE sample (count + pull arrival) for silence and rate; the health guard is deleted. Past
  the budget the mark is forgotten; a window spanning a porta outage is re-marked, not judged. An image without the
  count reads `#unknown` — no arrival-only fallback. The rule itself (D3.2: mark→latest, one period of slack, bound
  `bud + SweepTick + telCad`) stands. Slack, restart naming and stale reasons are **left to the fleet** (#326).
- **Deploy order is nodes first** — today's handler serves a pull carrying the extra field (probed 2026-10-03).
- **Not proven:** why two `-Os` RP2350 builds differ 21% per activation (flash placement suspected, #296); why an
  unpowered DS3231 blocks the bell's boot (#317); why a polled reader hangs the C6.
- **Bench (2026-10-03 14:20 MST):** `c6bench` on the gateway's USB, re-flashed with `c6bench-311-pause-dbg.bin`, waiting
  at `dbg:ready`, collector running. RP2350-Zero on the desktop's USB, running the 2026-09-21 Hanoi image. `c6spare`
  unplugged; the bell on its wall supply (old image: it will read `#unknown` after the gateway deploy until flashed).
- **Awaiting David:** plan Task 5 steps 3–4 (gateway deploy; the bell, daylight); close #319?; mark buildmap 3 in
  `review/program-sequence.md`; `review/DOCMAP.md` line 56; the three cabot relay items; where #297 goes; #300.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #296–#300, #303, #304, #306–#309, #315–#326.
