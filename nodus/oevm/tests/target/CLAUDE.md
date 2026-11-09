# nodus/oevm/tests/target — board scenarios

Loaded when working under `nodus/oevm/tests/target/`. Moved verbatim from the root `CLAUDE.md` (2026-09-30).

**What `tests/target/run.sh` actually runs — fifteen scenarios, and a full pass is ~70–100 min
of board time** (`liveness-heartbeat` alone budgets a 300 s timeout plus 180 s of observation;
`cad-rearm` was added 2026-08-26 for #240.2, `porta-refuse` the same day for #232,
`porta-silent` on 2026-08-27 for #269, `liveness-noradio` on 2026-08-29 for #274, and
`liveness-fastpoll` + `liveness-runonce` on 2026-09-17 for #273 — and
**both porta-* scenarios need their injector porta started by hand**, so a full sweep is not
unattended; see their envs). The runner globs
`*.env`, so a new manifest joins the sweep the moment it lands. Budget the time rather than
discovering it. `CLASS=disruptive` means the scenario
deliberately wedges, burns or reboots the node: **a disposable board only** (`c6spare`, which
is `TARGET_DEFAULT_BOARD` — resolve boards through the collector env files, never a table).

**A scenario whose node opens a socket needs `CONF_EXTRA` naming the radio, and only two nodes
do.** `prj.conf` alone has `NETWORKING/IPV6/UDP/SOCKETS` but **no L2**, so a C6 built without
`prj_esp32c6_thread.conf` has no interface and `net udpBindPort:` answers nil. The wedge probes
never notice — `WedgeProbeC6`, `SlowTimerNodeC6`, `PulseCoalesceProbeC6` and `StaleHandleProbeC6`
arm `timers every:` directly and open nothing. `PortaBridgeNodeC6` and `TimerlessNodeC6` bind. **The bind is
no longer load-bearing for `TimerlessNodeC6`** (#274, 2026-08-29): the bridge is now constructed
with `socket: nil` and `startTickingOn:` runs unconditionally, so a failed bind costs the node its
reporting and not its base tick, its wedge or its deadline. `liveness-noradio` is the gate that
holds that — the same node with no `CONF_EXTRA` at all, verified on a board 2026-08-29
(`deadline=16000ms`, 18.03 s of silence, `pass=2 fail=0`). What a radio still buys is EVIDENCE:
`liveness-heartbeat` keeps its `CONF_EXTRA` because its claim is that nothing speaks between the
wedge and the reboot, and on a node that cannot check in at all that claim is vacuous. The "no
`CONF_EXTRA`" convention is about `wedge-note`'s synthetic-trigger Kconfig split and **was never
about the radio** — conflating the two cost a board cycle on 2026-08-22 (#236, and
`zephyr/run.sh`'s #165 comment records the same shape from before).

| scenario | class | what it gates |
|---|---|---|
| `bridge-health` | observe | the node half of the porta gate — obs acked, `goal/name` converged, nothing evicted (#streamDropped 0) |
| `cad-rearm` | observe | an adopted cadence is a KEPT cadence (#240.2) — the re-arm from inside the old tick's own reaction kills the old registration over the real pulse mailbox, keeps ONE registration, and holds the new period. No radio: `PortaBridge socket:nil` |
| `bridge-forensics` | observe | the same workload under the forensics overlay (#222) |
| `porta-refuse` | disruptive | **a refused node survives past the 157 s death record** (#232, gating #199) — a C6 against a second porta instance answering `#err` to everything (`--test-refuse-all`, gw85224-01:16971, started by hand per the env), watched 240 s past the TAPs. `ref` climbing is the PASS; silence is the FAIL. The host cannot answer this: it never reproduced the death |
| `porta-silent` | disruptive | **a node whose UPLINK is gone survives it** (#269) — porta-refuse's mirror, and the gate porta-refuse could not be. A C6 against a second porta instance that DROPS every datagram and answers nothing (`--test-drop-all`, gw85224-01:16973, started by hand per the env — and it must be `./porta-269`, since the long-running `./porta` there predates the flag), watched 300 s past the TAPs. **`misses` climbing with the node ALIVE is the PASS**; the node going quiet is the FAIL — and TAP 1 is a conjunction (`misses >= 30` AND `ref = 0`), so a run that reaches #232's refusing injector by mistake fails loudly instead of scoring. **This gate caught something it was not built for**: it reproduced a node wedging at 111.7 s under a silent uplink, and the node was HEALTHY — `observer.c` judged the pulse mailbox's depth at a sampling instant, which aliases against a 4.007 s tick sampled every 1001 ms, so the detector was rebooting a node that was still draining and still printing ticks. Fixed by judging PROGRESS (`pulse_drained()`) not depth; GREEN since, 357 s under the condition that had killed it, with `wedge-detect` still firing. The archive was then swept (#270, 2026-08-27) and **the false trips are confined to that one session**: of the 58 archived `deadline=16000ms` events, 49 are REAL wedges on 08-19/20 — boot, ticks 1-6, then 18.2 s of silence against a 2 s cadence, and 35 of those also read `reactor=NOT-ADVANCING` with vm_sp frozen, which is the #222 prim-cache spin and does not depend on depth at all — 2 are `liveness-heartbeat`'s deliberate ones (#231 D1), and 7 are this session's. No attribution was overturned |
| `pulse-coalesce` | disruptive | the coalescer does not destroy logical timer events (#151) |
| `stale-handle` | disruptive | a timer event survives a later unrelated registration (#151) |
| `wedge-note` | disruptive | the `RebootNote` survives a watchdog reset (#215, row 1) |
| `wedge-detect` | disruptive | the detector fires on a real wedge, dumps the condition, reboots, reports `#watchdog` (#215, row 3). **Since #273 (re-verified 2026-09-17): the probe has no bridge, so it declares `appLiveness: 8000` itself and the dump reads `deadline=8000ms appCad=8000ms cond=app-stale`** |
| `wedge-quiet` | disruptive | **no false trip on a legitimately slow node** — one 10-minute timer, and the 600 s observation is why this run is long (#215, row 5) |
| `liveness-heartbeat` | disruptive | **the detector can see a node with NO app timer** — the kernel owns the base tick, so the deadline exists and nothing speaks between the wedge and the reboot (#231 D1). Needs the Thread `CONF_EXTRA`: its node binds. **GREEN ON A BOARD 2026-08-22** — `deadline=16000ms` (the kernel's 2000 ms tick × slack 8, the only timer in the image), 18.03 s of total console silence between the wedge and the reset, `pass=2 fail=0` on boot 2. **Re-verified 2026-09-17 under #273, unchanged: `deadline=16000ms appCad=16000ms`** — the bridge's declared default, no longer an inference from the shortest timer |
| `liveness-noradio` | disruptive | **#274's regression gate: a deadline exists even when the bind fails.** The same node as above with NO `CONF_EXTRA` — the exact condition that gave 300 s of total silence on 2026-08-22. GREEN ON A BOARD 2026-08-29: bind fails, `timerless tick` climbs at 2 s (there were none before the fix), wedge at tick 15, `deadline=16000ms` from the kernel base tick alone, 18.03 s of silence, reboot, `pass=2 fail=0`, then 106 ticks with no second wedge. The red it is shaped to catch is SILENCE, which is what made the defect invisible. **Re-verified 2026-09-17 under #273: `deadline=16000ms appCad=16000ms cond=app-stale`** — the number is unchanged, it is now the bridge's declared default (cad × 8) rather than an inference |
| `liveness-fastpoll` | disruptive | **#273 D1's reproduce-first gate: a 100 ms timer beside a 3 s blocking reaction every 4 s.** RED on the pre-#273 build (`deadline=800ms cond=pulse-undrained`, a false reboot 2 s into the first slow tick — archived 2026-09-17 `…20260917T181643`), GREEN the same day with the declared `appCad 6000`: 50 slow ticks, zero WEDGE lines. A 1.5 s block did NOT reproduce the red — the old rule sampled at 1 s and needed two in-block samples — so the block is 3 s |
| `liveness-runonce` | disruptive | **a node with NO repeating timer is judged at its declared number** (#273 D1). `deadline=4000ms appCad=4000ms pending=0` from the declaration alone (GREEN 2026-09-17); the vacuous outcome on the old rule is TIMEOUT, so this row asserts a deadline was FORMED |
| `timer-table-full` | disruptive | **a full timer table raises in the arming job and the node lives** (#81) — leg A in `main` under protectedFire (`armed=7` of 8: the probe's beat holds one, an unbridged kernel none), leg B the same raise inside a reaction, caught by drain and posted to `sys/crash` while the beat keeps coming, leg C a freed slot re-arms and fires. No radio. The host suite never runs `timer_zephyr.c`, whose full table was a k_oops before #81. GREEN ON A BOARD 2026-11-09 (master `1001a48a`, `pass=3 fail=0`, 60 s window clean) |

A scenario passing its TAPs is **not** the verdict: every one carries an observation window
that watches past it, because a gate that scores at tick 2 against a node that dies at tick 6
is a false green (#216).
