# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **FOCUS (David, 2026-10-04): local-first custos, all P1** — design `docs/design/2026-10-04-custos-peer-design.md` §6a.
  **#312 on `312-custos-profile`:** Q1/Q3 decided, D10 recorded; plan `docs/superpowers/plans/2026-10-05-custos-profile.md`
  Tasks 1–3 done (`a2c16e7`: imagemap, profile, offline `custos profile`). **Next session starts with:** David confirms the
  plan's proposed node-side shape (kernel owns the `dbg/req` handler; the cable line becomes a `dbg/req` write), then Task 4.
- **Awaiting David:** (1) the node-side shape above; (2) Q2 — std+debug vs std+debug/profile, on the numbers in the
  spec's Answers (+6.2 kB sram0 built in; open window +1.88 % CPU); (3) #329 — is ot_radio_workq 156 B free of 512 enough
  headroom at -O2 (all 14 scenarios pass), then close.
- **2026-10-05 bench:** `custos debug -port` first board run PASS on c6spare (#339 comment; #352 boot-stop P3). #353 P3:
  the synthetic wedge skips forensics.
- **One thread only.** Parked: #297's approved plan; #272 Task 5 Step 5. Merge queue: `330-canonical-booleans`.
- **Bench:** `c6bench` in the soak — never flash it. This desktop now builds Zephyr (west via `~/zephyr-esp32/.venv`, see
  memory) and the suite's aux gates all PASS here (2026-10-05, 288/0).
- **Decided — do not re-litigate:** custos = nsl-specific peer, porta language-neutral (D1–D2); USB first (D4); CLI → web →
  TUI (D5); porta and custos share only `protocol/` via a `replace` per go.mod (#338); `.oimg` SHA-256 is the program
  identity; -O2 is the C6 default (#329); one command reader in every image (#312 Q1); porta keeps commissioned images (D10).
- **Not proven:** #317, the C6 polled-reader hang, #327. cabot work deferred until this focus completes.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#328, #331–#335, #342, #345–#353; #233.
