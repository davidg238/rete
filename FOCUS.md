# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **0.1 is defined (David, 2026-10-09): one node, every verb — load, debug, patch, profile — through porta, over the radio, while it
  sleeps.** The demos are the ship's bell twice: wall C6 always-on (#380) and MagTag deep-sleep (#379), written factored by CRC and measured
  (notes on both; peer design addendum 2026-10-09). Holes the papers expose are filled before 0.1, never labelled. Lifecycle table: tracker #26.
- **FOCUS: remote debug over porta** (#342, D19). #375 is built and closed on `375-debug-manifest` (a1d8ff4, pushed, not merged): the
  breakpoint set rides `Debug{sha, breaks}`, the hold rides `obs/debug` (shapes: peer design D19 addendum; RECORDS §4.1). **Next session
  starts with:** a branch off master (or off `375-debug-manifest` if unmerged) for #376 — a debug image must not wait for a cable at boot:
  re-measure the boot stop in `platform_debug_readline` (`platform_zephyr.c`), then decide the no-cable path. Then #377, #380, #379, #233.
- **Papers** on `paper-split` (pushed, not merged): L draft 4, E draft 2 + Barry 1989 folded in (2809fbd; the gw copy is NOT refreshed).
  Copies at `http://gw85224-01:6971/dev/papers/` (markdown_it + the page's own `<style>`, scp to `gw85224-01:porta-v2/docs/papers/`).
- **Awaiting David:** merge `375-debug-manifest` (yes/no); read E and L, merge `paper-split` (yes/no); LICENSE and history rewrite (#368);
  close #314 (yes/no); when c6bench moves to `rete-custos`; `review/` names the deleted console tools.
- **Bench:** `c6bench` in the soak — never flash it. `rete-custos` (desktop, :6971) serves `c6` (the bell, cable unplugged = normal) and `c6spare`;
  `custos.env` carries `--esptool` (#370). `opti7040` has an RP2350.
- **Decided — do not re-litigate:** custos = nsl peer, porta language-neutral (D1–D2); share only `protocol/` (#338); agent first, HTTP the only
  agent surface (D11–D12); D13–D19; hot patch debug-build only (#313); one suite lock (#314); one `platform` is a 0.1 convenience (ledger row 32);
  two papers, one owner per topic; E a report not a defence, L claims nothing new (outlines addendum 2026-10-09); no third node for 0.1 (#384);
  the debug goal is the whole set, empty = detach, sha-gated (#375 addendum).
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #345–#357, #359–#361, #363, #366, #369–#374, #382–#386; #233.
