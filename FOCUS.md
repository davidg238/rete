# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **FOCUS (David, 2026-10-06): agent first** (peer design D11–D16). Six branches pushed, none merged, merge order:
  `367-load-agent-path` → `313-patch-restart-spike`, `front-door` → `d15-boards` (contains `314-verbs-proposal`) → `d16-collector`.
  **Next session starts with:** if David has run the c6spare load (`scratchpad/c6load.sh`: POST /load HanoiNode.oimg, expect sha `730f4404…`),
  merge #367, then d15, then d16; then D16 part 2 on `d16-collector` per `docs/superpowers/plans/2026-10-06-d16-custos-collector.md`
  (`/log`, udp sink, health/runs/panics, one `rete-custos` unit, delete `nodus/oevm/zephyr/tools/console/`). First action: `git switch d16-collector`.
- **Awaiting David:** the c6spare load run (#367); `POST /board/<n>/lend` yes/no (D16 plan, open point); LICENSE choice and history rewrite (#368);
  #358 re-bank (now includes #313's +1307 B mcu, hooks build); `review/tests/CLAIMS.md:86` (`mprof_resolve.py`); 4 `review/` files name the console tools.
- **Bench:** `c6bench` in the soak — never flash it. `c6spare`: std image, collector running (runs HanoiNode.oimg, sha `730f4404…`). `opti7040` has an RP2350.
- **Decided — do not re-litigate:** custos = nsl peer, porta language-neutral (D1–D2); porta and custos share only `protocol/` (#338); agent first, HTTP the only
  agent surface, CLIs unadvertised (D11–D12); a verb is complete with agent, web, TUI, raw HTTP (D13); load takes the .oimg as body, host VM prints the boot lines (D14);
  one `--board`/`--host name=…` per board, `/board/<name>/…` (D15; `--port` is the listen port); custos is the console collector, the Python is deleted (D16);
  hot patch is debug-build only (#313); 0.1 portable by processor, restrictions stated (overview); firmware flash stays `run.sh` for 0.1 (§3a).
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #342, #345–#361, #363, #366, #369; #233.
