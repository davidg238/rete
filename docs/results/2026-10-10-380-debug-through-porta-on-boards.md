# A debugger through porta, on two boards — rete#379 and rete#380, 2026-10-09/10

*The dated record behind paper E §9. The host spike it builds on is
`docs/results/2026-10-07-342-sleepy-node-debug-spike.md`; the mechanism is peer design D9 and D19 and the
`Debug{sha, breaks}` cell of rete#375. Console and porta logs are quoted on the two issues.*

## What was done

A breakpoint set at the gateway (`porta put --raw <node> goal/debug/main <Debug{sha, breaks}>`), with no cable
on the board, stopped a program on each of the two bell nodes; each was stepped and inspected through porta as
`dbg/req` rows, then released by writing the empty set. Both nodes run the debug image (hooks and the method
profiler compiled in) that is their shipped image.

| | MagTag, ESP32-S2, WiFi, deep sleep (rete#379) | wall C6, ESP32-C6, Thread, always on (rete#380) |
|---|---|---|
| program | `MagTagSleepyDebugNode`, 28 KB arena, 30 s sleeps (2026-10-09 17:12 MST) | `ShipsBellNodeC6`, 128 KB arena, 30 s check-ins (2026-10-10 08:04 MST) |
| how the break arrived | the cell is replayed into every cold boot's first check-in; `work:` stopped 1 s after boot | the cell rode the next check-in (armed 17 s after the write); the break hit at the check-in after, 56 s after the write |
| `backtrace` / `inspect` / `step`, porta put → node → porta store | 755 / 636 / 754 ms | 239 / 233 / 230 ms |
| held | 26 s; 186 collections, live 23 KB of 28; `obs/debug` held true | 1.2 s; `obs/debug` `blob 5208 held true` |
| release | `continue`; the next boot pulled the empty set and ran free (`held 0`) | the empty set → `dbg:ok clear` in 479 ms; `obs/debug` held false; the next check-in `misses 0` |
| frames reported | — | `checkIn@0 ← block (joinPorta) ← Kernel_Space>>drain ← main:args: ← Supervisor>>run:…`, resolved by the image map |

## What the numbers say

- **The command round trip is the node's held-exchange cadence, not the radio.** The C6 answers in about 230 ms
  per command and the MagTag in about 700 ms, against 3–4 ms on the host loopback; a held node polls porta at its
  bridge's held cadence, and the radio's own round trip is well inside that.
- **The wait for the stop is the check-in cadence**, as the host spike said: one or two cadences on an always-on
  node (the cell arrives on one check-in and the breakpoint's method runs on the next), the first check-in of
  the next wake on a deep-sleep node.
- **A breakpoint is placed by what runs.** The wall bell's `onTick` runs only at the alarm, the countdown and the
  watchdog, so a break there would have held the bell at 07:59:55 and delayed the ring; the break went on
  `checkIn`, which runs every 30 s. The image map says where a method is; the program says when.
- **Costs that remain.** The MagTag's arena sits at its ceiling while held (186 collections in 26 s). The C6's
  hold was short; a long hold on it costs what the host spike measured, a busy node and a growing store, at a
  rate ~1/230 ms rather than ~1,600/s.

## Record

- rete#379 (MagTag run, 2026-10-10T00:14 UTC comment), rete#380 (wall C6 flash and session, 2026-10-10T15:00 and
  15:05 UTC comments); paper E §9 and Appendix B carry the numbers.
- The three defects the MagTag run found and fixed (a WiFi node's reply socket #390, the VM's thread priority
  against the WiFi driver, boot id 0 on a cold boot) are on rete#379.
