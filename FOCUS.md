# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **0.1 (David, 2026-10-09): one node, every verb — load, debug, patch, profile — through porta, over the radio, while it sleeps.**
  Demos: the ship's bell twice, wall C6 always-on (#380) and MagTag deep-sleep (#379). Lifecycle table: #26 (debug row built 2026-10-10).
- **FOCUS: #233 — program OTA through porta, the last 0.1 verb** (in #380's scope since 2026-10-08; #380 stays open for it).
  Branch `380-wall-c6` (pushed, not merged; merge or branch `233-…` first). **Next session starts with:** re-read #233 and the
  OTA design, then the host fleet-sim gate (`host_ota`, standalone, 4 min) to see what the desk already proves; target c6spare, never
  the bell until it is proven. Then paper E §9's OTA line and the #380 close.
- **Done 2026-10-10:** bell flashed (`porta-v1-final-1525-g6bc935d-20261010T081015`, ELF+map on desktop and gw), 8 bells at 08:00,
  debugged through porta; paper E draft 4 published (§9 built on both bells, §3 AOT triple) + a no-comments copy; #395 rooting priced.
- **Awaiting David:** read E draft 4 (`/dev/papers/`) and L; LICENSE and history rewrite (#368); close #314; #396 — should the gateway
  copy of the ELF archive be a step of `flash-bell.sh`?
- **Bench:** the bell `c6` is on the WALL (off USB, clean image, alarm-driven, observe through porta). `c6spare` on USB (CP2102N
  `d2e21074`) running the CborRootStress bench image — disposable. MagTag `7cdfa11aaaaa` is the fridge clock (bridged, debug+profile).
  `c6bench` in the soak — never flash. `rete-custos` (desktop, :6971) serves `c6` and `c6spare`. `opti7040` has an RP2350.
- **Decided — do not re-litigate:** custos = nsl peer, porta language-neutral (D1–D2); share only `protocol/` (#338); agent first, HTTP
  the only agent surface (D11–D12); D13–D19; hot patch debug-build only (#313); one suite lock (#314); no third node for 0.1 (#384);
  the debug goal is the whole set, empty = detach, sha-gated (#375); boot stop holds only for a transport someone is on (#376); the
  debugger owns the held flag (#377); a WiFi node addresses porta by IPv6 (#379); the bell is classes by CRC, `uses:` is the seam, a sync
  re-seeds silently (#380, 2026-08-03 addendum 2026-10-09); E appendices lifted-references, plus a bare-listings copy (#380, 2026-10-10);
  AOT the nsl encoder rather than ship a C one, once a send-site class instrument exists (#395, results doc 2026-10-09).
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #345–#357, #359–#361, #363, #366, #369–#374, #378, #382–#397.
