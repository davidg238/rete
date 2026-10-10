# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **0.1 (David, 2026-10-09): one node, every verb — load, debug, patch, profile — through porta, over the radio, while it sleeps.**
  Demos: the ship's bell twice, wall C6 always-on (#380) and MagTag deep-sleep (#379), CRC-factored and measured. Lifecycle table: #26.
- **FOCUS: #379 proven, awaiting merge.** Branch `379-magtag-porta-debug` (66dd81c): the MagTag debug image re-arms a breakpoint
  from the `goal/debug/main` cell on every cold boot, is held and stepped through the production porta with no cable, detaches on
  the empty set (D19 addendum 2026-10-09 #379; run in #379). **Next session starts with:** David merges the branch and swaps the
  gateway to `porta-379` (three calls, in #379); then #380 — the wall C6 bell, every verb through porta: read `ShipsBellNodeC6.nsl`,
  flash `c6spare` first (never `c6` without `flash-bell.sh`, daylight). Then #233.
- **Awaiting David:** merge `379-magtag-porta-debug`; the gateway swap; read E and L; LICENSE and history rewrite (#368); close #314.
- **Bench:** MagTag `7cdfa11aaaaa` runs `MagTagSleepyDebugNode` (30 s sleeps, WiFi, porta by IPv6 ULA, UDP console to the desktop's
  stable ULA:54330), breakpoints detached, battery life hours not weeks — the clock is NOT on it. `c6bench` in the soak — never flash.
  `rete-custos` (desktop, :6971) serves `c6` (the bell) and `c6spare` (`DebugHoldProbeC6`, reports misses). `opti7040` has an RP2350.
- **Decided — do not re-litigate:** custos = nsl peer, porta language-neutral (D1–D2); share only `protocol/` (#338); agent first, HTTP
  the only agent surface (D11–D12); D13–D19; hot patch debug-build only (#313); one suite lock (#314); no third node for 0.1 (#384);
  the debug goal is the whole set, empty = detach, sha-gated (#375); boot stop holds only for a transport someone is on (#376); the
  debugger owns the held flag (#377); a WiFi node addresses porta by IPv6 and the VM's main thread sits below the radio driver (#379).
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #345–#357, #359–#361, #363, #366, #369–#374, #378, #382–#392; #233.
