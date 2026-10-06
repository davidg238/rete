# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **FOCUS (David, 2026-10-04): local-first custos, all P1** — design `docs/design/2026-10-04-custos-peer-design.md` §6a.
  #310 built and green on `c6spare` (`310-oimg-flash-region` @ `34d46da`, pushed). **Next session starts with:** #340
  (web debug page, §6a item 6) on a branch from master once #310 is merged — first action: read #340 and §6a, brainstorm.
  Then #341.
- **Awaiting David:** merge `310-oimg-flash-region`; `review/tests/CLAIMS.md:86` still cites `mprof_resolve.py`;
  #358 (footprint ratchet RED before #310: re-bank or shrink).
- **One thread only.** Parked: #297's approved plan; #272 Task 5 Step 5. Merge queue: `330-canonical-booleans`, `310-oimg-flash-region`.
- **Bench:** `c6bench` in the soak — never flash it. `c6spare` runs the #310 firmware (simple boot), PortaBridgeNodeC6 in
  `program_partition`. This desktop builds Zephyr; suite 288/0, aux all PASS but footprint (#358).
- **Decided — do not re-litigate:** custos = nsl-specific peer, porta language-neutral (D1–D2); USB first (D4); porta and
  custos share only `protocol/` (#338); `.oimg` SHA-256 is the program identity; -O2 is the C6 default (#329); profiler in
  the std image (#312 Q1–Q2); #310: program at `0x320000` 512K, firmware links none, simple boot stays (Q1–Q2), `custos
  load` reports "loaded" not "running" (design §6a step 5, as built).
- **Not proven:** #317, the C6 polled-reader hang, #327, #355. cabot deferred until this focus completes.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#328, #331–#335, #342, #345–#358; #233.
