# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **0.1 (David, 2026-10-09): one node, every verb — load, debug, patch, profile — through porta, over the radio, while it sleeps.**
  Demos: the ship's bell twice, wall C6 always-on (#380) and MagTag deep-sleep (#379). Lifecycle table: #26.
- **FOCUS: #233 — program OTA through porta.** Merged to master 2026-10-10 (`ed41787`); the MagTag thread continues on
  `233-magtag` (pushed, not merged; 15:05 stamp): first program over WiFi in one boot, in-channel staging (§10 T10),
  `OEVM_SPACE_MAX_CHANNELS` a per-build knob. **Next session starts with:** that branch's own handoff; then the bell over
  Thread (`flash-bell.sh`, daylight), then #398 (porta HTTP push front).
- **Landed today beside it (David-directed, merged `fee5630`):** #402 — every polled-console board (RP2350 USB-CDC, the
  dongle) had spun before `main:` since #312/#364, fixed (`consoleLine` answers nil); #403 — the RP2350 hot path in SRAM
  and `-O2` default, 8.51 µs/activation; the WeAct RP2350B port and the board registry (`BOARD_PORTS.md`), lesson §11
  (the XIP cache cliff), `run.sh`'s `[build] hot path:` line. Record: `docs/results/2026-10-10-rp2350-weact-first-boot.md`.
- **Awaiting David:** merge `233-magtag` when its thread concludes; read E draft 4 and L; LICENSE and history rewrite (#368);
  close #314; #396; #175's memo; **the dongle needs a reflash from master (#402)**.
- **Bench:** the bell `c6` is on the WALL (never flash). `c6spare` on USB at msi (cable dropped 12:44 when the MagTag went on
  the extender). MagTag `7cdfa11aaaaa` on the 233-magtag image. **opti7040 holds the WeAct RP2350B (`ttyACM1`) and the
  Waveshare Zero (`ttyACM0`), flashed by UF2 from builds on msi-7d43 — no toolchain on opti7040.**
- **Decided — do not re-litigate:** custos = nsl peer, porta language-neutral (D1–D2); share only `protocol/` (#338); agent first,
  HTTP the only agent surface (D11–D12); D13–D19; hot patch debug-build only (#313); one suite lock (#314); no third node (#384);
  debug goal = whole set, sha-gated (#375–#377); WiFi node addresses porta by IPv6 (#379); bell = classes by CRC (#380);
  OTA: code-push design §10 T1–T10 (2026-10-10); **on an XIP part the hot path lives in RAM and `-O2` is the RP2350 default
  (#403, ZEPHYR_LESSONS §11)**; a console that cannot arm answers nil, never `false` (#402).
- **Weekly review, not scheduled:** #175, #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #345–#357, #359–#361, #363, #366, #369–#374, #378, #382–#397, #399, #400, #404–#411.
