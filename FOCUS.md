# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **FOCUS (David, 2026-10-04): local-first custos, all P1** — design `docs/design/2026-10-04-custos-peer-design.md` §6a.
  #312 merged (`f544a7a`) and closed. **Next session starts with:** #310 on `310-oimg-flash-region` — execute
  `docs/superpowers/plans/2026-10-05-program-partition.md` **Native** (David, 2026-10-05), Task 1 (the ota seam) first.
  Then #340 (debug page), #341.
- **Awaiting David:** #312's ladder entry in `review/program-sequence.md`; `review/tests/CLAIMS.md:86` still cites `mprof_resolve.py`.
- **One thread only.** Parked: #297's approved plan; #272 Task 5 Step 5. Merge queue: `330-canonical-booleans`.
- **Bench:** `c6bench` in the soak — never flash it. `c6spare` runs #310 spike-3 firmware (MCUboot + program B) until
  Task 2 reflashes it to simple boot. This desktop builds Zephyr and every aux gate passes (288/0, 2026-10-05).
- **Decided — do not re-litigate:** custos = nsl-specific peer, porta language-neutral (D1–D2); USB first (D4); porta and
  custos share only `protocol/` (#338); `.oimg` SHA-256 is the program identity; -O2 is the C6 default (#329); profiler in
  the std image (#312 Q1–Q2); #310: program in its own partition `0x320000` 512K, firmware links none, simple boot stays,
  offset cited from `IMAGE-ARTIFACTS.md` §2a (Q1), the program-receives-program problem is #233's (Q2) — design §6a step 5.
- **Not proven:** #317, the C6 polled-reader hang, #327, #355 (does a port open reboot a C6?). cabot deferred until this focus completes.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#328, #331–#335, #342, #345–#356; #233.
