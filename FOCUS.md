# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **Next session starts with:** **#253 — the burn list before the 1.0 manifest hash** (the gate on going public; #79 is
  its other half). Next item **#118/#242** (node id = factory link address; decided, below). Branch: cut from `master`.
  First action: read #242's body, then re-measure what the tree does today before editing. Rest: #293, #292, #200 (b), #244–#246.
- **#133 is CLOSED (2026-10-01)**, merged at 181ab7a; no P0 is open. Record: the closing comment, `nodus/nsl/docs/compiler.md`
  (the `self` = `get_self_reg()` invariant), `docs/results/2026-10-01-133-ld-slot-rp2350-console.log`. Found-during: #304.
- **Bench (enumerated 2026-10-01):** the RP2350-Zero on this desktop runs the #133 gate image, not `TimerNode` (USB-CDC;
  BOOT+RUN = BOOTSEL). `c6spare`: JTAG node here, CP2102N absent, so the target harness cannot drive it. `c6bench`: CP2102N
  on gw85224-01, image unverified since 09-19. `nrf4ec0`/`nrf92c2` on `PortaBridgeNodeNrf`, `7adf` on `SoakNode` — unverified.
- **Decided — do not re-litigate** (records named): #273 H1/D1–D3 (`docs/design/2026-09-14-liveness-two-cadences-design.md`),
  #118/#242 node id = factory link address (`docs/design/2026-09-11-node-identity-hardware-rooted-design.md`; #118 is
  NOT overtaken by #186), #202/#201 series capacitor, #288 refuted, #271 direction 1 refuted, #281 closed (host-awake artefact).
- **Liveness thread's next move:** #272 — `DoSweep` in `Liveness.tla` requires Δwak at the declared rate; re-run
  `bench.cfg`/`slow.cfg` before any Go.
- **Awaiting David:** the three cabot relay items (focus-history, 2026-09-17); where #297 goes; whether #300 is wanted.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #296–#300, #303, #304.
