# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **Two threads, in parallel, one per session; neither edits the other's files.** Liveness/CBOR (#272 → #297, branch
  `297-cbor-native-walk`) and custos (#198, branch `198-custos-views`); both off `329-c6-speed`, which merges first.
- **custos — next session starts with:** branch `198-custos-views` (pushed). Task 7 of
  `porta/docs/superpowers/plans/2026-08-26-gatekeeper-query-core.md` — the five-question acceptance on a committed
  fixture — then Task 8's recorded run on gw85224-01: `~/custos-dev/custos-cli` against `~/custos-dev/snap-20261004.db`.
- **Liveness/CBOR — next session starts with:** implement #297 per `docs/superpowers/plans/2026-10-04-297-cbor-native-walk.md`,
  Task 1 first. After ~2026-10-04 20:35 MST: #272 Task 5 Step 5 (gw/liveness by verdict per node, post, close). Then
  #329's five radio scenarios at -O2 on `c6spare` (porta-* need the injector by hand), then #296.
- **Bench:** `c6bench` is in the soak — never flash it. `c6spare` + RP2350-Zero on the desktop's motherboard ports.
  custos on :6971 is a systemd user unit (`systemctl --user status custos`); porta is still hand-started (#282).
- **Decided — do not re-litigate:** custos = named queries only, auth silent, Tailscale is the perimeter; `liveness`
  shows porta's verdict, never re-derives it (first-cut design addenda D1, D3). #272 per D3.3. -O2 is the C6 default.
- **Not proven:** #317, the C6 polled-reader hang, #327 (deaf re-mark not in TLA), #333 (c6bench: two boards?).
- **Awaiting David:** merge `329-c6-speed`?; delete the 2.46 GB snapshot after Task 8?; `review/DOCMAP.md`
  regeneration; #319; DOCMAP line 56; buildmap 3; the three cabot relay items; #300.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298–#300, #303, #304, #306–#309, #315–#328, #331–#334.
