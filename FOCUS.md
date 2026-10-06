# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **FOCUS (David, 2026-10-06): agent first, the focus of the platform** (peer design D11). The next session starts
  with #364 (profile's agent path: `--profile-host`, real-VM gate, answer shape as #362) on a branch from master.
  First action: read #364, #362's D3–D4 and `custos/internal/custos/profile.go`, then brainstorm. #341 (TUI) waits.
- **Awaiting David:** publish the status page (Tailscale SSH check on gw85224-01 blocked it again, 2026-10-06 08:17);
  `review/tests/CLAIMS.md:86` still cites `mprof_resolve.py`; #358 (footprint RED: re-bank or shrink).
- **One thread only.** Parked: #297's approved plan; #272 Task 5 Step 5. Merge queue: `330-canonical-booleans`.
- **Bench:** `c6bench` in the soak — never flash it. `c6spare` runs the std PortaBridgeNodeC6 image (#364, 2026-10-06),
  collector running. Suite 288/0; aux all PASS but footprint (#358).
- **Decided — do not re-litigate:** custos = nsl-specific peer, porta language-neutral (D1–D2); USB first (D4); porta and
  custos share only `protocol/` (#338); `.oimg` SHA-256 is the program identity; -O2 is the C6 default (#329); profiler in
  the std image (#312 Q1–Q2); #310 layout as built; a method is its blob offset (#323); the debug page holds its cable
  from start-up and reads `debug.State` (#340 D1–D2); `--debug-host` is the second cable, the piped CLI keeps the pause
  rule and agents long-poll `Seq` (#362 D3–D4); agent first, every verb's agent path before a human front (D11); HTTP is the only agent surface, CLIs demoted (D12, #365).
- **Not proven:** #317, the C6 polled-reader hang, #327, #355. cabot deferred until this focus completes.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #342, #345–#361, #363; #233.
