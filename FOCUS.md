# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **FOCUS (David, 2026-10-04): local-first custos, all P1** — design `docs/design/2026-10-04-custos-peer-design.md` §6a.
  #340 merged (`50030da`) and closed. **Next session starts with:** #362 (a reliable agent path over `/debug`, before
  the TUI: David, 2026-10-06, rete is agent-first) on a branch from master — first action: read #362 and
  `docs/design/2026-10-06-custos-debug-page-design.md`, then brainstorm. Then #341.
- **Awaiting David:** publish the status page (Tailscale SSH check on gw85224-01 blocked it again, 2026-10-06);
  `review/tests/CLAIMS.md:86` still cites `mprof_resolve.py`; #358 (footprint RED: re-bank or shrink).
- **One thread only.** Parked: #297's approved plan; #272 Task 5 Step 5. Merge queue: `330-canonical-booleans`.
- **Bench:** `c6bench` in the soak — never flash it. `c6spare` runs the DebugProbeC6 hooks image, collector running; after
  a power cycle it holds at boot until sent `continue`. Suite 288/0; aux all PASS but footprint (#358).
- **Decided — do not re-litigate:** custos = nsl-specific peer, porta language-neutral (D1–D2); USB first (D4); porta and
  custos share only `protocol/` (#338); `.oimg` SHA-256 is the program identity; -O2 is the C6 default (#329); profiler in
  the std image (#312 Q1–Q2); #310 layout as built; a method is its blob offset (#323); the debug page holds its cable
  from start-up and reads `debug.State`, never the CLI's prose (#340 D1–D2).
- **Not proven:** #317, the C6 polled-reader hang, #327, #355. cabot deferred until this focus completes.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #342, #345–#361; #233.
