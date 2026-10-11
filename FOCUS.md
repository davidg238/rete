# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **0.1 (David, 2026-10-09): one node, every verb — load, debug, patch, profile — through porta, over the radio, while it sleeps.**
  Demos: the ship's bell twice, wall C6 always-on (#380) and MagTag deep-sleep (#379). Lifecycle table: #26.
- **Next session starts with: #233 OTA, branch `233-magtag` (pushed, not merged) — rebase it on master (#412 moved the C6's
  settings backend into `prj_esp32c6.conf` and fixed `ota_zephyr.c`'s stack guard), then close the same gap for the MagTag:
  no S2 conf sets `CONFIG_SETTINGS`, so an S2 build gets the stub runner and idles (#233's 2026-10-11 comment). Then the bell
  over Thread (`flash-bell.sh`, daylight), then #398 (porta HTTP push front).**
- **Awaiting David:** merge `233-magtag`; read E draft 4 and L; LICENSE and history rewrite (#368); close #314; #396; #175's memo;
  the dongle needs a reflash from master (#402); the RP2 remote-BOOTSEL design (#413).
- **Bench:** the bell `c6` is on the WALL (never flash). `c6spare` on USB at msi (holds wedge-quiet's probe). MagTag
  `7cdfa11aaaaa` on the 233-magtag image. **opti7040: both RP2350s parked in BOOTSEL** (WeAct `1-7`: #412's arm-2 bench image;
  Zero `1-3`: #296 arm 1); any UF2 dropped on the volume boots them. No toolchain on opti7040; build on msi.
- **Decided — do not re-litigate:** custos = nsl peer, porta language-neutral (D1–D2); share only `protocol/` (#338); agent first,
  HTTP the only agent surface (D11–D12); D13–D19; hot patch debug-build only (#313); one suite lock (#314); no third node (#384);
  debug goal = whole set, sha-gated (#375–#377); WiFi node addresses porta by IPv6 (#379); bell = classes by CRC (#380);
  OTA: code-push design §10 T1–T10; on an XIP part the hot path lives in RAM and `-O2` is the RP2350 default (#403, ZEPHYR_LESSONS §11);
  a console that cannot arm answers nil (#402); the image in SRAM is not a speed lever on the RP2350 (#296); **`OEVM_HOT` holds the
  send and allocation paths too, and the compiler's peephole is on by default (#412, optimizations.md Opt #6)**.
- **Weekly review, not scheduled:** #175, #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #345–#357, #359–#361, #363, #366, #369–#374, #378, #382–#397, #399, #400, #404–#411, #413, #414.
