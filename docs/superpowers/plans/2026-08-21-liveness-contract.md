# The liveness contract — implementation plan (#231)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every node declares a **silence budget** — the longest it may go without speaking —
at `obs/liveness`, and porta's verdict becomes `now - last_seen > bud`, so that nothing in this
system judges a node against a number the node never declared.

**Architecture:** Two new pinned paths and no new tables on the node side of the wire:
`obs/liveness` (the node's declaration, node authority) and `goal/liveness` (the operator's
*request*, which the node may snap or clamp — the `goal`/`obs` diff is the answer). porta stops
inferring cadence from exchange history and reads the declaration instead, which **deletes**
`CadenceMs`, `CadenceWindow`, `MinExchanges`, `SilenceFactor` (`k = 3`), `nodes.cadence_ms` and
`exchanges.leg`. One new gateway-side table, `gw_runs`, records the intervals porta was actually
listening, so a verdict formed across a gateway outage reads `#unknown` instead of a confident
lie. The node-side kernel heartbeat (D1/D3) lands last, on a board.

**Tech Stack:** nsl (`nodus/nsl/lib/kernel.nsl`, the `PortaBridge` class) compiled to `.oimg` and
run on tuvm; Go 1.26 + sqlite (`github.com/mattn/go-sqlite3`) + cobra in `porta/`; bash for the
host interop gate and the target harness.

**Spec:** `docs/design/2026-08-21-liveness-contract-design.md` — APPROVED 2026-08-21, D1–D7, and
§7's staging, which this plan's task order follows exactly. Read §4 ("what this does NOT solve")
and §5 (constants) before writing any code. Secondary, and all three are load-bearing here:
`docs/design/2026-08-10-node-memory-and-liveness-design.md` §6/§8 (what D4 reverses),
`docs/design/2026-08-16-vm-liveness-observer-design.md` §5 (the #215 detector this generalises),
`porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md` (D2/D4.1/D4.5, the store rules).

## Global Constraints

Every task's requirements implicitly include this section.

- **Two halves, one wire.** porta and the node **never** import each other's code. `protocol/`
  is the whole of the contract. A change that makes one side compile against the other is a bug.
- **Go 1.26.** Copyright header `// Copyright (c) 2026 Ekorau LLC` on **every new Go file**.
  `gofmt -l .` must print nothing (run from `porta/`) before every commit.
- **All values on the wire and in the store are raw deterministic CBOR** (D4.1). The one decode
  this plan adds is `Store.DeclaredBudget`, and it is a decode for *judgement*, not for display —
  it is called once per node per 30 s sweep tick and its failure mode is a skipped verdict, never
  a panic. Verdict comparison stays byte-wise against pre-encoded symbols.
- **All timestamps are milliseconds** (D4.2). All paths are **node-relative** (D4.5).
- **There is ZERO backward compatibility to maintain on the wire** — both ends are reflashed
  together. But **the live soak store on gw85224-01 must not be rebuilt**: it holds the history
  #190's evidence rests on, and rebuilding forces every node through a full re-push. Schema
  changes are in-place and idempotent, exactly as `addColumns` already is.
- **nsl dialect traps** (each has cost this program a session):
  - **no modulo** — integer division is `//`, so "round up to a multiple" is
    `((n + m - 1) // m) * m`.
  - **no `max:` / `min:` on Integer** — write the comparison out with `ifTrue:`.
  - A reified block (`whenever:do:`) **cannot resolve its outer chain** on this VM (#133), so a
    reaction body must be a single self-send.
  - `platform timers` answers a **fresh instance** each send — hold exactly ONE per image.
  - Never hand-build a Symbol (#226): a Symbol carries its own prim id.
- **Gates.** Before every commit that touches nsl:
  `cd nodus/nsl && python3 tools/run_tests_oevm.py` — `Failed: 0` **and** read the aux-gate line,
  which can be red while the count reads zero. `footprint: SKIP (no arm-zephyr-eabi toolchain)`
  is expected. Never run two nsl suites concurrently. Before every commit that touches Go:
  `cd porta && gofmt -l . && go test ./...`. Before the final commit of each of Tasks 5, 7 and 9:
  `cd nodus/tuvm && bash tests/porta_sync_host.sh` must print `PORTA SYNC OK`.
- **The host suite is blind to the ISR seam and to more than that** — a pure codec change once
  passed 265/0 plus `PORTA SYNC OK` and hung a C6 in six check-ins. Task 10 is the board task and
  it is not optional for closing #231; Tasks 1–9 close on host evidence alone and must say so.
- **Verbs live only at the CLI.** This plan adds no subcommand, no endpoint, no store table keyed
  by a feature (`gw_runs` is argued as the one exception, Task 8), and no wire verb.

## Constants this plan pins

The design's §5 tables two (the 24 h ceiling, and #215's `8 ×` slack). This plan must pin two
more to be runnable, and they are recorded here in the same form so the deferral is falsifiable.
**Both are node-side defaults that a declaration overrides, and both are deleted from the story
the moment Task 10 lands** — after which `cad` is the kernel's own base tick.

| Constant | Value | Standing |
|---|---|---|
| default `cad` for an app that declares nothing | **30 000 ms** | **Chosen by this plan.** The slowest cadence on this bench (`c6-bell`). Declaring a *slower* cadence than the node actually keeps is always safe — `cad` is an intent to speak at least that often, and porta judges only `bud` — so the conservative default cannot manufacture a `#silent`. Earns revision when a node exists whose real cadence is slower than 30 s and which declares nothing, at which point the default is wrong in the dangerous direction |
| default `bud` | **`8 × cad`** | **Chosen by this plan, reusing #215's measured slack rather than inventing a second multiplier.** This is *not* D4's deleted `k`: `k` was the **gateway** multiplying someone else's declaration by a number the gateway picked; this is the **node** choosing its own budget with its own loss rate in hand, and what reaches porta is one number with no multiplication left in it. 8 consecutive misses covers `c6-bell`'s measured ~50 % check-in loss with room. Earns revision from a node whose measured loss rate makes 8 too tight or absurdly loose |

## File structure

| File | Fate | Responsibility |
|---|---|---|
| `nodus/nsl/lib/kernel.nsl` (class `PortaBridge`) | modify | the `cadMs`/`budMs` slots, `liveness:budget:`, the grid/clamp helper, `publishLiveness`, and (Task 6) `applyLivenessGoal` |
| `nodus/tuvm/zephyr/PortaBridgeNode.nsl` | modify | the host twin declares its 2 s cadence |
| `nodus/tuvm/zephyr/PortaBridgeNodeC6.nsl`, `ProfileCostNodeC6.nsl`, `ShipsBellAlarmNodeC6.nsl` | modify | each bridged app declares its own tick period |
| `nodus/tests/22_spacesync/17_liveness_declared.nsl` | create | the record is published, once per boot, with the declared values |
| `nodus/tests/22_spacesync/18_liveness_grid.nsl` | create | the grid snap, the `bud >= cad` floor and the 24 h ceiling |
| `nodus/tests/22_spacesync/19_liveness_goal.nsl` | create | `goal/liveness` is read, snapped, re-published; a wrongly kinded leaf is a log line |
| `nodus/tests/22_spacesync/20_liveness_size.nsl` | create | the encoded size of the record, measured and pinned |
| `porta/internal/space/store.go` | modify | `PathDeclared`, `DeclaredBudget`; **delete** `CadenceMs`, `medianMs`, the three constants |
| `porta/internal/space/sweep.go` | modify | four verdicts judged on `bud`; `LivenessChange` loses `CadenceMs` |
| `porta/internal/space/schema.go` | modify | `gw_runs`; **drop** `nodes.cadence_ms` and `exchanges.leg` |
| `porta/internal/space/runs.go` | create | `StartRun`, `MarkAlive`, `WasDeaf` — the gateway's own deaf intervals (D6) |
| `porta/internal/portacli/serve.go` | modify | open a run, mark alive each tick, log the four verdicts |
| `porta/internal/wire/server.go` | modify | `RecordExchange` loses its `leg` argument |
| `nodus/tuvm/tests/porta_sync_host.sh` | modify | legs 9–11: the declaration arrives, the verdict is formed from it, the goal converges |
| `nodus/tuvm/zephyr/TimerlessNodeC6.nsl` | create (Task 10) | a node with **no** repeating app timer, which the #215 detector cannot see today |
| `nodus/tuvm/tests/target/liveness-heartbeat.env` | create (Task 10) | the board scenario |

---

## Task 1: the `Liveness` record, published from a compiled-in default

Design §7 step 1. Nothing judges on it yet — this proves the path, the shape and the size before
anything depends on the number, for the same reason the gauge landed before the fix.

**Files:**
- Modify: `nodus/nsl/lib/kernel.nsl` (class `PortaBridge`: the slot list ~line 1623, and beside
  `publishApps` ~line 2350)
- Test: `nodus/tests/22_spacesync/17_liveness_declared.nsl` (create)

**Interfaces:**
- Consumes: nothing.
- Produces: `PortaBridge>>cadMs` → Integer, `PortaBridge>>budMs` → Integer,
  `PortaBridge>>publishLiveness` → self (called from `checkIn`), and the cell
  `obs/liveness` holding `Liveness { cad: int, bud: int }`.

- [ ] **Step 1: Write the failing test**

Create `nodus/tests/22_spacesync/17_liveness_declared.nsl`:

```
(*
porta space-sync — the node DECLARES how long its silence may last (rete#231 D2).

The record is built and written by the BRIDGE, not by each app, for the reason
publishMetrics gives: every bridged app inherits the declaration without
re-implementing it, and obs/health's three mutually inconsistent definitions are
what happens when it is left to the apps.

WRITTEN ONCE PER BOOT, on obs/apps/main's precedent (#25): the value cannot change
without an app declaration or a goal arriving, and `put:` refreshes the cell's seq
unconditionally — so a per-check-in write would re-send this row on EVERY push
forever, against measured headroom, and pushFrame trims from the TAIL.

The socket is nil, so no I/O is attempted; publishLiveness is the seam under test.
expect:
tag true
cad 30000
bud 240000
published once true
seq advanced true
*)
class LivenessDeclared packageUsing: manifest = () (
  public main: platform args: args = (
    | space bridge before rec seqAfterFirst |
    space:: platform space.
    before:: space stamp.
    bridge:: platform PortaBridge socket: nil space: space id: 'test' peer: '::1' port: 1
      spans: platform Spans new.

    bridge publishLiveness.
    rec:: (space channel: 'obs/liveness') peek: '' ifAbsent: [ nil ].
    ('tag ' , (rec tag = #Liveness) printString) println.
    ('cad ' , (rec at: #cad ifAbsent: [ -1 ]) printString) println.
    ('bud ' , (rec at: #bud ifAbsent: [ -1 ]) printString) println.

    (* ONCE PER BOOT: a second send must not re-stamp the cell, or the row rides
       every push for the life of the node. *)
    seqAfterFirst:: space stamp.
    bridge publishLiveness.
    ('published once ' , (space stamp = seqAfterFirst) printString) println.

    (* The write must STAMP, or the cell exists locally and never rides a push:
       collectOutbound scans `changedSince: acked`. *)
    ('seq advanced ' , (seqAfterFirst > before) printString) println.
  )
) : (
)
```

- [ ] **Step 2: Run it and watch it fail**

```bash
cd /home/david/workspace/rete/nodus/nsl && python3 tools/run-one.py ../tests/22_spacesync/17_liveness_declared.nsl B
```

Expected: FAIL — `publishLiveness` does not exist (a `doesNotUnderstand`-class abort). Pass the
literal `B`; the default bakes Path A **over the tracked fixture**. A CRASH discards every line
already printed, so an empty diff here is the expected failure and not a runner problem.

- [ ] **Step 3: Add the slots**

In `nodus/nsl/lib/kernel.nsl`, class `PortaBridge`'s slot list (beside `otaStaged ::= false.`),
add:

```
      (* rete#231 D2 — the liveness contract. `cad` is how often this node INTENDS
         to speak; `bud` is the longest silence that is still healthy, and it is the
         ONLY number the gateway judges against (`now - last_seen > bud`). The gap
         between them is this node's own allowance for missed datagrams — c6-bell
         misses roughly half its check-ins to ordinary mesh loss, and the node is the
         only party that knows its own loss rate.

         The defaults are compiled in so that a node which has never heard from porta
         still has a budget, which is what makes a never-seen node judgeable at all.
         30 s is the slowest cadence on this bench; declaring a cadence SLOWER than
         the node keeps is always safe, because porta judges `bud` and speaking more
         often than declared violates nothing. `8 x` is #215's measured wedge slack,
         reused rather than a second multiplier invented — and note this is the node
         multiplying its OWN number, not the gateway multiplying someone else's,
         which is precisely the k=3 that rete#231 D4 deletes.

         `livenessPublished` is the obs/apps/main rule (#25): written once per boot,
         because a cell that never changes must not be allowed to crowd out the one
         that does under pushFrame's tail-trim. A declaration or an accepted goal
         clears it, which is the only way the value can move. *)
      cadMs ::= 30000. budMs ::= 240000. livenessPublished ::= false.
```

- [ ] **Step 4: Add `publishLiveness` and the accessors**

Immediately after `publishApps` in class `PortaBridge`:

```
    public cadMs = ( ^ cadMs )
    public budMs = ( ^ budMs )

    (* rete#231 D2 — the node's own authority, on the node's own path. porta writing
       obs/** would be a lie (2026-08-10 D7), so the operator gets goal/liveness and
       a tree diff instead (Task 6); the node always wins.

       Written once per boot for the reason publishApps gives at length: `put:` does
       not compare values, so a per-check-in write would re-send an unchanging ~30 B
       row on every push forever, and the row pushFrame's tail-trim starts dropping
       is whatever was written last. *)
    public publishLiveness = (
      livenessPublished ifTrue: [ ^ self ].
      space put: 'obs/liveness' value: (Record tag: #Liveness
        with: #cad -> cadMs
        with: #bud -> budMs).
      livenessPublished:: true.
      ^ self )
```

- [ ] **Step 5: Call it from `checkIn`**

In `PortaBridge>>checkIn`, in the publish group, immediately **after** `self publishApps.` and
**before** `self publishMetrics.`:

```
      self publishLiveness.
```

Add to the ordering comment already there (the one that explains `publishApps` before
`publishMetrics`) one sentence:

```
         obs/liveness joins that group and for the same reason — it is written once
         per boot, so it must be ahead of the per-check-in rows that can afford to
         lose a sample.
```

- [ ] **Step 6: Run the test and watch it pass**

```bash
cd /home/david/workspace/rete/nodus/nsl && python3 tools/run-one.py ../tests/22_spacesync/17_liveness_declared.nsl B
```

Expected: PASS, all five lines matching the `expect:` block.

- [ ] **Step 7: Run the full suite**

```bash
cd /home/david/workspace/rete/nodus/nsl && python3 tools/run_tests_oevm.py
```

Expected: `Failed: 0`, one more test than before, and the aux-gate line free of new red.

- [ ] **Step 8: Commit**

```bash
cd /home/david/workspace/rete
git add nodus/nsl/lib/kernel.nsl nodus/tests/22_spacesync/17_liveness_declared.nsl
git commit -m "feat(#231): a node says how long its silence may last, from a default nothing reads yet

Design step 1: the record and the path, published once per boot from a
compiled-in default. Nothing judges on it — a number nothing reads is a
number nobody has checked, so the gauge lands before the judgement."
```

---

## Task 2: the app declares, and the declaration is clamped to a grid

Design D3 (every period an integer multiple of the base) and D7 (a 24 h ceiling on the
declaration, **clamped and published**, never rejected — so an app asking for a week shows up as
a `goal`/`obs` divergence rather than as a silent refusal).

**Files:**
- Modify: `nodus/nsl/lib/kernel.nsl` (class `PortaBridge`, beside `publishLiveness`)
- Modify: `nodus/tuvm/zephyr/PortaBridgeNode.nsl:52-60`, `PortaBridgeNodeC6.nsl` (after the
  bridge is constructed), `ProfileCostNodeC6.nsl`, `ShipsBellAlarmNodeC6.nsl`
- Test: `nodus/tests/22_spacesync/18_liveness_grid.nsl` (create)

**Interfaces:**
- Consumes: `cadMs`, `budMs`, `livenessPublished` from Task 1.
- Produces: `PortaBridge>>liveness: aCadMs budget: aBudMs` → self (idempotent; re-publishes only
  when the adopted pair actually changes), and `PortaBridge>>livenessCeilingMs` → `86400000`.

- [ ] **Step 1: Write the failing test**

Create `nodus/tests/22_spacesync/18_liveness_grid.nsl`:

```
(*
porta space-sync — what the node ADOPTS is not always what it was asked for
(rete#231 D3/D7), and the divergence is the answer rather than an error.

Three rules, each of which the gateway must never have to know in advance:
  bud >= cad          — a budget shorter than the cadence promises a silence the
                        node cannot keep even when perfectly healthy
  bud on the grid      — every period is an integer multiple of the base (D3), so
                        obligations land on the same radio wakeup by construction
  bud <= 24 h          — CLAMPED and published, never rejected (D7): a managed
                        fleet means no node can make itself unjudgeable, and a
                        silent refusal would be indistinguishable from acceptance

The socket is nil; `liveness:budget:` is the seam under test.
expect:
plain 2000 16000
floor 2000 2000
grid 2000 46000
ceiling 30000 86400000
republished true
idempotent true
*)
class LivenessGrid packageUsing: manifest = () (
  bridgeOn: platform = (
    ^ platform PortaBridge socket: nil space: platform space id: 'test' peer: '::1' port: 1
        spans: platform Spans new )

  show: label bridge: b = (
    (label , ' ' , b cadMs printString , ' ' , b budMs printString) println.
    ^ self )

  public main: platform args: args = (
    | b seqBefore |
    (* An ordinary declaration passes through untouched: 8 x 2 s is already a
       whole number of cadences. *)
    b:: self bridgeOn: platform.
    b liveness: 2000 budget: 16000.
    self show: 'plain' bridge: b.

    (* A budget SHORTER than the cadence is raised to it. *)
    b:: self bridgeOn: platform.
    b liveness: 2000 budget: 500.
    self show: 'floor' bridge: b.

    (* 45 s is not a whole number of 2 s cadences: the node snaps UP to 46 s and
       publishes that. Nothing had to know the legal values in advance. *)
    b:: self bridgeOn: platform.
    b liveness: 2000 budget: 45000.
    self show: 'grid' bridge: b.

    (* A week is clamped to the ceiling, on the grid: 86400000 is 2880 x 30 s. *)
    b:: self bridgeOn: platform.
    b liveness: 30000 budget: 604800000.
    self show: 'ceiling' bridge: b.

    (* A declaration that MOVES the pair must re-publish; one that repeats it
       must not, or the row rides every push forever. *)
    b:: self bridgeOn: platform.
    b publishLiveness.
    seqBefore:: platform space stamp.
    b liveness: 2000 budget: 16000.
    b publishLiveness.
    ('republished ' , (platform space stamp > seqBefore) printString) println.
    seqBefore:: platform space stamp.
    b liveness: 2000 budget: 16000.
    b publishLiveness.
    ('idempotent ' , (platform space stamp = seqBefore) printString) println.
  )
) : (
)
```

- [ ] **Step 2: Run it and watch it fail**

```bash
cd /home/david/workspace/rete/nodus/nsl && python3 tools/run-one.py ../tests/22_spacesync/18_liveness_grid.nsl B
```

Expected: FAIL — `liveness:budget:` does not exist.

- [ ] **Step 3: Implement the declaration and the grid**

In `nodus/nsl/lib/kernel.nsl`, class `PortaBridge`, immediately before `publishLiveness`:

```
    (* rete#231 D7 — the one invented constant in the design, and it is a ceiling on
       the DECLARATION rather than a cadence: a node speaking once a month is free to
       try, it simply cannot promise a month of silence and still be considered
       healthy. 24 h is the coarsest granularity at which a fleet is still a fleet. *)
    livenessCeilingMs = ( ^ 86400000 )

    (* rete#231 D2/D3/D7 — the ONE place a liveness pair is adopted, whether it came
       from the app (here) or from porta (applyLivenessGoal). Two entry points and
       one rule, because a snap that differs between them is a grid the operator
       cannot learn by asking.

       CLAMPED, NEVER REJECTED. Every path out of this method adopts something and
       publishes it, so `goal/liveness` versus `obs/liveness` always answers "did the
       node accept?" — a silent refusal would read exactly like acceptance.

       NSL HAS NO MODULO, so "round up to a whole number of cadences" is spelled
       ((n + m - 1) // m) * m. *)
    adoptCad: aCadMs bud: aBudMs = (
      | cad bud ceiling |
      ceiling:: self livenessCeilingMs.
      cad:: aCadMs.
      cad < 1 ifTrue: [ cad:: 30000 ].
      cad > ceiling ifTrue: [ cad:: ceiling ].
      bud:: aBudMs.
      bud < cad ifTrue: [ bud:: cad ].
      bud:: ((bud + cad - 1) // cad) * cad.
      (* The clamp comes AFTER the snap and lands back on the grid: a ceiling that
         is not a whole number of cadences would publish a value the node's own rule
         says is illegal. `cad <= ceiling` above is what keeps this >= cad. *)
      bud > ceiling ifTrue: [ bud:: (ceiling // cad) * cad ].
      (cad = cadMs and: [ bud = budMs ]) ifTrue: [ ^ self ].
      cadMs:: cad.
      budMs:: bud.
      (* The pair moved, so the published cell is stale. Clearing the flag is the
         whole of the re-publish mechanism — no second path, no dirty list. *)
      livenessPublished:: false.
      ^ self )

    (* The app's declaration. It is a SETTER and not a constructor argument on
       purpose: Task 10 moves the base tick into the kernel, at which point the
       cadence half of this call goes away, and a sixth keyword at thirteen
       construction sites would have to go with it. *)
    public liveness: aCadMs budget: aBudMs = ( ^ self adoptCad: aCadMs bud: aBudMs )
```

- [ ] **Step 4: Run the test and watch it pass**

```bash
cd /home/david/workspace/rete/nodus/nsl && python3 tools/run-one.py ../tests/22_spacesync/18_liveness_grid.nsl B
```

Expected: PASS, six lines matching.

- [ ] **Step 5: Every bridged app declares its own period**

Five apps construct a `PortaBridge`. Each one that drives `bridge checkIn` from a timer declares
that timer's period; one that never checks in keeps the default. Add immediately after the
`bridge subscribeStream:` lines in each:

```
    (* rete#231 D2: this node's own promise about its radio. The budget is 8 x the
       cadence — #215's measured slack, chosen HERE by the node rather than by the
       gateway, because the node is the only party that knows its own loss rate. *)
    bridge liveness: self cadence budget: self cadence * 8.
```

- `nodus/tuvm/zephyr/PortaBridgeNode.nsl` — `cadence` is 2000.
- `nodus/tuvm/zephyr/PortaBridgeNodeC6.nsl` — `cadence` is 2000.
- `nodus/tuvm/zephyr/ProfileCostNodeC6.nsl` — `cadence` is 2000.
- `nodus/tuvm/zephyr/ShipsBellAlarmNodeC6.nsl` — `cadence` is 30000; its check-in rides
  `beatCh`, whose period is the same `cadence`.
- `nodus/tuvm/zephyr/ShipsBellNodeC6.nsl` — **verify first**:
  `grep -n 'bridge checkIn' nodus/tuvm/zephyr/ShipsBellNodeC6.nsl`. If it never checks in, add
  nothing and say so in the commit message; the compiled-in default stands and is honest.

- [ ] **Step 6: Run the full suite**

```bash
cd /home/david/workspace/rete/nodus/nsl && python3 tools/run_tests_oevm.py
```

Expected: `Failed: 0`. The apps under `nodus/tuvm/zephyr/` are not compiled by this suite, so
also confirm the host twin still builds by running the host gate at Task 5; a typo there is
otherwise invisible until then.

- [ ] **Step 7: Commit**

```bash
cd /home/david/workspace/rete
git add nodus/nsl/lib/kernel.nsl nodus/tests/22_spacesync/18_liveness_grid.nsl nodus/tuvm/zephyr/
git commit -m "feat(#231): the node adopts a budget on its own grid, and clamps rather than refuses

D3's integer multiple and D7's 24 h ceiling, in one method both entry
points share — a snap that differed between the app's declaration and
porta's request would be a grid the operator cannot learn by asking."
```

---

## Task 3: measure the record, and pin the size

§2's size law is the reason this repo measures rather than reassures: `obs/build`'s line read
"~40 B encoded" until #74 added a 43-byte string to it.

**Files:**
- Test: `nodus/tests/22_spacesync/20_liveness_size.nsl` (create)
- Modify: `protocol/PROTOCOL-NSL-RECORDS.md` §4.2 (the `obs/**` table + a paragraph)
- Modify: `protocol/PROTOCOL-NSL.md` §4.2 (the path/record row table near `obs/health`)

**Interfaces:**
- Consumes: the `Liveness` record shape from Task 1.
- Produces: the measured byte count, in a test's `expect:` block and in both protocol documents.

- [ ] **Step 1: Write the measuring test**

Create `nodus/tests/22_spacesync/20_liveness_size.nsl`. The `expect:` values below are
**placeholders** — Step 2 replaces them with what the encoder actually answers:

```
(*
porta space-sync — the encoded size of the liveness declaration (rete#231), measured
rather than reasoned about, on 09_batch_size.nsl's precedent and for §2's reason: this
document's size law was once replaced with a reassurance in the same edit that doubled
the record.

WORST LEGAL VALUES, not a fresh node's. CBOR is shortest-form, so the value decides the
field width; the widest legal pair is a 24 h budget (86400000, five bytes) on the
coarsest cadence the ceiling permits. Measuring the easy case is exactly how an estimate
becomes wrong in the field.

obs/liveness is written ONCE PER BOOT, so it does not join 09_batch_size's steady-state
frame — it rides one push, alongside obs/build and obs/apps/main. What matters here is
that one row plus its ~17 B of seq/path/kind fits the headroom that file measures.
expect:
liveness 0
*)
class LivenessSize packageUsing: manifest = () (
  public main: platform args: args = (
    | cbor rec |
    cbor:: platform Cbor.
    rec:: platform Record tag: #Liveness
      with: #cad -> 86400000
      with: #bud -> 86400000.
    ('liveness ' , (cbor encode: rec) byteSize printString) println.
  )
) : (
)
```

- [ ] **Step 2: Run it, read the number, pin it**

```bash
cd /home/david/workspace/rete/nodus/nsl && python3 tools/run-one.py ../tests/22_spacesync/20_liveness_size.nsl B
```

It will FAIL with a diff naming the real byte count. Put that number in the `expect:` block and
re-run to PASS. Record the number here in the plan as you go — Steps 3 and 4 quote it:

> measured `Liveness` size at worst legal values: **____ B**

- [ ] **Step 3: Add the row to `protocol/PROTOCOL-NSL-RECORDS.md` §4.2**

In the `obs/**` table, after the `obs/health` row:

```markdown
| `obs/liveness` | R | `Liveness { cad: int, bud: int }` — **the node's declaration of how long its silence may last** (rete#231 D2). `cad` is how often it intends to speak; `bud` is the longest silence that is still healthy, and `bud` is the **only** number porta judges against: `now - last_seen > bud → #silent`, one line with no constants in it. Two numbers and not one because the bench proved one is not enough — `c6-bell` runs a 30 s cadence and misses roughly half its check-ins to ordinary mesh loss, so judging it against its cadence calls a healthy node silent constantly; the gap between `cad` and `bud` is the node's own allowance for that loss, and the node is the only party that knows its rate. `bud` is an integer multiple of `cad` (D3) and is **clamped** to 24 h (D7) — clamped and published, never rejected, so an app asking for a week appears as a `goal`/`obs` divergence instead of a silent refusal. Written **once per boot**, on `obs/apps/<name>`'s precedent, and again only when the adopted pair moves. **__ B encoded, MEASURED 2026-08-__** at the worst legal values (a 24 h budget is five CBOR bytes where a 16 s one is three) |
```

Then, after the `obs/build` prose, add:

```markdown
**`obs/liveness` replaced an inference, and that is the whole point (rete#231 D4).** porta
used to infer each node's cadence from the median of its last ten inter-arrival gaps and
declare it silent at `3 × cadence`. Both terms were invented: `k = 3` was inherited from v1,
and the cadence came from history a node that never spoke cannot supply — which is why a node
already dead when the gateway shipped was never judged at all (#219), and why a fast node doing
routine blocking work was called silent (#220). 2026-08-10 D8 chose inference *because the
declaration did not exist*; it does now, and it arrives from the only party entitled to make it.
`CadenceMs`, the 10-gap window, the minimum-3-exchanges rule, `k`, `nodes.cadence_ms` and
`exchanges.leg` were all deleted with it.
```

- [ ] **Step 4: Add the row to `protocol/PROTOCOL-NSL.md` §4.2**

Beside the `health` row in the path table:

```markdown
| `liveness` (declare) | `obs/liveness` | cell | `{cad:, bud:}` — the node's own statement of how long it may be silent; porta judges `now - last_seen > bud` and infers nothing (rete#231) |
| `liveness` (request) | `goal/liveness` | cell | `{cad:, bud:}` — the operator **requests**; the node snaps to its grid, clamps to 24 h and publishes what it adopted. The `goal`/`obs` diff is the answer, so nothing needs to know the legal values in advance |
```

- [ ] **Step 5: Suite, then commit**

```bash
cd /home/david/workspace/rete/nodus/nsl && python3 tools/run_tests_oevm.py
cd /home/david/workspace/rete
git add nodus/tests/22_spacesync/20_liveness_size.nsl protocol/PROTOCOL-NSL-RECORDS.md protocol/PROTOCOL-NSL.md
git commit -m "docs(#231): the liveness record is pinned in the catalogue, with a measured size

A figure and not an assurance: obs/build's line read '~40 B' until #74
added a 43-byte string to it, in the same edit that removed the
measurement from a document whose section 2 is a size law."
```

---

## Task 4: porta reads the declaration

Design §7 step 2, first half. A pure read, added before anything depends on it.

**Files:**
- Modify: `porta/internal/space/store.go` (beside `PathLiveness`, ~line 192, and beside
  `CadenceMs`, ~line 490)
- Test: `porta/internal/space/store_test.go`

**Interfaces:**
- Consumes: `obs/liveness` as written by Task 1.
- Produces: `const space.PathDeclared = "obs/liveness"` and
  `func (s *Store) DeclaredBudget(nodeID string) (int64, bool, error)` — `ok` is false when the
  node has published no declaration, or has published one porta cannot read.

- [ ] **Step 1: Write the failing test**

Append to `porta/internal/space/store_test.go`:

```go
// The budget porta judges against comes from the NODE, and a node that has not
// declared one yields no budget rather than a guess — the same shape CadenceMs's
// `ok` had, for the opposite reason: there is nothing to infer from.
func TestDeclaredBudgetReadsTheNodesOwnRecord(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)

	if _, ok, err := st.DeclaredBudget("n1"); err != nil || ok {
		t.Fatalf("no declaration: ok = %v, err = %v, want false, nil", ok, err)
	}

	rec, err := cval.Encode(cval.Record{Tag: "Liveness", Fields: cval.Map{
		{K: cval.Symbol("cad"), V: int64(2_000)},
		{K: cval.Symbol("bud"), V: int64(16_000)},
	}})
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, err := st.Ingest("n1", PathDeclared, cval.KindCell, rec, 1, 0, ""); err != nil {
		t.Fatalf("Ingest: %v", err)
	}
	bud, ok, err := st.DeclaredBudget("n1")
	if err != nil || !ok || bud != 16_000 {
		t.Fatalf("DeclaredBudget = (%d, %v, %v), want (16000, true, nil)", bud, ok, err)
	}
}

// A record porta cannot read is NOT an error that stops the sweep: it is one node
// with no budget, and the honest answer is no verdict for that node. A gateway an
// unreadable value can take down is worse than no gateway.
func TestDeclaredBudgetSurvivesAGarbledDeclaration(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	for _, tc := range []struct {
		name string
		v    any
	}{
		{"not a record", cval.Symbol("live")},
		{"wrong tag", cval.Record{Tag: "Health", Fields: cval.Map{{K: cval.Symbol("bud"), V: int64(1)}}}},
		{"no bud field", cval.Record{Tag: "Liveness", Fields: cval.Map{{K: cval.Symbol("cad"), V: int64(2_000)}}}},
		{"bud is not an int", cval.Record{Tag: "Liveness", Fields: cval.Map{{K: cval.Symbol("bud"), V: "soon"}}}},
		{"bud is not positive", cval.Record{Tag: "Liveness", Fields: cval.Map{{K: cval.Symbol("bud"), V: int64(0)}}}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			b, err := cval.Encode(tc.v)
			if err != nil {
				t.Fatalf("encode: %v", err)
			}
			if _, err := st.Ingest("n1", PathDeclared, cval.KindCell, b, 1, 0, ""); err != nil {
				t.Fatalf("Ingest: %v", err)
			}
			if _, ok, err := st.DeclaredBudget("n1"); err != nil || ok {
				t.Fatalf("ok = %v, err = %v, want false, nil", ok, err)
			}
		})
	}
}
```

**Before writing the implementation**, confirm `Ingest`'s exact signature — it is the node-write
path and this plan quotes it from memory:

```bash
cd /home/david/workspace/rete/porta && grep -n "func (s \*Store) Ingest" internal/space/store.go
```

Adjust the two `Ingest` calls (and any `storeAtClock` helper name) to match what is actually
there; the rest of the test stands.

- [ ] **Step 2: Run it and watch it fail**

```bash
cd /home/david/workspace/rete/porta && go test ./internal/space/ -run DeclaredBudget -v
```

Expected: FAIL to compile — `st.DeclaredBudget` undefined.

- [ ] **Step 3: Implement it**

In `porta/internal/space/store.go`, beside `PathLiveness`:

```go
// PathDeclared is where a node states how long its silence may last (rete#231
// D2). It is an obs/** path because it is an observation the node makes about
// ITSELF — battery, duty cycle and radio conditions are its own — and porta
// writing there would be a lie (2026-08-10 D7). The operator's half is
// goal/liveness, which is a REQUEST; the node always wins.
const PathDeclared = "obs/liveness"
```

And, replacing `CadenceMs` entirely (Task 5 deletes the rest):

```go
// DeclaredBudget answers the silence budget a node has declared for itself, in
// milliseconds.
//
// This is the one place in the store that DECODES a value rather than moving
// bytes (D4.1 says decode only for display; this decodes for judgement, which
// is the narrow exception this path buys). Everything downstream — the verdict
// comparison in Sweep — is still byte-wise against pre-encoded symbols.
//
// ok is false for a node that has never declared, AND for a declaration porta
// cannot read: a wrong tag, a missing or non-positive `bud`, or bytes that are
// not a closed cval encoding at all. None of those is returned as an error,
// because the caller is a sweep over every node and one garbled record must not
// cost the fleet its verdicts. No verdict is the honest answer for that node;
// a default would be porta inventing the number this whole design exists to
// stop inventing.
func (s *Store) DeclaredBudget(nodeID string) (int64, bool, error) {
	var raw []byte
	err := s.db.QueryRow(
		`SELECT value_cbor FROM cells WHERE node_id = ? AND path = ?`,
		nodeID, PathDeclared).Scan(&raw)
	if err == sql.ErrNoRows {
		return 0, false, nil
	}
	if err != nil {
		return 0, false, err
	}
	v, err := cval.Decode(raw)
	if err != nil {
		return 0, false, nil
	}
	rec, ok := v.(cval.Record)
	if !ok || rec.Tag != "Liveness" {
		return 0, false, nil
	}
	for _, p := range rec.Fields {
		if k, ok := p.K.(cval.Symbol); !ok || k != "bud" {
			continue
		}
		bud, ok := p.V.(int64)
		if !ok || bud <= 0 {
			return 0, false, nil
		}
		return bud, true, nil
	}
	return 0, false, nil
}
```

- [ ] **Step 4: Run the tests and watch them pass**

```bash
cd /home/david/workspace/rete/porta && go test ./internal/space/ -run DeclaredBudget -v
```

Expected: PASS, both tests and all five sub-tests.

- [ ] **Step 5: Commit**

```bash
cd /home/david/workspace/rete/porta && gofmt -l . && go test ./...
cd /home/david/workspace/rete
git add porta/internal/space/store.go porta/internal/space/store_test.go
git commit -m "feat(#231): porta can read the number the node declared

One decode, for judgement rather than display, and it answers 'no budget'
for anything it cannot read — a sweep over every node must not lose the
fleet's verdicts to one garbled record."
```

---

## Task 5: the sweep judges on the declaration, and the inference is deleted

Design §7 step 2, second half, and D4/D5. This is the task that closes #220 and deletes four
constants. **Read the two accepted false-report modes in `sweep.go`'s comment before starting:**
I4 (a gateway outage reads as node silence) is what Task 8 finally fixes and must be rewritten,
not deleted; I5 (a sick node's inferred cadence inflates) **ceases to exist** with the inference
and its paragraph goes.

**Files:**
- Modify: `porta/internal/space/sweep.go` (the whole of it)
- Modify: `porta/internal/space/store.go` (delete `CadenceMs`, `medianMs`, `CadenceWindow`,
  `MinExchanges`, `SilenceFactor`)
- Modify: `porta/internal/space/schema.go` (drop `nodes.cadence_ms` and `exchanges.leg`)
- Modify: `porta/internal/wire/server.go:166` and `store.go:461` (`RecordExchange` loses `leg`)
- Modify: `porta/internal/portacli/serve.go:60-68`
- Test: `porta/internal/space/sweep_test.go` (rewrite the cadence-era tests)

**Interfaces:**
- Consumes: `DeclaredBudget` from Task 4.
- Produces: `LivenessChange{NodeID string, Verdict string, SinceMs, BudgetMs int64}` — `Silent
  bool` and `CadenceMs int64` are **gone**; `Verdict` is one of `live`, `silent`, `neverSeen`,
  `unknown`. `func (s *Store) RecordExchange(nodeID string, bootID, cursorIn, cursorOut,
  pushedLo, pushedHi int64, addr string) error` — the `leg string` parameter is gone.

- [ ] **Step 1: Write the failing tests**

Replace the cadence-era tests in `porta/internal/space/sweep_test.go`. Keep `livenessOf` and
`livenessRows` as they are. The new set:

```go
// A node is judged against the budget IT declared, and against nothing else. The
// rule is one line with no constants in it: now - last_seen > bud.
func TestSweepJudgesAgainstTheDeclaredBudget(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	checkIn(t, st, &clock, "n1", 0)
	declare(t, st, "n1", 2_000, 16_000)
	lastSeen := clock

	// Well inside the budget.
	clock = lastSeen + 10_000
	changes, err := st.Sweep()
	if err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if len(changes) != 1 || changes[0].Verdict != "live" {
		t.Fatalf("changes = %+v, want one #live", changes)
	}
	if changes[0].BudgetMs != 16_000 {
		t.Fatalf("change carries budget %d, want 16000", changes[0].BudgetMs)
	}

	// EXACTLY at the budget is still live: the inequality is strict, so the
	// declaration is honoured to its last millisecond.
	clock = lastSeen + 16_000
	if changes, err = st.Sweep(); err != nil {
		t.Fatalf("Sweep at the boundary: %v", err)
	} else if len(changes) != 0 {
		t.Fatalf("at exactly bud: changes = %+v, want no transition", changes)
	}
	if got := livenessOf(t, st, "n1"); got != "live" {
		t.Fatalf("verdict at exactly bud = %q, want live", got)
	}

	clock++
	if changes, err = st.Sweep(); err != nil {
		t.Fatalf("Sweep past the boundary: %v", err)
	} else if len(changes) != 1 || changes[0].Verdict != "silent" {
		t.Fatalf("one ms past bud: changes = %+v, want one #silent", changes)
	}
}

// #220: a node checking in every 2 s that declares a 16 s budget is NOT silent
// after 6 s of routine blocking work. Under the inference this was 3 x 2 s and
// the verdict was wrong; the node's own declaration is what makes it right, and
// the gateway multiplies nothing.
func TestSweepDoesNotCallAFastNodeSilentForOrdinaryBlockingWork(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	for i := 0; i < 5; i++ {
		checkIn(t, st, &clock, "n1", 2_000)
	}
	declare(t, st, "n1", 2_000, 16_000)

	clock += 6_500 // three missed check-ins
	changes, err := st.Sweep()
	if err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if len(changes) != 1 || changes[0].Verdict != "live" {
		t.Fatalf("changes = %+v, want #live — 6.5 s is inside a declared 16 s budget", changes)
	}
}

// D5: a node porta has NEVER heard from is a commissioning failure, not a
// liveness failure, and the two do not have the same operator action — go plug
// it in versus go find out why it stopped. It is judgeable immediately, which
// is what dissolved #219: under the inference it was invisible forever, because
// a node that never spoke supplies no history to infer from.
func TestSweepDistinguishesNeverSeenFromSilent(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	if err := st.EnsureNode("ghost"); err != nil {
		t.Fatalf("EnsureNode: %v", err)
	}

	clock += 60_000
	changes, err := st.Sweep()
	if err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if len(changes) != 1 || changes[0].Verdict != "neverSeen" {
		t.Fatalf("changes = %+v, want one #neverSeen", changes)
	}
	if got := livenessOf(t, st, "ghost"); got != "neverSeen" {
		t.Fatalf("verdict = %q, want neverSeen", got)
	}
}

// A node that has spoken but declared nothing gets NO confident verdict. porta
// has no number to judge against and will not invent one — which is the whole
// argument of this design, applied to porta's own convenience.
func TestSweepWithoutADeclarationIsUnknownAndNotSilent(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	checkIn(t, st, &clock, "n1", 0)

	clock += 10 * 60_000
	changes, err := st.Sweep()
	if err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if len(changes) != 1 || changes[0].Verdict != "unknown" {
		t.Fatalf("changes = %+v, want one #unknown", changes)
	}
}

// Transition-only, unchanged from the inference era: a row every 30 s would bury
// the one moment that matters, and `porta log` is meant to read as a list of the
// times the fleet went quiet.
func TestSweepWritesOnlyOnTransition(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	checkIn(t, st, &clock, "n1", 0)
	declare(t, st, "n1", 2_000, 16_000)
	for i := 0; i < 4; i++ {
		clock += 1_000
		if _, err := st.Sweep(); err != nil {
			t.Fatalf("Sweep: %v", err)
		}
	}
	if n := livenessRows(t, st, "n1"); n != 1 {
		t.Fatalf("gw/liveness rows = %d, want 1", n)
	}
}
```

Add the helper beside `livenessOf`:

```go
// declare writes what a node would have pushed at obs/liveness. It goes through
// Ingest and not a raw INSERT, so the test exercises the same path a real push
// takes — including the kind pin.
func declare(t *testing.T, st *Store, node string, cadMs, budMs int64) {
	t.Helper()
	rec, err := cval.Encode(cval.Record{Tag: "Liveness", Fields: cval.Map{
		{K: cval.Symbol("cad"), V: cadMs},
		{K: cval.Symbol("bud"), V: budMs},
	}})
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, err := st.Ingest(node, PathDeclared, cval.KindCell, rec, 1, 0, ""); err != nil {
		t.Fatalf("Ingest: %v", err)
	}
}
```

Delete outright: `TestSweepDeclaresANodeSilentAfterThreeCadences`,
`TestSweepBoundaryIsExactlyThreeCadencesNotOneTickBefore`, and the test around
`sweep_test.go:257` that asserts the inferred cadence is written back to `nodes.cadence_ms`. Also
`grep -n "CadenceMs\|MinExchanges\|SilenceFactor" porta/internal/space/*_test.go` and delete
every remaining cadence-inference test — they test a mechanism that no longer exists, and one
left behind is a compile error that reads like a regression.

- [ ] **Step 2: Run them and watch them fail**

```bash
cd /home/david/workspace/rete/porta && go test ./internal/space/ -run Sweep -v
```

Expected: FAIL to compile — `changes[0].Verdict` and `st.EnsureNode` (check the real name with
`grep -n "func (s \*Store) Ensure" internal/space/store.go`) do not exist yet.

- [ ] **Step 3: Rewrite `Sweep`**

Replace `porta/internal/space/sweep.go`'s constants, `LivenessChange` and `Sweep` with:

```go
// SweepTick is how often the verdict is re-formed. It is a floor on detection
// latency: a node whose budget is shorter than one tick is detected at the next
// tick, not at its budget. It is also, as of rete#231, the resolution of the
// gateway's own liveness record (runs.go).
const SweepTick = 30 * time.Second

// The four verdicts, pre-encoded once. The transition test compares these bytes
// against what is stored: values in the store are raw deterministic CBOR and are
// decoded only for display (D4.1), so decoding here to compare two symbols would
// break that rule for nothing.
//
// FOUR AND NOT TWO, because they are four different operator actions (rete#231
// D5/D6). #neverSeen is a commissioning failure — go plug it in. #silent is a
// liveness failure — go find out why it stopped. #unknown is porta declining to
// answer, and it has two causes, told apart by the row's `reason`: the node has
// declared no budget, or the window falls inside an interval porta was not
// listening. The store must not keep asserting the last thing it heard.
var (
	livenessLive      = mustEncode(cval.Symbol("live"))
	livenessSilent    = mustEncode(cval.Symbol("silent"))
	livenessNeverSeen = mustEncode(cval.Symbol("neverSeen"))
	livenessUnknown   = mustEncode(cval.Symbol("unknown"))
)

// LivenessChange is one transition, returned so the caller can log it. A sweep
// that changed nothing returns an empty slice — which is the normal case.
type LivenessChange struct {
	NodeID   string
	Verdict  string // live | silent | neverSeen | unknown
	SinceMs  int64  // how long since the node was last heard from, at the verdict
	BudgetMs int64  // the budget the NODE declared; 0 when it declared none
}

// Sweep forms one liveness verdict per node and writes gw/liveness ONLY where
// the verdict differs from the one already stored.
//
// THE RULE IS ONE LINE AND THERE ARE NO CONSTANTS IN IT: now - last_seen > bud,
// where bud is the budget the NODE declared at obs/liveness. porta infers
// nothing (rete#231 D4). What that deleted, and why it is a deletion rather
// than a replacement: cadence used to be the median of the last ten
// inter-arrival gaps, silence was 3 x that, and both the 10 and the 3 were
// numbers porta picked. A node that had never spoken supplied no gaps and was
// therefore never judged at all (#219); a fast node doing routine blocking work
// was judged against 3 x 2 s and called silent (#220). 2026-08-10 D8 chose
// inference because the declaration did not exist. It does now.
//
// Why a ticker and not a lazy read-time view (2026-08-10 §6.3): the failure this
// must catch is porta hearing NOTHING, and a system that evaluates only when
// read is silent exactly when everything is. The ticker is not a convenience.
//
// Why transition-only: it makes `porta log` a list of the times the fleet went
// quiet. A row every 30 s would bury the one moment that matters.
//
// One node's failure never costs another node its verdict: each node is
// evaluated independently, the changes returned are only the ones actually
// committed, and the returned error (via errors.Join) names every node that
// failed. Only a failure in the query that lists the nodes themselves — not
// per-node — aborts the sweep outright, because nothing can proceed without it.
func (s *Store) Sweep() ([]LivenessChange, error) {
	now := s.nowMs()
	rows, err := s.db.Query(`
		SELECT n.id, n.last_seen_ms, COALESCE(c.value_cbor, x'')
		FROM nodes n
		LEFT JOIN cells c ON c.node_id = n.id AND c.path = ?
		ORDER BY n.id`, PathLiveness)
	if err != nil {
		return nil, err
	}
	type row struct {
		id       string
		lastSeen sql.NullInt64
		stored   []byte
	}
	var nodes []row
	for rows.Next() {
		var r row
		if err := rows.Scan(&r.id, &r.lastSeen, &r.stored); err != nil {
			rows.Close()
			return nil, err
		}
		nodes = append(nodes, r)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, err
	}

	var changes []LivenessChange
	var errs []error
	for _, n := range nodes {
		want, verdict, since, bud, reason, err := s.verdictFor(n.id, n.lastSeen, now)
		if err != nil {
			errs = append(errs, fmt.Errorf("node %s: %w", n.id, err))
			continue
		}
		if bytes.Equal(n.stored, want) {
			continue
		}
		if err := s.WriteGateway(n.id, PathLiveness, want, reason); err != nil {
			errs = append(errs, fmt.Errorf("node %s: write verdict: %w", n.id, err))
			continue
		}
		changes = append(changes, LivenessChange{
			NodeID: n.id, Verdict: verdict, SinceMs: since, BudgetMs: bud,
		})
	}
	return changes, errors.Join(errs...)
}

// verdictFor is the whole decision, in the order the evidence allows.
//
// The ORDER is load-bearing. Speech is positive evidence and outranks
// everything: a node heard from inside its budget is #live even if porta was
// deaf for the ten hours before that. Absence is only ever evidence when porta
// was listening, which is why the deaf test sits between "past the budget" and
// "silent" rather than at the top.
func (s *Store) verdictFor(nodeID string, lastSeen sql.NullInt64, now int64) (want []byte, verdict string, since, bud int64, reason string, err error) {
	if !lastSeen.Valid {
		// D5: never heard from. Judgeable immediately and WITHOUT a budget,
		// because the question "has this node ever spoken" needs no number.
		return livenessNeverSeen, "neverSeen", 0, 0,
			"neverSeen: no exchange has ever arrived from this node", nil
	}
	since = now - lastSeen.Int64
	bud, ok, err := s.DeclaredBudget(nodeID)
	if err != nil {
		return nil, "", 0, 0, "", fmt.Errorf("declared budget: %w", err)
	}
	if !ok {
		return livenessUnknown, "unknown", since, 0,
			fmt.Sprintf("unknown: %d ms since the last exchange, and this node has declared no budget", since), nil
	}
	if since <= bud {
		return livenessLive, "live", since, bud,
			fmt.Sprintf("live: %d ms since the last exchange, budget %d ms", since, bud), nil
	}
	return livenessSilent, "silent", since, bud,
		fmt.Sprintf("silent: %d ms since the last exchange, budget %d ms", since, bud), nil
}
```

Delete `recordCadence` entirely. Add `"database/sql"` to the imports; drop any that go unused.

Task 8 inserts the deaf test into `verdictFor` between the budget comparison and the `#silent`
return — leave the shape as written so that edit is one block.

- [ ] **Step 4: Delete the inference from `store.go`**

Delete `CadenceMs`, `medianMs`, and the whole `const ( CadenceWindow … MinExchanges …
SilenceFactor … )` block with its comment. Delete the `"sort"` import if nothing else uses it.

Then delete the `leg` bookkeeping, which existed only to make the inference correct (#217 C1):

- `store.go` — `RecordExchange` loses its trailing `leg string` parameter and the `leg` column
  from its INSERT.
- `wire/server.go:166` — drop the `"pull"` argument. Check for a second call site:
  `grep -rn "RecordExchange" porta/ --include=*.go`.
- `schema.go` — remove `leg` from the `exchanges` DDL, remove `cadence_ms` from the `nodes` DDL,
  remove the `ALTER TABLE exchanges ADD COLUMN leg` entry from `addColumns`, and rewrite the
  `exchanges` table comment (it currently explains the pull/push filter that no longer exists) to
  say what the table is now for: one row per request, the replay index, and the `last_seen`
  heartbeat.

Add a `dropColumns` list beside `addColumns`, run the same way — executed on every `Open` with
its error discarded, so a fresh store and a live one converge:

```go
// dropColumns are run after addColumns on every Open, and their errors are
// discarded exactly as addColumns' are: on a fresh store the column is already
// absent and sqlite says so. Both entries are columns whose ONLY reader was the
// cadence inference rete#231 D4 deleted, and a column nobody reads is precisely
// what that decision mocked — nodes.cadence_ms was chosen over a declaration in
// 2026-08-10 D8 because it "was found to be a dead column", and leaving it dead
// a second time would be the same mistake with the reasons reversed.
//
// KEEP THE `nodes` AND `exchanges` COLUMN COMMENTS ABOVE THE STATEMENT, NEVER
// INSIDE THE PARENTHESES: sqlite implements DROP COLUMN by editing the stored
// DDL text, and an interior comment makes the rewritten statement unparseable.
var dropColumns = []string{
	`ALTER TABLE nodes ADD COLUMN cadence_ms INTEGER`, // no-op on a fresh store; see below
	`ALTER TABLE nodes DROP COLUMN cadence_ms`,
	`ALTER TABLE exchanges DROP COLUMN leg`,
}
```

Drop the first line if a plain `DROP COLUMN` on a fresh store is already a discarded error — the
test in the next step decides it. Run both a fresh-store and a migrated-store open in the
existing migration test (`porta/internal/space/store_test.go:342` is the precedent) and confirm
the second `Open` is a no-op.

- [ ] **Step 5: Update the serve log line**

`porta/internal/portacli/serve.go`, in `sweepLiveness`:

```go
			for _, c := range changes {
				logf("porta: %s is #%s — %d ms since its last exchange, declared budget %d ms",
					c.NodeID, c.Verdict, c.SinceMs, c.BudgetMs)
			}
```

Delete the `verdict := "live"; if c.Silent {…}` block above it.

- [ ] **Step 6: Rewrite the two accepted false-report modes**

In `sweep.go`'s doc comment, replace the I4/I5 block with:

```go
// ONE ACCEPTED FALSE-REPORT MODE, and one that is now FIXED:
//
//   - I4 — A GATEWAY OUTAGE READ AS NODE SILENCE. FIXED by rete#231 D6: porta
//     records the intervals it was actually listening (runs.go), and a verdict
//     whose window falls inside a gap reads #unknown. Before that fix,
//     restarting porta after an outage longer than a node's budget wrote
//     #silent for every affected node and then #live as each checked back in —
//     a burst byte-identical in `porta log` to a genuine fleet-wide failure.
//
//   - I5 — A SICK NODE'S INFERRED CADENCE INFLATED, DELAYING DETECTION. GONE
//     with the inference itself: there is no cadence to inflate, and a node in
//     a reboot loop is judged against the budget it declared before it got sick.
//
//   - I7 — A POWERED-OFF NODE IS INDISTINGUISHABLE FROM A WEDGED-AND-HALTED
//     ONE, and nothing on the gateway side can change that (design §4). What
//     the budget buys is knowing WHEN TO START WORRYING, which is what was
//     missing. Do not read #silent as "crashed".
```

- [ ] **Step 7: Run everything**

```bash
cd /home/david/workspace/rete/porta && gofmt -l . && go test ./...
```

Expected: PASS. `grep -rn "CadenceMs\|SilenceFactor\|MinExchanges\|cadence_ms" porta/` must
return nothing outside `docs/`.

- [ ] **Step 8: The host gate, then commit**

```bash
cd /home/david/workspace/rete/nodus/tuvm && bash tests/porta_sync_host.sh
```

Expected: `PORTA SYNC OK`. The store schema changed and the exchange path changed with it; this
is the only host gate that runs a real node against a real `porta serve`.

```bash
cd /home/david/workspace/rete
git add porta/
git commit -m "feat(#231): porta judges the number the node declared, and infers nothing

D4 reverses 2026-08-10 D8. Deleted with the inference: k = 3, the 10-gap
window, the minimum-3-exchanges rule, CadenceMs, nodes.cadence_ms and the
leg column that existed only to make the inference correct. #220 closes:
a fast node doing routine blocking work is no longer called silent,
because nothing multiplies its cadence by a number it never chose.

Four verdicts where there were two (D5): #neverSeen is a commissioning
failure and #silent is a liveness failure, and they are not the same
operator action."
```

---

## Task 6: the host gate witnesses the declaration and the verdict

The first end-to-end proof: two independently written implementations of one wire, which is the
only kind of test that catches a protocol misreading.

**Files:**
- Modify: `nodus/tuvm/tests/porta_sync_host.sh` (new assertions inside the existing `[7/8]` drive
  step — the `[N/8]` numbering counts build/start phases, not legs, so it does not change)

**Interfaces:**
- Consumes: the node's `obs/liveness` (Tasks 1–2) and porta's verdict (Task 5).
- Produces: two new legs in the gate's output, and the `PORTA SYNC OK` line unchanged.

- [ ] **Step 1: Add the leg, and watch it fail first**

Before adding the assertion, run the gate as-is and confirm it is green; a leg added to a red
gate cannot be told from the thing that was already red.

Add after the `obs/build` leg:

```bash
# rete#231: the node's own declaration of how long its silence may last. This is
# the leg that proves the record survives the wire — the node builds it with its
# Cbor class and porta decodes it with a hand-rolled codec that was written from
# the record catalogue, so a shape either side got wrong shows up HERE and not on
# a board six weeks later.
for _ in $(seq 1 100); do
  /tmp/porta get --db "$DB" "$NODE" obs 2>/dev/null | grep -q "obs/liveness" && break; sleep 0.2
done
/tmp/porta get --db "$DB" "$NODE" obs/liveness | grep -q "#cad: 2000" || {
  echo "FAIL: obs/liveness does not carry the node's declared 2 s cadence"
  /tmp/porta get --db "$DB" "$NODE" obs/liveness; exit 1; }
/tmp/porta get --db "$DB" "$NODE" obs/liveness | grep -q "#bud: 16000" || {
  echo "FAIL: obs/liveness does not carry the declared 16 s budget (8 x cadence)"
  /tmp/porta get --db "$DB" "$NODE" obs/liveness; exit 1; }
echo "  obs/liveness carries the node's declared cadence and budget"
```

Confirm the exact display form first — `porta get` formats a Record through `cval.Format`, and
this plan is guessing at `#cad: 2000`:

```bash
cd /home/david/workspace/rete/porta && grep -n "Symbol" internal/cval/format.go
```

Adjust both greps to whatever `Format` actually prints.

- [ ] **Step 2: Add the verdict leg**

The sweep ticks every 30 s, which is longer than this gate's whole run — so drive one tick
directly rather than waiting, and say why:

```bash
# rete#231: porta's verdict, formed from the number the node declared. The sweep
# ticks every 30 s (space.SweepTick), which outlasts this gate, so the verdict is
# asserted by a `porta get` after a forced tick rather than by sleeping. If a
# `--sweep-now` flag does not exist, extend the wait to one tick and say so here
# rather than sleeping a mystery interval.
sleep 31
/tmp/porta get --db "$DB" "$NODE" gw/liveness | grep -q "live" || {
  echo "FAIL: porta did not judge a node checking in every 2 s as #live"
  /tmp/porta get --db "$DB" "$NODE" gw; cat /tmp/porta_serve.log; exit 1; }
echo "  gw/liveness reads #live, formed from the declared 16 s budget"
```

A 31 s sleep is a real cost on a gate that otherwise runs in seconds. If it pushes the gate past
a minute, prefer adding `porta serve --sweep-tick <dur>` (a test knob, exactly like the existing
`--test-drop-ack`) and pass `--sweep-tick 1s` from this script. Take that option if the sleep
lands badly; record which you chose in the commit message.

- [ ] **Step 3: Run the gate**

```bash
cd /home/david/workspace/rete/nodus/tuvm && bash tests/porta_sync_host.sh
```

Expected: `PORTA SYNC OK`, with the two new lines printed.

- [ ] **Step 4: Commit**

```bash
cd /home/david/workspace/rete
git add nodus/tuvm/tests/porta_sync_host.sh
git commit -m "test(#231): the declaration crosses the wire, and porta judges on it

Two independently written codecs against one record — the only kind of
test that catches a protocol misreading, and the reason it runs before
hardware."
```

---

## Task 7: `goal/liveness` — the operator requests, the node answers

Design §7 step 3 and D2. The operator does not *set* the budget; they **request** one, and the
ordinary `goal/X` vs `obs/X` tree diff answers "did the node accept?".

**Note what is deliberately NOT in this task.** A requested `cad` is adopted only when the kernel
owns the tick (Task 10); until then the node's cadence is its app's timer, and publishing a
cadence it does not keep would be the `port=up` lie in miniature. So this task adopts the
**budget** and republishes the node's real cadence, which is exactly the design's own example
working: ask for something off the grid, watch `obs` disagree, and the grid is self-describing
without ever being published.

**Files:**
- Modify: `nodus/nsl/lib/kernel.nsl` (class `PortaBridge`: `applyLivenessGoal`, and one call in
  `checkIn`)
- Test: `nodus/tests/22_spacesync/19_liveness_goal.nsl` (create)
- Modify: `nodus/tuvm/tests/porta_sync_host.sh` (the convergence leg)
- Test: `porta/internal/space/store_test.go` (the golden that pins the gate's hex)

**Interfaces:**
- Consumes: `adoptCad:bud:` from Task 2, `peekCell:ifStream:` from the kernel.
- Produces: `PortaBridge>>applyLivenessGoal` → self, called from `checkIn` after `applyBatch:`.

- [ ] **Step 1: Write the failing test**

Create `nodus/tests/22_spacesync/19_liveness_goal.nsl`:

```
(*
porta space-sync — the operator REQUESTS a budget and the node answers (rete#231 D2).

porta may not write obs/**, so it writes goal/liveness and the node publishes what it
actually adopted. There is no accept/reject protocol and no error path: the goal/obs
diff IS the answer, which is the convergence view this system already has. An operator
asking for 45 s on a 2 s node sees 46 s come back, and learns the grid without anyone
publishing it.

A WRONGLY KINDED LEAF IS A LOG LINE, NOT A CRASH, and `peekCell:ifStream:` is why:
`porta put --raw --kind stream` is a legal write, kind is pinned by the first write and
re-created by replay, and a bare `peek:` would raise 'not a cell' here on EVERY tick
forever — a raise that protectedFire: catches by ABANDONING the rest of the reaction,
so obs/health and tel/log would silently stop while the exchange kept working (#180,
#206 are this failure with a different path).

expect:
adopted 2000 46000
clamped 2000 86400000
stream is a log line true
absent leaves it alone true
*)
class LivenessGoal packageUsing: manifest = () (
  bridgeOn: platform = (
    | b |
    b:: platform PortaBridge socket: nil space: platform space id: 'test' peer: '::1' port: 1
        spans: platform Spans new.
    b liveness: 2000 budget: 16000.
    ^ b )

  goal: platform bud: ms = (
    (platform space channel: 'goal/liveness')
      put: '' value: (platform Record tag: #Liveness with: #cad -> 2000 with: #bud -> ms).
    ^ self )

  public main: platform args: args = (
    | b before |
    (* 45 s is not a whole number of 2 s cadences: adopted as 46 s. *)
    b:: self bridgeOn: platform.
    self goal: platform bud: 45000.
    b applyLivenessGoal.
    ('adopted ' , b cadMs printString , ' ' , b budMs printString) println.

    (* A week is clamped to the ceiling and PUBLISHED (D7), never refused. *)
    b:: self bridgeOn: platform.
    self goal: platform bud: 604800000.
    b applyLivenessGoal.
    ('clamped ' , b cadMs printString , ' ' , b budMs printString) println.

    (* A stream where a cell was pinned is a peer disagreeing with the protocol:
       reported, and the node keeps running with what it had. *)
    b:: self bridgeOn: platform.
    (platform space channel: 'goal/liveness') out: '' value: 'nonsense'.
    b applyLivenessGoal.
    ('stream is a log line ' , (b budMs = 16000) printString) println.

    (* The common case, and it must cost one cell peek: no goal at all. *)
    b:: self bridgeOn: platform.
    before:: b budMs.
    b applyLivenessGoal.
    ('absent leaves it alone ' , (b budMs = before) printString) println.
  )
) : (
)
```

**Check two dialect details before running**: whether `(space channel: 'goal/liveness') out:
value:` is the write that creates a STREAM leaf (`grep -n "public out: " nodus/nsl/lib/kernel.nsl`),
and whether the third case needs a fresh `platform space` per sub-case — the space is shared
across this test's four blocks, and a leaf pinned as a stream in case 3 stays pinned for case 4.
If it does, give each sub-case its own path or reorder so the stream case runs last.

- [ ] **Step 2: Run it and watch it fail**

```bash
cd /home/david/workspace/rete/nodus/nsl && python3 tools/run-one.py ../tests/22_spacesync/19_liveness_goal.nsl B
```

Expected: FAIL — `applyLivenessGoal` does not exist.

- [ ] **Step 3: Implement it**

In `nodus/nsl/lib/kernel.nsl`, class `PortaBridge`, after `liveness:budget:`:

```
    (* rete#231 D2 — porta's REQUEST, read once per check-in from the tail of the
       apply step. No timer, no reaction: the cadence is the check-in's, and a
       `whenever:` on this path would also have to be a bare self-send anyway
       (#133, a reified block cannot resolve its outer chain on this VM).

       THE NODE ALWAYS WINS, and it wins by publishing rather than by refusing.
       Every request is adopted through the same `adoptCad:bud:` the app's own
       declaration goes through, so the grid and the ceiling apply identically —
       an operator asks for 45 s on a 2 s node, `obs/liveness` answers 46 s, and
       the divergence is the whole answer. Nothing had to know the legal values.

       THE CADENCE HALF IS NOT HONOURED YET, deliberately. The app owns the timer
       until the kernel base tick lands (rete#231 D1, plan Task 10); adopting a
       requested cadence the node does not actually keep would publish a number
       the node is not honouring, which is the `port=up` lie this design exists to
       refuse. `cadMs` is therefore re-asserted from what the node really does,
       and the request's cadence shows as a goal/obs divergence until Task 10.

       `peekCell:ifStream:` and never `peek:` — see otaCheck's comment at length:
       a gateway can legally create this leaf as a STREAM, and a raise here would
       be caught by `protectedFire:`, abandoning the rest of the reaction that
       drives the tick. obs/health and tel/log would stop, permanently, with
       nothing logged. *)
    public applyLivenessGoal = (
      | rec bud |
      rec:: space peekCell: 'goal/liveness' ifStream: [
        self telLog: #error text: 'liveness bad record'.
        nil ].
      rec == nil ifTrue: [ ^ self ].
      rec tag = #Liveness ifFalse: [
        self telLog: #error text: 'liveness bad record'.
        ^ self ].
      bud:: rec at: #bud ifAbsent: [ nil ].
      bud == nil ifTrue: [
        self telLog: #error text: 'liveness bad record'.
        ^ self ].
      ^ self adoptCad: cadMs bud: bud )
```

In `checkIn`, immediately after `self publishApps.` — before `publishLiveness`, so that a request
arriving on this tick is published on this tick rather than the next:

```
      self applyLivenessGoal.
```

- [ ] **Step 4: Run the test and watch it pass**

```bash
cd /home/david/workspace/rete/nodus/nsl && python3 tools/run-one.py ../tests/22_spacesync/19_liveness_goal.nsl B
cd /home/david/workspace/rete/nodus/nsl && python3 tools/run_tests_oevm.py
```

Expected: both PASS, `Failed: 0`.

- [ ] **Step 5: Pin the gate's raw CBOR with a Go golden**

`porta put` takes scalars or `--raw <hex>`; a Record needs the hex, and a hex nobody checks is a
constant that rots silently. Add to `porta/internal/cval/encode_test.go`:

```go
// The hex nodus/tuvm/tests/porta_sync_host.sh writes to goal/liveness. A gate
// that pastes a constant nothing checks is a gate that keeps passing after the
// record's shape changes underneath it, so the constant is pinned HERE, where a
// shape change breaks a test instead of quietly writing a record the node will
// log as bad.
func TestGoalLivenessGoldenForTheHostGate(t *testing.T) {
	b, err := Encode(Record{Tag: "Liveness", Fields: Map{
		{K: Symbol("cad"), V: int64(2_000)},
		{K: Symbol("bud"), V: int64(45_000)},
	}})
	if err != nil {
		t.Fatalf("Encode: %v", err)
	}
	const want = "REPLACE_ME"
	if got := hex.EncodeToString(b); got != want {
		t.Fatalf("goal/liveness golden = %s, want %s\n(if the record's shape changed on purpose, update BOTH this constant and the hex in nodus/tuvm/tests/porta_sync_host.sh)", got, want)
	}
}
```

Run it, read the real hex from the failure, and put it in both `want` and the gate script.

- [ ] **Step 6: Add the convergence leg to the host gate**

```bash
# rete#231 D2, and this is the leg that proves the operator half. 45 s is not a
# whole number of the node's 2 s cadence, so the node must publish 46 000 and the
# two paths must DISAGREE — that disagreement is the answer, not a failure, and a
# gate that asserted agreement here would be asserting that the node's grid does
# not exist.
#
# `porta put` takes scalars or --raw hex, and a Record needs the hex. This
# constant is Liveness { cad: 2000, bud: 45000 } and it is pinned by
# TestGoalLivenessGoldenForTheHostGate in porta/internal/cval — a hex nobody
# checks is a constant that rots silently, and it would rot into a record the
# node logs as bad while this gate went on passing.
GOAL_LIVENESS_HEX=REPLACE_WITH_THE_GOLDEN
/tmp/porta put --db "$DB" --raw "$GOAL_LIVENESS_HEX" "$NODE" goal/liveness >/dev/null
for _ in $(seq 1 100); do
  /tmp/porta get --db "$DB" "$NODE" obs/liveness 2>/dev/null | grep -q "46000" && break; sleep 0.2
done
/tmp/porta get --db "$DB" "$NODE" obs/liveness | grep -q "46000" || {
  echo "FAIL: the node did not snap a 45 s request onto its 2 s grid"
  /tmp/porta get --db "$DB" "$NODE" obs/liveness; exit 1; }
/tmp/porta get --db "$DB" "$NODE" goal/liveness | grep -q "45000" || {
  echo "FAIL: goal/liveness no longer holds what the operator asked for — the node overwrote a goal/ path"
  /tmp/porta get --db "$DB" "$NODE" goal/liveness; exit 1; }
echo "  goal/liveness 45 s -> obs/liveness 46 s: the node snapped, and the diff IS the answer"
```

The second assertion is not decoration: `obs/**` is the node's authority and `goal/**` is
porta's, and a node that "converged" by rewriting the goal would pass the first check alone.

- [ ] **Step 7: Run the gate, then commit**

```bash
cd /home/david/workspace/rete/nodus/tuvm && bash tests/porta_sync_host.sh
```

Expected: `PORTA SYNC OK`.

```bash
cd /home/david/workspace/rete
git add nodus/nsl/lib/kernel.nsl nodus/tests/22_spacesync/19_liveness_goal.nsl \
        nodus/tuvm/tests/porta_sync_host.sh porta/internal/cval/encode_test.go
git commit -m "feat(#231): the operator asks, the node answers with what it adopted

D2: no accept/reject protocol, because the goal/obs diff already is one.
Ask 45 s of a 2 s node and 46 s comes back — the grid is self-describing
without ever being published, and the node always wins.

The cadence half waits for the kernel tick (Task 10): publishing a
cadence the node does not keep would be port=up with better manners."
```

---

## Task 8: the gateway records its own deaf intervals

Design D6, §7 step 5 — independent of the node side and of Task 10. This is #225's gateway half:
on 2026-08-19 `gw/liveness` read `#live` for **ten hours and six minutes** while `c6-bell` missed
every single check-in, because `Store.Sweep` runs inside `porta serve` and a gateway that is down
writes nothing. The record was not missing; it was positively wrong, and unfalsifiable from the
store alone.

**Files:**
- Create: `porta/internal/space/runs.go`
- Create: `porta/internal/space/runs_test.go`
- Modify: `porta/internal/space/schema.go` (the `gw_runs` table)
- Modify: `porta/internal/space/store.go` (a `runID` field on `Store`)
- Modify: `porta/internal/space/sweep.go` (`verdictFor` gains the deaf test)
- Modify: `porta/internal/portacli/serve.go`

**Interfaces:**
- Consumes: `verdictFor` from Task 5.
- Produces: `func (s *Store) StartRun() error`, `func (s *Store) MarkAlive() error`,
  `func (s *Store) WasDeaf(fromMs, toMs int64) (bool, error)`.

**Why a table, when D2 says a feature adds rows and not tables.** D2 governs per-node feature
data, whose unit of extension is a path. This is not per-node and has no path: it is the
gateway's record of its own presence, and the alternatives are worse in kind. A reserved node id
would put a fake node in `nodes`, which is the table `httpsrv`'s `NodeCounter` reports the fleet
size from. A per-tick row in `events` needs a `node_id` and would write 2 880 rows a day into the
log `porta log` exists to keep readable. One row per porta *run* is the smallest honest record,
and it answers a second question nothing else can: when was this gateway up.

- [ ] **Step 1: Write the failing tests**

Create `porta/internal/space/runs_test.go`:

```go
// Copyright (c) 2026 Ekorau LLC

// internal/space/runs_test.go
package space

import "testing"

// The gap BETWEEN two runs is a deaf interval, and a window that touches one is
// a window porta cannot testify about.
func TestWasDeafSpansTheGapBetweenRuns(t *testing.T) {
	var clock int64 = 1_000
	st := storeAtClock(t, &clock)

	if err := st.StartRun(); err != nil {
		t.Fatalf("StartRun: %v", err)
	}
	clock = 100_000
	if err := st.MarkAlive(); err != nil {
		t.Fatalf("MarkAlive: %v", err)
	}

	// porta is killed here and comes back an hour later.
	clock = 3_700_000
	if err := st.StartRun(); err != nil {
		t.Fatalf("StartRun (second): %v", err)
	}
	clock = 3_730_000
	if err := st.MarkAlive(); err != nil {
		t.Fatalf("MarkAlive (second): %v", err)
	}

	for _, tc := range []struct {
		name     string
		from, to int64
		want     bool
	}{
		{"wholly inside the first run", 2_000, 90_000, false},
		{"wholly inside the second run", 3_705_000, 3_720_000, false},
		{"spanning the outage", 90_000, 3_720_000, true},
		{"starting inside the outage", 200_000, 3_720_000, true},
		{"ending inside the outage", 90_000, 200_000, true},
		{"wholly inside the outage", 200_000, 300_000, true},
		{"before any recorded run", 0, 500, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			got, err := st.WasDeaf(tc.from, tc.to)
			if err != nil {
				t.Fatalf("WasDeaf: %v", err)
			}
			if got != tc.want {
				t.Fatalf("WasDeaf(%d, %d) = %v, want %v", tc.from, tc.to, got, tc.want)
			}
		})
	}
}

// #225's exact scenario: a node checking in every 30 s with a 4 min budget, and a
// gateway down for ten hours. The verdict across that window must NOT be #silent —
// porta has no evidence in either direction — and it must not stay #live either,
// which is what actually happened on 2026-08-19.
func TestSweepAcrossAGatewayOutageIsUnknownAndNotSilent(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	if err := st.StartRun(); err != nil {
		t.Fatalf("StartRun: %v", err)
	}
	checkIn(t, st, &clock, "bell", 0)
	declare(t, st, "bell", 30_000, 240_000)
	if err := st.MarkAlive(); err != nil {
		t.Fatalf("MarkAlive: %v", err)
	}
	if _, err := st.Sweep(); err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if got := livenessOf(t, st, "bell"); got != "live" {
		t.Fatalf("before the outage: verdict = %q, want live", got)
	}

	// porta is down for ten hours and six minutes, then restarts.
	clock += 10*3_600_000 + 6*60_000
	if err := st.StartRun(); err != nil {
		t.Fatalf("StartRun (after the outage): %v", err)
	}
	if err := st.MarkAlive(); err != nil {
		t.Fatalf("MarkAlive: %v", err)
	}
	changes, err := st.Sweep()
	if err != nil {
		t.Fatalf("Sweep after the outage: %v", err)
	}
	if len(changes) != 1 || changes[0].Verdict != "unknown" {
		t.Fatalf("changes = %+v, want one #unknown — porta was deaf for the whole window", changes)
	}

	// The node checks back in: evidence of speech outranks everything.
	checkIn(t, st, &clock, "bell", 1_000)
	if err := st.MarkAlive(); err != nil {
		t.Fatalf("MarkAlive: %v", err)
	}
	if changes, err = st.Sweep(); err != nil {
		t.Fatalf("Sweep after the check-in: %v", err)
	} else if len(changes) != 1 || changes[0].Verdict != "live" {
		t.Fatalf("changes = %+v, want #live once the node speaks again", changes)
	}
}
```

- [ ] **Step 2: Run them and watch them fail**

```bash
cd /home/david/workspace/rete/porta && go test ./internal/space/ -run "WasDeaf|Outage" -v
```

Expected: FAIL to compile — `StartRun`, `MarkAlive`, `WasDeaf` undefined.

- [ ] **Step 3: Add the table**

In `porta/internal/space/schema.go`, at the end of the `schema` const:

```sql
-- One row per porta RUN: when this process started serving, and the last moment
-- it was known to be doing so (rete#231 D6). It is the gateway's record of its
-- own presence, and it exists because Store.Sweep runs INSIDE porta serve — so a
-- gateway that is down writes nothing and every node's verdict freezes at
-- whatever it last was. On 2026-08-19 that meant gw/liveness read #live for ten
-- hours and six minutes while c6-bell missed every single check-in (rete#225).
--
-- A TABLE AND NOT A PATH, which is the one exception to "a feature adds rows,
-- not tables" (D2): D2 governs per-NODE data, whose unit is a path, and this is
-- not about a node. A reserved node id would put a fake node in `nodes`, which
-- is what httpsrv's NodeCounter reports the fleet size from; a per-tick row in
-- `events` needs a node_id and would write 2880 rows a day into the log that
-- exists to stay readable.
--
-- alive_ms is stamped by the sweep on every tick, so a crash loses at most one
-- SweepTick of the gateway's life to the deaf interval that follows. That is the
-- conservative direction: it can only turn a confident verdict into #unknown,
-- never the reverse.
CREATE TABLE IF NOT EXISTS gw_runs (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  started_ms INTEGER NOT NULL,
  alive_ms   INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_gw_runs_started ON gw_runs(started_ms);
```

- [ ] **Step 4: Implement `runs.go`**

```go
// Copyright (c) 2026 Ekorau LLC

// internal/space/runs.go
package space

// The gateway's record of its own presence (rete#231 D6). The store must not
// keep asserting the last thing it heard: a verdict formed across an interval
// porta was not listening is not evidence about the node, and reporting it as
// one is the same defect as a gate that reads green while testing nothing —
// the record is not missing, it is positively wrong, and it is unfalsifiable
// from the store alone.

// StartRun opens this process's row. Called once, from `porta serve`, before
// the sweep goroutine starts.
func (s *Store) StartRun() error {
	now := s.nowMs()
	res, err := s.db.Exec(
		`INSERT INTO gw_runs (started_ms, alive_ms) VALUES (?, ?)`, now, now)
	if err != nil {
		return err
	}
	id, err := res.LastInsertId()
	if err != nil {
		return err
	}
	s.runID = id
	return nil
}

// MarkAlive stamps the open run. The sweep calls it at the TOP of every tick,
// BEFORE forming any verdict, and that order is load-bearing: a verdict formed
// at `now` against a run whose alive_ms is one tick behind would find its own
// window uncovered and report #unknown about a gateway that was plainly awake.
//
// A store with no open run (every read-only CLI path) is a no-op rather than an
// error: `porta get` does not claim to have been listening.
func (s *Store) MarkAlive() error {
	if s.runID == 0 {
		return nil
	}
	_, err := s.db.Exec(`UPDATE gw_runs SET alive_ms = ? WHERE id = ?`, s.nowMs(), s.runID)
	return err
}

// WasDeaf answers whether [fromMs, toMs] contains ANY moment porta was not
// known to be listening.
//
// It is deliberately a coverage question and not a gap question: the window is
// deaf unless the recorded runs cover every millisecond of it. That makes the
// honest answer the DEFAULT — a store that predates this table answers "deaf"
// for every window before its first run, which is exactly true of it, and no
// backfill is possible or attempted.
func (s *Store) WasDeaf(fromMs, toMs int64) (bool, error) {
	rows, err := s.db.Query(
		`SELECT started_ms, alive_ms FROM gw_runs ORDER BY started_ms`)
	if err != nil {
		return false, err
	}
	defer rows.Close()
	cursor := fromMs
	for rows.Next() {
		var started, alive int64
		if err := rows.Scan(&started, &alive); err != nil {
			return false, err
		}
		if alive < cursor {
			continue // a run that ended before the window opened
		}
		if started > cursor {
			return true, nil // an uncovered gap at the head of what is left
		}
		cursor = alive
		if cursor >= toMs {
			return false, nil
		}
	}
	if err := rows.Err(); err != nil {
		return false, err
	}
	return cursor < toMs, nil
}
```

Add the field to `Store` in `store.go`:

```go
	// The open gw_runs row, or 0 for a store nobody is serving from (every
	// read-only CLI path). See runs.go.
	runID int64
```

- [ ] **Step 5: Teach `verdictFor` the deaf test**

In `sweep.go`, between the `since <= bud` return and the `#silent` return:

```go
	// D6: absence is only evidence when porta was listening. A window that
	// touches an interval porta was down yields #unknown — never #silent,
	// because the gateway has no evidence in either direction, and never
	// #live, because it has none of that either. Note the position: this is
	// BELOW the `since <= bud` test, because speech is positive evidence and
	// outranks the gateway's own absence.
	deaf, err := s.WasDeaf(now-since, now)
	if err != nil {
		return nil, "", 0, 0, "", fmt.Errorf("deaf intervals: %w", err)
	}
	if deaf {
		return livenessUnknown, "unknown", since, bud,
			fmt.Sprintf("unknown: %d ms since the last exchange, budget %d ms, and porta was not listening for part of it", since, bud), nil
	}
```

- [ ] **Step 6: Wire it into `serve`**

In `porta/internal/portacli/serve.go`, after `space.Open` succeeds:

```go
			// rete#231 D6: this process's own presence, recorded so that a
			// verdict formed across an outage can read #unknown instead of
			// asserting the last thing porta heard. A failure here is fatal
			// and not logged-and-continued: a gateway that cannot record
			// whether it was listening will produce confident verdicts it
			// cannot support, which is worse than not starting.
			if err := st.StartRun(); err != nil {
				return err
			}
```

And in `sweepLiveness`, as the first statement of the `case <-t.C:` arm:

```go
			// BEFORE any verdict — see MarkAlive's comment: a verdict formed
			// against a stale alive_ms would find its own window uncovered.
			if err := st.MarkAlive(); err != nil {
				logf("porta: liveness sweep: marking the gateway alive: %v", err)
			}
```

- [ ] **Step 7: Run everything**

```bash
cd /home/david/workspace/rete/porta && gofmt -l . && go test ./...
cd /home/david/workspace/rete/nodus/tuvm && bash tests/porta_sync_host.sh
```

Expected: PASS, and `PORTA SYNC OK`.

- [ ] **Step 8: Falsify it by hand, the way #225 happened by accident**

The design names this scenario explicitly, and it has already happened once for real:

```bash
cd /home/david/workspace/rete/porta
rm -f /tmp/deaf.db
go build -o /tmp/porta ./cmd/porta
/tmp/porta serve --db /tmp/deaf.db --udp 127.0.0.1:16999 --http-port 0 &
# ... let a node (or the host gate's node, repointed) check in and declare ...
# then kill porta for longer than the node's budget, restart it, and read:
/tmp/porta get --db /tmp/deaf.db bench-node gw/liveness   # expect #unknown, not #live
/tmp/porta log --db /tmp/deaf.db bench-node --limit 20    # the reason names the outage
```

Record the observed output in the commit message. If the node is inconvenient to drive, the
same shape is provable with `porta put --raw` writing an `obs/liveness` and a hand-stamped
`last_seen_ms`; say which you did.

- [ ] **Step 9: Commit**

```bash
cd /home/david/workspace/rete
git add porta/
git commit -m "feat(#231,#225): the gateway records when it was not listening

gw/liveness read #live for ten hours and six minutes on 2026-08-19 while
c6-bell missed every check-in, because Sweep runs inside porta serve and
a gateway that is down writes nothing. The record was not missing, it
was positively wrong.

One row per porta run, and a verdict whose window is not fully covered
reads #unknown — never #silent, because porta has no evidence in either
direction. Speech still outranks it: a node heard from inside its budget
is #live however deaf the gateway was before that."
```

---

## Task 9: promote what Tasks 1–8 learned, and close what they close

The promotion rule is part of every close-out: an issue is not closed until its durable knowledge
is in a mainline document of record. The design's §8 names each destination.

**Files:**
- Modify: `docs/design/2026-08-10-node-memory-and-liveness-design.md` §8 (a dated correction)
- Modify: `porta/docs/ARCHITECTURE.md` (silence detection, the four verdicts, the deaf intervals)
- Modify: `review/tests/CLAIMS.md` — **`review/` is David's tree: ask before editing it.** If
  permission is not in hand, write the claim rows into the commit message and say so here.
- Modify: `docs/next-steps-overview.md` (the #231 paragraph)

**Interfaces:**
- Consumes: everything Tasks 1–8 landed.
- Produces: no code.

- [ ] **Step 1: The dated correction to 2026-08-10 §8**

Do not edit the old table's rows in place — mark them. Append to §8:

```markdown
### Correction, 2026-08-21 (rete#231 D4): four of these constants are deleted, not tuned

`k = 3`, the 10-gap cadence-inference window, the minimum-3-exchanges rule and `CadenceMs`
itself are **gone**, along with `nodes.cadence_ms` and the `exchanges.leg` column that existed
only to make the inference correct (#217 C1).

D8 above chose inference over declaration, and its stated reason was that `nodes.cadence_ms`
*"was found to be a dead column — declared and never written or read anywhere in the Go tree."*
**Inference was never argued to be better than declaration; it was chosen because the
declaration did not exist.** It does now — `obs/liveness`, written by the only party that knows
its own radio conditions — and the gateway's rule is one line with no constants in it:
`now - last_seen > bud`.

See `docs/design/2026-08-21-liveness-contract-design.md`. The two false-report modes this
document accepted are resolved: **I4** (a gateway outage read as node silence) is fixed by D6's
deaf intervals, and **I5** (a sick node's inferred cadence inflating) cannot occur without an
inference to inflate.
```

- [ ] **Step 2: `porta/docs/ARCHITECTURE.md`**

Find the section describing liveness (`grep -n -i "liveness\|silent\|cadence"
porta/docs/ARCHITECTURE.md`) and replace its mechanism description with:

```markdown
**Liveness — porta judges nodes against the number they declared, and infers nothing.**
Every node publishes `obs/liveness` = `Liveness { cad, bud }`: how often it intends to speak, and
the longest silence that is still healthy. `bud` is the only number the gateway uses, and the
rule is `now - last_seen > bud`. The node is the only party that knows its own loss rate —
`c6-bell` misses roughly half its 30 s check-ins to ordinary mesh loss — so a gateway judging it
against a cadence would call a healthy node silent constantly.

Four verdicts at `gw/liveness`, because they are four different operator actions:

| verdict | means | do |
|---|---|---|
| `#live` | heard from inside its budget | nothing |
| `#silent` | it spoke, then stopped, past its own budget | go find out why |
| `#neverSeen` | porta has never had an exchange from it | commissioning failure — go plug it in |
| `#unknown` | porta will not answer: the node declared no budget, or porta was not listening for part of the window (see `gw_runs`) | read the row's `reason` |

`#unknown` is the one that took an incident to earn. `Store.Sweep` runs inside `porta serve`, so
a gateway that is down writes nothing and every verdict freezes: on 2026-08-19 `gw/liveness` read
`#live` for ten hours and six minutes while the node it described missed every check-in. porta
now records one row per run in `gw_runs`, and a verdict whose window that record does not fully
cover reads `#unknown`. **A powered-off node is still indistinguishable from a wedged one** —
nothing on the gateway side can change that. What the budget buys is knowing when to start
worrying.
```

- [ ] **Step 3: `docs/next-steps-overview.md`**

Replace the `#231` paragraph's "PLAN IS THE NEXT ACTION" opening with what actually landed, what
is host-proven, and the one thing that is not: **Task 10 needs a board, and until it runs, D1's
kernel heartbeat and D3's coalescing are designed and unbuilt.** Name the plan file.

- [ ] **Step 4: Close the issues, with evidence and with the deviations named**

```bash
cd /home/david/workspace/rete
gh issue comment 220 --body "..."   # closed: the fast node's 3 x cadence deadline is gone with the inference
gh issue comment 225 --body "..."   # gateway half closed by D6; node half needed nothing and never did
gh issue comment 152 --body "..."   # a sink can now tell a dead node from a quiet one: the node says which
gh issue comment 231 --body "..."   # steps 1,2,3,5 landed on host evidence; step 4 is Task 10 and needs a board
```

Each comment states what was verified and **how**, and names the deviations from what the design
proposed — at minimum: (a) the two default constants this plan pinned that the design left open
(§"Constants this plan pins"), and (b) that `goal/liveness`'s **cadence** half is accepted but
not honoured until the kernel owns the tick, because publishing a cadence the node does not keep
would be the very lie D1 refuses.

Do **not** close #231 itself: Task 10 is part of it.

- [ ] **Step 5: Commit**

```bash
git add docs/ porta/docs/ARCHITECTURE.md
git commit -m "docs(#231,#220,#225): the declaration replaces the inference in the record

2026-08-10 D8 gets a dated correction rather than a quiet edit: it chose
inference because the declaration did not exist, and the record should
say so where the next reader will look."
```

---

## Task 10: the kernel owns the base tick and the heartbeat — **needs a board**

Design §7 step 4, D1 and D3. Staged last on purpose. Budget **35–50 minutes of board time** for a
full target run and schedule it in daylight if the bell is involved: flashing an actuator node is
a physical act.

**The point is not cost, it is meaning.** A heartbeat a wedged node can still emit is a lie — it
is `port=up` exactly, the failure mode #152 is about. So the heartbeat rides the reactor, in nsl,
and adds **no C at all**: `timer_target_snapshot` already reports every live repeating timer to
#215's observer, so one kernel-owned `timers every:` gives the detector a deadline that always
exists *and* drives the check-in. A node with no app timer stops being invisible to the detector.

**Files:**
- Modify: `nodus/nsl/lib/kernel.nsl` (`PortaBridge` gains the base tick and drives its own
  check-in)
- Modify: the five bridged apps (their `timers every: self cadence …` moves into the kernel)
- Create: `nodus/tuvm/zephyr/TimerlessNodeC6.nsl`
- Create: `nodus/tuvm/tests/target/liveness-heartbeat.env`
- Modify: `nodus/tuvm/tests/target/run.sh` (register the scenario), `CLAUDE.md`'s scenario table

**Interfaces:**
- Consumes: `cadMs` from Task 2 — which now becomes the **actual** period, so
  `applyLivenessGoal` can start honouring the cadence half.
- Produces: `PortaBridge>>startTicking: aTimers on: aChannel` (name it in the task, then keep it
  stable), and a node whose kernel wakes it even when the app declares nothing.

**Before writing a line of this task, re-read three memories' worth of hard-won constraints:**

- `platform timers` answers a **fresh instance** every send, and the C timer registry is keyed
  globally — two instances alias each other's tokens and misfire silently. The bridge must be
  **handed** the app's one `timers`, never mint its own.
- `pulse_pump` is `K_FOREVER`-blocking on Zephyr and non-blocking on posix, and that same
  `K_FOREVER` is **both** healthy-idle and wedged — so no pump-completion check can separate
  them. The judgement is against the app's declared periods, which is the whole of #215.
- A `whenever:do:` block cannot resolve its outer chain (#133): the reaction body is one
  self-send.

- [ ] **Step 1: Ask what is actually on the bench**

The bench changes between sessions. Confirm which boards are attached and which is
`TARGET_DEFAULT_BOARD` **through the collector env files**, never from a table:

```bash
ls /home/david/workspace/rete/nodus/tuvm/tests/target/*.env
grep -rn "TARGET_DEFAULT_BOARD" /home/david/workspace/rete/nodus/tuvm/tests/target/lib/
```

`CLASS=disruptive` means a disposable board only. Do not point this at the bell.

- [ ] **Step 2: Move the tick into the kernel, host-first**

Add to `PortaBridge` a method that registers the base tick and reacts by checking in, and change
each app from "own a timer, call `bridge checkIn`" to "hand the bridge its timers and channel".
Prove it on the host **before** any board: `porta_sync_host.sh` must still print `PORTA SYNC OK`
with the node's tick now owned by the kernel, and the whole nsl suite must stay `Failed: 0`.

Write the failing test first — `nodus/tests/22_spacesync/21_kernel_tick.nsl`, asserting that a
bridge handed a `timers` registers exactly one repeating timer at `cadMs` and that a second
`startTicking:` does not register a second one.

- [ ] **Step 3: The timerless node**

Create `nodus/tuvm/zephyr/TimerlessNodeC6.nsl`: a node whose app declares **no** repeating timer
at all (GPIO-driven or purely reactive), constructs a bridge, and can be made to wedge on demand
— follow `SlowTimerNodeC6.nsl` and whatever `wedge-detect.env` drives. Before this task, the
#215 detector has no deadline for such a node and never fires; after it, the kernel's own base
tick is the deadline and the dump must name it.

- [ ] **Step 4: The scenario**

Create `nodus/tuvm/tests/target/liveness-heartbeat.env` in `wedge-quiet.env`'s form — a header
that says what it gates and what it CANNOT see, then the variables. It must witness the design's
three board-only falsifications:

1. **a node with no app timer is judged at all** — wedge `TimerlessNodeC6`; before, nothing
   fires; after, the detector's dump names the kernel base tick as its deadline;
2. **the heartbeat's absence means something** — confirm **no** check-in is emitted between the
   wedge and the reboot. A check-in appearing here would prove the C-side design was chosen after
   all, and would be the `port=up` lie;
3. **radio wakeups coalesce** — two obligations at different multiples of one base, counted over
   a window against the sum of independent periods.

`OBSERVE_S` must outlast the verdict: a scenario's TAPs passing is not the verdict (#216), and a
window that reaches its verdict before the evidence arrives is #223's exact defect.

- [ ] **Step 5: Run it on the board**

```bash
cd /home/david/workspace/rete/nodus/tuvm/tests/target && ./run.sh liveness-heartbeat
```

Then run the two scenarios most likely to regress from a kernel-owned timer — `wedge-quiet`
(the false-trip gate) and `bridge-health` — before claiming the rung. Paste the actual console
evidence into the issue; a summary of a board run is not a board run.

- [ ] **Step 6: Record the scenario in `CLAUDE.md`**

Add the row to the target-scenario table, with its class and what it gates. That table is what
the next session budgets board time from.

- [ ] **Step 7: Commit and close #231**

```bash
cd /home/david/workspace/rete
git add nodus/ CLAUDE.md
git commit -m "feat(#231): the kernel owns the heartbeat, so its absence means something

D1: nsl on the reactor and not C on its own thread, and the reason is not
cost — a heartbeat a wedged node can still emit is port=up, the exact lie
#152 is about. One kernel timers-every: feeds both consumers and adds no
C: timer_target_snapshot already reports it to #215's observer, so a node
with no app timer stops being invisible to the detector."
```

Close #231 with the board evidence, and promote the kernel-heartbeat argument to the VM doc under
`review/lib/` and the rung in `review/program-sequence.md` — **with David's permission, since
`review/` is his tree.**

---

## Self-review notes for the executor

Three things this plan knows it is guessing at. Check each before trusting the code around it:

1. **`Store.Ingest`'s signature** (Tasks 4, 5) and **`EnsureNode`'s name** (Task 5) are quoted
   from the call sites, not from the declarations. `grep` them first.
2. **`cval.Format`'s output form** (Task 6) — `#cad: 2000` is a guess at how a Record's Symbol
   keys print. The greps in the host gate depend on it.
3. **The nsl test space is shared across sub-cases** (Task 7): a leaf pinned as a stream stays
   pinned. If `platform space` cannot be re-minted per case, reorder so the stream case is last.

And one thing it deliberately does not do: **nothing here touches #218** (a node pushing
`gw/liveness` overwrites the gateway's verdict — Ingest has no namespace guard). It is adjacent,
it is real, and it is a separate defect with a separate fix.
