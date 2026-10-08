# Debugging and profiling a sleepy node through porta — host spike for rete#342

**Question (David, 2026-10-07; peer design D9):** what does it take to debug and profile a node that
sleeps, with no cable, through porta?

**Answer: on the desktop it works end to end through a real `porta serve`, for both kinds of sleep.** It took
about 110 lines of kernel and Supervisor nsl (`git diff --stat`: +132 −25 with comments), no VM change and no porta change. On a board, five things stand between this and D9's
demo; two of them are decisions for David. Gate: `nodus/oevm/tests/sleepy_debug_host.sh` (`SLEEPY DEBUG OK`).

## Two kinds of sleep, and what each needs

| | VM stays alive (long check-in cadence; a Thread sleepy end device) | VM dies (deep sleep: every wake is a cold boot) |
|---|---|---|
| Where the breakpoint lives | the VM's breakpoint table, set by `dbg:break` lines on `dbg/req` | the `goal/debug/main` **cell**: porta replays cells to a fresh boot's cursor 0, and replays no `dbg/req` row (#262) |
| When it is applied | at the next check-in | inside the first check-in of every wake, before the program's work |
| Proven by | gate leg A | gate leg B (killing the process is the simulated sleep; wake 2 can only have been re-armed by the cell) |

## What was built

| Piece | Where | What it does |
|---|---|---|
| A line link on the debugger | `nodus/nsl/lib/supervisor.nsl` `Debugger` (`useLink:`, `readLine`, `say:`, `wake`) | Every reply goes through `say:` and every command through `readLine`. With no link they are the cable, as before. Replies are still echoed to the console. `wake` is the pause a line on the cable already causes (`requestStep: 1`). |
| `Kernel>>debugger` | `nodus/nsl/lib/kernel.nsl` | The Supervisor registers its Debugger before `main:` (set once). The mirror stays in the Supervisor. |
| `DebugLink` | `kernel.nsl` | Lines in, from `dbg/req` rows and the `goal/debug/main` cell. Replies out, as `dbg/resp` rows offered straight to the bridge's pending queue. Its `readLine` runs **held exchanges** while it has no line. |
| `PortaBridge>>debugOverSpace`, `heldExchange` | `kernel.nsl` | Opt-in: hands the debugger the link. The program is stopped, so no reaction runs and no drain happens. The bridge therefore takes `dbg/req` and `goal/debug/main` in `applyEntry:`, and wakes the debugger at the end of `checkIn`, after the exchange, so held exchanges never nest inside one. A held exchange is one pull and one push. An `exchanging` flag refuses to start one inside another (a stop inside kernel exchange code reads as EOF, which resumes). |
| `SleepyDebugNode.nsl` | `nodus/oevm/zephyr/` | Host node, 10 s cadence. `work:` is where the breakpoint goes. |

## Measurements (host, loopback, 2026-10-07)

| | |
|---|---|
| Breakpoint written → stop reported in porta's store | 9.9 s: one cadence. The breakpoint waits at porta for the node's next check-in. |
| One debug command while held (`porta put` → node → porta store) | 3–4 ms (`backtrace`, `inspect`, `step`) |
| Held exchanges | ~1,600/s (63,666 in a ~39 s hold; manual run) |
| Node CPU while held | 99.9% (the debugger polls; it does not sleep) |
| porta's store during a 39 s hold | WAL grew ~4 MB (each pull is a write) |
| After `dbg:continue` from a 39 s hold | three ticks fired back to back (the cadence timer catching up) |
| porta's liveness verdict while held | `#neverSeen`: this node publishes no `obs/health`, and porta has no "held" verdict |

**Falsified.** Breaking the `dbg/req` intercept turns leg A red (`no 'dbg:paused break 8448 0' on dbg/resp`).
Breaking the `goal/debug/main` intercept turns leg B red (`wake 1 did not stop in work:`). Leg C is the control:
the same rows sent to a standard image get no answer on `dbg/resp`.

## What a board needs, in order

1. **A sleepy node that talks to porta. None exists today.** The MagTag deep-sleeps but has no porta bridge.
   Every bridged node (`PortaBridgeNode*`, `ShipsBell*C6`) is always on. The battery ShipsBells (v3 design
   §9, MTD) is designed and not built. Which board this is, and which kind of sleep, is David's call.
2. **The boot stop waits for a cable.** A hooks-enabled Zephyr image is the debug image, and its Supervisor
   blocks in `platform_debug_readline` (`rl_getc`, `platform_zephyr.c`) for the first line. With no cable
   attached it never boots. A debug image that will be debugged through the space must not take the boot stop.
   This was read from the code, not run.
3. **A stop held through the space reads as a wedge.** `debug_held` is set only inside the cable read
   (`platform_zephyr.c:178`). A stop served by the link never calls that read, so after `appCad` the observer
   scores `APP_STALE` (`observer_core.h` §3.2) and the node reboots. The fix is one mechanism: bracket the
   whole command loop with the held state, whichever link serves it. That needs a primitive, which is a VM change.
4. **Holding costs a busy CPU.** `awaitReply:` spins on a non-blocking `receive`, and the held loop exchanges
   back to back. On Thread each exchange costs a radio round trip, so the rate falls, but the CPU and the radio
   stay up the whole time. The fix needs no tunable: porta long-polls a held node's pull, answering when a
   `dbg/req` row arrives or at a bound, and the node blocks in its receive. D9's "measure the battery cost of a
   hold before inventing a timeout" is measured on a board, against this.
5. **Profile on a cold-boot node** (below).

## Profile

- **VM alive:** works today. The request is a `dbg/req` row (`profile <ms>`), the window is timed by the
  kernel's Timers, so it closes while the reactor sleeps, and the rows leave by the console (profile design
  D4). In a debug image with the link, `profile` lines go to the kernel's Profiler and never to the debugger,
  so a profile request never holds the node (gate leg D).
- **Cold boot:** three gaps, none built. The request row is not replayed to a fresh boot (#262). The
  manifest already has a `goal/profile/<app>` cell (`Profile{action, durationS, continuous}`) that nothing
  implements. An open window dies with the VM at `deepSleep:`, because the rows print only when it closes. A
  kernel `deepSleep:` should close an open window first (one line). The rows also need the console tee
  (#56) to flush before the radio sleeps.

## Decisions for David

| # | Question | The spike did | Proposed |
|---|---|---|---|
| a | The shape of `goal/debug/<app>`. The manifest's `Debug{action: #attach/#detach}` carries no breakpoints. | a List of command lines, porta without `--manifest` for leg B | **Replace** `Debug{action}` with the breakpoints plus the program's sha: a breakpoint is a blob offset, which means nothing in another image. Presence of the cell is the attach. |
| b | D9 says the stop is reported under `obs/**`; PROTOCOL-NSL says `dbg/resp`. | `dbg/resp` lines | Keep the lines on `dbg/resp`, and add one `obs/debug` cell (held, and where) so porta and the monitor can show a held node when custos is not running ("who is told"). |
| c | `debugOverSpace` is opt-in per app. | opt-in | Automatic for every bridged debug image ("the target picks the transport"), once the boot stop (2) is fixed. |
| d | porta's liveness verdict for a held node | none | A held node is neither live nor dead. Its `obs/debug` cell (b) is what porta would read. |

**Decided (David, 2026-10-08): peer design D19.** a: yes (#375). b: yes, add `obs/debug` (#375). c: USB or porta, no opt-in (#376). d: via `obs/debug` (#375). Board gaps: 1 → #379 MagTag and #380 C6; 2 must be fixed (#376); 3 make the VM change (#377); 4 the spin is bad, the long-poll proposal is not accepted, and what an abandoned session does is TBD (#378); 5 after debug (#381).

Recorded on #342. This document is the spike's record. Durable decisions go to peer design §3 (D9).
