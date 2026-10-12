# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **0.1 (David, 2026-10-09): one node, every verb — load, debug, patch, profile — through porta, over the radio, while it sleeps.**
  Demos: the ship's bell twice, wall C6 always-on (#380) and MagTag deep-sleep (#379). Lifecycle table: #26.
- **FOCUS (David, 2026-10-12): performance, #37.** Merged today: Opt #15 L4(c) `gen_tag` dropped (AWFY C6 0.988, WeAct 0.981), Opt #16
  L5 the `send_resolve` head (C6 0.978, WeAct 0.949; 860 B banked); L3 dropped. Day on c6spare: Richards 0.819, DeltaBlue 0.699; Toit ~4.8× on both.
  **Next session starts with (David):** 1) branch `37-alloc-hdr` (`38c9b557`, the one-word arena header, −8 B/object, speed level): msi
  gates + a heap census, then merge; 2) the allocator fast path (List, Storage); 3) the rest of L5 (capability, `Symbol =`, prim-id tests
  ahead of the probe). Then VM prims (String `copyFrom:to:`, `sin`/`cos`, Float→Integer); GC (#421); Dictionary as a prim.
  Board A/B: `board_build.sh` on msi (from `~/rete-l6`) + `board_ab.sh` here, ~10 min for three revisions on four boards.
- **Parked: #233** (program OTA). Next there: the S2 recipe at `OEVM_SPACE_MAX_CHANNELS=16`, the bell over Thread, #398. The network
  (bell, MagTag, dongle) is reflashed once #37 reaches "reasonable gains" (David, 2026-10-12), not per lever.
- **Awaiting David:** #368's history rewrite (force-push); close #314; #396; read E draft 4 and L; #175's memo; the dongle reflash
  (#402); the RP2 remote-BOOTSEL design (#413).
- **Bench (opti7040):** `c6spare` `/dev/ttyUSB1`, `c6b` `/dev/ttyUSB0` (agree to the ms). WeActs usb `1-5`, `1-7` parked in BOOTSEL. The bell `c6`
  is on the WALL (never flash); MagTag `7cdfa11aaaaa` on the extender. msi: `~/stic-harness.patch` (rebased 2026-10-12).
- **Decided — do not re-litigate:** custos = nsl peer, porta language-neutral (D1–D2); share only `protocol/` (#338); agent first (D11–D12);
  D13–D19; hot patch debug-build only (#313); one suite lock (#314); no third node (#384); debug goal = whole set (#375–#377); WiFi node
  → porta by IPv6 (#379); bell = classes by CRC (#380); OTA §10 T1–T10; Apache-2.0 (#368); dialect row 33; Opt #9–#16; footprint banks
  follow levers down; no RAM split for speed (#296 §7); AWFY is the #37 bench; **a loop-body lever is measured on a board first, and never
  at the loop's send sites** (L2, L3, L6); **every XIP target runs its hot path from RAM** (Opt #14); method shape is a header field (L6).
- **Weekly review, not scheduled:** #175, #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #345–#357, #359–#361, #363, #366, #369–#374, #378, #382–#397, #399–#401, #404–#411, #413–#415, #417–#423.
