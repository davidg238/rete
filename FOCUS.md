# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **Next session starts with:** **#253 — the burn list before the 1.0 manifest hash** (the gate on going public; #79 is
  its other half). First item **#133** (the one open P0; David confirmed 2026-10-01). Branch: cut `133-ld-slot`
  from `master` (`302-rpi-pico2` is merged). First action: re-measure #133 on the current tree with
  `run-one.py <repro>.nsl B` before touching codegen. Rest of the list: #118/#242, #293, #292, #200 (b), #244–#246.
- **#302 is CLOSED (2026-10-01):** RP2350 = 11.13 µs/activation vs the C6's 11.83; record
  `docs/results/2026-10-01-rp2350-first-numbers.md`, rung on `review/program-sequence.md`, board axes in
  `nodus/oevm/zephyr/docs/BOARD_PORTS.md`. Left for their own rungs: a QMI flash driver (NVS/bootId), entropy, uart0, SWD.
- **Bench:** the RP2350-Zero is on this desktop's USB running `TimerNode` at the 442368 arena (USB-CDC console; RUN =
  reset, BOOT+RUN = BOOTSEL). Unverified since 09-19: `c6bench` (gw85224-01) on `PortaBridgeNodeC6` `g2488c2e`;
  `c6spare` disposable, its CP2102N was missing from USB; `nrf4ec0`/`nrf92c2` on `PortaBridgeNodeNrf`; `7adf` on `SoakNode`.
- **Decided — do not re-litigate** (records named): #273 H1/D1–D3 (`docs/design/2026-09-14-liveness-two-cadences-design.md`),
  #118/#242 node id = factory link address (`docs/design/2026-09-11-node-identity-hardware-rooted-design.md`; #118 is
  NOT overtaken by #186), #202/#201 series capacitor, #288 refuted, #271 direction 1 refuted, #281 closed (host-awake artefact).
- **Liveness thread's next move:** #272 — `DoSweep` in `Liveness.tla` requires Δwak at the declared rate; re-run
  `bench.cfg`/`slow.cfg` before any Go.
- **Awaiting David:** the three cabot relay items (focus-history, 2026-09-17); where #297 goes; whether #300 is wanted.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #296–#300, #303.
