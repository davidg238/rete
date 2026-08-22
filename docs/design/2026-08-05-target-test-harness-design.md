# A diagnostic is a workload, not a test framework — the target harness

**#56 step 4.** Design record, 2026-08-05.

Steps 1–3 of #56 are closed: the console leaves the board, panics symbolize, and metrics
reach porta out of band. Step 4 is the one the issue calls *"the real unlock; (1) and (3)
are what make it possible"* — `reset → scenario → assert → report`, so on-device behaviour
is a gate like the host suite rather than something a human drives by hand and interprets.

---

## 1. What this is, and what it is not

**It is** a host-side runner that deploys a workload to a device, collects what that
workload published, and renders a verdict.

**It is not** a test framework on the device. There is no test category in the VM, no
on-node runner, no assertion library in the kernel. A diagnostic is an nsl workload like
any other, and its results are entries at a path — the same posture as the rest of the
system: no verbs, add a path.

That distinction is the whole design, and it comes from one requirement (David,
2026-08-05):

> ultimately we need to be able to run tests/diagnostics on devices in the field … so
> wiping the db and starting a gateway is not an option … must be just another workload.

Everything below is a consequence of taking that literally *now*, while the only targets
are on a bench, rather than discovering it later when the bench affordances have already
been baked into the contract.

## 2. What already exists, and the contradiction between the two

Two target gates are in the tree, and they disagree about the console:

| | `nodus/tuvm/tests/stale_handle_c6.sh` (#151, 2026-08-05) | `nodus/tuvm/tests/porta_sync_c6.sh` (2026-08-02) |
|---|---|---|
| console | **requires** the #56 collector, never opens the port | **starts its own reader** and holds it across the flash |
| anchor | build id archived for *this* flash | none |
| assertions | on the console, both arms positively | at **porta** — the independently written implementation |
| privilege | none beyond flashing | wipes a db, starts a gateway over `ssh`, injects drops |

`porta_sync_c6.sh` predates the collector, so its private reader was correct when written
and is now a violation of the single-reader rule (two readers split the stream and both
halves look plausible). That is resolved here, not preserved.

Both scripts contain patterns worth keeping, and this design keeps them explicitly:
preflight that **fails rather than skips**, assertions that are **positive on both arms**,
and an anchor tying every assertion to the image that was actually written.

### The finding: the wipe was necessary, and it was hiding a defect

> **Corrected 2026-08-05, by building the scenario this section is about.** What
> follows was written as *"the wipe was never necessary"*, and the argument below is
> sound as far as it goes — the `starts at 1` assertion really does become
> `starts at baseline+1` and the privilege really does evaporate. But the wipe was
> load-bearing for a second reason nobody had seen, in the opposite direction:
>
> **porta's stored high-water for a node survives the node's reboot, and the node's
> space counter does not.** A reflashed node pushes seq 1..n against a stored
> high-water of thousands, porta discards every row as already-applied, and the ack
> carries that stale high-water — which `applyAck:` adopts. From then on the node
> believes everything it writes has been accepted, porta holds observations hours old,
> and neither end can say so. The wipe was resetting the high-water on every run, which
> is exactly why this gate had been green for three days over a defect that makes a
> node's observability silently permanent-dead in the field.
>
> Found by `bridge-health`'s first hardware run, which scored **3/3 while every cell the
> node wrote was being thrown away** — the vacuous green and the loss had the identical
> cause, because `changedSince: acked` is empty for the same reason the rows are
> discarded. Both halves are closed: the workload now asserts `acked <= space seq`
> (a watermark above this node's own counter is proof the two ends are talking about
> different boots), and porta rewinds its high-water when a pull declares **both** a
> lower cursor and a lower `hw` (`Store.NoteCheckIn`; PROTOCOL-NSL §"Five rules", rule 5).
> The wipe is now genuinely unnecessary — but it was not, and the difference is a real bug
> it was concealing.
>
> **The first version of that fix was wrong, and the split's other half caught it.**
> Rewinding on `hw` alone treats a MISSED ACK as a restart — and there the node is
> legitimately re-offering rows porta already committed, so the rewind deletes the
> `seq <= hw` dedupe and manufactures the duplicate it was meant to prevent. The decomposed
> `porta_sync_c6.sh` failed within one leg on `the gateway never logged a duplicate-skip`,
> where it had logged 959 before. That is the D4.1 split earning its keep in the direction
> it was argued for: the workload cannot see delivery, and the thing that can, did.

The original argument, which stands on its own terms:

`porta_sync_c6.sh` already contains both halves of this argument.

- `assert_exactly_once` demands the series **start at 1**, and only a fresh store gives
  that. Its comment justifies the check correctly — `porta log --limit N` truncates from
  the oldest end, and the bridge's `maxPending` eviction is oldest-first, so contiguity
  alone does not pin the start.
- `gw_drops` takes a **delta across the leg** on a cumulative counter, precisely because
  the gateway's count is not zero when the leg begins.

The second pattern is the one that survives contact with a field device, where nothing
will ever be wiped. `starts at 1` becomes `starts at baseline+1` — the same assertion at
the same strength — and the wipe, the `ssh` and the privilege all evaporate. This is why
the answer to *"should dev machines run a porta in docker?"* is **no, not for this**: the
disposability was a symptom of an assertion shape, not a requirement.

## 3. Decisions

### D1 — A scenario is a deployable workload; there is no on-device test framework

The device runs a job. The job publishes what it found. The harness is entirely host-side.

A scenario that cannot be expressed as a workload is not a scenario. If it needs to wipe a
store, restart a daemon or reach the other end of the wire, it is a **bench integration
test of that other end**, and it lives outside the harness under its own name.

### D2 — Two seams, symmetric: `deploy` and `collect`

```
                    BENCH (this rung)                FIELD (later)
  deploy   ──────►  west flash via run.sh            .oimg / job push over porta,
                    (c6bench, disposable)            under a capability rooted at the
                                                     diag subtree
  collect  ◄──────  the #56 collector's log,         tel/test/<scenario> stream,
                    anchored on build id             read via porta from a baseline
```

Two functions with fixed signatures:

```bash
deploy::<impl>  <scenario_env>                       # → prints the build id it deployed
collect::<impl> <build_id> <scenario> <timeout_s>    # → prints that scenario's TAP lines
```

The signature is the same for both implementations. What differs is *configuration*, not
arity: `collect::console` needs a board name and the collector's store path;
`collect::porta` needs a porta address and the baseline cursor of D4. Each reads its own
from the environment, so the runner never learns which transport it is on.

Everything else — preflight, plan checking, verdict, report — sees only TAP lines and a
build id, so it is identical on both. Moving to the field is a swap of two functions, not
a rewrite. **The seam is the deliverable**; the field implementations are not built here.

### D3 — The workload emits TAP-lite: a declared plan, then results

```
TAP stale-handle plan=2
TAP stale-handle ok 1 control delivered=1
TAP stale-handle ok 2 test delivered=1
TAP stale-handle done pass=2 fail=0
```

Rules:

- Every line self-identifies with the scenario, so interleaved output from two workloads,
  or a log holding an earlier run, cannot be misattributed.
- The runner FAILS — not times out — when `done` is missing, when `pass + fail != plan`,
  when the result indices are not exactly `1..plan` each appearing once, when `done`'s own
  tallies disagree with the results that arrived, or on any `not ok`. A truncated run and a
  passing run must not look alike.

> **Amended during implementation, 2026-08-05.** An earlier draft put `build=<id>` on the
> plan line, so the anchor would be emitted by the thing being tested rather than inferred
> by the thing testing it. **A node cannot do that today.** `obs/build` carries `#chip` and
> nothing else — `PortaBridgeNodeC6.nsl:63`, `ShipsBellAlarmNodeC6.nsl:133` — which is
> exactly **#74**, still open. The C6 *prints* a build id at boot from the DROM note, but
> that is the C side; nsl has no accessor for it.
>
> So the bench anchor is the **boot banner**: the runner reads forward from the line
> carrying the build id `run.sh` archived for that flash. Sound here, because the collector
> writes one stream in order.
>
> The consequence worth naming: **the field path's anchor is blocked on #74.** Over porta
> there is no boot-banner ordering to lean on, so until a node can name its own build there
> is no way to tie a remote result to an image. That is a concrete dependency between two
> issues nobody had connected. A `platform buildId` accessor would close both, and is
> deliberately not built here — it is a VM prim, not harness work.

The plan count is the structural defense against this bench's signature failure: a gate
that goes green having armed nothing. It matters more in the field, where the transport is
a lossy UDP/Thread path rather than a wire you can trust.

TAP was chosen over a bespoke `RESULT` line because the plan count is exactly the property
needed and TAP is not invented here. Only four line forms are used; no diagnostics, no
YAML blocks, no subtests.

### D4 — Assertions are delta-based against a baseline. No scenario wipes anything

`collect::porta` reads from a **baseline cursor taken before the deploy** — never from "a
fresh store". A scenario that requires an empty store is asserting something it does not
mean. `starts at 1` becomes `starts at baseline+1`: same assertion, same strength, no
privilege.

Consequence, stated so it is not rediscovered: no scenario wipes, restarts or `ssh`es
anything. In the field there is one porta, it is shared, and it is append-only.

### D4.1 — Two assertion sources, and the split is principled

A workload can only testify about what it can see. Some properties are visible **only at
the other end of the wire** — "`tel/log` arrived exactly once" is a statement about
delivery, and a node that dropped a row is precisely the node that cannot report it.

So assertions come from two places, and which one owns a property is decided by
observability, not convenience:

- **The workload asserts what only it can see** — its own counters, its own convergence,
  its own `#streamDropped`. These are the TAP lines.
- **The collector asserts what only the far end can see** — contiguity from the baseline,
  no duplicates, no gaps. This is not a per-scenario assertion; it is a property of the
  collection itself, checked identically for every scenario that arrives over a stream,
  and it is the same check `porta_sync_c6.sh`'s `assert_exactly_once` performs today.

The second is why D3's grammar wants to be a stream in the field: contiguity stops being
string parsing and becomes something the transport already guarantees and reports.

### D5 — A workload declares what it does to the device, and the class governs where it may run

| class | means | may deploy to |
|---|---|---|
| `observe` | reads state; no timing impact | anything, including a live field node |
| `disruptive` | affects timing or load, may reboot or latch the device | disposable only |

`stale-handle` and `pulse-coalesce` are both `disruptive` — each deliberately burns the
reactor. A bridge health check would be `observe`.

> **Amended during implementation, 2026-08-05.** The draft had three classes, splitting
> `disruptive` into `perturb` ("disposable, **or a field node with explicit consent**") and
> `exclusive` ("disposable only"). The distinction rests entirely on a consent mechanism
> that does not exist and that nothing here would exercise — an invented tunable with no
> data to choose it from, which the standing posture says to defer.
>
> Collapsed to two. **What would earn the third:** a consent path on the field deploy — a
> grant recorded under the node's own tree that a `perturb` workload can be checked
> against. At that point `perturb` means "runnable where consent is recorded", which is a
> mechanism rather than an adjective. Until then two classes answer every question this
> harness can actually ask.

One mechanism covers both "do not flash the ship's bell" and "do not wedge a node in the
field". **A hardcoded by-id deny-list is kept as a backstop on the flash path only**, and
that redundancy is deliberate rather than sloppy: a mislabelled workload is a plausible
mistake and an ended multi-day soak is not recoverable. The bell's adapter hash is refused
with no override env — if the bell must be flashed, that is a hand operation with
`run.sh`, not something a test suite has a code path for.

### D6 — The runner never opens the serial port

It requires that board's collector unit to be active and not reporting `port=down`, and
reads the collector's log. Two readers split the stream rather than doubling it.
`porta_sync_c6.sh`'s private reader is retired.

### D7 — The build-id anchor is mandatory

`stale_handle_c6.sh` today has a `DO_FLASH=0` path that asserts against "whatever is
already running" with no anchor. That hole closes: skip the deploy and you must supply the
build id you claim to be testing, or the runner refuses to run. Without the anchor, result
lines from a previous run sit in the same file and score a green against firmware that was
never built — failure mode 1 of nine on this bench's own false-green list, and the one
this class of gate would otherwise hit every time.

### D8 — Its own command; the nsl suite states the absence positively

`nodus/tuvm/tests/target/run.sh` is never invoked by the nsl suite — no hardware
dependency, no shared `/tmp` hazard, no ~1-minute flash cycle inside a suite that runs
constantly. But the nsl suite gains one line naming what it did **not** test:

```
  target gates: NOT RUN BY THIS SUITE — the posix timer backend bypasses the
    pulse mailbox, so coalescing, shedding and stale handles are UNTESTED here.
    Run: nodus/tuvm/tests/target/run.sh   (3 scenarios)
```

This is **not** a SKIP. A skip names an unverified cause; this names a verified one —
`timer_zephyr.c` posts through the mailbox, `timer_posix.c` posts straight into the space
via `space_post`, and all 243 suite tests run on the latter. The reader most likely to be
misled by `243 passed, 0 failed` is the one who never opens `review/tests/CLAIMS.md`, and
that reader is often an agent reading the gate line alone.

### D9 — One deploy per scenario, and what would earn better

Three scenarios means three flash cycles, serially, on one board. That is the degenerate
case whose cost can be measured. **#67** (time-to-deployed-change via `.oimg` push versus a
flash cycle) is unmeasured; when it is measured, an `.oimg`-push `deploy::` implementation
is the obvious optimisation and D2's seam is already the place it plugs in. No knobs are
invented for it now.

### D10 — No docker for this; the disposable porta is a process, not an image

A container on a dev machine is not on the Thread mesh. c6bench reaches porta at a
mesh-local address via the border router, which is why porta runs on `gw85224-01` at all —
so a local container does not replace it for any scenario here. Where disposability is
genuinely wanted, `porta serve --db <tmp>/t.db --udp [::]:<port> --http-port 0` is a fresh
throwaway gateway with no image to drift out of sync with the tree;
`nodus/tuvm/tests/porta_sync_host.sh` already does exactly this on loopback.

Docker earns its keep on a different job — **#25's fourth item**, N VMs plus a toy porta in
one CI run with no hardware and no Go toolchain. That can arrive later without touching
this design, because by D4 the harness takes a porta address and a baseline as
configuration and does not care what is listening.

## 4. Components

```
nodus/tuvm/tests/target/
  run.sh                  the runner: preflight → deploy → collect → parse → report
  lib/target.sh           preflight, board resolution, locking, TAP parsing, report
  lib/deploy_flash.sh     deploy::flash      — west + run.sh, returns the build id
  lib/collect_console.sh  collect::console   — reads the #56 collector's log
  stale-handle.env
  pulse-coalesce.env
  bridge-health.env
```

A scenario manifest is sourced bash — the repo's existing idiom:

```bash
SCENARIO=stale-handle
NODE=StaleHandleProbeC6.nsl
CONF_EXTRA="prj_esp32c6_thread.conf"
CLASS=disruptive
TIMEOUT_S=120
```

There is deliberately **no `DRIVER=` escape hatch.** Admitting one imports bench privilege
into the scenario contract, and once one scenario has a driver the next one will too.

### Report

```
target gates — board c6bench
  stale-handle     PASS 2/2      38s  porta-v1-final-824-g92e2968-20260805T142329
  pulse-coalesce   PASS 3/3      96s  porta-v1-final-825-ga646d0a-20260805T151204
  bridge-health    FAIL 2/3      64s  porta-v1-final-825-ga646d0a-20260805T151731
TARGET 2/3 PASS — 1 FAILED
```

Nonzero exit on any FAIL. The header names the board and the build id so a pasted report
is self-describing — a report that cannot say which board and which image produced it is
the same class of defect as an unanchored assertion.

## 5. The scenarios

> **What shipped, 2026-08-05: all three. `TARGET 3/3 PASS`** on c6bench —
> `bridge-health` 3/3 in 24 s, `pulse-coalesce` 3/3 in 76 s, `stale-handle` 2/2 in 18 s.
> The runner and the first two landed in d73b698; `bridge-health` and the
> `porta_sync_c6.sh` split landed after it, and the split turned out to be the cheap part
> — what was expensive was the defect the decomposition exposed (§2's correction).
>
> The earlier note here read *"the third arrives with the split, or the split proves
> harder than expected and that is a finding worth having on its own."* Neither: the
> split was easy and the finding came from the scenario, not the script.
>
> Two things the falsification pass caught that no amount of design would have:
>
> - **The collector and `esptool` were fighting over the tty.** A tty has no exclusive open,
>   so both read it and the ROM handshake gets split. It is a RACE, which is why both
>   existing gates had carried it while appearing reliable — the first flash of the session
>   won, the second lost. Closed in the shared layer: the collector is stopped for the
>   deploy window and the board is then reset AGAIN deliberately, because `esptool`'s own
>   post-flash reset happens while there is no reader and takes the boot banner — the anchor
>   — with it.
> - **The bell interlock ran too late.** With the collector check first, a deploy aimed at
>   the bell was refused with `port=down — wrong by-id, or unplugged`: a guess, and one that
>   invites the operator to go and plug the bell into USB, which is the single action the
>   interlock exists to prevent. The board-level refusal now runs before the lock, before
>   the collector check, before anything reads the board's state.

1. **`stale-handle`** — migrated from `stale_handle_c6.sh`. `StaleHandleProbeC6.nsl` gains
   TAP lines; the bash shrinks to a manifest. Class `disruptive`.
2. **`pulse-coalesce`** — `PulseCoalesceProbeC6.nsl` exists, was hand-run once, and has no
   gate. It is today's "a probe nobody runs is decoration" case. Class `disruptive`.
3. **`bridge-health`** — the node half of `porta_sync_c6.sh`, split out per D1. Per D4.1
   the properties divide by who can see them: the **workload** asserts that it published
   `obs/health`, that `goal/name` converged to `obs/name`, and that its own
   `#streamDropped` is 0; the **collector** asserts that the `tel/log` entries arrived
   contiguously from the baseline with no duplicates. Class `observe`, which makes it the
   first workload that could later run against a live field node as a health check.

   On the bench this scenario is collected over the console like the others, so its
   delivery half is checked by the plan count rather than by a stream. That is a real
   difference between bench and field for this one scenario, and it is named here rather
   than glossed: the bench run proves the node-side properties and the field run proves
   both.

   > **As built, 2026-08-05.** The workload is `PortaBridgeNodeC6.nsl` itself — the
   > bridge certifies itself and then goes on bridging forever, which is the only shape a
   > health check can have. A separate probe node would have been a third copy of the
   > bridge body to keep in step with two others, for no property a copy could assert
   > that the original cannot. It emits once, at the first tick where all three hold or
   > at its own 180 s deadline, whichever comes first — so an unattached node yields a
   > FAIL naming which property is false, never a silence the runner has to interpret.
   >
   > **Result 1 is not the one the design named.** "It published `obs/health`" is not
   > node-observable, and the closest thing that is — `changedSince: acked` is empty —
   > passed 3/3 over a node whose every write was being discarded (§2). What it asserts
   > instead is *every obs/ cell I wrote has been acked* **and** *porta's watermark is
   > inside this boot's sequence*. The second half is the load-bearing one and it cost
   > nothing on the wire.
   >
   > Result 2 needs a `goal/name` to exist at porta, and says so when there is none
   > rather than passing vacuously. That is not a bench affordance leaking in: a node's
   > configured name is an ordinary operator write, it persists, a cold-booted node
   > replays it from cursor 0, and "does obs match goal" is the convergence view itself.

### What happens to `porta_sync_c6.sh`

Its legs 4–8 — wipe the db, start a gateway, inject ack drops, diff the gateway's
cumulative counters — are a **bench integration test of porta**, driven from privilege no
field caller has. They are not a device diagnostic and do not become scenarios. The script
stays, is relabelled for what it tests, and loses its private console reader (D6). By D4's
delta assertions its db wipe should also be removable; that is expected to shrink it
substantially and is **not** assumed here — if the wipe turns out to be load-bearing for a
reason not yet seen, that is a finding to write down rather than a step to force.

> **Done, 2026-08-05 — and the wipe WAS load-bearing.** The script now borrows the shared
> gateway instead of starting one, restores the exact command line it found (on every exit
> path, including a failed assertion — an injector left armed would silently corrupt every
> later measurement on this bench), reads the collector's log instead of opening the port,
> and scores two legs instead of four steps of setup plus four legs. Eight steps became
> five; the node-side legs left entirely.
>
> `assert_exactly_once` became `assert_exactly_once_since <rows-at-baseline>
> <last-tick-at-baseline>` — same three properties, no wipe. The non-obvious part is that
> the baseline has to be a **row COUNT and not a seq or a tick value**: both restart at 1
> on every node boot, so a store holding a previous session (which is now the normal case,
> not the exceptional one) makes any `> baseline` comparison on either meaningless. Row
> order is the only thing that survives a reboot.
>
> And the finding the parenthesis above asked for, in the direction nobody was looking:
> the wipe was concealing a porta defect, not merely compensating for an assertion shape.
> See §2's correction.

## 6. Failure modes, and how each is gated

| failure | gate |
|---|---|
| Gate scores against the previous firmware | D7: mandatory build-id anchor, emitted by the workload in its plan line |
| Workload crashed mid-run, read as a pass | D3: `pass + fail != plan` is a FAIL, distinct from a timeout |
| Workload never armed anything | D3: the plan is declared before results, and both arms are separate `ok` lines |
| Console lines lost in transit (field) | D3 plan count; and in the field the stream's own contiguity-from-baseline check |
| Two readers split the stream | D6: the runner never opens the port |
| Preflight skip hides a run that never happened | Every preflight FAILS, each naming a verified cause |
| A gate flashes the ship's bell | D5 class check **plus** the by-id deny-list backstop, no override |
| Two runs race over one board | The `.lock` convention already in the console store |
| The suite exists but nothing calls it | D8: the nsl suite prints the absence and the command |
| **An assertion and the failure it should catch share a cause** | Added 2026-08-05 after `bridge-health` scored 3/3 over total data loss: `changedSince: acked` was empty *because* porta was discarding the rows. No mechanism gates this one — the only defence is to check the assertion against the FAR END once, by hand, the first time it goes green. A green whose evidence was never corroborated off-node is not yet evidence. |

## 7. Out of scope, recorded so it is deferred rather than forgotten

- **The field path itself.** `deploy::push` and `collect::porta` are not built here. D2
  fixes where they plug in; the capability grant that confines a field diagnostic to a diag
  subtree is a separate design.
- **The bell's `watch` mode.** Asserting invariants over a window on a node you cannot
  drive ("every half-hour boundary in the last 12 h was rung", "`pulseDrops` stayed 0") has
  different failure semantics from a gate — absence of evidence is #152's entire problem —
  and is deliberately not merged into this rung.
- **Parallel scenarios across boards.** One board, serial.
- **`.oimg`-push deploy.** D9; #67's measurement earns it.
- **Docker/CI porta.** D10; belongs to #25's fourth item.

## 8. Testing the harness itself

A harness that cannot be falsified is decoration, so each of these is run before the rung
closes, and each must fail **for the stated reason**:

1. Point a scenario at a build id that was never flashed → refuses, naming the anchor.
2. Truncate the collector log after `ok 1` → FAIL on plan mismatch, not a timeout.
3. Deploy with the collector stopped → preflight fail naming the collector, not a 120 s
   timeout with a misleading cause.
4. Mark `stale-handle` as `observe` and target the bell's by-id → refused twice, once by
   class and once by the deny-list.
5. Flip one probe's expected value → that scenario FAILs and the run exits nonzero.
6. **(rete#216)** Point a scenario at a log whose progress line stops after the done
   line → FAIL naming the STALL, not a PASS. Falsifiable without a board, and run by
   the nsl suite: `python3 -m pytest nodus/tuvm/tests/target/test_observe.py`.

### Run 2026-08-05 — all five, and two of them found real defects

| # | outcome |
|---|---|
| 1 | `TIMEOUT: build id NEVER-FLASHED-XYZ never appeared in …` then `FAIL 0/0`, `rc=1`. The message distinguishes *the image did not boot* from *it booted but never finished*, because those have different next steps and one timeout string that cannot tell them apart sends you to the wrong one. |
| 2 | `FAIL 1/2 no done line — the workload did not finish (saw 1 ok, 0 not ok of 2)`. Also checked: a lost middle result where the aggregate would still add up (`plan promised 2 results, 1 arrived`) and a duplicated index where `pass+fail == plan` (`result 1 appeared 2 times`). Both would pass a naive tally. |
| 3 | `FAIL: collector rete-console@c6bench is not running; this harness reads its log and never opens the port`. |
| 4 | **Found a defect.** The refusal came out as `port=down — wrong by-id, or unplugged` — the collector check ran *before* the interlock. Two things wrong: it named a cause it had not verified, and on the bell `port=down` is *healthy*, so the message invites the operator to plug the bell into USB — the one action the interlock exists to prevent. Fixed by moving the board-level refusal ahead of the lock and the collector check. Now: `refusing to deploy to 'c6': …is the ship's bell…`, `rc=1`, whether the manifest says `disruptive` or is mislabelled `observe`. |
| 5 | A never-flashed anchor exits `rc=1` through the full runner, so FAIL→nonzero is wired. Both directions of the class gate checked in isolation: `disruptive` on a non-disposable board refuses; `observe` on the same board passes through. |

**A sixth, which the design did not anticipate and the run did.** Two scenarios back to back
flashed the first and failed the second with `device reports readiness to read but returned
no data`. `esptool` and the collector were both reading the tty — a race both pre-existing
gates carry, which is why neither ever looked broken. Closed in the shared layer, and the
non-obvious half is the sequencing: `esptool` resets the board when it finishes, so the boot
banner — the anchor — is emitted while the collector is still stopped. The board is
therefore reset a second time, deliberately, once there is a reader. A hand-run
`zephyr/run.sh --flash` still races; filed as **#154**.

**A seventh, from `bridge-health`, and the most expensive of them: the gate went GREEN on a
node whose every write was being discarded.** Falsification 5 says "flip an expected value
and watch it fail", which this scenario passed — and it would still have passed while
testifying to nothing, because the assertion (`changedSince: acked` is empty) and the
failure (porta skipping the rows) had the same cause. What caught it was not a harness
mechanism but the habit of checking a first green against the independently written
implementation: `porta get c6-bench obs/health` was returning a record hours old. The
lesson is now a row in §6, and the assertion carries a second clause that has no shared
cause with the first (`acked <= space seq`).

**An eighth, in the deploy of that fix, and it took the gateway down for a minute.**
`ssh gw 'pkill -f "porta serve --db X"'` MATCHES ITS OWN COMMAND LINE: pkill kills the
remote shell mid-script, ssh exits 255, and the statements after it — installing the new
binary, restarting — never run. The pattern was inherited verbatim from the old
`porta_sync_c6.sh`, where `|| true` had been swallowing the same self-kill for months
(the porta process died too, so it "worked"). Fixed with the bracket trick,
`pkill -f "porta serv[e]"` — and then it happened a SECOND time, because the bracket only
helps if the literal appears nowhere else in the command, and the restart half of the same
line carried `./porta serve --db …`. The rule that actually holds: **the kill gets its own
ssh invocation.**

**A ninth: the fix for the seventh was wrong, and the OTHER half of the split caught it.**
Rewinding porta's high-water whenever the node declares a lower `hw` also fires on a
MISSED ACK — where the node is legitimately re-offering rows porta already committed — so
it deleted the `seq <= hw` dedupe and manufactured the duplicate that dedupe exists to
prevent. `bridge-health` passed it; `porta_sync_c6.sh` failed inside one leg with `the
gateway never logged a duplicate-skip`, against 959 before. The discriminator is the
CURSOR, and it is structural rather than heuristic: the cursor is advanced by the batch and
the watermark by the ack, so a swallowed ack cannot move the cursor, while a reboot moves
both. Worth naming twice: the D4.1 split ("the workload asserts what only it can see, the
far end asserts delivery") was argued on paper and then paid out in a case neither half
could have caught alone.

**A tenth, small and old: two fetches where the original had one.** The rewritten
`porta_sync_c6.sh` sampled its baseline row-count and its baseline tick in two `porta log`
calls, and a push landing between them produced `the leg's first tick is 2, not 3 — an
entry was lost` about an entry that was never lost. The script it replaced already carried
this fix *and* its reasoning, in a comment; the rewrite dropped both. Now a
`take_baseline` function, so the two numbers cannot be taken separately.

## 9. Closes / touches

**Closes the last leg of #56.** The runner and all three scenarios are built and green on
hardware: `TARGET 3/3 PASS`, 2026-08-05 (`bridge-health` 3/3, `pulse-coalesce` 3/3,
`stale-handle` 2/2). Touches **#151** (its gate is scenario
1), **#67** (D9 names what would earn the `.oimg` deploy), **#25** (D10 hands docker to its
fourth item), **#118** (a node's gateway address is a source constant, which is why the
field path uses the production porta plus a baseline rather than a private instance),
**#152** and **#153** (both are `watch`-shaped and explicitly out of scope).

**And it closed a defect nobody had filed** (now **#157**, filed after the fact and closed on
arrival, because the tracker is how this program is navigated), because the wipe it deleted
was the thing hiding it: a rebooted node's cells were discarded by porta as already-applied,
silently and permanently. Node side asserts it (`PortaBridgeNodeC6>>noteObsAcked`), porta side fixes it
(`Store.SyncHighWater` + `handlePull` reading the `hw` it had been decoding and ignoring),
and the contract records it (PROTOCOL-NSL, rule 5 of five). Three regression tests in
`porta/internal/wire/wire_test.go` cover the reboot, the seq re-use it implies, and the
absence of a duplicate under a stale pull.

**Blocks and blocked-by, discovered during implementation:**

- **#74 blocks the field `collect`.** A node cannot name its own build, so off the bench
  there is no anchor. See the D3 amendment.
- **#154** — a hand-run `zephyr/run.sh --flash` still races the collector for the tty. The
  harness closes it for its own deploys only.
- **#155** — `run.sh` hardcodes the console instance as `c6`, so a `c6bench` flash
  mislabels `last-flashed-c6`. Small: the ELF archive itself is keyed by build id and is
  correct.

**Promotion on close:** `review/tests/CLAIMS.md` gains the claim that the host suite is
silent about the ISR/mailbox seam and which gate speaks to it;
`docs/next-steps-overview.md` moves #56 to closed and #25's host slice to next.

---

## Addendum — 2026-08-09: D5's deny-list named one of the bell's TWO adapters

**D5 as written is corrected, not merely extended.** The backstop it describes —
"the by-id deny-list, no override" — was `TARGET_DENY_BYID_SUBSTR="4e5729ee"`, the bell's
**CP2102N console** hash. That is the adapter *this harness* flashes over
(`deploy_flash.sh:53` sets both ports from `target::board_byid`). But the hand path,
`zephyr/run.sh`, flashes the C6 over the **native USB-JTAG** node
(`20:6E:F1:19:54:38`) — a different physical channel.

So the constant could not have protected the hand path even if it had been copied there,
which is what rete#185 originally proposed. **A deny-list naming one adapter reads as
protection of the board and is not.** The table in §6 ("A gate flashes the ship's bell")
was true only for gates; it said nothing about the path an operator actually uses at the
bench, and the danger window there — the bell attached while the disposable board is not —
is opened by the *other* board failing, which is precisely when nobody is thinking about
the bell. It was live for about five hours on 2026-08-09 when c6spare's USB-JTAG cable died.

**What replaced it** (`c55668f`, `26af2fe`): `nodus/tuvm/tools/bench-identity.sh` is now the
one home for who-is-who on this bench — both of the bell's identities with the
`bell::is_bell_port` predicate over them, plus the instance↔adapter mapping in **both**
directions. `target::board_byid` and `target::logfile` delegate to it rather than carrying
a second copy of the env-file and log-path layout.

Three decisions worth keeping, because each was reached by being wrong first:

- **Policy lives at the call site, identity in the shared file.** `target.sh` refuses with
  no override; `run.sh` refuses unless `I_REALLY_MEAN_THE_BELL=1`. Keeping the asymmetry
  D5 argued for in the *callers* is what stops this harness from ever inheriting a door it
  must not have.
- **The identity check must run first.** Placed after `run.sh`'s board `case` block, the
  refusal was pre-empted by the console-ambiguity check, whose message is about something
  else entirely. That is the same lesson `target::assert_not_the_bell` learned by
  falsifying itself on 2026-08-05 — recorded there, and then repeated one file away.
- **A resolved deny beats a substring deny.** The live case in rete#185 is an explicitly
  pinned `ESPTOOL_PORT`, and an explicit pin is usually `/dev/ttyACM0` — a name carrying no
  identity. Both sides are resolved to their canonical device node before comparison.

**On testing a safety interlock:** `nodus/tuvm/tools/test_bench_identity.sh` never touches a
board and never issues a flash — its end-to-end cases aim at a port that does not exist, so
a regressed guard fails to open a bogus path rather than writing to the bell. Its first
draft was a **false green**: it built the bell's by-id path out of the constant under test,
and passed cleanly with that constant replaced by a bogus string. A hash is only checked by
being written down twice, by two hands.

### §9 touches, resolved

- **#154** — closed as *recorded rather than fixed*. The race is real; the analysis moved
  into `run.sh` at the flash step, where whoever hits it is already looking. One correction
  to it: the **C6 is not exposed** (it flashes over JTAG and consoles over the CP2102N), so
  it bites the `esp32` and `esp32s2` profiles. Its remaining fix — moving
  `target::collector_pause` and the second reset out of this harness — was blocked only on
  `run.sh` not knowing the instance name, and is now unblocked.
- **#155 / #170** — the same defect, fixed together. `run.sh` resolves the instance from the
  console port instead of hardcoding `c6`. Filed as "small" because the build-id archive is
  correct either way; the consequence was larger, because `last-flashed-c6` was overwritten
  with another board's build id while the bell was mid-soak, and only porta's `obs/build`
  still held the truth.
- **Still open, recorded in #187:** the collector env files name only the console adapter,
  so "flashed one board, read another's console" is undetectable in general. Only the bell's
  half is caught, via its hardcoded JTAG identity — which must stay hardcoded, since a
  backstop that depends on a file in `~/.config` is not one.

## Addendum — 2026-08-15: a cell read taken just after a deploy can blend two boots

Found gating rete#161 on hardware (2026-08-14), and it nearly inverted the finding.

> **Superseded as a rule, kept as the "why" (2026-08-15).** This began as a discipline — *after
> a deploy, read published state from `porta log`, never `porta get`* — and that is no longer
> the defence. `porta get` now marks any row that predates the node's current boot and prints a
> `! MIXED READ` banner, because every stored row records the boot that published it. See
> `porta/docs/design/2026-08-15-boot-anchored-cell-reads-design.md` and
> `porta/docs/ARCHITECTURE.md` §3.4.
>
> The reason it did not stay a rule is worth more than the rule was: rete#210 named the
> documented-discipline option and its own objection to it — it "relies on remembering, which
> is what this project keeps learning does not hold." A harness whose correctness depends on
> the operator having read this paragraph is not a harness.

**What still holds for the harness.** The TAP verdict is unaffected either way — it comes off
the console. Any figure read out of porta afterwards is the exposed part, and the guard is now
in the tool rather than in the reader: a mixed read announces itself, and `porta log --boot N`
is how a single boot is reconstructed when one is needed.

**Why.** `cells` holds the latest value per `(node, path)`, and a rebooted node rewrites its
whole tree over **several seconds**: in a measured run the sixteen `diag/spans/**` rows landed
across an 11 s window (`09:10:31`–`09:10:41`), which begins *after* `collect::console` has
already returned its verdict. A `porta get` taken inside that window answers with a **mixture**
— this boot's values for the paths that have arrived, the previous boot's for the paths that
have not — and nothing in a row says which boot it came from.

`obs/build` cannot guard against this from inside the same snapshot, because `obs/build` is
itself a cell subject to the same window. This is the D7 anchor problem one level out: the
build id anchors the *console*, and it does not anchor a cell read.

**What it cost.** Two builds one commit apart were flashed minutes apart to compare
`pushTrim`. The control's read returned the previous boot's `pushTrim` (`188 / 51 923 µs`,
byte-identical), which would have supported the conclusion that the change costs nothing at
cold boot. Its real figure was `66 / 185 µs` — the opposite conclusion. It was caught only
because two runs of *different binaries* producing identical microsecond timings is
impossible; with two closer-behaving builds it would have passed as data.

**How it was actually fixed, which is not what this addendum first proposed.** The freshness
column sketched here — inferring staleness from the per-cell timestamp — was never needed. The
node already sends a **persisted, monotonic `bootId`** on every envelope (`bid`, added for
rete#162/#117), porta already stored it on `nodes` and `exchanges`, and `Ingest` already held
it at the instant it wrote `cells` and `events`. It was being discarded for exactly the two
tables an operator reads. Recording it makes the answer *exact* rather than inferred, and adds
no concept the schema did not already contain.

Worth carrying past this issue: the first design written after a scare tends to propose a new
signal, when the one that was needed had already been on the wire for two rungs. Check what is
already in hand before adding a mechanism to estimate it.


---

## Addendum — 2026-08-17 (rete#216): the verdict was taken and nothing watched after

**A scenario's TAPs passing was the whole verdict, and that is a false green.** On
2026-08-16 `bridge-health` returned **PASS 3/3 in 22 s** against a build that hung four
ticks later (#213): the gate scores at tick 2, the node dies at tick 6, the board is
released. The same PASS would have been issued for the two boots that died — the healthy
arm's PASS was correct only by luck of which build was under it.

This is the failure mode the whole directory exists to prevent, in the harness itself. D7
already refuses to score against firmware it did not write; nothing refused to score
against firmware that was about to stop.

### What changed

`collect::observe` (`lib/collect_console.sh`) runs **after** a PASS and can turn it into a
FAIL. Two independent questions, because this bench's deaths come in both shapes:

- **PROGRESS** — did the scenario's own output keep arriving? #213 and #192 die *silently*:
  no fault, no panic, no reboot banner, console still `port=up`. No fault grep will ever
  match them, so only a positive predicate can see it. The predicate is the scenario's, not
  the harness's — `bridge-health` watches `tick N misses …`, and `none` is a legal,
  *visible* declaration for a one-shot probe that stops by design.
- **FAULT** — did a loud death appear? Cheap, and it covers the complementary set,
  including for the probes that have no progress line.

`OBSERVE_S` and `OBSERVE_PROGRESS` are **mandatory** in every manifest, exactly as `CLASS`
is. The default that would otherwise be applied silently is the one that produced the false
green, and a new scenario must state its answer rather than inherit one.

### Three decisions worth keeping

- **Not `TIMEOUT_S`.** The manifests describe that as "the outer bound on a workload that
  has already decided, never the thing that decides". Lengthening it would make a slow
  attach and a post-verdict death the same event, reported with the same cause.
- **The window reads only what arrives after the verdict, not from the anchor.** The anchor
  *is* a boot banner and a C6 prints two — a fault scan rooted there would match `rst:0x`
  on every healthy run. Marking the log's end at window start makes a `rst:0x` inside the
  window mean what it says.
- **180 s, and where the number came from.** Every death this bench has recorded lands
  inside it: #192 at tick 2–3 (~6 s), #213 at tick 6 (~12 s), #199 at 2 min 37 s (~157 s,
  the longest). The healthy controls ran to tick 209 and tick 1799, so the separation is
  not subtle. This is a measured window, not a chosen one, and it is re-opened when a death
  outlives it. The two probes carry the same 180 s: no post-verdict death has been recorded
  for either, which is an absence of evidence and not evidence for a smaller number.

### What this cost, and what it bought

Each scenario grows by its window — the three-scenario run goes from roughly 7 minutes to
roughly 16. That is the price of the gate meaning what it says, and it is named here rather
than discovered at the bench.

**And the window is falsifiable without hardware.** It is logic over the collector's log, so
`tests/target/test_observe.py` drives the real bash function against synthetic logs — six
cases including the stall, the fault, the halting probe, and the double boot banner — and
the nsl suite runs it as `target-observe-py`. §8's list above gains it as item 6. The board
is now spent only on what genuinely needs one, which is the first time anything under
`tests/target/` has been exercisable by the host suite.

---

## Addendum — 2026-08-20 (rete#223): the window reached its verdict before the evidence arrived

The #216 window above is right about *when* to stop watching and was wrong about *what it
could claim*. Its stall poll is `OBSERVE_POLL_S`, default **10 s**; the #215 detector's
deadline is derived from the app's own cadence, which for `bridge-health` is `8 × 2000 ms` =
**16 s** plus up to a second of observer tick. On 2026-08-19 that ordering printed

> *"no line matching … **with no fault, no panic and no reboot.** This is #213's signature"*

**eighteen seconds before** `tuvm: WEDGE cond=pulse-undrained overdue=16ms deadline=16000ms`
landed in the same log. The verdict was correct — a node that wedged and rebooted inside an
observe window has died — but **the reason is the part a human acts on**, and this one sent
the reader after #213's unexplained mechanism when the dump naming the condition was three
lines further down. It is the same defect as a green message claiming something it never
checked, wearing red.

Three changes, in the order they depend on each other:

- **The detector's own dump is asked first**, before the generic fault scan. `rst:0x` says
  something died; `cond=pulse-undrained overdue=16ms` says which condition fired and by how
  much, and they are different next steps.
- **The stall message stops asserting an absence it cannot yet know.** It says "no fault
  signature *so far*", names the deadline it may be racing, and points the reader further down
  the log — the rule the `none`-progress scenarios already followed, applied to the branch that
  was still claiming more than it had checked.
- **`OBSERVE_POLL_S` becomes a manifest key**, and `bridge-health` declares **30**, *derived*
  from the node's own `8 × 2000 ms` rather than picked. The manifest says to re-derive it if
  the cadence changes, because it is a multiple and not a constant — the same rule §2 of the
  liveness design applies to the detector itself. A scenario whose node declares a slower
  cadence must raise it, or the window can never observe the detector doing its job.

**The general rule this rung leaves behind: a window that can out-race the instrument it is
watching must either wait for it or decline to describe what it did not see.**

One method note worth keeping. The new key had to be cleared between scenarios — the same
hazard `TAP_NAME` hit once. Rather than add a second hand-maintained list, the host test
asserts the invariant over **every** key any manifest declares, and it was watched failing the
moment `OBSERVE_POLL_S` was introduced. Watching a guard fail is the only way to know it
guards. All of it is falsifiable without a board (10 host tests), which is #216's split doing
its job: the board stayed on its soak.

---

## Addendum — 2026-08-21 (rete#230): a workload that ends must withdraw what it declared

The first full seven-scenario run after #215's detector landed came back **6/7**, and the red
was not a VM defect. `stale-handle` passed both TAPs, printed its done line, and was then
reported dead:

```
tuvm: WEDGE cond=pulse-undrained overdue=801ms deadline=200ms up=4030ms pending=1 … consec=2
tuvm: WEDGE cond=pulse-undrained overdue=801ms deadline=200ms up=4030ms pending=1 … consec=3
```

The probe arms a **25 ms repeater** as a heartbeat — necessarily, since `pumpEvents` blocks
`K_FOREVER` on an empty mailbox and without a standing wake source its control arm parks and
proves nothing — and never cancels it. `main:` returns, nothing drains the mailbox again, and
`8 × 25 ms` is the `deadline=200ms` in the dump. **The detector is right.** `every: 25` is a
statement by the app that it expects to be woken every 25 ms; a node holding a live repeater it
no longer drains is wedged, however deliberately it got there. Wedge → reboot → the probe runs
again → wedge → `consec=3` → **HALT**, so a probe finishing correctly was reported as a death
three times and left the board parked.

Fixed in the probe, in two lines: `timers cancel:` the heartbeat after `tapDone`. That leaves
the node with **no declared repeating timer**, which is the liveness design's §2 blind spot and
exactly where a finished probe belongs — it is no longer asking to be woken, so there is
nothing to be overdue. Not fixed with a manifest key declaring "a post-verdict wedge is
expected here": that is #223's mistake in a new costume, and the window would stop meaning what
it says.

> **The rule, and it binds every scenario written from here: a workload that ends must first
> withdraw every expectation it declared.** A repeating timer is a promise about how often this
> node will be woken. A node that stops keeping a promise it never withdrew is
> indistinguishable — correctly — from one that has failed to keep it.

**This is the third time the instrument and the harness have disagreed about what a stopped
node means**, and the three are worth reading together: #216 (the verdict was taken and nothing
watched after), #223 (the window reached its verdict before the evidence arrived), #230 (the
workload stopped without withdrawing its expectations). Each was found by hardware, and none of
them could have been. The remaining gap is that **nothing asserts the rule** — the other four
probes comply today by luck of how they were written, not by a check.

---

## Addendum, 2026-08-22 (rete#236) — the harness's output contract, and what it cost to break

`deploy_flash.sh`'s header has always stated the rule:

> `deploy::flash` -> prints the build id, and NOTHING else, on stdout.
> Everything diagnostic goes to stderr, because the caller captures stdout.

**The file broke its own rule in one place.** The build ran as `out=$( … 2>&1 )`, folding the
build's stderr into the captured stdout; `$out` was echoed only when the build *failed*, and
otherwise read for exactly one line, the `[archive]` build id. So a build that succeeded
discarded every warning it had emitted.

`zephyr/run.sh` prints `[thread] radio layer not requested — dataset overlay NOT applied` for an
esp32c6 built with no OpenThread, and its own #165 comment records what that costs: *"shipped an
image with no radio: PortaBridgeNodeC6 failed its UDP bind at boot and sat dead for over an
hour."* On 2026-08-22 it fired for `liveness-heartbeat`, was swallowed here, and the node sat
silent for 300 s while the harness reported only `TIMEOUT … never reached its done line` — true,
and naming nothing. One board cycle to rediscover what had already been printed.

**Decision: the merge is removed, and nothing is added.** `run.sh` already routes correctly —
`[archive] <id> -> <path>` is stdout (`tools/console/archive.sh:48`), the `[thread] !` lines are
stderr — and the runner already captures stdout only (`build_id=$(deploy::flash …)`). Deleting
`2>&1` restores the contract end to end.

**Rejected: grepping the build output for known warnings.** A pattern list goes stale pointing at
the warnings someone thought of, and the next warning to matter is by definition the one nobody
wrote a pattern for. Same reason there is no table of board hashes in `target.sh`.

**Rejected: asserting the built `.config` against the manifest.** It sounds stronger and it would
**not have caught this** — the `.config` matched what the manifest asked for, and the manifest
asked for no radio. The manifest was the wrong thing. Worth revisiting for *silent* mismatches,
but it is a different defect. **Check a candidate guard against the actual failure, not against
the class of failure.**

**Cost, measured before choosing.** A full pristine C6 build writes **1041 lines to stdout**
(still captured, still hidden) and **3 to stderr**, all benign CMake info. Three lines a build.

**Gated by `tests/target/test_deploy_output.py`**, which drives the real bash function against a
stub `run.sh` — the warning case failed first with `stderr was: ''`. It also pins the contract's
other half so it cannot be traded away: the build id is still the only thing on stdout. The suite
discovers the file with no registration (`target-observe-py` 10 → 14 tests).

### The consequence, and why it is filed rather than patched

Restoring the channel immediately put a benign warning on **every** run — the collector this
harness pauses deliberately, which `run.sh` correctly reports as "not running" before the flash.
It is not silenced: the same warning is load-bearing on the hand-deploy path, where nobody resets
the board again. The caller knows the pause was intentional and the callee cannot, so the fix is
about who declares it — a decision, filed as **#237**.

**The general rule this leaves behind: a signal restored is not a signal heard.** After "can it be
seen?" comes "does seeing it still mean something?", and a warning that always means nothing
devalues every warning that travels beside it.
