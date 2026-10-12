# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **0.1 (David, 2026-10-09): one node, every verb — load, debug, patch, profile — through porta, over the radio, while it sleeps.**
  Demos: the ship's bell twice, wall C6 always-on (#380) and MagTag deep-sleep (#379). Lifecycle table: #26.
- **FOCUS (David, 2026-10-12): performance, #37.** L1 merged (Opt #12; AWFY L1 ÷ base C6 0.936, WeAct 0.961); L2 reverted (won on
  the host, lost on both boards). **Next, queued on #37 (2026-10-12 comment):** (1) M, the counting build (send/frame design §M) on
  c6spare + host: is the frame Toit's 6–7× Richards/DeltaBlue lead? (2) the lever M points to (L3/L4/L5), board-first;
  (3) VM prims: String `copyFrom:to:`, `sin`/`cos`, Float→Integer; (4) GC performance (Havlak fragmentation, #421); (5) compiler
  bytecodes, Dictionary as a prim. Board A/B: `docs/results/2026-10-12-37-send-frame/board_build.sh` (msi) + `board_ab.sh`
  (opti7040), ~15 min for four revisions; host A/B `ab.sh`; AWFY counts only via `awfy_counts.py`. A ledger row per attempt.
- **Parked: #233** (program OTA). Next there: the S2 recipe at `OEVM_SPACE_MAX_CHANNELS=16` (one BOOT0+RESET flash), the bell over
  Thread, #398. **Registry version 6 since `124ab677`: every board needs a reflash from master before OTA work.**
- **Awaiting David:** #368's history rewrite (force-push); close #314; #396; read E draft 4 and L; #175's memo; the dongle reflash
  (#402); the RP2 remote-BOOTSEL design (#413); re-bank the MCU footprint at 44602 (bank 45356) and host at 76929 (bank 77801)?
- **Bench (opti7040):** `c6spare` `/dev/ttyUSB1` (CP2102N `d2e21074…`) and `c6b` `/dev/ttyUSB0` (`ac2dce6f…`, from the gateway) — both
  C6 QFN40 v0.0, agree within 2 ms. WeActs usb `1-5` and `1-7` both parked in BOOTSEL (2026-10-12 10:18); `weact_bootsel.py` parks a running one.
  The Zero is off the bus. The bell `c6` is on the WALL (never flash). MagTag `7cdfa11aaaaa` on the extender (BOOT0+RESET only).
  msi: `~/rete-board` (board_build's worktree), STIC harness `~/stic-harness.patch` (uncommitted, applied per build). Build on msi.
- **Decided — do not re-litigate:** custos = nsl peer, porta language-neutral (D1–D2); share only `protocol/` (#338); agent first,
  HTTP the only agent surface (D11–D12); D13–D19; hot patch debug-build only (#313); one suite lock (#314); no third node (#384);
  debug goal = whole set, sha-gated (#375–#377); WiFi node → porta by IPv6 (#379); bell = classes by CRC (#380); OTA: code-push
  §10 T1–T10; XIP hot path in RAM, `-O2` on RP2350 (#403); `OEVM_HOT` + peephole (#412); Apache-2.0 everywhere (#368); dialect
  row 33; `OP_ITER_AT` kernel-only; iterator inlining (#37 do-inline design); stored hashes compare with `=` (Opt #9); the loop
  trusts its methods (Opt #10); arithmetic slow path out of line (Opt #11); no RAM split for speed (#296 §7); AWFY is the #37
  bench, Havlak host-only; no access mechanism until visibility is designed, then checked at compile time or cache fill (Opt #12,
  dialect row 25); **a lever that changes the loop body is measured on a board first** (send/frame design, addendum 2026-10-12).
- **Weekly review, not scheduled:** #175, #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #345–#357, #359–#361, #363, #366, #369–#374, #378, #382–#397, #399–#401, #404–#411, #413–#415, #417–#421.
