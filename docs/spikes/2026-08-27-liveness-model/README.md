# The liveness contract, model-checked (2026-08-27)

A TLA+ model of ONE node and porta over a channel that may drop anything, written to
find the holes the two halves cannot see in each other. Companion to the diagrams in
`docs/spikes/2026-08-27-liveness-*.png` and to rete#269 / #270.

```
TLA_JAR=/tmp/tla2tools.jar ./run.sh      # three configs, three questions
```

**All three configs are expected to report a violation.** Each one is a hole and the
trace is the evidence; a green run means something upstream changed, so read the trace
before believing it.

Units are scaled: one model step = 2 s of wall clock, so the bench node's 2 s cadence is
`Cad = 1`. Every constant names its wall-clock original in the `.cfg`.

## What it found

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
