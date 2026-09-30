# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **Next session starts with:** branch `302-rpi-pico2`, **#302 — the `rpi_pico2` (RP2350) rung, P1**. cabot's board
  move (M28) is gated on the measured µs/activation against the C6's 11.83. Task 1 is DONE by building, no board
  (`4697937`). Zephyr v4.2 has no RP2350 flash driver (Task 3's settings partition cannot land; bootId = 0) and no
  entropy driver; the C6's 11 s watchdog fits the RP2350 (16.78 s ceiling), not the RP2040 (8.39 s).
- **Rehearsal on the Feather RP2040** (`c4714a3`; hookup in `nodus/oevm/zephyr/boards/adafruit/feather_rp2040/README.md`).
  First action: cable in → `ls -l /dev/serial/by-id/` → `~/.config/rete/console-pico2.env` → `layoutdump/capture.py`
  + BOOTSEL + drop `layoutdump/build-feather/zephyr/zephyr.uf2` → `layout-rp2040.json` →
  `run.sh --board feather_rp2040 --node TimerNode.nsl --flash`, two RESETs, `bootId` 1→2→3. An M0+ Hanoi figure is a
  BOUND only, never on #301. Pico 2: same steps with `--board rpi_pico2` / `layout-rp2350.json`, then Task 5 →
  `docs/results/2026-09-DD-rp2350-first-numbers.md` → #301 + cabot.
- **Bench:** `c6bench` (gw85224-01) runs `PortaBridgeNodeC6` `g2488c2e`, collector hand-started (recipe in the
  `porta-gateway-operational-state` memory); `c6spare` is `TARGET_DEFAULT_BOARD` and disposable — its CP2102N was
  missing from USB on 09-19, check before trusting its collector; `nrf4ec0`/`nrf92c2` run `PortaBridgeNodeNrf` from
  wall chargers; `7adf` stays on `SoakNode`. Dongles flash by DFU only.
- **Decided — do not re-litigate** (records named): #273 H1/D1–D3 (`docs/design/2026-09-14-liveness-two-cadences-design.md`),
  #118/#242 node id = factory link address (`docs/design/2026-09-11-node-identity-hardware-rooted-design.md`; #118 is
  NOT overtaken by #186), #202/#201 series capacitor, #288 refuted, #271 direction 1 refuted, #281 closed (host-awake artefact).
- **Liveness thread's next move:** #272 — `DoSweep` in `Liveness.tla` requires Δwak at the declared rate; re-run
  `bench.cfg`/`slow.cfg` before any Go.
- **Awaiting David:** the three cabot relay items (focus-history, 2026-09-17 — nothing touches rete until confirmed);
  where #297 goes relative to the bench work; whether #300 is ever wanted.
- **Weekly review, not scheduled:** #253's burn list (#118/#242, #293, #292, #200 (b), #133/#244–#246), #280, #282–#287,
  #290, #291, #296–#300, #303.
