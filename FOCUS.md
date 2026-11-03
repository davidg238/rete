# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **rete is PUBLIC since 2026-11-01** (#368: tree scrubbed `48d8ab30`, history kept, Apache-2.0; quiet, for the YC application). Two live threads.
  msi: **#449** (P1, OTA nsl on the nRF52840 DK profile) on `449-ota-nrf` (f58ad555, 5 ahead / 3 behind master), shown once on OCM.1 over BLE: 164,000 B in 143 s, trial b confirmed.
  opti7040: **#432** (P1, 0.1's last verb) on `432-patch-porta` (d6a3d1f6, Tasks 1–7 built, 63 behind master); Task 8 docs, Task 9 bench; its FOCUS lines live there.
- **Next session (msi) starts with:** branch `449-ota-nrf`, rebase on master. J-Link on OCM.1 (David), rebuild on the DK profile's 96 KB arena (David: be aggressive),
  flash, re-pair (= erase), `porta blob put` + `goal/apps/main`, watch `sys/metrics` and the console; if it holds, merge. Known: the DK Thread profile overflows RAM by 1,764 B with the OTA stack.
  OCM.1 build: ttgm `firmware/boards/ocm1.overlay` + `--extra-module ~/workspace/ttgm/firmware`, `prj_dk_ble.conf`, rtt_console + lis2dh confs, `OEVM_OTA_PUBKEY` = gw `~/porta-v2/porta.pub`.
- **Top three after that (David asked 2026-11-02):** close 0.1 (#449 + #432 merged); #383 the same node in MicroPython, by rows; #437 the ACC01 on the chair. Then #440, #443, #455.
- **Awaiting David:** the J-Link on OCM.1; the thermal board for #455; close #368 ("history kept") and the 14 `review/` files with LAN addresses; the ACC01 reflash from master;
  #201/#202 for 0.1; #396; paper E draft 6 and L (#429); #175; #413/#424 designs; bands #294/#295. Delayed by David: #427.
- **Bench:** OCM.1 (`e044a5dcefbb`, J-Link OFF, on the Pixel relay) ran `tick2` from slot b; console at custos gw :6994 (`curl 127.0.0.1:6994/log?tail=20`, hand-started).
  Production porta on gw is `porta-444`. Dongles dark (David). An RTT read halts the core and drops the BLE link: read the console over BLE.
- **Decided — do not re-litigate:** public, history not rewritten (#368, 2026-11-01); README says "agent-first" in David's words (#368); ttgm's programs/overlays/schematics stay in ttgm, drivers may come in (CLAUDE.md);
  OTA via internal A/B, not NOR (#449); debug without the profiler (#450); dongles dark; a row carries its age (#444); store-until-acked in flash (#443); BLE mainline + pair-once + production porta (#439);
  re-pair = erase (rule 5); `--extra-module` (#438); kinds motion/ir (#447); custos = nsl peer (D1–D2); share only `protocol/` (#338); agent first (D11–D12); Apache-2.0 (#368).
- **Weekly review, not scheduled:** #175, #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328, #331–#335,
  #345–#357, #359–#361, #363, #366, #369–#371, #373, #374, #378, #382–#397, #399–#401, #404–#411, #413, #414, #417–#429, #431, #441, #442, #445, #447, #452, #454, #455.
