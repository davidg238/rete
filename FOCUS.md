# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **0.1 (David, 2026-10-09): one node, every verb — load, debug, patch, profile — through porta, over the radio, while it sleeps.**
  Load, debug, profile built through porta on both bells (#415 closed 2026-10-14). **Left: patch through porta, #432 (P1; David 2026-10-14: in 0.1).**
- **Next session starts with:** branch `432-patch-porta` (pushed), focus #432. Read `docs/design/2026-10-15-432-patch-through-porta-design.md`
  (Q1–Q5 decided). Plan `docs/superpowers/plans/2026-10-15-432-patch-through-porta.md` (P1–P10 correct the design); build from Task 1, the host leg in `sleepy_debug_host.sh`, falsified first.
- **Awaiting David:** #368's history rewrite (force-push); #201/#202 for 0.1 or not; build
  `POST /load?build=last` or drop it from the peer design; #396; paper E draft 6 and L (#429); #175's memo; #413 and #424's designs; bands for #294, #295.
- **Bench:**
  - gw: `porta-398 serve --db soak-2026-08-04.db --manifest MANIFEST-415.cddl` (HTTP :6970). Ours, safe to stop: a test porta in
    `~/porta-415` (UDP 6979 / HTTP 6980, c6b's) and `custos-415` on 127.0.0.1 :6990 (c6b), :6991 (MagTag), :6992 (wall bell).
  - c6b (opti7040 `ttyUSB0`), the MagTag (program `ed7c71ee…`) and the wall bell (`206ef1195438`, program `35eff2a1…`, goal seq 10)
    run #415's firmware (MagTag/c6b firmware 7; the bell `flash-bell.sh` at `c5b0b66d`). msi is `msi-7d43` now; plain `msi` misresolves.
- **Decided — do not re-litigate:** custos = nsl peer, porta language-neutral (D1–D2); share only `protocol/` (#338); agent first (D11–D12); D13–D19;
  #432 Q1–Q5 (the patch set a blob named by `Debug{sha, breaks, patches}`, replayed at boot; a shrink reboots; unsigned; fetch each wake;
  custos's porta debug verbs in scope); #415 Q1–Q9 + Q2′ (one-shot retained `goal/profile`, one request mechanism, the tee target a kept cell, no build-time literal); OTA §10 T1–T10 +
  addendum; #398 Q1–Q5; hot patch debug-build only (#313); one suite lock (#314); no third node (#384); Apache-2.0 (#368); Opt #9–#19;
  visibility three levels (#372).
- **Weekly review, not scheduled:** #175, #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #345–#357, #359–#361, #363, #366, #369–#371, #373, #374, #378, #382–#397, #399–#401, #404–#411, #413, #414, #417–#429, #431.
