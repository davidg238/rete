# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **Two live threads.** msi: **#437** (P0) — the ACC01 on a chair, wake on motion, then sit/stand ("getting out of the chair"); #439 rules 1–8 done.
  opti7040: **#432** (P1, 0.1's last verb, patch through porta) on `432-patch-porta` — its FOCUS lines live there.
- **Next session (msi) starts with:** branch `439-ble-relay`. Read David's labelled chair trials at `http://gw85224-01:6972/` (episodes, 25 Hz x/y/z),
  pick the got-up rule from them (tilt before/after against the quiet baseline, the push), build it into the ttgm app; then current measured (#440 gates it).
- **Awaiting David:** chair trials (attachment under the seat pan); #368's history rewrite; #201/#202 for 0.1; #396; paper E draft 6 and L (#429); #175;
  #413/#424 designs; bands #294/#295. Delayed by David: #427 (dongle RAM). Not yet shown for #439: a second central refused.
- **Bench:** ACC01 (`d9885967b66d`) on msi's J-Link (S/N 260115156), bonded to the Pixel, reports to production porta :6969; image `build-acc01-motion`
  (debug logs, RTT console). App, overlay, flash/RTT scripts, viewer: `~/workspace/acc01-backup-2026-10-23/bench-439/` (`AccMotionBle.nsl`, `capview.py`;
  `flashkeep.jlink` keeps the bond). Viewer runs on gw (`~/capview`, :6972, read-only). The node arms 30 s after boot: a sensor touched earlier dies (#436).
- **Decided — do not re-litigate:** the ACC01 is mounted on the chair, not worn (David); BLE mainline + pair-once + production porta; power bar = months on 2× AA,
  measured; no version byte in the frame (rule 4); re-pair = flash erase (rule 5) — #439; ACC01 on BLE (#437); ACC01 board lives in ttgm (#438);
  sensor seam by kind/alias, milli-SI; custos = nsl peer (D1–D2); share only `protocol/` (#338); agent first (D11–D12); D13–D19; #415 Q1–Q9; OTA §10; Apache-2.0 (#368).
- **Weekly review, not scheduled:** #175, #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328, #331–#335,
  #345–#357, #359–#361, #363, #366, #369–#371, #373, #374, #378, #382–#397, #399–#401, #404–#411, #413, #414, #417–#429, #431, #441, #442.
