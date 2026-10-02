# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **Next session starts with:** **#118/#242** (burn list #253, item 2) — built, host-gated, NOT closed. Branch
  `118-node-id-factory-address` (pushed, not merged). Plan: `docs/superpowers/plans/2026-10-02-node-id-factory-address.md`,
  Tasks 0–9 done; next is Task 10. First action: flash `NodeIdProbe.nsl` on `c6spare` and one nRF board, compare the
  printed id with the sticker (on the nRF this settles raw `DEVICEADDR` vs the BLE form with the top two bits set).
- **Then, David present (Task 11–12):** gateway restart with the new manifest (sha moves `844ba659…` → `d7eefc87…`; a node
  built from the branch mismatches the running gateway until then), reflash every node (bell in daylight), promote to
  `porta/docs/ARCHITECTURE.md` + `protocol/PROTOCOL-NSL.md`, merge, close #118/#242. Burn list after: #305, #293, #292, #200 (b), #244–#246.
- **Bench (2026-10-02):** the RP2350-Zero on this desktop runs `NodeIdProbe` (USB-CDC; BOOT+RUN = BOOTSEL) and prints
  `node 4530454d38330003`. `c6spare`: JTAG node here, CP2102N absent. `c6bench`, `nrf4ec0`/`nrf92c2`, `7adf`: unverified, old build-time ids.
- **Decided — do not re-litigate** (record: `docs/design/2026-09-11-node-identity-hardware-rooted-design.md` + its 2026-10-02
  addendum): id = factory id from silicon, lowercase hex (12 on ESP/nRF, 16 on RP = flash serial via `hwinfo`); nRF reads
  FICR `DEVICEADDR`; host answers `OEVM_NODE_ID` or fails; board name = `OEVM_BOARD_NAME` → optional `obs/build.board`;
  an unknown board family is a compile error. Older: #273 H1/D1–D3, #202/#201 series capacitor, #288 refuted, #281 closed.
- **Not shown for #118:** a real image for esp32, esp32s2 (#196/#227) or feather_rp2040 (#306) — the new file compiles there,
  no link; `run.sh`'s board-name mismatch path (`exit 3`).
- **Liveness thread's next move:** #272 — `DoSweep` in `Liveness.tla`; re-run `bench.cfg`/`slow.cfg` before any Go.
- **Awaiting David:** the board proof and deploy above; the three cabot relay items (focus-history, 2026-09-17); where #297 goes; whether #300 is wanted.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #296–#300, #303, #304, #306.
