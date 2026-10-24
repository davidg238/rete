# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **Two live threads.** msi: **#437** (P0): the ACC01 on a chair, wake on motion, then "got out of the chair". #444 is merged to master, not deployed.
  opti7040: **#432** (P1, 0.1's last verb, patch through porta) on `432-patch-porta`; its FOCUS lines live there.
- **Next session (msi) starts with:** branch `master`. Read ttgm's webcam join (`~/workspace/ttgm/bench/webcam-watcher/join.py` against
  `http://gw85224-01:6972/captures.json`), pick the got-up rule from the matched episodes, and build it into the ttgm app; then measure current (#440 gates it).
- **Awaiting David:** a time to deploy #444 (restart porta-398 from master + reflash the ACC01 together, then capview `--time-basis capture`); chair trials;
  #368's history rewrite; #201/#202 for 0.1; #396; paper E draft 6 and L (#429); #175; #413/#424 designs; bands #294/#295. Delayed by David: #427.
- **Bench:** ACC01 (`d9885967b66d`, boot 20, on the chair, J-Link detached) → Pixel → production porta :6969; image `build-acc01-motion`. Bench files:
  `~/workspace/ttgm/bench/439-ble-spike/` (davidg238/ttgm). On gw: capview :6972 (`~/capview`), the console via custos-415 :6993 (`curl 127.0.0.1:6993/log?tail=20`).
  The accel can hang until a power cycle (#445): hours of no episodes while check-ins continue mean it has happened.
- **Decided — do not re-litigate:** a row carries its age; porta stamps arrival − age (#444, RECORDS §3); store-until-acked in flash (#443, David's comment);
  ACC01 on the chair, not worn; BLE mainline + pair-once + production porta; power bar = months on 2× AA; no version byte (rule 4); re-pair = erase (rule 5), #439;
  ACC01 board lives in ttgm (#438); sensor seam by kind/alias, milli-SI; custos = nsl peer (D1–D2); share only `protocol/` (#338); agent first (D11–D12); D13–D19; #415 Q1–Q9; Apache-2.0 (#368).
- **Weekly review, not scheduled:** #175, #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328, #331–#335,
  #345–#357, #359–#361, #363, #366, #369–#371, #373, #374, #378, #382–#397, #399–#401, #404–#411, #413, #414, #417–#429, #431, #441, #442, #445.
