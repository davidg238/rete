# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **FOCUS (David, 2026-10-06): agent first** (peer design D11–D12). #365 (`51af498`) and #341 (`fe33e34`, the TUI) merged
  and closed. **Next session starts with:** #367 (`custos load`'s agent path over HTTP) on a branch from master. First
  action: read `custos/internal/load` and how #362/#364 built theirs (endpoint, host target, real-VM gate).
- **Awaiting David:** `review/tests/CLAIMS.md:86` still cites `mprof_resolve.py`; #358 (footprint RED: re-bank or shrink); the #341 ladder entry.
- **One thread only.** Parked: #297's approved plan; #272 Task 5 Step 5. Merge queue: `330-canonical-booleans`.
- **Bench:** `c6bench` in the soak — never flash it. `c6spare` runs the std PortaBridgeNodeC6 image, collector running.
  `opti7040` (Omarchy, tailnet) has an RP2350 and its own Claude in a rete clone. Suite 288/0; aux all PASS but footprint (#358).
- **Decided — do not re-litigate:** custos = nsl-specific peer, porta language-neutral (D1–D2); USB first (D4); porta and
  custos share only `protocol/` (#338); `.oimg` SHA-256 is the program identity; -O2 is the C6 default (#329); profiler in
  the std image (#312); a method is its blob offset (#323); agent first (D11); HTTP is the only agent surface, CLIs kept
  but unadvertised (D12, #365); the CLI is shelved, HTTP is #314's contract (David, 2026-10-06); custos owns the cable and every front, the TUI included, is its HTTP client (TUI design D2, #341).
- **Not proven:** #317, the C6 polled-reader hang, #327, #355. cabot deferred until this focus completes.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #342, #345–#361, #363, #366; #233.
