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

### The finding: the wipe was never necessary

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

> **What actually shipped, 2026-08-05: the runner and TWO of the three.** `stale-handle` and
> `pulse-coalesce` are green on hardware (`TARGET 2/2 PASS`). **`bridge-health` is not
> built** — it is the one that needs `porta_sync_c6.sh` decomposed, and the decomposition is
> the part of this design I was least sure of. Building the runner against two dissimilar
> workloads first was the smaller step; the third arrives with the split, or the split
> proves harder than expected and that is a finding worth having on its own.
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

### What happens to `porta_sync_c6.sh`

Its legs 4–8 — wipe the db, start a gateway, inject ack drops, diff the gateway's
cumulative counters — are a **bench integration test of porta**, driven from privilege no
field caller has. They are not a device diagnostic and do not become scenarios. The script
stays, is relabelled for what it tests, and loses its private console reader (D6). By D4's
delta assertions its db wipe should also be removable; that is expected to shrink it
substantially and is **not** assumed here — if the wipe turns out to be load-bearing for a
reason not yet seen, that is a finding to write down rather than a step to force.

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

## 9. Closes / touches

Closes the last leg of **#56**. Touches **#151** (its gate is scenario 1), **#67** (D9
names what would earn the `.oimg` deploy), **#25** (D10 hands docker to its fourth item),
**#118** (a node's gateway address is a source constant, which is why the field path uses
the production porta plus a baseline rather than a private instance), **#152** and **#153**
(both are `watch`-shaped and explicitly out of scope).

**Promotion on close:** `review/tests/CLAIMS.md` gains the claim that the host suite is
silent about the ISR/mailbox seam and which gate speaks to it;
`docs/next-steps-overview.md` moves #56 to closed and #25's host slice to next.
