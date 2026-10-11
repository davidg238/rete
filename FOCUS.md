# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **0.1 (David, 2026-10-09): one node, every verb — load, debug, patch, profile — through porta, over the radio, while it sleeps.**
  Demos: the ship's bell twice, wall C6 always-on (#380) and MagTag deep-sleep (#379). Lifecycle table: #26.
- **FOCUS: #233 — program OTA through porta.** The MagTag took its first push over WiFi (2026-10-10 14:04, one boot: in-channel
  stage, activate, reboot wake with no strike, confirm; design §10 "T10 as built"). Branch `233-magtag` (master merged in 2026-10-11,
  #412 included; not yet merged to master). **Next session starts with:** (1) merge `233-magtag`; (2) the S2 recipe at
  `OEVM_SPACE_MAX_CHANNELS=16` (two wakes counted 11) and `OEVM_MPROF_SLOTS` at its default — one BOOT0+RESET flash, read `space:`
  and the link margin; (3) the bell: cable-flash the wall C6 with the OTA half (daylight, `flash-bell.sh`), one push over Thread;
  (4) E §9's MagTag line; then #398.
- **Awaiting David:** merge `233-magtag`; the flash above; #400 (earned by a measured slowdown — not yet); read E draft 4 and L;
  LICENSE and history rewrite (#368); close #314; #396; #175's memo; the dongle needs a reflash from master (#402); the RP2
  remote-BOOTSEL design (#413).
- **Bench:** the bell `c6` is on the WALL (never flash). MagTag `7cdfa11aaaaa` on the extender runs build `…T140328` + program v2
  (`c879e8…`, slot b, confirmed; goal = v2 on gw); it enumerates only in download mode (BOOT0+RESET). `c6spare` on USB at msi.
  **opti7040: both RP2350s parked in BOOTSEL holding spike images** (WeAct `1-7`/`sdc1`, Zero `1-3`/`sdb1`); any UF2 dropped on the
  volume boots them; no toolchain on opti7040. gw porta = `porta-379 --db soak-2026-08-04.db`; CLI writes are David's (classifier).
- **Decided — do not re-litigate:** custos = nsl peer, porta language-neutral (D1–D2); share only `protocol/` (#338); agent first,
  HTTP the only agent surface (D11–D12); D13–D19; hot patch debug-build only (#313); one suite lock (#314); no third node (#384);
  debug goal = whole set, sha-gated (#375–#377); WiFi node addresses porta by IPv6 (#379); bell = classes by CRC (#380);
  **OTA: code-push design §10 T1–T10** — firmware is the runner, two 768K slots, NVS manifest, MCUboot's ed25519, the cable
  rule, one-boot trial + 150 s, fetch thread where it fits (T9) / in-channel on the S2 (T10), RAM cursor, registry a knob,
  never strike on a non-timer boot; run.sh knobs: the caller's value wins (2026-10-10); on an XIP part the hot path lives in RAM
  and `-O2` is the RP2350 default (#403); a console that cannot arm answers nil (#402); the image in SRAM is not a speed lever on
  the RP2350 (#296, 2026-10-10).
- **Weekly review, not scheduled:** #175, #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #345–#357, #359–#361, #363, #366, #369–#374, #378, #382–#397, #399–#401, #404–#411, #413.
