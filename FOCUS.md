# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **Next session starts with:** one queued yes/no for David — file the five 0.1 buildmap items in `docs/overview.md` as
  issues (program in its own flash region, debuggable C6 build, profiling packaged, patch-and-resume spike, one command
  surface)? Then the bench focus is unchanged: **#118/#242**, branch `118-node-id-factory-address` (pushed, not merged), plan
  `docs/superpowers/plans/2026-10-02-node-id-factory-address.md` Task 10: flash `NodeIdProbe.nsl` on `c6spare` and one nRF
  board, compare the printed id with the sticker.
- **Then, David present (Task 11–12):** gateway restart with the new manifest (sha `844ba659…` → `d7eefc87…`), reflash every
  node (bell in daylight), promote to `porta/docs/ARCHITECTURE.md` + `protocol/PROTOCOL-NSL.md`, merge, close #118/#242.
- **Decided 2026-10-02 — do not re-litigate** (record: `docs/overview.md`; `docs/design/2026-10-02-frozen-1.0-surface.md` §0a;
  #253, #79): first public release is a 0.x, explicitly unstable, freeze is a later 1.0.0; 0.1 = one person's first hour on
  one ESP32-C6 on one USB cable, with debug, profile and patching a running method on the device, and `.nsl` reload over the
  cable without reflashing the firmware; remote (gateway, OTA of `.nsl`, not the VM) adds later. For 1.0.0: a gateway serves
  several versions and a node one; three-part version number, hash stays as identity; the Toit "v1" has no claim on the
  numbering (`protocol/v1/` → `protocol/archive/`); authentication is per node, by level (#291). Command line is the
  contract, MCP a later thin front (proposed, `docs/development.md` §5). Older: the #118 identity decisions (design 2026-09-11 + addendum).
- **Not built, only written:** everything above is paper. Drafts awaiting review: the frozen surface (11 decisions), security
  levels (9 decisions); neither blocks 0.1. Burn list (#253) is now owed before 1.0.0: #305, #293, #292, #200 (b).
- **Bench (2026-10-02):** unchanged — RP2350-Zero prints `node 4530454d38330003`; `c6spare` JTAG node here, CP2102N absent;
  `c6bench`, `nrf4ec0`/`nrf92c2`, `7adf` unverified. The status page was NOT published: gw85224-01 wants the Tailscale browser check.
- **Awaiting David:** the buildmap question; the #118 board proof and deploy; `review/DOCMAP.md` line 56 (his tree, still
  cites `protocol/v1/`); the three cabot relay items; where #297 goes; whether #300 is wanted; c6spare's CP2102N.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #296–#300, #303, #304, #306–#309. Liveness next: #272.
