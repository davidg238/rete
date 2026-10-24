# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **Two live threads.** msi: **#439** (P0) — porta over Bluetooth LE via a relay on David's Pixel, into mainline builds; with
  ttgm's P0s #436/#437/#438. opti7040: **#432** (P1, 0.1's last verb, patch through porta) on `432-patch-porta` — its FOCUS lines live there.
- **Next session (msi) starts with:** branch `439-ble-relay`. Read #439's 2026-10-23 "Mainline decided" comment, write the design record in
  `docs/design/`, then build: bonded pairing, porta :6969, no IP stack in BLE builds (`log_udp.c` tee onto the datagram seam), the frame as a `protocol/` contract.
- **Awaiting David:** #437 — does the ACC01 sleep on Thread SED or BLE now (re-opened by #439); #427 dongle lever (tee on nRF or arena); a current
  meter (PPK2?) for #439's power gate; #368's history rewrite; #201/#202 for 0.1; #396; paper E draft 6 and L (#429); #175; #413/#424 designs; bands #294/#295.
- **Bench:** ACC01 (`d9885967b66d`) on msi's J-Link PLUS (S/N 260115156), console over RTT only — read the ring and advance RdOff (scratchpad `rttread.py`
  is gone next session; the RTT block is `_SEGGER_RTT` in the ELF). Original flash: `~/workspace/acc01-backup-2026-10-23/`. Pixel `pixel-10-pro` on the tailnet,
  relay APK by Taildrop (no adb pairing yet). gw: production porta-398 :6969; test porta `~/porta-415` :6979/:6980 (ours).
- **Decided — do not re-litigate:** BLE mainline + pair-once + production porta (#439, David 2026-10-23); ACC01 board lives in ttgm, rete carries
  only the mechanism (#438); sensor seam by kind/alias, milli-SI (`docs/design/2026-10-23-sensor-seam-design.md`); custos = nsl peer (D1–D2); share only
  `protocol/` (#338); agent first (D11–D12); D13–D19; #415 Q1–Q9; OTA §10; hot patch debug-only (#313); one suite lock (#314); Apache-2.0 (#368).
- **Weekly review, not scheduled:** #175, #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328, #331–#335,
  #345–#357, #359–#361, #363, #366, #369–#371, #373, #374, #378, #382–#397, #399–#401, #404–#411, #413, #414, #417–#429, #431.
