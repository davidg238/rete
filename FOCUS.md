# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **Next session starts with (David present, daylight):** the **#118/#242 deploy**, branch `118-node-id-factory-address`, plan
  `docs/superpowers/plans/2026-10-02-node-id-factory-address.md` Task 11. First action: the read-only check on `gw85224-01`
  for tooling keyed on bench names. Then gateway restart with the new manifest (sha `844ba659…` → `d7eefc87…`), reflash every
  node (bell in daylight, dongles need RESET), Task 12: promote to `porta/docs/ARCHITECTURE.md` + `protocol/PROTOCOL-NSL.md`,
  merge, close.
- **Then: #311, the debugger on the C6** (David, 2026-10-02), branch `311-line-in-spike` (on top of the #118 branch): the
  per-bytecode hooks, the enable flag, and a held program that has timers. The input path is proven; record
  `docs/results/2026-10-02-311-line-in-spike.md`.
- **Decided 2026-10-02 — do not re-litigate:** the nRF id forces the top two bits, so it is the sticker value (design
  2026-09-11 addendum). **No SLIP**: plain lines over the console cable, SLIP only if one cable ever needs it (#311 record).
  Buildmap filed as #310–#314. Older, same day: 0.x first, 0.1 = one C6 on one cable, freeze is 1.0.0 (`docs/overview.md`, #253).
- **Not proven:** the top-two-bits change has compiled and never run on a board (`nrf4ec0` cannot show it; read `nrf92c2`).
  Why a polled reader hangs the C6 is not traced. The dongle and RP2350 were not rerun on the interrupt-driven path.
- **Bench (2026-10-02 14:20 MST):** `c6spare` on ONE cable (CP2102N) on the USB extender, running the echo probe; `nrf4ec0`
  on a motherboard port, running the echo probe (off the mesh until reflashed); RP2350-Zero unplugged, echo probe on it.
  `c6bench`, `nrf92c2`, `7adf` unverified. Flash the C6 with `ESPTOOL_PORT=<CP2102N by-id>` and the collector stopped (#316, #154).
- **Awaiting David:** the deploy session; `nodus/oevm/zephyr/nrf4ec0.jpg` (his photo, untracked — keep or delete);
  `review/DOCMAP.md` line 56 (still cites `protocol/v1/`); the three cabot relay items; where #297 goes; whether #300 is wanted.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #296–#300, #303, #304, #306–#309, #315, #316. Liveness next: #272.
