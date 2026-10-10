# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **0.1 (David, 2026-10-09): one node, every verb — load, debug, patch, profile — through porta, over the radio, while it sleeps.**
  Demos: the ship's bell twice, wall C6 always-on (#380) and MagTag deep-sleep (#379), CRC-factored and measured. Lifecycle table: #26.
- **FOCUS: #380 — the wall C6 over the same classes.** Branch `380-bell-classes` (on top of `379-magtag-porta-debug`, both unmerged):
  `lib/Bells.nsl` + the `uses:` pragma + `MagTagDeepSleepNode` as a 14-line `main:`, bridged, on the fridge on the debug+profile
  image (measurements in #380; design addendum in `docs/design/2026-07-24-magtag-deep-sleep-clock-design.md`). **Next session starts
  with:** read `sys/metrics` for the 20:00 watch end (36 KB arena under sync+paint), then `ShipsBellNodeC6.nsl` → the same classes
  minus `StatsPage` plus the striker as its own class, flash `c6spare` first (never `c6` without `flash-bell.sh`, daylight), debug+profile
  image through porta; its `main:` goes beside the 14-line one in paper E Appendix A. Then #233.
- **Awaiting David:** merge `379-magtag-porta-debug` then `380-bell-classes`; the gateway swap to `porta-379` (#379); pick a paper E
  Appendix A form (inline vs `-refs` copy, both at `gw85224-01:6971/dev/papers/`); read E and L; LICENSE and history rewrite (#368); close #314.
- **Bench:** MagTag `7cdfa11aaaaa` is the fridge clock again — `MagTagDeepSleepNode`, bridged, debug+profile, 30-min sleeps, panel live,
  UDP console to the desktop's stable ULA:54330 (a listener may still be running from 2026-10-09). `c6bench` in the soak — never flash.
  `rete-custos` (desktop, :6971) serves `c6` and `c6spare`. `opti7040` has an RP2350.
- **Decided — do not re-litigate:** custos = nsl peer, porta language-neutral (D1–D2); share only `protocol/` (#338); agent first, HTTP
  the only agent surface (D11–D12); D13–D19; hot patch debug-build only (#313); one suite lock (#314); no third node for 0.1 (#384);
  the debug goal is the whole set, empty = detach, sha-gated (#375); boot stop holds only for a transport someone is on (#376); the
  debugger owns the held flag (#377); a WiFi node addresses porta by IPv6, VM below the radio driver (#379); the bell is classes by CRC,
  application classes never in the kernel, `uses:` is the seam (#380, 2026-10-09).
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #345–#357, #359–#361, #363, #366, #369–#374, #378, #382–#393; #233.
