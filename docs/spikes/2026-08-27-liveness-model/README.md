# The liveness contract, model-checked (2026-08-27)

A TLA+ model of ONE node and porta over a channel that may drop anything, written to
find the holes the two halves cannot see in each other. Companion to the diagrams in
`docs/spikes/2026-08-27-liveness-*.png` and to rete#269 / #270.

```
TLA_JAR=/tmp/tla2tools.jar ./run.sh      # nine configs, each naming the answer it expects
```

**Every config names the answer it expects and the runner compares** — a mismatch in
EITHER direction is the finding. Eight are expected to report a violation; `sound.cfg`
is expected to complete, and it is the only set of numbers found so far under which the
contract holds. (This file previously said "all three are expected to violate". That was
true of the three configs that existed and became false the moment a rule that holds was
found — the runner now carries the expectation, so it cannot go stale silently again.)

The `Rule` constant picks which verdict porta applies, and all three live in one spec so
the shipped rule stays runnable beside its replacement:

| `Rule` | the test | whose |
|---|---|---|
| `"arrival"` | `now - last_seen <= bud` | what porta ships (`sweep.go:185`) |
| `"sweepDelta"` | that, AND `wak` advanced between the last two sweeps | rete#272, read as a delta |
| `"progressAge"` | `now - last_PROGRESS <= bud` | rete#272, read as an age |

Units are scaled: one model step = 2 s of wall clock, so the bench node's 2 s cadence is
`Cad = 1`. Every constant names its wall-clock original in the `.cfg`.

## The H-labels, and where each one now lives

`H1`..`H8` were coined in this review and appear in `2026-08-27-liveness-4-holes.png`, in
this README, and in the commit messages. **They are not a numbering scheme with any
standing of its own** — every one now resolves to an issue, and the issue is the record:

| label | the hole | issue |
|---|---|---|
| H1 | the declared cadence is a promise nothing checks — 4.007 s delivered against 2000 declared | [#271](https://github.com/davidg238/rete/issues/271) |
| H2 | porta cannot honour a budget shorter than its own 30 s sweep | [#273](https://github.com/davidg238/rete/issues/273) |
| H3 | the verdict cannot see a reboot loop — it reads arrival, never `boot_id` | [#272](https://github.com/davidg238/rete/issues/272) |
| H4 | two independent eights that happen to agree | [#273](https://github.com/davidg238/rete/issues/273) |
| H5 | `misses` rides the push leg — the leg that is failing | [#272](https://github.com/davidg238/rete/issues/272) |
| H6 | `startTicking:` is downstream of the socket bind, so a failed bind leaves no detector | [#274](https://github.com/davidg238/rete/issues/274) |
| H7 | the node never acts on its own silence | [#272](https://github.com/davidg238/rete/issues/272) |
| H8 | porta's deafness is modelled, the node's is not | [#272](https://github.com/davidg238/rete/issues/272) |

**Eight labels, three defects.** H3/H5/H7/H8 are one thing — porta judges ARRIVAL where
the node judges PROGRESS, which is rete#269's own defect one layer up, and the progress
counter is already on the wire (`obs/health.wak` = the bridge's `ticks`). H1/H2/H4 are one
thing — four deadlines chosen independently that nothing derives from each other. H6 is a
small ordering bug and stands alone.

## What the rete#272 change found (2026-08-28)

The issue's next action was "change `DoSweep` to require that `wak` advanced". There are
**two readings of "advanced"**, and the model separates them — which is the whole reason
to run it before writing Go.

### 1. As a between-sweeps DELTA — insufficient, and it fails the same way rete#269 did

`bench-delta.cfg` still violates `NeverStaleLive`, at the same depth as `bench.cfg`:

```
State 30: DoSweep   state = "wedged"  sinceProgress = 8   seenWak = 4  sweepWak = 0  -> verdict = "live"
State 31: Tock      state = "wedged"  sinceProgress = 9                                verdict = "live"
```

The counter DID advance — four ticks in, early in a 30 s window the node spent its last
7 steps wedged. **Comparing two samples 30 s apart says the node progressed somewhere in
that interval, not that it is progressing now**, which is rete#269's aliasing argument one
more level up: a delta over the wrong window is another point sample wearing a monotonic
counter's clothes.

### 2. As an AGE — the right signal, and the residue is arithmetic, not signal choice

`progressAge` (`now - last_progress <= bud`, where `last_progress` moves only when the
stored `wak` INCREASES) is the exact analogue of the node's own `pulse_drained()` deadline.
`bench-age.cfg` and `slow-age.cfg` still violate — but the residue is now a pure
inequality between four numbers rather than a wrong variable, and it is entirely rete#273:
porta's worst-case verdict age is `Bud` plus one `Sweep` of sampling latency, and nothing
requires that sum to fit inside the deadline the node enforces on itself.

### 3. The relation that makes it hold — `sound.cfg` is GREEN

```
Bud + Sweep <= Cad * Slack
```

`sound.cfg` is `bench.cfg`'s node and detector with `Bud = 4` (8 s) and `Sweep = 4` (8 s)
against `Cad * Slack = 8` (16 s). **3 167 616 distinct states, no violation.** Today's
numbers are `16000 + 30000 = 46000` against a 16000 ms deadline — off by 2.9x, which is
why the verdict can read `#live` about a node its own detector is about to reboot.

Two checks on that green, because a green invariant is exactly the false result this
program keeps meeting:

- **Not vacuous.** `sound-vacuity.cfg` runs `LiveIsReachable == verdict # "live"` and it
  IS violated — porta really does say `live` in that state space.
- **Not a saturation artifact.** The counters saturate at `Cap`, which can only suppress
  `live` and so could hide a violation. Green at `Cap` = 12, 24 and 40.

**The reboot loop falls out of the age rule for free**, and without reading `boot_id`:
`wak` is per-boot, so a rebooted node comes back BELOW the value porta holds, the stored
value never increases, and the age keeps climbing. One caveat the model does surface: the
comparison is on raw counter values, so a boot that climbs past the last stored sample
before the next sweep does clear the age. That is a real hole in the raw-`wak` test, and
it is bounded by the same `Bud + Sweep` sum.

## What the original run found

### 1. The model was wrong first, and the board corrected it

`CheckIn` originally advanced the next tick by `Cad + 2*ReplyBudget`, which predicts a
**6 s** delivered period under a silent uplink. c6spare measured **4.007 s**. The model
was wrong: the base tick is a *periodic* timer whose posts coalesce, so a tick that
overruns its own period does not push the next one out — the delivered period is
`max(Cad, work)`, not `Cad + work`. `max(2000, 2 x 2000) = 4000`. The spec now says so,
and the fact is written down where it was previously only implicit in the coalescer.

This is the exercise earning its keep before it proved anything: it forced a question
the prose had never asked.

### 2. H1 — the delivered period is not the declared one (`h1.cfg`, 3 states)

`DeliveredMatchesDeclared == tickIn <= Cad` is violated as soon as one reply goes
missing: `tickIn = 2`, i.e. 4 s delivered against 2 s declared. Every deadline
downstream — the wedge deadline, the published budget — is still derived from the 2 s.

### 3. H2 + H3 + H4 are ONE defect, and the trace says so (`bench.cfg`, 31 states)

`NeverStaleLive == (verdict = "live") => (sinceProgress <= WedgeDeadline)` — porta must
not call a node live while that node has made no progress for longer than the deadline
the node itself published. Violated:

```
State 30: DoSweep      state = "wedged"   sinceSeen = 8   sinceProgress = 8   -> verdict = "live"
State 31: Tock         state = "wedged"   sinceSeen = 9   sinceProgress = 9      verdict = "live"
                                                                                 sweepIn = 14
```

The sweep fires at the last instant `sinceSeen <= Bud` still holds and writes `#live`.
One step later the node is past its own wedge deadline — and the verdict stands
unchallenged for another 14 steps (28 s), because it is only re-formed every `Sweep`.

The three holes are the same arithmetic seen from three sides:

- **H4** `Bud` and `WedgeDeadline` are both 16 s and neither derives from the other
  (`cadence x 8` in nsl; `WEDGE_SLACK` in C). Equal deadlines leave no margin at all.
- **H2** the verdict is re-formed every 30 s, so it is stale by up to `Sweep` — which is
  nearly twice the budget it is enforcing.
- **H3** what porta measures is `sinceSeen`, an arrival time. What an operator reads it
  as is `sinceProgress`, a health fact. They are different variables in this model
  because they are different things in the system, and nothing joins them.

`exchanges.boot_id` is NOT NULL on every row and would close the gap between them; the
verdict never reads it.

### 4. The same property fails wider when the budget outgrows the deadline (`slow.cfg`)

`slow.cfg` gives the node a budget far longer than its wedge deadline, which is the shape
the compiled-in default produces (`kernel.nsl:1705`: cad 30000 / bud 240000 — a budget
8x the cadence, against a wedge deadline also 8x the cadence, but measured from a
different event). The violation arrives at depth 29, **before any reboot** (`boots = 0`):

```
State 29: DoSweep      state = "wedged"   sinceSeen = 11   sinceProgress = 9   -> verdict = "live"
                                          Bud = 30         WedgeDeadline = 8
```

Here the staleness is not a sampling artifact — the whole gap between the two numbers is
margin in which a wedged node reads `#live`. What the model does NOT show, because
nothing in it drives the node to loop, is the reboot-loop case; that one is arithmetic
rather than a trace: a node whose wedge-reboot cycle keeps arrivals inside `Bud` never
crosses the only test porta applies.

## What is deliberately not modelled

The CBOR shapes, the store, cursors and replay, and more than one node. This model is
about time and health; anything that does not bear on those is state space paid for
twice. It is also a model of the *contract*, not of the code: it can show that the rules
admit a bad run, never that an implementation follows the rules.
