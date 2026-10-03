# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **Next session starts with: #311, the last queued item** (David, 2026-10-02), branch `311-line-in-spike` (master
  merged in at `516e8bd`; do not rebase, it is pushed). First action: a person's front end — file:line →
  `dbg:break <id> <off>` through the `.stmap` and the registry, and a page in `docs/` for a human. Then merge; closing
  #311 is David's call. Record: `docs/results/2026-10-02-311-line-in-spike.md` parts 2–3.
- **Proven 2026-10-02 (#311):** `dbg:pause` on a C6 — a line in while running stops it at its next bytecode (17/17,
  `dbg_session.py session`); one command loop, the boot wait is the first pause. `CONFIG_UART_INTERRUPT_DRIVEN` is in
  `prj_esp32c6.conf` (the bell gets it at its next flash — daylight).
- **Decided 2026-10-02 — do not re-litigate:** a hooks image IS the debug image and waits at boot, no timeout. The pause
  rule is the one sentence above (design addendum, in-image-debugger doc). **No SLIP.** 0.1 = one C6 on one cable.
- **Answered:** #319 — a pyserial open of the CP2102N reboots the C6, ten of ten today (DTR/RTS deasserted). **Not
  proven:** why an unpowered DS3231 blocks the bell's boot (#317); why a polled reader hangs the C6.
- **Lesson (memory, instrument-failure):** a late `dbg:ok pause` cost six board builds and was the driver's line reader.
- **Bench (2026-10-02 20:40 MST):** no C6 on the desktop. `c6bench` on the gateway's USB running the #311 debug image
  (`~/rete-bench/c6bench-311-pause-dbg.bin`; rollback `c6bench-118.bin`; collector `restart-collector-c6bench.sh`).
  `c6spare` unplugged; the bell on its wall supply; both dongles running the bridge.
- **Awaiting David:** close #311 after the front end?; close #319?; `review/DOCMAP.md` line 56; the three cabot relay
  items; where #297 goes; #300.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #296–#300, #303, #304, #306–#309, #315–#320. Liveness next: #272.
