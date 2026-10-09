# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **0.1 is defined (David, 2026-10-09): one node, every verb — load, debug, patch, profile — through porta, over the radio, while it
  sleeps.** The demos are the ship's bell twice: wall C6 always-on (#380) and MagTag deep-sleep (#379), written factored by CRC and measured
  (notes on both; peer design addendum 2026-10-09). Holes the papers expose are filled before 0.1, never labelled. Lifecycle table: tracker #26.
- **FOCUS: remote debug over porta** (#342, D19). #375, #376 and #377 are merged to master (2063e99) and closed: the breakpoint set rides
  `Debug{sha, breaks}`, the hold rides `obs/debug`, a debug image boots with no cable, and the held state brackets the command loop whichever
  link serves it (D19 addenda 2026-10-09). **Next session starts with:** a branch off master for #380 — the wall C6 bell, always-on, every
  verb through porta over the radio: read the #380 note and `ShipsBellNodeC6.nsl`, decide what the demo's script is, flash `c6spare` first
  (never `c6` without `flash-bell.sh`, daylight). Then #379, #233. #378 (hard spin while held: 1488 exchanges in 97 s) stays open.
- **Papers** merged to master (2026-10-09): L draft 4, E draft 2 + Barry 1989 (2809fbd). Copies at `http://gw85224-01:6971/dev/papers/`
  (markdown_it + the page's own `<style>`, scp to `gw85224-01:porta-v2/docs/papers/`).
- **Awaiting David:** read E and L; LICENSE and history rewrite (#368); close #314 (yes/no); when c6bench moves to `rete-custos`;
  `review/` names the deleted console tools.
- **Bench:** `c6bench` in the soak — never flash it. `rete-custos` (desktop, :6971) serves `c6` (the bell, cable unplugged = normal) and `c6spare`
  (CP2102N only: flash with `ESPTOOL_PORT`+`CONSOLE_PORT` = its by-id, run.sh lends the cable itself; the Zephyr env is
  `~/zephyr-esp32/.venv` + `zephyr-env.sh` + `ZEPHYR_SDK_INSTALL_DIR`); `custos.env` carries `--esptool` (#370). `opti7040` has an RP2350.
  `c6spare` runs `DebugHoldProbeC6` (#377) pointed at the stopped `porta-377` injector port, so it reports misses until reflashed.
- **Decided — do not re-litigate:** custos = nsl peer, porta language-neutral (D1–D2); share only `protocol/` (#338); agent first, HTTP the only
  agent surface (D11–D12); D13–D19; hot patch debug-build only (#313); one suite lock (#314); one `platform` is a 0.1 convenience (ledger row 32);
  two papers, one owner per topic; E a report not a defence, L claims nothing new (outlines addendum 2026-10-09); no third node for 0.1 (#384);
  the debug goal is the whole set, empty = detach, sha-gated (#375); the boot stop holds only for a transport someone is on (#376); the debugger
  owns the held flag, one writer, prim args read with `is_truthy` (#377).
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #345–#357, #359–#361, #363, #366, #369–#374, #378, #382–#389; #233.
