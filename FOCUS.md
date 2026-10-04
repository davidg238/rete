# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **Next session starts with:** branch `297-cbor-native-walk` (pushed; off `329-c6-speed`). Focus **#297** (P2). First
  action: execute the approved plan `docs/superpowers/plans/2026-10-04-297-cbor-native-walk.md` **natively** (David,
  2026-10-04), Task 1 Step 1 (failing `test_gen_builtins` case). Time-gated, after ~2026-10-04 20:35 MST: #272 plan
  Task 5 Step 5 (gw/liveness table, close).
- **Then, one per session:** #329 — the five radio scenarios at -O2 on `c6spare`, then merge; #296 (RP2350 placement).
- **Bench:** `c6bench` is in the soak — never flash it. `c6spare` + RP2350-Zero on this desktop's motherboard ports.
- **Decided — do not re-litigate:** #297 = C walk through NATIVE vtable bindings, nsl walk kept as oracle + fallback,
  ivar shape checked at bake (design doc + its 2026-10-04 addendum: walk in `platform.c`, `NATIVES` dict, Path A
  oracle-only). #272 per D3.3 + its 2026-10-03 addendum. -O2 is the C6 default (#329).
- **Not proven:** #317, the C6 polled-reader hang, #327, #330 (two trues on Path B).
- **Awaiting David:** close #319?; buildmap 3; DOCMAP line 56; the three
  cabot relay items; #300.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298–#300, #303, #304, #306–#309, #315–#328, #330, #331.
