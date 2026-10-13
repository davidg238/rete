# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **0.1 (David, 2026-10-09): one node, every verb — load, debug, patch, profile — through porta, over the radio, while it sleeps.**
  Demos: the ship's bell twice, wall C6 always-on (#380) and MagTag deep-sleep (#379). Lifecycle table: #26. Left: the profile row (#415).
- **2026-10-12: the deploy row is complete.** #233 closed (OTA on both bells; promotion in design §10's addendum, E §9, ladder, CLAIMS);
  #398 closed (porta's write routes, custos `--porta-node`; an agent's push to the wall bell confirmed in 2 min 37 s, `efe37e7e`).
- **Next session starts with:** master, focus **#415** (profile through porta): read the issue and the profile design (`docs/design/
  2026-10-05-custos-profile-over-usb.md`, peer design D19), then draft the profile row's design in `docs/design/` with numbered questions.
- **Awaiting David:** #415's band (P3, but the last 0.1 row); #368's history rewrite (force-push); close #314; #396; read E draft 5 and L
  (#429: re-lift Appendix A with it); #175's memo; #413 and #424's designs; bands for #294 and #295; `review/program-sequence.md` "Known stale
  docs" still calls `optimizations.md` a pre-pivot snapshot (it is the live ledger); #201/#202 for 0.1 or not; #430 (Go gate red since #375).
- **Bench:** gw runs **`porta-398`** (hand-started, `serve --db soak-2026-08-04.db --manifest MANIFEST-377.cddl`, HTTP :6970; CLI is
  `~/porta-v2/porta-398 {get,log,put,blob put}`). Wall bell (`206ef1195438`) on v3 `6bf07588…`, its `goal/debug/main` stale (debugger off
  until rewritten). MagTag (`7cdfa11aaaaa`) console → msi UDP 54330. opti7040: `c6b` `ttyUSB0`; a WROOM-32 (CP2104 `01558C55`).
  msi: one nRF dongle (`SoakNode`, old firmware). Bake a board image with `OEVM_DEBUG_HOOKS=1` when the build had it.
- **Decided — do not re-litigate:** custos = nsl peer, porta language-neutral (D1–D2); share only `protocol/` (#338); agent first (D11–D12);
  D13–D19; hot patch debug-build only (#313); one suite lock (#314); no third node (#384); debug goal = whole set (#375–#377); WiFi node → porta
  by IPv6 (#379); bell = classes by CRC (#380); OTA §10 T1–T10 + 2026-10-12 addendum; push = general porta routes, answered at once, CIDR-only
  for 0.1, HTTP only (#398 Q1–Q5); Apache-2.0 (#368); dialect row 33; Opt #9–#19; a loop-body lever is measured on a board first; every XIP
  target runs its hot path from RAM (Opt #14); visibility: three levels, unmarked protected (#372); no nRF52840 firmware change (#427).
- **Weekly review, not scheduled:** #175, #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #345–#357, #359–#361, #363, #366, #369–#371, #373, #374, #378, #382–#397, #399–#401, #404–#411, #413, #414, #417–#430.
