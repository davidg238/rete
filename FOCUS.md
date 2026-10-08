# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **FOCUS (David, 2026-10-08): remote debug of sleepy and running nodes over porta** (#342, peer design D19). Host spike
  merged to master (`1012090`) (`docs/results/2026-10-07-342-sleepy-node-debug-spike.md`). **Next session starts with:**
  a branch off master for #375: update `Debug{…}` in `protocol/MANIFEST.cddl` to carry the breakpoints (+ sha) and add `obs/debug`, editing
  the manifest, `records.yaml`, the goldens and porta together; then make `sleepy_debug_host.sh` leg B manifest-armed. Then #376, #377.
- **Queue:** #375 → #376 (USB or porta, no opt-in; no boot wait) → #377 (VM held state) → #380 (C6 demo) → #379 (MagTag demo), both with program OTA (#233);
  #378 open (no spin; an abandoned session); #381 cold-boot profile after debug.
- **Awaiting David:** the paper review; LICENSE and history rewrite (#368, a force-push); close #314 (yes/no); when c6bench moves to `rete-custos`; `review/` names the deleted console tools.
- **Bench:** `c6bench` in the soak — never flash it. `rete-custos` (desktop, :6971) serves `c6` (the bell, cable unplugged = normal)
  and `c6spare` (std `porta-v1-final-1422`); `custos.env` carries `--esptool` (#370). `opti7040` has an RP2350.
- **Decided — do not re-litigate:** custos = nsl peer, porta language-neutral (D1–D2); share only `protocol/` (#338); agent first, HTTP the
  only agent surface (D11–D12); a verb is complete with agent, web, TUI, raw HTTP (D13); D14–D18 (load, boards, collector, patch, build/run);
  remote debug D19 (two demos, Debug record + obs/debug, USB or porta with no opt-in, VM held state); hot patch debug-build only (#313);
  one suite lock (#314); one `platform` is a 0.1 convenience (ledger row 32).
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #345–#357, #359–#361, #363, #366, #369–#374; #233.
