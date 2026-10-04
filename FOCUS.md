# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **Next session starts with:** branch `272-progress-verdict` (pushed). Focus **#272** (P1). First action, after
  ~2026-10-04 20:35 MST: plan `porta/docs/superpowers/plans/2026-10-03-272-count-on-the-pull.md` Task 5 Step 5 — on
  gw85224-01, read `gw/liveness` rows (the `events` table, read-only) per node by verdict with each `#stalled` reason;
  post the table on #272, then close it (Tasks 1–6 otherwise done — #272 comments).
- **Done 2026-10-03:** board gate on `c6bench` 3/3 + 180 s clean; every node and the gateway on `f515ac5` (gate
  `17b7ca90…`, rollback `porta.pre272`); restart gave no `#stalled`. Two fleet data, both one sweep of `#stalled`: a
  ~10 s mesh outage (all nodes), and the bell's replug (restart inside the window) — left to #326, not re-litigated.
- **Decided — do not re-litigate (design D3.3 + its 2026-10-03 addendum):** the count rides the pull; porta reads ONE
  sample; no count reads `#unknown` even past budget; slack, restart naming, stale reasons are left to the fleet (#326).
- **Not proven:** #296 (RP2350 21%), #317 (now moot? DS3231 powered from the C6 since 2026-10-03), the C6 polled-reader
  hang, #327 (the deaf re-mark is not in the TLA model).
- **Awaiting David:** `review/DOCMAP.md` regenerated (date stamps only, left uncommitted); close #319?; buildmap 3 in
  `review/program-sequence.md`; DOCMAP line 56; the three cabot relay items; #297; #300.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #296–#300, #303, #304, #306–#309, #315–#328.
