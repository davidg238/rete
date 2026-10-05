# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **FOCUS (David, 2026-10-04): local-first custos, all P1** — design `docs/design/2026-10-04-custos-peer-design.md`
  §6a; tracker #26 top block. **#337 and #343 done** (master `4135e59`). **Next session starts with the merge queue**
  on master, on David's word: `329-c6-speed`, then `198-custos-views` (its `FOCUS.md`/status page conflict with
  master's — take master's). Then #338 on a branch off master, #339, #312, #310, #340, #341. #344 follows #330.
- **One thread only.** Parked: #297's approved plan; #272 Task 5 Step 5. Merge queue: `329-c6-speed`, then
  `198-custos-views`, then `330-canonical-booleans`.
- **Bench:** `c6bench` in the soak — never flash it. `c6spare` + RP2350-Zero on this desktop's motherboard ports.
  This desktop lacks system pytest, 32-bit glibc, cmake, ARM toolchain: run the suite under a venv with `pytest` + `pexpect`; portability-32
  FAIL and cmake-parse/footprint SKIP are environment.
- **Decided — do not re-litigate:** custos = nsl-specific peer, porta language-neutral (D1–D2); USB first (D4);
  CLI → web → TUI (D5); shared wire code = Go module under `protocol/` (D7); `dbg.py`/`mprof_resolve.py` rewritten
  in Go and deleted (F7). One image map, the `.oimg` SHA-256 is the program identity, C6-only checked-in fixture
  (#337 decisions 1–5, `protocol/IMAGE-ARTIFACTS.md` §6). -O2 is the C6 default (#329).
- **Not proven:** #317, the C6 polled-reader hang, #327.
- **Awaiting David:** merge `329-c6-speed` now, or after its five radio scenarios at -O2 on `c6spare`? cabot work deferred until this focus completes.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#328, #331–#335, #342; #233.
