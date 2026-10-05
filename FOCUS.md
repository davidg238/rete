# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **FOCUS (David, 2026-10-04): local-first custos, all P1** — design `docs/design/2026-10-04-custos-peer-design.md` §6a.
  **#312 built** on `312-custos-profile` (plan `docs/superpowers/plans/2026-10-05-custos-profile.md`, Tasks 1–6;
  `475d578` live capture, `b5c6c0d` page + Python deleted). **Next session starts with:** #310 (the nsl program in its
  own flash region, over the cable) on a new branch — read #310 and §6a step 2, then plan. Then #340 (debug page), #341.
- **Awaiting David:** merge `312-custos-profile` and close #312 (ladder entry in `review/program-sequence.md` is yours;
  `review/tests/CLAIMS.md:86` still cites `mprof_resolve.py`).
- **One thread only.** Parked: #297's approved plan; #272 Task 5 Step 5. Merge queue: `330-canonical-booleans`, `312-custos-profile`.
- **Bench:** `c6bench` in the soak — never flash it. c6spare runs a std PortaBridgeNodeC6 with the profiler. This desktop
  builds Zephyr (source `~/zephyr-esp32/.venv` + `zephyr-env.sh`) and every aux gate passes (288/0, 2026-10-05).
- **Decided — do not re-litigate:** custos = nsl-specific peer, porta language-neutral (D1–D2); USB first (D4); CLI → web →
  TUI (D5); porta and custos share only `protocol/` via a `replace` per go.mod (#338); `.oimg` SHA-256 is the program
  identity; -O2 is the C6 default (#329); one profile handler in every image (#312 Q1); profiler in the std image, C6
  default (Q2, #354); porta keeps commissioned images (D10); page capture is cable-only, porta-triggered capture is a
  recorded regression. Record: `docs/design/2026-10-05-custos-profile-over-usb.md` § Answers.
- **Not proven:** #317, the C6 polled-reader hang, #327, #355 (does a port open reboot a C6?). cabot deferred until this focus completes.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#328, #331–#335, #342, #345–#355; #233.
