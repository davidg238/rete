# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **FOCUS (David, 2026-10-06): agent first** (peer design D11–D18). 2026-10-07 evening went to the nsl design paper
  (`nodus/nsl/docs/nsl-design-paper.md` + `.pdf`, `504bee2`); David reviews it 2026-10-08 night. **Next session starts with:**
  David's paper comments if he brings them; otherwise #368 — `gh issue view 368 --comments`, then act on the LICENSE and history-rewrite answers.
- **Awaiting David:** paper review; LICENSE choice and history rewrite (#368, a force-push); close #314 (yes/no); the Tailscale browser
  check (status page NOT published 2026-10-07, twice); when c6bench moves to `rete-custos`; `review/program-sequence.md` and `review/tests/CLAIMS.md` name the deleted console tools.
- **Bench:** `c6bench` in the soak — never flash it. `rete-custos` (desktop, :6971, master build) serves `c6` (the bell, cable unplugged = normal)
  and `c6spare` (std firmware `porta-v1-final-1422`, HanoiNode sha `730f4404…`); `custos.env` carries `--esptool` (#370). `opti7040` has an RP2350.
- **Decided — do not re-litigate:** custos = nsl peer, porta language-neutral (D1–D2); porta and custos share only `protocol/` (#338); agent first, HTTP the only
  agent surface (D11–D12); a verb is complete with agent, web, TUI, raw HTTP (D13); load takes the .oimg as body (D14); `--board`/`--host name=…`,
  `/board/<name>/…` (D15); custos is the console collector (D16); patch verb, map carries `image.layout` (D17); build/run, run host-only,
  compiler at `RETE_NSL_TOOLS` (D18); hot patch is debug-build only (#313); one suite lock, an aux SKIP fails, run-one defaults to B (#314);
  one all-powerful `platform` is a 0.1 convenience, not design (ledger row 32, 2026-10-07).
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #342, #345–#357, #359–#361, #363, #366, #369–#374; #233.
