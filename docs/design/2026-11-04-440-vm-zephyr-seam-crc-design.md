# The VM↔Zephyr seam as CRC cards — one wait, every arrival an event

2026-11-04, settled 2026-11-05 · #440 (P0, the busy-poll), #458 (radio off with the VM live), #459 (nRF idle
vs System OFF), #457 (nRF System OFF, deferred) · the engineering half of the 2026-11-03 review ("the model
is elegant, the C has accreted: 56 sites of the core reaching into the Zephyr backend, 91 case arms, nine
lists"). Made under the `invarlint-designer` skill: the cards under `nodus/oevm/invarlint/cards/` are the design
of record for the seam, and this document is the why.

## Why

A node on two AA cells has to last years. The VM today cannot idle: `awaitReply:` spins on
`socket receive` for its whole budget, the HTTP listener forces the pulse pump non-blocking, and the
OTA fetch polls every 5 ms. The 2026-10-23 J-Link samples on the ACC01 found 12 of 12 PCs in the
interpreter and none in idle; the Bluetooth work queue starved and pairing failed (#440). No radio verb
can save power while the CPU never sleeps (#458), and no sleep mode can be chosen until the VM idles
at all (#459).

The same photograph (invarlint, master `06be9a02`) says why the seam is hard to reason about: one card,
`ZephyrBackends`, sanctioned every crossing, so nothing named which seam a call belongs to, which seams may
block, or where an arrival turns into an event.

## Decided by David

**2026-11-04**

- **The goal is the most power saving possible: two to three years on two AA cells.** Everything
  below serves that.
- **nRF52840 sleeps in place.** System ON idle keeps RAM, the RTC and the VM; its floor is within a
  few µA of System OFF, which costs a cold boot. No System OFF until a measurement says idle fails
  the budget (#457 stays deferred; #459 measures).
- **ESP32 keeps deep sleep** as the MagTag does today, RAM loss accepted.
- **Radio on/off is one common verb across BLE, Thread and WiFi**, and radio-off with the VM live is
  the requirement, not radio-off as part of a sleep (#458).
- **Shape B: the check-in becomes reactions.** No wait inside a reaction, anywhere. Bigger rewrite,
  simpler result; "we are only at 0.1".

**2026-11-05**

- **One sleeping block, unified across UDP, BLE and HTTP.** A stopped program waits in that same block too;
  there is no second receive and no named exception.
- **A run-once node is event-driven like every other program.** Set up, start the check-in, run the reactor;
  the deep sleep happens in the reaction to "exchange finished". No wait-until-idle call exists.
- **A check-in starts an exchange if idle and returns at once.** Apps do their work before it: the health row
  rides the same push it rides today (`nodus/nsl/lib/kernel.nsl`, the `obs/health` note in the trim comment),
  with fresher values. Only work that truly needs the end reacts to "finished".
- **The radio is an app policy the bridge carries out:** always on, per exchange, or off for a duration.
- **Off always has a duration.** A bug in an app's turn-back-on code must not strand a node, so an app can only
  say "off for N"; when N expires the bridge does one exchange whatever the app does, and a fix can arrive.
- **ttgm's check-in cadence is 60 s, with 4 allowed misses**, so porta calls a dead chair node dead in about 4
  minutes. The builder changes the chair apps in ttgm's own repo, nothing copied
  into rete. The BLE transport is already wholly in rete (`udp_ble.c`, `relay_frame.h`,
  `protocol/RELAY-BLE.md`, `relay/android/`); only applications live in ttgm.
- **Current measurement is delayed:** David has the tools but it is significant setup. No gate here waits on
  it; #458 and #459 stay open for it.

## The budget, so the design has a number to serve

| Cells | Usable | 2 years | 3 years |
|---|---|---|---|
| 2× AA alkaline | ~2,000–2,500 mAh | ~115–140 µA average | ~75–95 µA average |

Datasheet floors (not yet ours, #459 measures): nRF52840 System ON idle with full RAM ≈ 2–3 µA,
System OFF ≈ 0.4–1 µA. The gap buys nothing against a ~90 µA line. What spends the budget is the CPU
awake (3–7 mA), the radio (tens of µA advertising at a slow interval, mA while connected and chatty)
and the sensors. So the design has three legs: the VM truly idles, the radio is off except when
talking, and each wake does little.

## As found (the photograph, by callee)

| Crossing | Sites | What it is |
|---|---|---|
| core → `src/backend/zephyr` | 109 | 38 console writes; 27 reached from the prim switch, one backend each; the rest are http, span, ota, udp and serial calling their own port, as meant |
| core → `zephyr/src` | 24 | every one from the prim switch to the target leaves defined inside `main.c` |
| adapters → core | 9 | the pulse pump posting into the space, the http adapter handing a request line up |
| `main.c` → adapters | 14 | boot start-up; the liveness observer reading pulse counters |

Six things post into the pulse mailbox today (timers, GPIO edges, sensor triggers, the console line,
the RTC alarm, and the host's console service). Three arrivals do not: UDP, BLE and HTTP. Those three
are exactly where the VM polls. The pulse pump is already the one place the VM thread blocks
(`K_FOREVER` on Zephyr, which is what lets the idle thread run); the spins exist only because an
arrival that posts nothing can only be noticed by asking.

## The rule, in one sentence

**Every arrival posts a doorbell on its channel; the VM thread blocks only in the pulse pump, with no
deadline; a deadline is a timer.**

Nothing new is invented: the mailbox, the doorbell (#312, `pulse_post_isr_value`), the ring and the reaction
all exist; the design wires the three missing sources to them and deletes the polls. `pulse_pump_nb` and any
pump-with-deadline go away. A blocking wake always has a cause, because every state that waits on the network
holds an armed budget timer.

## Arrivals

Four invariants, on every backend, so that UDP, BLE and HTTP are one mechanism:

1. **The payload is readable before the doorbell rings.** The reaction pops until empty, so a doorbell that
   overtakes its payload would lose the wakeup.
2. **The token is a constant per channel, not a sequence number**, so a burst coalesces into one pending
   wakeup and cannot shed a timer or GPIO edge through `pulse_drops` (tuvm#54/#55). The console's per-line
   sequence is the one exception and stays, because a line is a unit of work; a datagram is not until the
   reaction pops it.
3. **One doorbell per socket until the VM has read it to empty.** BLE needs nothing for this: its callback runs
   once per frame on the Bluetooth thread, and coalescing absorbs the rest. A reader over `zsock_poll` is
   *level*-triggered: left alone it sees the same unread datagram forever and spins, which is the defect this
   design exists to remove. So on ringing it drops that socket from its poll set, and restores it when the VM's
   receive answers empty. One socket parked never silences another. A datagram landing between that answer and
   the restore is seen at once, because the poll is level-triggered.
4. **Every reaction that owns a socket reads it to empty, on every path**, including a budget expiry and a
   discard while idle. Invariant 3 waits on that answer, and three paths would otherwise never give it: a
   coalesced doorbell covering two datagrams, a doorbell shed by a full mailbox or channel ring, and a miss.
   Any of them would leave a node alive, idle and deaf for good. With this rule every exchange ends in a
   restore, so a lost doorbell costs at most one budget. No reader timeout, which would reintroduce a poll.

**VM sockets only.** The OTA fetch and the console tee bind sockets too (`src/ota.c`, `log_udp.c`) and read them
on their own threads, so a doorbell is registered by the `basicBind:` prim (`src/udp.c`), not inside the backend's
bind. The channel is `sys/net/<fd>`, registered there so it exists before the first datagram can land; it is keyed
by fd because BLE's port-0 bind picks a port the core never learns (`udp_ble.c`). Link changes ring
`sys/net/link`. The posix adapter keeps a socket table so the pump's `poll()` covers every registered socket and
the listener. One reader thread serves all of a node's registered sockets and its listener; BLE's callbacks need
no thread at all.

**HTTP posts readiness only.** Accept, read and parse stay on the VM thread, in the prim the doorbell's reaction
calls; `http_core.c`'s single-threaded ring needs no lock. `http_poll` accepts one connection today and returns
nothing, so it answers whether it accepted and the reaction loops until it did not, which is invariant 4 for a
listener. The cost, conceded: the read is bounded at 200 ms on the one VM thread, so a slow client can delay the
*service* of a UDP doorbell by that much, though never its post. Moving the request-line read to the reader
earns itself if that is ever measured to matter.

**Closing a socket waits for the reader** (R1): the fd is never closed under an active poll, so `udp_close` waits,
bounded by one pass of the reader, for it to drop the socket. The one wait on the VM thread outside the pump.

**A listener that cannot accept pauses** (David, 2026-11-07, PR #462). When accept fails for want of buffers (#65),
the listener counts it, stops listening and listens again after a one-shot timer, so a wedged node idles rather than
spins. Every socket a program may open is pollable, and a socket that cannot get a doorbell fails its bind loudly:
a socket that never rings is a deaf node.

## The cards (intended)

One card per seam, each a root of the `adapters` context, each an interfacer unless marked. The sentences,
each with its source, are in the files; this table is the map. `ZephyrBackends` is retired and nothing it
bound is left unbound.

| Card | Responsibility | Where |
|---|---|---|
| **Pulse** | the mailbox; the one place the VM thread blocks | `nodus/oevm/invarlint/cards/pulse.yaml` |
| **Timer** | monotonic time; every deadline in the system | `timer.yaml` |
| **Net** | the sockets over any transport; the doorbell; the link state | `net.yaml` |
| **Http** | the listener and the response path; readiness only | `http.yaml` |
| **Console** | the console, both directions; `Interpreter → Console` is the 38 sites' edge | `console.yaml` |
| **Serial** | the byte port | `serial.yaml` |
| **Sensor** | kinds, readings, armed triggers | `sensor.yaml` |
| **Sleep** | the per-SoC sleep and the RTC family, behind one verb | `sleep.yaml` |
| **Ota** | the stage; the fetch's 5 ms poll is named debt until R5 | `ota.yaml` |
| **Span** (service-provider) | stamps and elapsed time; no state | `span.yaml` |
| **Identity** (information-holder) | board name, boot id, node id | `identity.yaml` |
| **Radio** | the radio on and off, with the VM live | `nodus/oevm/invarlint/revisions/r3-radio/`, lands with R3's code |
| **PortaBridge** (coordinator, nsl) | one exchange at a time, moved on by events | `nodus/oevm/invarlint/revisions/r2-check-in/`, waits for invarlint to read nsl (invarlint#177) |
| **Targets, Boot, App, CoreCallbacks, TimerSnapshot, Space, Interpreter, WellKnownSymbols** | as today | `Targets`' leaves leave `main.c` in R6 |

Context cards: `core` = `src/*` (one segment, so the inline functions in `src/*.h` are core too), `adapters` =
`src/backend/zephyr/*`, `host` = `src/backend/posix/*` (empty in the C6 photograph, by design), `app` =
`zephyr/src/*`. Backend cards bind their header's names, not prefix globs, because the core defines `http_*`,
`udp_*`, `serial_*`, `span_*` and `ota_*` functions of its own (a glob pulled 23 core callees into
`root-bypassed`). The RTC alarm sits with the RTC family on `Sleep`, so one card holds the wake clock.

**Predicted and measured agree on `06be9a02`: new 0, stale 0, root-bypassed 0, context-straddled 0,
owns-violated 0.** The map reads core→adapters 62 functions from 115 sites, core→app 15 from 18,
app→adapters 12 from 14, app→core 9 from 13, adapters→core 7 from 9, every crossing landing on a root. Nothing
stopped being sanctioned, which is what "cards only" should read. core→app 15 is by card, not by code: Sleep's
six leaves are still in `main.c`, so the code's number stays 21 until R6. The guard was broken to prove it: narrowing
`Pulse`'s binding fires 29 keys, and taking `root` off `Console` fires 21. The prediction is
`nodus/oevm/invarlint/out/whatif/r0-seam-cards/`.

## The check-in as reactions (shape B)

The protocol does not change: two node-initiated datagrams, PULL→BATCH and PUSH→ACK, idempotent by
cursor (`porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md`). porta is untouched. What
changes is the node's shape: `PortaBridge` becomes a state machine driven by events.

"Read" below always means invariant 4: read the socket to empty, handling each datagram in turn.

| State | check-in (cadence) | `sys/net/link` up | socket doorbell, `sys/net/<fd>` | budget timer |
|---|---|---|---|---|
| **idle** | policy off: counted, nothing sent. Link down: counted skip. Per exchange: radio on, arm link budget, → linking. Else send PULL, arm budget, → pulling | — | read; discard all (stale, counted crossed) | — |
| **linking** | ignored | send PULL, arm budget, → pulling | read; discard all | miss, radio off, → idle, post finished |
| **pulling** | ignored | — | read; `#batch` → apply, publish, send PUSH, re-arm budget, → pushing; `#ack` → crossed, discard; undecodable → discard | read first: a reply found is an arrival, not a miss. Else miss, then publish and send PUSH anyway, re-arm, → pushing (today's behaviour: a pull miss still pushes) |
| **pushing** | ignored | — | read; `#ack` → applyAck, → *leaving*; `#batch` → crossed, discard | read first, as above. Else miss, → *leaving* |

**Leaving** an exchange, in this order, is the same moment as today's tail of `checkIn`, so `otaCheck` still runs
after the push on the same exchange: cancel the budget and → idle; wake the debugger if lines arrived;
`otaCheck` (its one-tick delay, design D6, is kept); per-exchange policy turns the radio off; post `finished`.
Idle comes first (as built, R2, PR #470): the wake stops the program at its next bytecode, inside the leaving
itself, so the held loop must find an idle bridge, not one still pushing with no budget armed. An exchange the
held loop started ends silently (no wake, no `otaCheck`, no `finished`); an app's exchange that ends while held
skips the wake and `otaCheck` but still posts `finished`.

- `awaitReply:`, `replyBudget`'s clock loop, `isCrossed:expecting:` as a loop test, and the
  `exchanging` flag's busy-wait meaning are deleted; `tagOf:` and the crossed count stay.
- **A reply and an expiry can both be pending; the budget post names its leg.** The bridge cancels the budget on
  every transition and `Timers>>dispatch:` drops a token no longer pending, but that is not enough (as built, R2,
  PR #470): an expiry already dispatched onto `sys/porta/budget` can sit in the space while a doorbell reaction
  moves the exchange on. Each arm counts a leg and the post carries it; a post for an older leg is ignored
  (`22_spacesync/32_check_in_reactions.nsl`). The budget is armed before the PULL is sent, so a full timer
  table (#81) leaves the bridge idle rather than waiting with no wake; a full table at the push re-arm ends the
  exchange, logs `timer table full`, and the next check-in retries (`22_spacesync/33_check_in_table_full.nsl`). Any raise in the
  stretch between the pull budget's expiry and the push re-arm, when no budget is armed, does the same, logged as
  `exchange raised` (`22_spacesync/35_lapse_raise.nsl`).
- The six spans survive: `pullWait` opens at the PULL send and closes in the reaction that receives the
  BATCH. An app's `sCheckIn` bracket around a call that returns at once measures nothing and is deleted.
- **The bridge takes its `Timers` at construction**, the `spans` precedent, so a bridge cannot exist in a shape
  that blocks forever. Budget is one one-shot, against `OEVM_MAX_TARGET_TIMERS`. Since #81 a full table raises
  `timer table full` in the job that armed it and the node lives (`timer_backend.h`); a bridged node's standing
  slots are still named on the `Timer` card.

### What an app does

| Program | Today | After |
|---|---|---|
| Periodic node (17 programs call `bridge checkIn`: 15 in rete, 2 in ttgm) | check in, then publish health, print counters | publish, print, then check in; the counters are the last exchange's |
| Run-once node (`MagTagDeepSleepNode`, `MagTagSleepyDebugNode`) | check in, drain by hand, check in, deep sleep | set up, check in, reactor; the reaction to `finished` takes the next step, the last of which is the deep sleep. The hand `space drain` is gone: the push is built after reactions have run |
| Backlog flush (`AccMotionBle>>drainOutbox`) | loop of up to 40 check-ins | on `finished`, if rows remain and under the same bound, check in again |
| Bell (`ShipsBellNodeC6`) | skips a check-in while striking, because it would stretch the gap | the check-in no longer blocks, so the guard's reason is gone; builder removes it if the board scenario agrees |

Also touched by R2: `DebugLink>>heldExchange` and `Debugger>>readLine`; `TimerlessNodeC6`, bridged with no app
timer, whose gate is re-read under reactions; `ThreadSensorNode.nsl`, the documented example; and the suite tests
under `22_spacesync` that drive `checkIn` synchronously.

`finished` is one channel, `sys/porta/exchange`. It exists because the run-once nodes and the flush need it,
not as the general pattern.

### While the program is stopped

A stop holds the stack, so no reaction can run; this is the one place the pump is entered from inside a frame.
It is the same block, and it idles. The held loop arms the budget, blocks in the pump, then services the
bridge directly: read the socket, read the clock against the deadline, and cancel the budget when a leg ends, so
its expiry is not delivered on the continue. The bridge's `Timers` is what guarantees the wake; a held VM is
exempt from the liveness observer, so nothing else would rescue it. Everything else the pump delivers queues
in the space for the continue, and overflow is shed and counted, as today. When an exchange finishes it starts
another, until a line arrives on the link or the cable, as `Debugger>>readLine` does today.

**This fixes a hazard shape B would otherwise create.** A check-in no longer blocks, so a breakpoint can hit
in a user's reaction while an exchange is in flight. Today that reads as "exchange already on the stack"
and `readLine` answers nil (`nodus/nsl/lib/supervisor.nsl`). The held loop finishes the exchange in flight
instead, so a stop is always served. A breakpoint hit while `linking` waits for the link the same way.

## Radio

One verb at the seam, `radio_*`; one cell, `sys/radio`, holding the state, so the last push says so.

| Policy (the app sets it) | The bridge does | Who can reach the node |
|---|---|---|
| **always** (default) | nothing; skips a check-in while the link is down | any time the link is up |
| **per exchange** | radio on for each check-in, off when it finishes | at the check-in, so within one cadence |
| **off for N** | one last exchange to carry the cell, then off; check-ins are counted and nothing is sent; at N, one exchange whatever the app does | nobody until N or the app turns it on (no goals, no OTA, no debugger), so a fix waits at most N |

- N is a timer, like every deadline, and the one number: the bridge re-declares the expected gap from it with the
  existing liveness declaration, so the silence is not read as death. porta is untouched. The kernel invents no
  period.
- `linkBudget` is one number, set from the first reconnect measured at R3. Until R3, per exchange does not
  exist, so nothing needs a guess. What would earn a second number: a transport whose reconnect differs enough
  to matter, measured.
- Reconnecting through the phone is unmeasured; 3 s made per exchange look impossible, 60 s does not.

## Revisions, in order (each one an invarlint revision with a `whatif` prediction and a gate)

| R | Scope | Gate |
|---|---|---|
| **R0 cards** | the cards and context cards above; on this branch | invarlint PASS, new 0, stale 0; debt named per card |
| **R1 arrivals** (C) | Net: BLE `recv_cb` posts after `ring_put`; the reader on Zephyr sockets; posix posts from `poll()` inside the pump. Http: the same reader. `pulse_pump_nb` and `http_poll`-in-`pumpEvents` deleted | nsl suite, `porta_sync_host.sh`, `http_roundtrip_host.sh`; new host tests for invariant 4 (two datagrams under one doorbell; a miss, then a reply), because the non-blocking host pump cannot otherwise see a lost restore; `bridge-health` on a C6 (PASS 3/3, 2026-11-08). As built, David 2026-11-08: the C6 http scenario does not exist (#463) and nothing on the C6 measures idle, so idle evidence is R2's J-Link gate |
| **R2 check-in** (nsl) | the state machine, the held loop, every app in the table; `awaitReply:` deleted; the OTA fetch untouched. In ttgm's repo: the chair apps' new order, flush, 60 s cadence and `misses: 4` | `porta_sync_host.sh` PORTA SYNC OK; `tutorial-porta`; the debugger host gate; the `porta-*` board scenarios; `CONFIG_BT_LONG_WQ_PRIO=0` dropped and pairing still succeeds; **J-Link PC samples on OCM.1 land in idle between check-ins**, with rete's `PortaBridgeNodeBle` and then the chair app |
| **R3 radio** (#458) | `Radio` card; the verb, `sys/radio`, the policy; `sys/net/link` | OCM.1: off, on, reconnects through the Pixel relay; the reconnect time measured, which sets `linkBudget` |
| **R4 measure** (#459) | delayed by David: OCM.1 always-on, idle, per exchange at 60 s, radio off, against the 90 µA line | the numbers in #459; #457 earns itself or closes |
| **R5 OTA fetch** | reactions in the kernel, one state per block; the OTA thread and `ota_sleep_ms` deleted | `host_ota` gate; #449's OTA over BLE re-run |
| **R6 structure** | the twenty leaves out of `main.c` into `gpio_zephyr.c`, `display_zephyr.c`, `bell_zephyr.c`, `clock_zephyr.c`, `sleep_zephyr.c`; the prim switch split per seam (`udp_dispatch` and `serial_dispatch` are the pattern); one source list that CMake and the host Makefile both read | invarlint moved keys = predicted; `ab.sh` no speed change; `footprint.sh` |

R0–R2 close #440. R3 does the work of #458 and R4 of #459; both stay open for the current numbers. R5 and R6 are
the review's "the C has accreted" and can follow 0.1.

## Who does what

The session David starts is the designer: it writes the cards and holds the map, and writes no target code.
One builder subagent per revision (`invarlint-work`, in its own worktree), one fresh reviewer per return
(`invarlint-review`, runs every check itself). The designer never judges a return, and David merges. Toolsmith
gaps go to invarlint by issue: invarlint#154, #176, #177.

## Degenerate cases shipped (each with what would earn the knob back)

- **No pump-with-deadline.** A deadline is a timer. Earned back only if a timer per wait is measured
  to cost more than it saves, which nothing suggests.
- **The host pump stays non-blocking** under the virtual clock: the suite drives time. Behaviour
  (doorbell on arrival) is identical; only the blocking differs. Earned back if a host gate needs real
  idling.
- **Serial `readLine` polls `basicAtEnd`** inside one line already arriving. Bounded; a doorbell per
  line (the console already has one) earns itself when a serial node is measured spinning.
- **nRF System OFF (#457)** earns itself if R4 measures System ON idle with the radio off above the
  budget share the chair node can afford. Then one verb, `deepSleep:`, grows the nRF body.
- **The reader thread's priority and stack** copy the log tee's until measured.
- **The listener's pause after a failed accept is 1 s**, chosen without a measurement. Earned back by measuring how
  long the #65 condition takes to clear under the burst that caused it.
- **The pending bound stays 64 rows.** At one row per 60 s tick that is about an hour of outage before
  oldest-first eviction bites, sooner in a motion burst. #443's flash store is what earns more.
- **"invarlint PASS" means the ESP32-C6 photograph.** `udp_ble.c`, the nRF build and all of the nsl are not in
  it, so R1's BLE doorbell, R2 and R3 are gated by the suite and the board, not the ratchet, until invarlint can
  hold a second compile database and read nsl (invarlint#177).

## Left to the builder

- Zephyr socket arrivals: the reader over `zsock_poll` versus `net_context` receive callbacks. Either keeps the
  four invariants; pick by stack cost and by what the BLE path already does. A poll set changing under the reader
  needs a wake (an eventfd in the set); not yet run.
- The exact Thread and WiFi radio-off calls, and whether a radio-off socket send answers an error or
  silently drops (the state machine treats both as a miss).
- How suite tests that fake `Socket>>receive` become doorbell-driven fakes under `advanceMillis:`, and how the
  debugger's host gate drives the held loop's clock (`OEVM_TIMER_WALLCLOCK` exists for real loops).
- The reactor loop stays `[ platform pumpEvents. space drain ]` in every app; folding it into the
  kernel is not this design's business.

## Done when (#440)

The inverse of the 2026-10-23 measurement: J-Link PC samples on OCM.1 between check-ins land in
Zephyr's idle, not the interpreter; `CONFIG_BT_LONG_WQ_PRIO=0` is gone and pairing succeeds; the
porta gates are green on host and board.

## Promotion

`nodus/oevm/docs/vm.md` "The reactor, the space and the wire" is rewritten to the one-sentence rule;
the cards under `nodus/oevm/invarlint/cards/` are the design of record for the seam; the ladder
(`review/program-sequence.md`) and `review/tests/CLAIMS.md` take the power claim once R4 has the number.

## Addendum 2026-11-10 — R5, the OTA fetch as reactions

- **One mechanism for every board.** The fetch thread (T9) and the in-channel fallback (T10,
  `CONFIG_OEVM_OTA_INCHANNEL`) both go; the kernel's `OtaFetch` runs on the VM thread as reactions, one state per
  block, moved on by the doorbell of the bridge's own socket (one socket, one doorbell; a block frame is told from a
  batch by its first byte, and the block plane is still the peer's port plus one) or the block's budget timer. No reaction waits, so the app-stale
  rule that drove T9 off-thread cannot fire, and the reason T10 existed for the S2 is gone too.
- **The C keeps the bytes, not the loop.** Ota still owns the stage (append, read, truncate, size), the SHA-256
  and the ready record; `ota_sleep_ms`, `ota_thread_start` and `fetch_block`/`fetch_run` are deleted.
- **"Finished" means the bridge is idle:** no exchange and no fetch in flight. That is what lets a run-once node
  (the MagTag) stage a new program before it sleeps, which is the only thing T10 was for. One sentence, no new event.
- **Kept from T9 unchanged:** the stage length is the resume cursor; a peer error is not retried and on a resume
  empties the stage; an exhausted budget keeps the stage; a hash mismatch empties it; a failed fetch is not
  remembered and the next check-in retries. Budget: 5 attempts of 1 s per block, as built (`OTA_ATTEMPTS`), until
  a measurement earns another number.
- **A retarget mid-fetch never activates the old image:** a fetched stage is activated only while the goal still
  names its hash; otherwise the next check-in stages the new goal (T9's behaviour).
- Card: `nodus/oevm/invarlint/revisions/r5-ota-fetch/ota_fetch.yaml`; revision `r5-ota-fetch.yaml`.
