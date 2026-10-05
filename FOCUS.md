# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **FOCUS (David, 2026-10-04): local-first custos, all P1** — design `docs/design/2026-10-04-custos-peer-design.md`
  §6a; tracker #26 top block. **#338 and #339 merged and closed** (master `d91357a`).
  **Next session starts with (David, 2026-10-05), in order:** (1) David answers #312's three questions
  (`docs/design/2026-10-05-custos-profile-over-usb.md`, branch `312-custos-profile`); (2) on `c6spare`: a first board run
  of `custos debug -port`, then #329's five radio scenarios at -O2. Then build `custos profile`; then #310, #340, #341.
- **One thread only.** Parked: #297's approved plan; #272 Task 5 Step 5. Merge queue: `330-canonical-booleans`.
- **Bench:** `c6bench` in the soak — never flash it. This desktop has no cmake/ARM toolchain/32-bit glibc (portability-32
  FAIL, cmake-parse/footprint SKIP are environment); run the suite under a venv with `pytest` + `pexpect`.
- **Decided — do not re-litigate:** custos = nsl-specific peer, porta language-neutral (D1–D2); USB first (D4);
  CLI → web → TUI (D5); porta and custos share only the `protocol/` Go module, via a `replace` per go.mod, no `go.work`
  (§6a addendum, #338); the store schema is a contract, fixtures in `protocol/fixtures/store/`. `.oimg` SHA-256 is the
  program identity (`protocol/IMAGE-ARTIFACTS.md` §6). -O2 is the C6 default (#329).
- **Not proven:** #317, the C6 polled-reader hang, #327.
- **Awaiting David:** #312's three questions. cabot work deferred until this focus completes.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#328, #331–#335, #342; #233.
