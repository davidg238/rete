# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **Next session starts with:** branch `329-c6-speed` (pushed; it carries all of #272). Focus **#272** (P1). First action,
  after ~2026-10-04 20:35 MST: plan `porta/docs/superpowers/plans/2026-10-03-272-count-on-the-pull.md` Task 5 Step 5 —
  on gw85224-01, read `gw/liveness` rows (the `events` table, read-only) per node by verdict with each `#stalled`
  reason; post the table on #272, then close it.
- **Then, one per session:** #329 — the five radio scenarios at -O2 on `c6spare` (bridge-forensics is the stack gate:
  `ot_radio_workq` had 168 B free at -Os; porta-* need the injector by hand), then merge; #297 (Cbor encode prim);
  #296 (RP2350 placement — #324's rows are its evidence).
- **Bench:** `c6bench` is in the soak — never flash it. `c6spare` + RP2350-Zero on this desktop's motherboard ports.
- **Decided — do not re-litigate:** #272 per design D3.3 + its 2026-10-03 addendum (the count rides the pull; one
  sample; slack/restart/stale left to #326). -O2 is the C6 default (#329, `438028d`); the RP2350 waits on #296.
- **Not proven:** #317 (moot? DS3231 now C6-powered), the C6 polled-reader hang, #327 (deaf re-mark not in TLA).
- **Awaiting David:** `review/DOCMAP.md` regenerated (date stamps only, uncommitted); close #319?; buildmap 3 in
  `review/program-sequence.md`; DOCMAP line 56; the three cabot relay items; #300.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298–#300, #303, #304, #306–#309, #315–#328.
