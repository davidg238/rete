# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **0.1 (David, 2026-10-09): one node, every verb — load, debug, patch, profile — through porta, over the radio, while it sleeps.**
  Demos: the ship's bell twice, wall C6 (#380) and MagTag (#379). Left: the profile row, **#415**, on branch `415-profile-porta` (pushed, not merged).
- **Next session starts with:** branch `415-profile-porta`, focus #415. Read the design's last two sections
  (`docs/design/2026-10-12-415-profile-through-porta-design.md`). Then **F10**: the last lines before a deep sleep never arrive over WiFi,
  even with the drain waiting on Zephyr's queues (`4480c256`). Measure what the old fixed 150 ms delivered, and look for a WiFi-driver
  TX-done. Then read the MagTag's carried window dumping at a wake (`logudp: the last sleep's drain took N ms` is printed at boot).
- **Then:** the wall bell (cable to msi + `flash-bell.sh`, branch firmware); delete `OEVM_LOG_UDP6`; merge; promotion (E §9, D19 addendum, #26).
- **Awaiting David:** cable the wall bell; merge of `415-profile-porta`; #368's history rewrite (force-push); close #314; #396; E draft 5 and L
  (#429); #175's memo; #413 and #424's designs; bands for #294 and #295; #201/#202 for 0.1 or not; #430 (Go gate red since #375).
- **Bench:**
  - gw runs `porta-398 serve --db soak-2026-08-04.db --manifest MANIFEST-415.cddl` (restarted 2026-10-13, HTTP :6970).
  - Test processes, all ours: a porta in `~/porta-415` (UDP 6979 / HTTP 6980, c6b's); `custos-415` on 127.0.0.1:6990 (c6b) and :6991 (MagTag,
    production porta, advertising gw's ULA `…fe2f:710`).
  - c6b (opti7040 `ttyUSB0`) runs branch firmware with a `gwPort 6979` image copy.
  - The MagTag (msi USB, data cable now) runs branch firmware, program `ed7c71ee…`, goal matching.
  - The wall bell (`206ef1195438`) is untouched, on v3.
- **Decided — do not re-litigate:** custos = nsl peer, porta language-neutral (D1–D2); share only `protocol/` (#338); agent first (D11–D12); D13–D19;
  #415 Q1–Q9 + Q2′ (one-shot retained `goal/profile`, one request mechanism, the tee target a kept cell); OTA §10 T1–T10 + addendum; #398 Q1–Q5;
  hot patch debug-build only (#313); one suite lock (#314); no third node (#384); Apache-2.0 (#368); Opt #9–#19; visibility three levels (#372).
- **Weekly review, not scheduled:** #175, #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #345–#357, #359–#361, #363, #366, #369–#371, #373, #374, #378, #382–#397, #399–#401, #404–#411, #413, #414, #417–#431.
