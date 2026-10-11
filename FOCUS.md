# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **0.1 (David, 2026-10-09): one node, every verb — load, debug, patch, profile — through porta, over the radio, while it sleeps.**
  Demos: the ship's bell twice, wall C6 always-on (#380) and MagTag deep-sleep (#379). Lifecycle table: #26.
- **Next session starts with: #412 (David, 2026-10-10) — merge the evening's gains from `296-image-in-sram` (23 commits, pushed, not
  merged) in order: the `gc.c` allocator fixes, then the compiler peephole, each gated on msi-7d43 (aux line green there) and each
  with before/after benchmarks on the RP2350 in ONE sitting (`Spike296Node.nsl` + `docs/spikes/2026-10-10-296-image-in-sram/`).
  Read `/tmp/suite-combined-msi.log` on msi first. Record: `docs/results/2026-10-10-296-image-in-sram.md` §6–§10.**
- **Other live thread: #233 OTA** — merged `ed41787`; the MagTag continues on `233-magtag` (pushed, not merged); then the bell over
  Thread (`flash-bell.sh`, daylight), then #398 (porta HTTP push front).
- **Awaiting David:** merge `233-magtag`; read E draft 4 and L; LICENSE and history rewrite (#368); close #314; #396; #175's memo;
  the dongle needs a reflash from master (#402); decide arm 2 (#412 item 3) and the RP2 remote-BOOTSEL design (#413).
- **Bench:** the bell `c6` is on the WALL (never flash). `c6spare` on USB at msi. MagTag `7cdfa11aaaaa` on the 233-magtag image.
  **opti7040: both RP2350s parked in BOOTSEL holding spike images** (WeAct `1-7`/`sdc1`: peephole+allocator; Zero `1-3`/`sdb1`:
  arm 1); any UF2 dropped on the volume boots them; master images are in `build-rpi_pico2-B` on msi. No toolchain on opti7040.
- **Decided — do not re-litigate:** custos = nsl peer, porta language-neutral (D1–D2); share only `protocol/` (#338); agent first,
  HTTP the only agent surface (D11–D12); D13–D19; hot patch debug-build only (#313); one suite lock (#314); no third node (#384);
  debug goal = whole set, sha-gated (#375–#377); WiFi node addresses porta by IPv6 (#379); bell = classes by CRC (#380);
  OTA: code-push design §10 T1–T10; on an XIP part the hot path lives in RAM and `-O2` is the RP2350 default (#403, ZEPHYR_LESSONS §11);
  a console that cannot arm answers nil (#402); **the image in SRAM is not a speed lever on the RP2350 — placement is finished,
  the floor is instructions (#296, record §6–§7, 2026-10-10)**.
- **Weekly review, not scheduled:** #175, #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #345–#357, #359–#361, #363, #366, #369–#374, #378, #382–#397, #399, #400, #404–#411, #413.
