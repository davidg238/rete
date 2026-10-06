# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **FOCUS (David, 2026-10-04): local-first custos, all P1** — design `docs/design/2026-10-04-custos-peer-design.md` §6a.
  #323 merged (`3a5a11b`) and closed. **Next session starts with:** #340 (the web debug page, §6a item 6) on a branch from
  master — first action: read #340 and §6a, then brainstorm. Then #341.
- **Awaiting David:** publish the status page (Tailscale SSH check on gw85224-01 blocked the scp again, 2026-10-06);
  `review/tests/CLAIMS.md:86` still cites `mprof_resolve.py`; #358 (footprint RED: re-bank or shrink; #323 added ~315 B).
- **One thread only.** Parked: #297's approved plan; #272 Task 5 Step 5. Merge queue: `330-canonical-booleans`.
- **Bench:** `c6bench` in the soak — never flash it. `c6spare` runs the DebugProbeC6 hooks image (#323), collector running.
  This desktop builds Zephyr (activate `~/zephyr-esp32/.venv`); suite 288/0, aux all PASS but footprint (#358).
- **Decided — do not re-litigate:** custos = nsl-specific peer, porta language-neutral (D1–D2); USB first (D4); porta and
  custos share only `protocol/` (#338); `.oimg` SHA-256 is the program identity; -O2 is the C6 default (#329); profiler in
  the std image (#312 Q1–Q2); #310 layout as built (§6a step 5); a method is its blob offset, registry deleted, Path A
  debugging dropped (#323 design); slot expansion is #359.
- **Not proven:** #317, the C6 polled-reader hang, #327, #355. cabot deferred until this focus completes.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #342, #345–#361; #233.
