# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **Next session starts with:** branch `272-progress-verdict` (pushed, `07f3750`). Focus **#272** (P1). First action:
  plan `porta/docs/superpowers/plans/2026-10-03-272-count-on-the-pull.md` Task 5 Step 1 — `nodus/oevm/tests/target/run.sh
  bridge-health` on `c6bench` (resolve via its collector env file), then Step 2: reflash `c6bench`, `nrf4ec0`, `nrf92c2`.
- **Done 2026-10-03:** Tasks 1–4 built, reviewed, gated (go ok, PORTA SYNC OK, nsl 288/0 aux all PASS) — #272 comment.
- **Decided — do not re-litigate (design D3.3 + its 2026-10-03 addendum):** the count rides the **pull** (`#wak`,
  required); porta reads ONE sample; past the budget the mark is forgotten; a window spanning a porta outage is
  re-marked. An image without the count reads `#unknown` — **even past its budget** (David, 2026-10-03). Slack,
  restart naming and stale reasons are left to the fleet (#326).
- **Deploy order is nodes first** — `TestPullCarryingACountIsServed` passed on the old handler.
- **Not proven:** why two `-Os` RP2350 builds differ 21% per activation (#296); why an unpowered DS3231 blocks the
  bell's boot (#317); why a polled reader hangs the C6; the deaf re-mark is not in the TLA model (#327).
- **Bench (2026-10-03 16:25 MST):** `c6bench` on the gateway's USB with `c6bench-311-pause-dbg.bin` at `dbg:ready`;
  RP2350-Zero on the desktop (2026-09-21 Hanoi image); `c6spare` unplugged; bell on its wall supply, old image.
- **Awaiting David:** Task 5 Steps 3–4 (gateway deploy; the bell, daylight — until then its pulls are refused and no
  `goal/**` reaches it); close #319?; buildmap 3 in `review/program-sequence.md`; `review/DOCMAP.md` line 56; the
  three cabot relay items; where #297 goes; #300. mdtree was stopped for memory — restart it if wanted.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #296–#300, #303, #304, #306–#309, #315–#327.
