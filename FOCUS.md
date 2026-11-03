# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **rete is PUBLIC since 2026-11-01** (#368, history kept, Apache-2.0). **#449 merged and closed 2026-11-03** (`089f8c2b`; OTA nsl on the nRF52840 DK profile, 96 KB arena, shown on OCM.1).
  Live thread elsewhere — opti7040: **#432** (P1, 0.1's last verb) on `432-patch-porta` (Tasks 1–7 built, behind master); Task 8 docs, Task 9 bench; its FOCUS lines live there.
- **Next session (msi) starts with:** branch `master`; #383 (the same node in MicroPython, by rows) — read the issue and its design first. Top three (David, 2026-11-02): close 0.1 (#432), #383, #437 (ACC01 on the chair). Then #440, #443, #455.
- **Awaiting David:** the ACC01 reflash from master (production porta refuses its pre-#444 image; check its schematic for an LED on P1.12 — OCM.1's was the DK spi3 CS, ttgm `f72b67d`); the thermal board for #455;
  close #368 and the 14 `review/` files with LAN addresses; #201/#202 for 0.1; #396; paper E draft 6 and L (#429); #175; #413/#424 designs; bands #294/#295. Delayed by David: #427.
- **Bench:** OCM.1 (`e044a5dcefbb`, J-Link attached, on the Pixel relay) runs `AccMotionBle-tick3` from slot b (OTA'd 2026-11-02); console at custos gw :6994 (`curl 127.0.0.1:6994/log?tail=20`, hand-started).
  Production porta on gw is `porta-444`; gateway store writes need David's hand. ttgm-webcam (msi user unit) re-enabled for correlation. Dongles dark. An RTT read halts the core and drops BLE.
- **Decided — do not re-litigate:** public, history not rewritten (#368); README "agent-first" (#368); object model credited to Piumarta and Warth (`fa26c321`); ttgm's programs/overlays/schematics stay in ttgm, drivers may come in (CLAUDE.md);
  OTA via internal A/B, not NOR (#449); debug without the profiler (#450); dongles dark; a row carries its age (#444); store-until-acked in flash (#443); BLE mainline + pair-once + production porta (#439);
  re-pair = erase (rule 5); `--extra-module` (#438); kinds motion/ir (#447); custos = nsl peer (D1–D2); share only `protocol/` (#338); agent first (D11–D12); Apache-2.0 (#368).
- **Weekly review, not scheduled:** #175, #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328, #331–#335,
  #345–#357, #359–#361, #363, #366, #369–#371, #373, #374, #378, #382–#397, #399–#401, #404–#411, #413, #414, #417–#429, #431, #441, #442, #445, #447, #452, #454, #455, #456.
