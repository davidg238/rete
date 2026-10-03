# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **Next session starts with: #311, the remainder of the debug work** (David, 2026-10-02), branch `311-line-in-spike`
  (master merged in at `516e8bd`; do not rebase, it is pushed). First action: `dbg:pause` — stop a RUNNING program from
  the cable (today breakpoints are set only at boot or while paused; `supervisor.nsl` `initialLoop`/`commandLoop`).
  Then fold `CONFIG_UART_INTERRUPT_DRIVEN` into `prj_esp32c6.conf` (daylight: the bell reflashes too), then a file:line
  front end through the `.stmap`. The queue is the last comment on #311; the record is
  `docs/results/2026-10-02-311-line-in-spike.md` part 2.
- **Proven 2026-10-02 (#311):** debugger on a C6 over one cable; held 21 s against an 8 s deadline, no reset; hooks cost
  11.6% + 494 B flash. Enable follows the build (`platform_debug_enabled`); observer takes `held` (§3.2).
- **Decided 2026-10-02 — do not re-litigate:** a hooks image IS the debug image and waits at boot for its cable, no
  timeout (record part 2, "what this does not cover"). Bench-named spaces are historical. `7adf` stays on the July soak.
  **No SLIP** (#311 record). 0.x first, 0.1 = one C6 on one cable, freeze is 1.0.0 (`docs/overview.md`, #253).
- **Not proven:** #319 (does a second port open reset a C6?); why an unpowered DS3231 blocks the bell's boot (#317); why a
  polled reader hangs the C6; no nRF on the bench has raw top two bits clear.
- **Bench (2026-10-02 19:20 MST):** no C6 on the desktop. `c6bench` on the gateway's USB, back on `c6bench-118.bin`
  (rollback bins and `esptool-venv` live on the gateway; its collector is `~/rete-bench/restart-collector-c6bench.sh`).
  `c6spare` unplugged; the bell on its wall supply; both dongles running the bridge.
- **Awaiting David:** close #311 once the three items are placed?; `review/DOCMAP.md` line 56; the three cabot relay
  items; where #297 goes; #300.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #296–#300, #303, #304, #306–#309, #315–#320. Liveness next: #272.
