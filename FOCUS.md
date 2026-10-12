# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **0.1 (David, 2026-10-09): one node, every verb — load, debug, patch, profile — through porta, over the radio, while it sleeps.**
  Demos: the ship's bell twice, wall C6 always-on (#380) and MagTag deep-sleep (#379). Lifecycle table: #26.
- **FOCUS: #233 — program OTA through porta.** The MagTag took its first push over WiFi (2026-10-10; design §10 "T10 as built").
  The S2 settings gap is closed (every S2 build carries the runner's settings; slots without settings is an `#error`). **Next:**
  (1) the S2 recipe at `OEVM_SPACE_MAX_CHANNELS=16`, `OEVM_MPROF_SLOTS` default — one BOOT0+RESET flash, read `space:`;
  (2) the bell over Thread (daylight, `flash-bell.sh`); then #398. Found-during: #418 (the WiFi fragment's own recipe overflows dram0).
- **Second thread, opti7040: #37 (RP2350 first, then what fits smaller parts).** Opt #11 merged 2026-10-12 (`5596ca03`): per-case
  operand decode, SMI arithmetic on the tagged words; WeAct STIC every row 0.7–24 %; C6 cross-VM: nsl beats Toit on 5/8 STIC rows, Toit
  6–7× on Richards/DeltaBlue (`docs/results/2026-10-12-37-c6-cross-vm/`). **Starts with (David, 2026-10-12): the frame and send cost**
  (~500 cycles a round trip): a design step. Draft in `docs/design/`, measure on Richards on c6spare. Bench: Are We Fast Yet (plan on #37). Then GC performance,
  the S2 verifier boot cost (MagTag in hand), compiler bytecodes, Dictionary as a prim. Every attempt gets a row in optimizations.md's Ledger. No RAM split for speed (#296 §7).
- **Awaiting David:** #368's last item, the history rewrite (private names, force-push); close #314; #396;
  read E draft 4 and L; #175's memo; the dongle reflash from master (#402); the RP2 remote-BOOTSEL design (#413).
- **Bench:** the bell `c6` is on the WALL (never flash). `c6spare` on opti7040 USB (`/dev/ttyUSB1`, CP2102N `…d2e21074…`; msi's `console-c6spare.env` still names it there), holds MicroPython 1.29.0 (the cross-VM run; driver `docs/results/2026-10-12-37-c6-cross-vm/run_all.sh`). MagTag
  `7cdfa11aaaaa` on the extender, build `…T140328` + program v2 (slot b, confirmed); enumerates only in BOOT0+RESET.
  **opti7040: both RP2350s parked in BOOTSEL** (WeAct `1-7`: the Opt #11 STIC image, driver `run_412.sh`; Zero `1-3`); msi worktree `~/rete-37head` (STIC harness applied uncommitted, `~/37h-build.sh`), `~/rete-37c6` (`~/37-c6sum-build.sh`); opti7040 worktree `~/rete-37head`; a UF2 on the volume boots them; build on msi.
  The plain ESP32 (WROOM-32, CP2104 `/dev/ttyUSB0`) is retired from #37 work (David: too painful).
- **Decided — do not re-litigate:** custos = nsl peer, porta language-neutral (D1–D2); share only `protocol/` (#338); agent first,
  HTTP the only agent surface (D11–D12); D13–D19; hot patch debug-build only (#313); one suite lock (#314); no third node (#384);
  debug goal = whole set, sha-gated (#375–#377); WiFi node addresses porta by IPv6 (#379); bell = classes by CRC (#380);
  OTA: code-push design §10 T1–T10 (in-channel on the S2, registry a knob, run.sh: the caller's value wins); XIP hot path in RAM,
  `-O2` on RP2350 (#403); `OEVM_HOT` + peephole on (#412); **one licence, Apache-2.0, porta and devsdk included; third-party
  code keeps its headers (#368, README §License, 2026-10-11)**; modifying a collection inside its own `do:` is undefined, no fail-fast (dialect row 33); `OP_ITER_AT` kernel-only (#37 design D3);
  iterator inlining: both receivers, slots by name, off in debug, land (#37 do-inline design); collection initial capacity stays 8; stored hashes compare with `=` (Opt #9); footprint re-banked, MCU 45356 / host 77801 (David, 2026-10-12); the loop trusts its methods, checked once at load and patch (Opt #10); arithmetic slow path out of line, not inlined (Opt #11).
- **Weekly review, not scheduled:** #175, #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #345–#357, #359–#361, #363, #366, #369–#374, #378, #382–#397, #399–#401, #404–#411, #413–#415, #417–#419.
