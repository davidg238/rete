# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **Two live threads.** msi: **#449** (P1, OTA nsl on the nRF52840 DK profile) on `449-ota-nrf` (19fce2aa+conclude), pushed, not merged.
  Shown on OCM.1 over BLE through production porta: 164,000 B in 143 s, trial b confirmed. opti7040: **#432** (P1) on `432-patch-porta`; its FOCUS lines live there.
- **Next session (msi) starts with:** branch `449-ota-nrf`. Rebuild OCM.1 on the DK profile's new 96 KB arena (David: be aggressive; host sweep on #449),
  re-attach the J-Link (David) and flash it, push a program with `porta blob put` + `goal/apps/main`, and watch `sys/metrics` and the console. If it holds, merge to master.
  OCM.1 build: ttgm `firmware/boards/ocm1.overlay` + `--extra-module ~/workspace/ttgm/firmware`, `prj_dk_ble.conf`, rtt_console + lis2dh confs, `OEVM_OTA_PUBKEY` = gw `~/porta-v2/porta.pub`.
- **Awaiting David:** the ACC01 reflash from master (production porta refuses its pre-#444 image); #368's history rewrite;
  #201/#202 for 0.1; #396; paper E draft 6 and L (#429); #175; #413/#424 designs; bands #294/#295. Delayed by David: #427.
- **Bench:** OCM.1 (`e044a5dcefbb`, J-Link OFF, on the Pixel relay; ttgm may push nsl programs OTA, recipe on #449) ran `tick2` AccMotionBle from slot b; its console at custos gw :6994
  (`curl 127.0.0.1:6994/log?tail=20`, hand-started). Production porta on gw is `porta-444` (#444; snapshot `soak-pre444-20261027.db`). Dongles nrf4ec0/nrf92c2 dark (David).
  An RTT read halts the core and drops the BLE link: read the console over BLE.
- **Decided — do not re-litigate:** ttgm's programs, overlays and schematics stay in ttgm, drivers may come in (CLAUDE.md, 2026-10-27); OTA via internal A/B, not NOR (#449);
  debug without the profiler (#450); dongles left dark (David, 2026-10-27); a row carries its age (#444); store-until-acked in flash (#443); BLE mainline + pair-once + production porta (#439);
  re-pair = erase (rule 5); out-of-tree drivers via `--extra-module` (#438); kinds motion/ir (#447); custos = nsl peer (D1–D2); share only `protocol/` (#338); agent first (D11–D12); Apache-2.0 (#368).
- **Weekly review, not scheduled:** #175, #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328, #331–#335,
  #345–#357, #359–#361, #363, #366, #369–#371, #373, #374, #378, #382–#397, #399–#401, #404–#411, #413, #414, #417–#429, #431, #441, #442, #445, #447, #452, #454.
