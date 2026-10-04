# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **Next session starts with:** **#330 (P0) + #75 (P1), David 2026-10-04** — Path B has two `true`/`false` objects. Branch
  `330-canonical-booleans` off `297-cbor-native-walk`. First action: re-measure with the spike's `TwoTrues.nsl`
  (`docs/spikes/2026-10-04-cbor-prim-arms/`), then find where the second object is minted. Fix #75 (P1, decoded Symbols not interned) in the
  same pass — David 2026-10-04: one canonical-Symbol rule for both.
  Time-gated, after ~2026-10-04 20:35 MST: #272 plan Task 5 Step 5 (gw/liveness table, close).
- **Then:** #297 — execute the approved plan `docs/superpowers/plans/2026-10-04-297-cbor-native-walk.md` natively,
  Task 1 Step 1 (if #330 lands first, drop the plan's "accept both trues" constraint).
- **After that, one per session:** #329 — the five radio scenarios at -O2 on `c6spare`, then merge; #296 (RP2350 placement).
- **custos thread (#198) is CLOSED 2026-10-04** (§10 answered on the real store; design addendum E). Branch
  `198-custos-views` (pushed) merges after `329-c6-speed`. Next on it, weekly review: #332 pushdown, #335.
- **Bench:** `c6bench` is in the soak — never flash it. `c6spare` + RP2350-Zero on this desktop's motherboard ports.
- **Decided — do not re-litigate:** #297 = C walk through NATIVE vtable bindings, nsl walk kept as oracle + fallback,
  ivar shape checked at bake (design doc + its 2026-10-04 addendum: walk in `platform.c`, `NATIVES` dict, Path A
  oracle-only). #272 per D3.3 + its 2026-10-03 addendum. -O2 is the C6 default (#329).
- **Not proven:** #317, the C6 polled-reader hang, #327.
- **Awaiting David:** buildmap 3; DOCMAP line 56; the three
  cabot relay items; #300.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298–#300, #303, #304, #306–#309, #315–#328, #331–#335.
