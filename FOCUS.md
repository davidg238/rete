# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **FOCUS (David, 2026-10-06): agent first** (peer design D11–D16). Branches pushed, none merged, merge order:
  `367-load-agent-path` → `313-patch-restart-spike`, `front-door` → `d15-boards` (contains `314-verbs-proposal`) → `d16-collector`.
  D16 part 2 is built on `d16-collector` (`2eac3fb`, #154 `1618d93`). **Next session starts with:** if David has run the c6spare load
  (`scratchpad/c6load.sh`: POST /load HanoiNode.oimg, expect sha `730f4404…` — custos now runs as `rete-custos`, so load through it),
  merge #367, then d15, then d16. First action: `gh issue view 367 --comments`; then the first real harness flash proves #154.
- **Awaiting David:** the c6spare load run (#367); when c6bench moves to `rete-custos` (soak); LICENSE choice and history rewrite (#368);
  #358 re-bank (+1307 B mcu from #313; footprint is the only red aux); `review/program-sequence.md`, `review/tests/CLAIMS.md` name the deleted console tools.
- **Bench:** `c6bench` in the soak — never flash it. `rete-custos` (desktop, :6971) collects `c6` (the bell, cable unplugged = normal) and `c6spare`
  (std image, HanoiNode.oimg, sha `730f4404…`); boards in `~/.config/rete/custos.env`. `opti7040` has an RP2350.
- **Decided — do not re-litigate:** custos = nsl peer, porta language-neutral (D1–D2); porta and custos share only `protocol/` (#338); agent first, HTTP the only
  agent surface, CLIs unadvertised (D11–D12); a verb is complete with agent, web, TUI, raw HTTP (D13); load takes the .oimg as body (D14);
  one `--board`/`--host name=…` per board, `/board/<name>/…` (D15); custos is the console collector, Python deleted, `run.sh` owns the lend (D16, #154);
  hot patch is debug-build only (#313); 0.1 portable by processor (overview); firmware flash stays `run.sh` for 0.1 (§3a).
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #342, #345–#361, #363, #366, #369; #233.
