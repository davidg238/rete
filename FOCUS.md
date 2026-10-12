# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **0.1 (David, 2026-10-09): one node, every verb — load, debug, patch, profile — through porta, over the radio, while it sleeps.**
  Demos: the ship's bell twice, wall C6 always-on (#380) and MagTag deep-sleep (#379). Lifecycle table: #26. Left: the OTA row (#398, #233).
- **#372 closed (2026-10-12): visibility enforced as Newspeak defines it, unmarked = protected.** c6spare AWFY 1.018, STIC 1.010 vs master
  (accepted); ledger "A cost, not a lever" in `nodus/oevm/docs/optimizations.md`. #37 closed for 0.1 before it ("good enough").
- **Next session starts with (David to choose the focus):** queued: the network reflash (bell, MagTag, dongle #402) — now required, since the
  method header changed and old firmware cannot load a new image — then #398 + **#233** (program OTA: the S2 recipe at
  `OEVM_SPACE_MAX_CHANNELS=16`, the bell over Thread).
- **Awaiting David:** the next focus; #368's history rewrite (force-push); close #314; #396; read E draft 4 and L; #175's memo; the dongle reflash
  (#402); the RP2 remote-BOOTSEL design (#413); #424's design; `review/program-sequence.md` "Known stale docs" still calls `optimizations.md` a
  pre-pivot snapshot — it is the live ledger; #201/#202 (the bell's striker after a failed flash) for 0.1 or not.
- **Bench (opti7040):** `c6spare` `/dev/ttyUSB1`, `c6b` `/dev/ttyUSB0`. WeActs usb `1-5`, `1-7` parked in BOOTSEL. The bell `c6` is on the WALL
  (never flash); MagTag `7cdfa11aaaaa` on the extender. Board A/B: `board_build.sh` on msi + `board_ab.sh` here; msi worktree `~/rete-372`.
- **Decided — do not re-litigate:** custos = nsl peer, porta language-neutral (D1–D2); share only `protocol/` (#338); agent first (D11–D12);
  D13–D19; hot patch debug-build only (#313); one suite lock (#314); no third node (#384); debug goal = whole set (#375–#377); WiFi node
  → porta by IPv6 (#379); bell = classes by CRC (#380); OTA §10 T1–T10; Apache-2.0 (#368); dialect row 33; Opt #9–#19; footprint banks
  follow levers down; no RAM split for speed (#296 §7); **a loop-body lever is measured on a board first**; every XIP target runs its hot path
  from RAM (Opt #14); method shape and access are header fields (L6, #372); #37 closed for 0.1; visibility: three levels, unmarked
  protected, DNU at protected, no compile-time rule, unsent protected accessors not built, entry points public
  (`docs/design/2026-10-12-372-method-visibility-design.md`).
- **Weekly review, not scheduled:** #175, #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #345–#357, #359–#361, #363, #366, #369–#371, #373, #374, #378, #382–#397, #399–#401, #404–#411, #413–#415, #417–#425.
