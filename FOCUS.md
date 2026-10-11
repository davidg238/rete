# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **0.1 (David, 2026-10-09): one node, every verb — load, debug, patch, profile — through porta, over the radio, while it sleeps.**
  Demos: the ship's bell twice, wall C6 always-on (#380) and MagTag deep-sleep (#379). Lifecycle table: #26.
- **FOCUS: #233 — program OTA through porta.** The MagTag took its first push over WiFi (2026-10-10; design §10 "T10 as built").
  Branch `233-magtag`, master merged in twice (2026-10-11: #412, LICENSE); Go gates + PORTA SYNC OK; not merged to master.
  **Next session starts with:** (0) the nsl suite on the branch (aux line green); (1) the S2 settings gap (#233 2026-10-11
  comment): `prj_esp32s2_ota.conf` sets `CONFIG_SETTINGS`, plain/wifi S2 builds still get the stub runner — move it to
  `prj_esp32s2.conf` (check storage vs the slots) and add the slots-without-settings guard in `runner.c`; (2) merge `233-magtag`;
  (3) the S2 recipe at `OEVM_SPACE_MAX_CHANNELS=16`, `OEVM_MPROF_SLOTS` default — one BOOT0+RESET flash, read `space:`;
  (4) the bell over Thread (daylight, `flash-bell.sh`); then #398.
- **Second thread, opti7040 (#233 delayed on this machine): #37, branch `37-ocdo-spike` (a spike, pushed, not for merging).**
  `oc do:` lowered with a spike `OP_ITER_AT` is 0.82× MicroPython on the WeAct (#37 comments). **Starts with:** decide how the
  compiler emits `OP_ITER_AT` from kernel source, put the kernel's `OrderedCollection>>do:` on it, full nsl suite, WeAct rerun.
- **Awaiting David:** merge `233-magtag`; #368's last item, the history rewrite (private names, force-push); close #314; #396;
  read E draft 4 and L; #37's band (P2, now this machine's next); #175's memo; the dongle reflash from master (#402); the RP2 remote-BOOTSEL design (#413).
- **Bench:** the bell `c6` is on the WALL (never flash). `c6spare` on USB at msi (holds wedge-quiet's probe). MagTag
  `7cdfa11aaaaa` on the extender, build `…T140328` + program v2 (slot b, confirmed); enumerates only in BOOT0+RESET.
  **opti7040: both RP2350s parked in BOOTSEL** (WeAct `1-7`, Zero `1-3`); a UF2 on the volume boots them; build on msi.
- **Decided — do not re-litigate:** custos = nsl peer, porta language-neutral (D1–D2); share only `protocol/` (#338); agent first,
  HTTP the only agent surface (D11–D12); D13–D19; hot patch debug-build only (#313); one suite lock (#314); no third node (#384);
  debug goal = whole set, sha-gated (#375–#377); WiFi node addresses porta by IPv6 (#379); bell = classes by CRC (#380);
  OTA: code-push design §10 T1–T10 (in-channel on the S2, registry a knob, run.sh: the caller's value wins); XIP hot path in RAM,
  `-O2` on RP2350 (#403); `OEVM_HOT` + peephole on (#412); **one licence, Apache-2.0, porta and devsdk included; third-party
  code keeps its headers (#368, README §License, 2026-10-11)**; modifying a collection inside its own `do:` is undefined (dialect row 33, #37).
- **Weekly review, not scheduled:** #175, #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #345–#357, #359–#361, #363, #366, #369–#374, #378, #382–#397, #399–#401, #404–#411, #413–#415.
