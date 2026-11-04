# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **Next session (msi) starts with:** branch `master`; **#440 (P0) as a design deep-dive** (David, 2026-11-03): read the issue and its last comment's proposal (`Socket>>receiveWithin:` → a blocking `udp_receive_wait` per backend), then draft the design in `docs/design/`. #440 blocks #458 (P2, radio off while the VM runs, "very important").
- **#432 (0.1's last verb)** on `432-patch-porta` `b2a4e3eb`: master merged in, Task 8 done, every gate green (suite EXIT 0, 307/0). Left: Task 9 bench, the close-out, then the master merge. opti7040's session is reachable over Remote Control (`opti7040-quizzical-crab`).
- **Top after that (David, 2026-11-02):** #383, #437 (ACC01 on the chair). Then #443, #455.
- **Awaiting David:** #432 Task 9 (an explicit "restart porta" on gw, then the bell and MagTag patched at the bench); the three Task 6 deviations in #432's As built; review/ promotions for #432; whether the leg-P gate joins CLAUDE.md; the ACC01 reflash from master; the thermal board for #455; close #368 and the 14 `review/` files with LAN addresses; #201/#202; #396; paper E draft 6 and L (#429); #175; #413/#424 designs; bands #294/#295. Delayed: #427.
- **Bench:** OCM.1 (`e044a5dcefbb`, J-Link attached, on the Pixel relay) runs `AccMotionBle-tick3` from slot b, always on (no nRF sleep exists: #457/#459); console at custos gw :6994 (`curl 127.0.0.1:6994/log?tail=20`, hand-started).
  Production porta on gw is `porta-444`; gateway store writes need David's hand. Dongles dark. An RTT read halts the core and drops BLE.
- **Decided — do not re-litigate:** public, history not rewritten (#368); object model credited to Piumarta and Warth (`fa26c321`); ttgm's programs/overlays/schematics stay in ttgm (CLAUDE.md); OTA via internal A/B (#449); debug without the profiler (#450);
  a row carries its age (#444); store-until-acked in flash (#443); BLE mainline + pair-once + production porta (#439); re-pair = erase; `--extra-module` (#438); kinds motion/ir (#447); custos = nsl peer (D1–D2); share only `protocol/` (#338); agent first (D11–D12); Apache-2.0 (#368).
- **Weekly review, not scheduled:** #175, #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328, #331–#335,
  #345–#357, #359–#361, #363, #366, #369–#371, #373, #374, #378, #382–#397, #399–#401, #404–#411, #413, #414, #417–#429, #431, #441, #442, #445, #447, #452, #454, #455, #456, #457, #459.
