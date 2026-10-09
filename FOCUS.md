# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **0.1 is defined (David, 2026-10-09): one node (the C6, then the MagTag), every verb — load, debug, patch, profile — through porta,
  over the radio, while it sleeps** (#380/#379 with #375–#377 and #233 inside; tracker #26 has the lifecycle table). Nobody is waiting;
  holes the papers expose are filled before 0.1, not labelled. Everything off that path is P3 for 0.1.
- **FOCUS (David, 2026-10-08): remote debug of sleepy and running nodes over porta** (#342, peer design D19). **Next session starts
  with:** a branch off master for #375: update `Debug{…}` in `protocol/MANIFEST.cddl` to carry the breakpoints (+ sha) and add `obs/debug`, editing
  the manifest, `records.yaml`, the goldens and porta together; then make `sleepy_debug_host.sh` leg B manifest-armed. Then #376, #377.
- **Paper thread (2026-10-09):** L (language, `nodus/nsl/docs/nsl-design-paper.md`, draft 4: an engineering account for three readers,
  three kinds of C) and E (`nsl-embedded-paper.md`, draft 2: a report, not a defence; Clohessy 1996 as lineage; #61 beside CBOR; MicroPython
  straight, #383) on branch `paper-split`; reading copies at `http://gw85224-01:6971/dev/papers/` (markdown_it + the page's style, scp
  to `gw85224-01:porta-v2/docs/papers/`). Outline and decisions: `nodus/nsl/docs/design/2026-10-08-paper-split-outlines.md`.
- **Queue:** #375 → #376 → #377 → #380 (C6 demo) → #379 (MagTag demo), both with program OTA (#233); #378 open; #381 cold-boot profile; #297 (CBOR walk to C).
- **Awaiting David:** read E and L, and merge `paper-split` (yes/no); LICENSE and history rewrite (#368, a force-push); close #314 (yes/no);
  when c6bench moves to `rete-custos`; `review/` names the deleted console tools.
- **Bench:** `c6bench` in the soak — never flash it. `rete-custos` (desktop, :6971) serves `c6` (the bell, cable unplugged = normal)
  and `c6spare`; `custos.env` carries `--esptool` (#370). `opti7040` has an RP2350.
- **Decided — do not re-litigate:** the 0.1 demos are the ship's bell twice — wall C6 always-on (#380) and MagTag deep-sleep (#379), one program, two
  power modes, no third node (#384 waits; David, 2026-10-09); custos = nsl peer, porta language-neutral (D1–D2); share only `protocol/` (#338); agent first, HTTP the only agent
  surface (D11–D12); D13–D19; hot patch debug-build only (#313); one suite lock (#314); one `platform` is a 0.1 convenience (ledger row 32);
  two papers, one owner per topic; agent-first split by question; Thread node stays E's appendix; L carries the grammar (split outlines doc).
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #345–#357, #359–#361, #363, #366, #369–#374, #382; #233.
