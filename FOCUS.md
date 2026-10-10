# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **0.1 (David, 2026-10-09): one node, every verb — load, debug, patch, profile — through porta, over the radio, while it sleeps.**
  Demos: the ship's bell twice, wall C6 always-on (#380) and MagTag deep-sleep (#379), CRC-factored and measured. Lifecycle table: #26.
- **FOCUS: #380 — flash the wall C6, then the verbs at it.** Branch `380-wall-c6` (a92bef6, pushed, not merged). `ShipsBellNodeC6` over
  `lib/Bells.nsl` (Dial shared; RtcClock, Trigger, Striker) is built in `nodus/oevm/zephyr/build-esp32c6-bell`, host gates green, not
  flashed. **Next session starts with:** daylight, bell on the powered hub, `nodus/oevm/zephyr/flash-bell.sh` (open the
  flashing-an-actuator-node memory first); watch boot + first strike on porta (`tel/log` row, `obs/build`); then the debugger at it
  through porta; measurements to #380. **Then, same session (David 2026-10-10):** #395 rooting arm — arm B of the CBOR AOT spike
  holding a fresh object across every recursive send, rooted, under `OEVM_GC_STRESS`, measured on c6spare (recipe in the #395 comment); with the number in, the AOT-vs-pure-C note in paper E §11 (#395). Then #233.
- **Awaiting David:** read E (draft 3, Appendices A–D lifted) and L; LICENSE and history rewrite (#368); close #314. Gateway runs
  `porta-379` + `MANIFEST-377.cddl` since 2026-10-09 19:55.
- **Bench:** the bell `c6` is on USB (CP2102N `4e5729ee`, `/dev/ttyUSB0`) still running the old alarm image; `c6spare` NOT attached.
  MagTag `7cdfa11aaaaa` is the fridge clock (bridged, debug+profile, 30-min sleeps, UDP console :54330). `c6bench` in the soak — never
  flash. `rete-custos` (desktop, :6971) serves `c6` and `c6spare`. `opti7040` has an RP2350.
- **Decided — do not re-litigate:** custos = nsl peer, porta language-neutral (D1–D2); share only `protocol/` (#338); agent first, HTTP
  the only agent surface (D11–D12); D13–D19; hot patch debug-build only (#313); one suite lock (#314); no third node for 0.1 (#384);
  the debug goal is the whole set, empty = detach, sha-gated (#375); boot stop holds only for a transport someone is on (#376); the
  debugger owns the held flag (#377); a WiFi node addresses porta by IPv6, VM below the radio driver (#379); the bell is classes by CRC,
  application classes never in the kernel, `uses:` is the seam, one wall node over the same library, a sync re-seeds silently
  (#380, 2026-08-03 design addendum 2026-10-09); paper E appendices in the lifted-references form (#380).
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #345–#357, #359–#361, #363, #366, #369–#374, #378, #382–#394; #233.
