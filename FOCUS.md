# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **FOCUS (David, 2026-10-04): local-first custos, all P1** — when done we know if this is "something better". Design
  `docs/design/2026-10-04-custos-peer-design.md` §6a; tracker #26 top block. **Next session starts with #337** (the
  `.stmap`/`methods.json`/`.oimg` contract, proposed home `protocol/`), on a branch off master. Then #338 (the move,
  after `198-custos-views` merges), #339 debug, #312 profile, #310 load, #340 web, #341 TUI.
- **One thread only (David, 2026-10-04): the separate nodus session is shut down; this thread also does #310.**
  #330 and #75 are closed (`95bb141`, branch `330-canonical-booleans`, unmerged). Parked: #297's approved plan; #272
  plan Task 5 Step 5 (the soak ended ~20:35 MST 2026-10-04). Merge queue: `329-c6-speed`, then `198-custos-views`
  (#329 still owes the five radio scenarios at -O2 on `c6spare`).
- **Bench:** `c6bench` is in the soak — never flash it. `c6spare` + RP2350-Zero on this desktop's motherboard ports.
- **Decided — do not re-litigate:** custos = top-level nsl-specific peer, porta language-neutral (D1–D2); porta on the
  NUC, custos on the dev machine, Tailscale between (D3); USB first, firmware OTA deferred (D4); CLI → web → TUI (D5);
  shared wire code = a Go module under `protocol/`, never copied (D7); `dbg.py`/`mprof_resolve.py` rewritten in Go and
  deleted (F7). #297 = C walk via NATIVE vtable bindings. -O2 is the C6 default (#329).
- **Not proven:** #317, the C6 polled-reader hang, #327.
- **Awaiting David:** nothing. cabot relay 3(b) (three candidate issues) and 3(c) (generated MANIFEST block + physical
  units) are deferred: no cabot work until this focus completes (David, 2026-10-04).
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298–#300, #303, #304, #306–#309, #315–#328, #331–#335, #342; #233 (P3, the OTA backend — not the USB area, which is #310).
