# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **0.1 (David, 2026-10-09): one node, every verb — load, debug, patch, profile — through porta, over the radio, while it sleeps.**
  Demos: the ship's bell twice, wall C6 always-on (#380) and MagTag deep-sleep (#379). Lifecycle table: #26. Left: the OTA row's front (#398) + #233's close-out.
- **#233 (P1), 2026-10-12: program OTA proven on master on both bells** — the MagTag over WiFi, the wall C6 over Thread, each confirmed and on time at 18:30
  (`38a5500f`; #233 comment). Found: #426 (a reverted push restages forever), #427 (master overflows the nRF dongle's RAM), #428 (ESP32 alloc ×1.31).
- **Next session starts with:** close #233 — paper E §9's OTA line, the #26 row, promotion (ladder, CLAIMS, design §10 addendum: the catch-up sync, the
  bell's missing key, clear a node's goal before a cable flash); then **#398** (`custos load --porta`); then #415 (the profile row).
- **Awaiting David:** #368's history rewrite (force-push); close #314; #396; read E draft 4 and L; #175's memo; the RP2 remote-BOOTSEL design (#413);
  #424's design; bands for #294 and #295 (none since 09-17); `review/program-sequence.md` "Known stale docs" still calls `optimizations.md` a pre-pivot
  snapshot — it is the live ledger; #201/#202 (the bell's striker after a failed flash) for 0.1 or not.
- **Bench:** opti7040: `c6b` `ttyUSB0`; the wall bell (`206ef1195438`) back on the wall; the MagTag (`7cdfa11aaaaa`) console → msi UDP 54330; a WROOM-32
  (CP2104 `01558C55`) on master's STIC image. msi: one nRF dongle (`SoakNode`, old firmware), worktree `~/rete-reflash`. porta on gw: `~/porta-v2/porta-379
  {get,log,put,blob put} --db soak-2026-08-04.db`. Bake a board image locally with `OEVM_DEBUG_HOOKS=1` when the build had it (it changes the bytes).
- **Decided — do not re-litigate:** custos = nsl peer, porta language-neutral (D1–D2); share only `protocol/` (#338); agent first (D11–D12); D13–D19; hot
  patch debug-build only (#313); one suite lock (#314); no third node (#384, after 0.1, a timed measurement); debug goal = whole set (#375–#377); WiFi node
  → porta by IPv6 (#379); bell = classes by CRC (#380); OTA §10 T1–T10; Apache-2.0 (#368); dialect row 33; Opt #9–#19; a loop-body lever is measured on a
  board first; every XIP target runs its hot path from RAM (Opt #14); visibility: three levels, unmarked protected
  (`docs/design/2026-10-12-372-method-visibility-design.md`); no nRF52840 firmware change while RAM is this short (#427, 2026-10-12).
- **Weekly review, not scheduled:** #175, #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #345–#357, #359–#361, #363, #366, #369–#371, #373, #374, #378, #382–#397, #399–#401, #404–#411, #413–#415, #417–#428.
