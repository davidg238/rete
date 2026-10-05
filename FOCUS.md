# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **FOCUS (David, 2026-10-04): local-first custos, all P1** — design `docs/design/2026-10-04-custos-peer-design.md` §6a.
  **#312 on `312-custos-profile`**, plan `docs/superpowers/plans/2026-10-05-custos-profile.md`: Tasks 1–4 done
  (`3c886c2`: `Kernel>>profiler`, cable + porta + debug loop, proven on c6spare). **Next session starts with:** Task 5 —
  `custos profile -port <tty> -ms N` on ONE port handle (opening the C6 port reboots it, #319), then Task 6 (page +
  delete `mprof_resolve.py`/`view.py`). Then #310, #340 (debug page only now), #341.
- **Awaiting David:** nothing.
- **One thread only.** Parked: #297's approved plan; #272 Task 5 Step 5. Merge queue: `330-canonical-booleans`.
- **Bench:** `c6bench` in the soak — never flash it. c6spare runs a std PortaBridgeNodeC6 with the profiler. This desktop
  builds Zephyr (source `~/zephyr-esp32/.venv` + `zephyr-env.sh`) and every aux gate passes (288/0, 2026-10-05).
- **Decided — do not re-litigate:** custos = nsl-specific peer, porta language-neutral (D1–D2); USB first (D4); CLI → web →
  TUI (D5); porta and custos share only `protocol/` via a `replace` per go.mod (#338); `.oimg` SHA-256 is the program
  identity; -O2 is the C6 default (#329, closed); one profile handler in every image, window closes on the kernel's
  Timers (#312 Q1, option A); profiler in the std image, C6 default, others earn it (Q2, #354); porta keeps commissioned
  images (D10). Record: `docs/design/2026-10-05-custos-profile-over-usb.md` § Answers.
- **Not proven:** #317, the C6 polled-reader hang, #327. cabot work deferred until this focus completes.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#328, #331–#335, #342, #345–#354; #233.
