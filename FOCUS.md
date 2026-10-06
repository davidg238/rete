# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **FOCUS (David, 2026-10-04): local-first custos, all P1** — design `docs/design/2026-10-04-custos-peer-design.md` §6a.
  **Next session starts with:** #323 on `323-debug-stack-locals` (from master `e46bdbd`) — execute
  `docs/superpowers/plans/2026-10-05-debugger-stack-and-names.md` **Native**, Task 1 (`vars` in the image map). Then #340, #341.
- **Awaiting David:** publish the status page (Tailscale SSH check on gw85224-01 blocked the scp, 2026-10-05);
  `review/tests/CLAIMS.md:86` still cites `mprof_resolve.py`; #358 (footprint ratchet RED before #310: re-bank or shrink).
- **One thread only.** Parked: #297's approved plan; #272 Task 5 Step 5. Merge queue: `330-canonical-booleans`.
- **Bench:** `c6bench` in the soak — never flash it. `c6spare` runs the #310 firmware (simple boot), PortaBridgeNodeC6 in
  `program_partition`. This desktop builds Zephyr; suite 288/0, aux all PASS but footprint (#358).
- **Decided — do not re-litigate:** custos = nsl-specific peer, porta language-neutral (D1–D2); USB first (D4); porta and
  custos share only `protocol/` (#338); `.oimg` SHA-256 is the program identity; -O2 is the C6 default (#329); profiler in
  the std image (#312 Q1–Q2); #310 layout as built (§6a step 5). #323 before #340; #323 = backtrace + names only (slots →
  #359); the blob offset is the only method identity, registry deleted, Path A debugging dropped (#323 design).
- **Not proven:** #317, the C6 polled-reader hang, #327, #355. cabot deferred until this focus completes.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328, #331–#335, #342, #345–#359; #233.
