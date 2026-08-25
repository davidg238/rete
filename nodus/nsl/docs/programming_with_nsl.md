# Programming with nsl

A task-oriented guide to writing device programs in **nsl** — the no-NLR, no-fibers,
capability-secure Newspeak dialect that runs on oevm.

This is the *how do I write a program* document. It does not replace:

- **[`NEWSPEAK_DIALECT.md`](./NEWSPEAK_DIALECT.md)** — the syntax/dialect reference (what the language *is*).
- **`oevm/review/language-and-library-spec.md`** and the milestone design specs under
  `docs/design/` — the authoritative, frozen decisions.
- **`oevm/review/WHY.md`** — the pitch and rationale.

Where those documents own a fact, this guide **links** to them rather than restating it, so
the two never drift. Every chapter is anchored to a runnable test in the **`nsl-tests`** repo —
if the guide says the program prints `tick 1`, there is a file you can run that prints `tick 1`.

> **Status.** This covers **phase 1 (host proof)** — everything landed through Milestone D and
> proven on the host both-path harness (see the [Appendix](#appendix--running-a-program)).
> Sections tagged _phase 2_ describe mechanisms that are designed but not yet built (the full
> `on:do:`/`ensure:` surface, the Zephyr seam, on-target drivers). Where a section is still an
> outline, it says so at the top.

---

## Index

1. [The mental model](#1-the-mental-model) — no fibers, no exceptions, no NLR _(outline)_
2. [Language essentials](#2-language-essentials) — classes, methods, blocks, the no-NLR rule _(outline)_
3. [Data and values](#3-data-and-values) — integers, strings, bytes, List / Map / Record _(outline)_
4. [Channels and the reactor](#4-channels-and-the-reactor) — the concurrency model
5. [State machines](#5-state-machines) — the fiber replacement
6. [Time](#6-time) — timers and the virtual clock
7. [Failure](#7-failure) — let-it-crash and supervision
8. [Putting it together](#8-putting-it-together) — the demo-node walk-through
9. [Not here yet](#9-not-here-yet) — the phase-2 roadmap _(outline)_
- [Appendix — running a program](#appendix--running-a-program)

---

## 1. The mental model

> _Outline — to be expanded. Authoritative sources: `oevm/review/WHY.md`,
> the addendum A3 note in `oevm/review/2026-07-03-first-step-design-addendum.md`,
> and `oevm/review/lib/exceptions.md`._

nsl is deliberately missing three things you may expect from a Smalltalk/Newspeak:

- **No fibers / no green threads.** There is no VM-level suspension, ever. Concurrency is
  expressed as *events on channels* dispatched by a run-to-completion **reactor**
  (§4). A "process" is a set of reactions plus, usually, a state machine (§5).
- **No exceptions.** There is no `on:do:`/`ensure:` in phase 1. The only error verb is
  `self error: aString`, which raises a **crash** that unwinds the current reaction and is
  reported as a first-class value on `sys/crash` (§7). Errors are handled by
  *supervision*, not by unwinding to a handler.
- **No non-local return (NLR).** A `^` return inside a block that is genuinely passed to a
  method (`do:`, `detect:ifNone:`, `whenever:do:`) is **removed from the language** — it does
  not compile-and-warn, it is a hard model boundary (`nsl-tests/07_no_nlr`). `^` is legal
  only inside inlined control flow (`ifTrue:`, `whileTrue:`, `to:do:`), where no real closure
  frame exists.

The payoff of these three subtractions is a runtime with **no hidden control flow**: no stack
to unwind across a yield, no handler to search, no scheduler thread. That is what makes the
whole system fit a 256 KB nRF52840 with predictable RAM, and what makes every test in this
guide bit-for-bit deterministic.

**To expand here:** the "let it crash" philosophy vs. defensive programming; why a
capability (a `Channel`) is the unit of authority; the error tiers from
`oevm/review/lib/errors.md` (Tier-1 programmer bug, Tier-2 expected-and-supervised).

---

## 2. Language essentials

> _Outline — to be expanded. Authoritative source: [`NEWSPEAK_DIALECT.md`](./NEWSPEAK_DIALECT.md).
> Anchored tests: `nsl-tests/01_expressions`, `02_control_flow`, `03_blocks`, `07_no_nlr`._

A program is a **class** with a `main:args:` (or `packageUsing:`-style) entry point. The shape
you will see throughout this guide:

```smalltalk
class Containment packageUsing: manifest = ( | goodHits ::= 0. | ) (
  public main: platform args: args = (
    | space good |
    space:: platform space.
    ...
  )
) : (
)
```

- `| goodHits ::= 0. |` — instance slots, with `::=` giving an initial value.
- `foo:: expr` — assignment. `::` is the assignment operator (not `:=`).
- `[ :x | ... ]` — a block; `[ :x :y | ... ]` takes two args.
- The trailing `) : ( )` is the class-side (metaclass) body — often empty, or holds a
  constructor like `chan: c timers: t platform: p = ( ^ self new setChan: c timers: t platform: p )`.
- `platform` is the single **capability root** handed to `main:` — every authority
  (channels, timers, the supervisor, the standard classes) is reached *through it*. There is
  no ambient global state (`nsl-tests/09_channels/04_no_ambient`).

**The no-NLR rule, concretely.** These are fine (inlined — no real closure):

```smalltalk
x > 0 ifTrue: [ ^ 'positive' ].
[ i <= n ] whileTrue: [ i:: i + 1 ].
```

This does **not** compile — `^` inside a block passed to a real method:

```smalltalk
coll do: [ :e | e < 0 ifTrue: [ ^ 'found' ] ].   (* rejected: non-local return in an escaping closure *)
```

The frozen diagnostic substring is verified by the harness (`nsl-tests/07_no_nlr`). The idiom
you use instead is a plain temp plus `detect:ifNone:` or an explicit `whileTrue:` loop.

**To expand here:** literals (Symbols `#foo`, the `#` in `printString`); `,` is String↔String
only; cascades; the `outer` slot-0 mechanism for kernel-internal helpers.

---

## 3. Data and values

> _Outline — to be expanded. Authoritative sources: `oevm/review/lib/integers.md`,
> `messages.md`, `cbor.md`. Anchored tests: `nsl-tests/08_messages`._

nsl distinguishes **value objects** (immutable, structurally compared) from **heap objects**
(identity, mutable). The value algebra is total: every value has a **rank**, and
order/equality/hash are defined across all ranks (`kernel.ns` `valueCompare:with:`,
`value:equals:`, `valueHashOf:`).

| rank | type | notes |
|-----:|------|-------|
| 0 | nil | the single sanctioned "absent" |
| 1 | Boolean | `false < true` |
| 2 | Integer | 64-bit everywhere; boxed `Int64` on 32-bit targets |
| 3 | Float | |
| 4 | String | byte-wise order |
| 5 | Bytes | byte-wise order |
| 6 | Symbol | compared via `printString` (keeps the `#`) |
| 7 | List | element-wise, shorter-is-less |
| 8 | Map | immutable value map; entry-wise over sorted entries |
| 9 | Record | tagged: `Record tag: #crash with: #text -> t with: #channel -> c` |

`Record` is the workhorse for structured messages — a tag Symbol plus named fields, read with
`rec at: #field ifAbsent: [...]`. You will see it carrying crash records in §7.

**To expand here:** the 64-bit integer contract; `List`/`Map`/`Record` construction and the
`platform List`/`platform Map` accessors; why a capability cannot be a Record field
(`valueRank` −1); CBOR wire format for off-device messages.

---

## 4. Channels and the reactor

This is the heart of the concurrency model. There are no threads; there is a **tuple space**
addressed by **channels**, and a single **reactor** (`drain`) that dispatches queued events to
registered reactions, run-to-completion.

### The four verbs

Everything is one of four sends on a `Channel`:

| verb | meaning |
|------|---------|
| `space channel: 'a'` | derive a **narrower** capability addressing sub-path `a` |
| `chan out: rel value: v` | **post** value `v` on the channel at relative path `rel` |
| `chan whenever: rel do: [ :v \| ... ]` | **register** a reaction; owned by the receiver |
| `chan take: rel` | **remove and return** one queued value (or `nil`), fires no reaction |

`platform space` gives you the root capability (rooted at `''`); every `channel:` send narrows
it. Posting merely enqueues — nothing runs until you call `space drain`.

### Reactions fire only on drain, in registration order

```smalltalk
(* nsl-tests/09_channels/08_fanout_order.ns *)
a:: space channel: 'a'.
r1:: [ :v | ('r1-' , v) out ].
a whenever: '' do: r1.
a whenever: '' do: [ :v | ('r2-' , v) out ].
a whenever: '' do: r1.                       (* the same block, registered twice — no dedupe *)
a whenever: '' do: [ :v | ('r3-' , v) out ].
a out: '' value: 'e'.
space drain.
```
```
expect:
r1-e
r2-e
r1-e
r3-e
```

Fan-out visits reactions in **registration order** with **no dedupe** — the stream is
`r1, r2, r1, r3`. (The trailing `r3` is deliberate: `[r1, r2, r1]` alone is a palindrome, so it
would print identically under reverse-order fan-out; `r3` breaks the symmetry and pins *order*,
not merely no-dedupe.)

### `take:` vs. reactions

`take:` is the imperative counterpart — it pulls one value out and **bypasses** the reactor:

```smalltalk
(* nsl-tests/09_channels/05_take_semantics.ns *)
((space take: 'x') == nil) printString out.   (* true  — never posted *)
space out: 'x' value: 5.
(space take: 'x') printString out.             (* 5     — taken once *)
((space take: 'x') == nil) printString out.    (* true  — and only once *)
space whenever: 'y' do: [ :v | 'fired' out ].
space out: 'y' value: 7.
(space take: 'y') printString out.             (* 7     — taken... *)
space drain.                                    (* ...so the reaction never fires *)
'done' out.
```

A taken event is gone — the registered reaction on `y` never sees it. `nil` is the single
sanctioned "absent" return.

### Capabilities only narrow

A `channel:` derivation can only ever *attenuate* authority — never widen, never leak sideways:

```smalltalk
(* nsl-tests/09_channels/03_attenuation.ns *)
capA::   space channel: 'a'.
capAB::  capA channel: 'b'.
capABC:: capAB channel: 'c'.
capABC out: '' value: 5.
(capABC take: '') printString out.                       (* 5    *)
(((capAB channel: 'other') take: '') == nil) printString out.  (* true — a sibling sees nothing *)
(((capA  channel: 'x')     take: '') == nil) printString out.  (* true — a sibling of the root, too *)
```

Holding `a/b/c` lets you address exactly `a/b/c` and below — not `a/b/other`, not `a/x`. This is
the whole security story: you hand a subsystem the narrowest channel it needs, and it *cannot*
address anything else.

### The `sys/` reserved namespace

Channels under `sys/` are the runtime's own: `sys/crash` carries crash records (§7),
`sys/timer` carries timer fires (§6), `sys/log` carries supervisor log records. You register
reactions on them like any other channel; the runtime posts to them on your behalf.

### Run-to-completion

`drain` dispatches one event fully — all its reactions — before the next. A reaction that
**posts** merely enqueues; the new event is picked up on a *later* drain iteration, never
recursively. `drain` is **not re-entrant**: a reaction must not itself call `drain`
(`nsl-tests/09_channels/06_run_to_completion`).

---

## 5. State machines

The **`Protocol`** class is the fiber replacement — "one object per device protocol; events in,
actions out; current state explicit, inspectable, and trivially persistable." Where another
language would `yield` inside a driver loop, nsl makes the state an explicit, named value.

### Defining and driving a machine

```smalltalk
(* nsl-tests/10_fsm/01_protocol.ns *)
machine:: platform Protocol named: #tmp117.
machine state: #idle       on: #start   do: [ :m :e | nil ]                      then: #converting.
machine state: #converting on: #tick    do: [ :m :e | chan out: '' value: 42 ]  then: #idle.
machine state: #converting on: #timeout do: [ :m :e | nil ]                      then: #idle.
machine startIn: #idle.

machine handle: #start.    machine currentState printString out.   (* #converting *)
machine handle: #tick.     machine currentState printString out.   (* #idle       *)
machine handle: #start.    machine currentState printString out.   (* #converting *)
machine handle: #timeout.  machine currentState printString out.   (* #idle       *)
space drain.                                                        (* result: 42  *)
```

- `state: S on: E do: [ :m :e | ... ] then: T` — from state `S`, event `E` runs the action block
  and moves to `T`. The block gets the machine `m` and the event `e`.
- `handle: E` — feed an event; runs the matching transition's action and advances the state.
- `currentState` answers the state Symbol.

Note the last line: the `#tick` action *posts* on a channel (`chan out: '' value: 42`), and the
observer only fires on `drain` — after all four state prints. The FSM action and the reactor are
the **same seam**; a transition that needs to talk to the rest of the system posts an event, it
does not call across.

### Snapshot and restore — persistence without a stack

Because the whole state of a protocol *is* its current-state symbol, deep-sleep persistence is
trivial: snapshot the symbol, and restore rehydrates the machine to it. There is no suspended
call stack to save.

```smalltalk
(* nsl-tests/10_fsm/02_snapshot_restore.ns *)
machine startIn: #idle.
machine handle: #start.
('at ' , machine currentState printString) out.       (* at #converting       *)
snap:: machine snapshot.
('snap ' , snap printString) out.                      (* snap #converting     *)
machine handle: #done.
('moved ' , machine currentState printString) out.     (* moved #idle          *)
machine restore: snap.
('restored ' , machine currentState printString) out.  (* restored #converting *)
```

This is exactly what a driver's `resume:` uses after a restart or a wake-from-sleep (§8): every
state's work is re-startable, so restoring the state symbol is enough — there is nothing else to
rebuild. Use `validate` after wiring a machine to check it is well-formed
(`nsl-tests/10_fsm/01c_validate`); an unknown event can be handled via a fallback block or
silently ignored (`01b_unhandled`).

---

## 6. Time

Time is an **input you drive by hand**, not wall-clock. Every timer test is bit-for-bit
reproducible because the clock only moves on an explicit `advanceMillis:`. The story spans three
layers.

### Layer 1 — the C seam (virtual clock)

There is one clock variable and one thing that moves it (`oevm/src/platform.c`):

```c
static uint64_t host_clock_ms = 0;

static int host_advance(uint64_t ms) {
    host_clock_ms += ms;                                    // the ONLY way time moves
    for (i = 0; i < host_n_timers; i++) {
        while (t->live && t->due_ms <= host_clock_ms) {
            space_post("sys/timer", 9, TAG_SMI(t->token));  // "firing" = post a token
            if (t->repeating) t->due_ms += t->interval_ms; else { t->live = 0; break; }
        }
    }
}
```

Firing a timer is **not** a callback — it posts the timer's token onto the `sys/timer` channel,
back into the same event queue everything else uses. The C layer carries only `(token, ms)`; it
knows nothing about channels or messages, and nothing happens until someone drains.

### Layer 2 — the `Timers` class (fan-out)

`Timers` (in `lib/kernel.ns`) maps a token back to "which channel, which message":

```smalltalk
after: ms post: aMessage on: aChannel = (
  tok:: self allocateToken.
  pending at: tok put: (Pending chan: aChannel msg: aMessage repeating: false).
  seam timerAfter: ms token: tok.        (* hand the token to C *)
  ^ tok )

dispatch: tokenEvent = (
  p:: pending at: tokenEvent ifAbsent: [ nil ].
  p == nil ifFalse: [
    p repeating ifFalse: [ pending removeKey: tokenEvent ].
    p chan out: '' value: p msg ] )      (* NOW post the real message on the real channel *)
```

The cancel race is resolved right here: a timer that fires *after* you cancelled it arrives as a
token no longer in `pending`, so `dispatch:` drops it silently — deliberate transient-signal
semantics.

### Layer 3 — the program

One line bridges the seam to the fan-out; after that you drive everything with
`advanceMillis:` + `drain`:

```smalltalk
(* nsl-tests/11_supervision/06_timers.ns *)
timers:: Timers usingSpace: space seam: platform.
space whenever: 'sys/timer' do: [ :e | timers dispatch: e ].   (* the bridge *)
ping:: space channel: 'ping'.
ping whenever: '' do: [ :m | ticks:: ticks + 1. ('tick ' , ticks printString) out ].

oneShot:: timers after: 50 post: #t on: ping.
platform advanceMillis: 10.  space drain.   (* 50 > 10, not due -> ticks stays 0 *)
platform advanceMillis: 100. space drain.   (* 50 <= 110, fires once -> tick 1 *)
repeater:: timers every: 30 post: #r on: ping.
platform advanceMillis: 30.  space drain.   (* fires -> tick 2, reschedules to 170 *)
timers cancel: repeater.
platform advanceMillis: 300. space drain.   (* repeater dead -> nothing *)
```

You can read the whole trace off the clock arithmetic:

| step | clock | outcome |
|------|------:|---------|
| `after: 50` | 0 | due at 50 |
| `advanceMillis: 10` | 10 | not due — `early nil` |
| `advanceMillis: 100` | 110 | one-shot fires — `tick 1` |
| `every: 30` | 110 | due at 140 |
| `advanceMillis: 30` | 140 | fires, reschedules to 170 — `tick 2` |
| `advanceMillis: 300` | 440 | cancelled — nothing |

The `early nil` assertion is the important one: a one-shot does **not** fire before its due time,
and because the clock is explicit, "50 > 10" is a hard fact, not a timing hope — no flakiness is
possible.

### The full round trip

`nsl-tests/11_supervision/07_event_seam.ns` proves the whole loop end-to-end —
**timer → C seam → `sys/timer` → `Timers dispatch:` → job's reaction → result channel →
observer** — off a single `advanceMillis: 20`, printing `observed 42`. This is the mechanism the
demo-node watchdog is built on (§8).

### A real bug this shape caught

`nsl-tests/11_supervision/08_timer_zero_interval.ns` guards a **Critical** found in review: a
repeating timer with `every: 0` used to hang forever (`due_ms += 0` never advances past the
clock). The fix floors a repeating interval to 1 ms in `host_timer_add`. The test's real
assertion is that *`space drain` returns at all* — if the fix regresses, the test hangs instead
of failing an expectation.

---

## 7. Failure

nsl handles errors by **letting things crash and supervising them**, not by unwinding to a
handler. There is no `on:do:` in phase 1; there is `self error:`, a crash boundary that contains
it, a first-class crash record, and a `Supervisor` that decides what to do.

### The crash boundary contains a reaction

`Space>>drain` fires every reaction through a C setjmp boundary (`vm_protected_fire` /
`vm_raise` in `oevm/src/vm.c`). A reaction that calls `self error:` is unwound and **abandoned
mid-body** — its remaining statements never run — but the process survives and drain moves on:

```smalltalk
(* nsl-tests/11_supervision/01_containment.ns *)
good whenever: '' do: [ :m | goodHits:: goodHits + 1 ].
bad  whenever: '' do: [ :m | self error: 'boom'. badHits:: badHits + 1 ].   (* crashes every firing *)
sys  whenever: 'crash' do: [ :rec | crashes:: crashes + 1 ].
1 to: 10 do: [ :i | good out: '' value: i. bad out: '' value: i ].
space drain.
```
```
expect:
good 10       (* unaffected *)
bad 0         (* body abandoned at the error — proves the unwind *)
crashes 10    (* one crash record per contained crash *)
done          (* the node never exits *)
```

`good` counts all 10; `bad`'s own counter stays **0** because its body is abandoned at
`self error:`; and one crash record lands on `sys/crash` per crash. Observe crashes with a
**reaction on `sys/crash`**, not `take:` — the drain would otherwise consume the transient
records.

### Crash records are values

A contained crash posts a `Record tag: #crash` with `#text` (the error string) and `#channel`
(the firing channel) onto `sys/crash` — a first-class value you can inspect, log, or route
(§3). Crashes raised while firing a `sys/`-rooted reaction are reported out-of-band and dropped
instead of re-posted, to avoid a crash-amplification loop.

### The supervisor: restart, then quarantine

A `Supervisor` watches `sys/crash`, attributes each crash to a **job** (by channel prefix), and
applies a policy: restart on crash, but after **K=3 consecutive** crashes, **quarantine** (stop
restarting). A *progress* event resets the counter.

```smalltalk
(* nsl-tests/11_supervision/05_restart_quarantine.ns *)
sup:: platform supervisor.
sup register: crasher
    restart: [ starts:: starts + 1.
               crasher whenever: 'tick' do: [ :m | self error: 'always' ] ]
    progress: (space channel: 'crasher/never').        (* never posted -> never resets *)
survivor whenever: '' do: [ :m | survivorHits:: survivorHits + 1 ].
sup start.
crasher whenever: 'tick' do: [ :m | self error: 'always' ].   (* prime the first registration *)
1 to: 6 do: [ :i | crasher out: 'tick' value: i. survivor out: '' value: i ].
space drain.
```
```
expect:
starts 2            (* 2 restarts, then the 3rd crash quarantines instead *)
quarantined true
survivor 6          (* a healthy job is completely unaffected *)
done
```

The supervisor's own logic is ordinary application-style code — restart is "sweep the crashed
job's reactions, re-run its start thunk":

```smalltalk
(* nsl-tuvm/lib/kernel.ns — Supervisor *)
handleCrash: rec = (
  chPath:: rec at: #channel ifAbsent: [ nil ].
  j:: self jobAddressing: chPath.
  j == nil ifTrue: [ ^ self ].          (* crash on an unowned channel *)
  j quarantined ifTrue: [ ^ self ].
  j bumpCrashes.
  (j crashes >= quarantineLimit)
    ifTrue: [ self quarantine: j ]
    ifFalse: [ self restart: j ] )

restart: j = ( platform spaceDropReactionsOwnedBy: j rootCap. j restartThunk value. ^ self )
```

The restart contract works because reactions are **owned by the capability object** they were
registered on (`whenever:do:` records `owner: self`), and restart sweeps by identity against the
exact `rootCap` the job was registered with — so a re-registration lands on the same channel the
supervisor is watching, with no double registration
(`nsl-tests/11_supervision/04_owner_sweep`).

---

## 8. Putting it together

`nsl-tests/11_supervision/09_demo_node_integration.ns` is the **Milestone D north star** — a
TMP117-shaped temperature node that composes *everything*: a real `Protocol` FSM (§5), real
timers arming a watchdog (§6), the crash boundary (§7), and the supervisor's
restart/quarantine/progress-reset policy — driven through healthy and wedged sample cycles.

### The driver

```smalltalk
class TempNode = ( | chan timers plat machine timeout ::= nil. | ) (

  startConversion = ( timeout:: timers after: 50 post: #timeout on: chan. )   (* arm the watchdog *)
  readMilliC      = ( ^ 235 )                                                  (* mock sensor *)
  publish: milliC = ( chan out: 'reading' value: milliC. )

  buildMachine = (
    machine:: plat Protocol named: #tmp117.
    machine state: #idle       on: #sample  do: [ :m :e | startConversion ]                    then: #converting.
    machine state: #converting on: #done    do: [ :m :e | timers cancel: timeout. publish: readMilliC ] then: #idle.
    machine state: #converting on: #timeout do: [ :m :e | self error: 'tmp117 timeout' ]        then: #idle.
    machine validate. )

  public start = (
    buildMachine.
    machine startIn: #idle.
    chan whenever: '' do: [ :e | machine handle: e ]. )

  public resume: snapshot = (                         (* restart / wake: fresh FSM on the SAME channel *)
    self start.
    machine restore: (snapshot == nil ifTrue: [ #idle ] ifFalse: [ snapshot ]). )
) : (
  public chan: c timers: t platform: p = ( ^ self new setChan: c timers: t platform: p )
)
```

The `chan` capability is set once at construction and never reassigned — that is what makes
restart safe: `resume:` re-runs `start`, re-registering the reaction against the **same** channel
the supervisor sweeps and re-registers.

### Wiring and driving it

The `main:` registers the node as a supervised job whose `progress:` capability is its own
reading channel, then plays the sensor by hand — a **healthy** cycle posts `#done`; a **wedged**
cycle withholds `#done` and instead advances the clock past the 50 ms timeout, so the watchdog
fires `#timeout` → the FSM's timeout transition → `self error:` → contained crash → `sys/crash`
→ supervisor:

```smalltalk
sup register: jobChan
    restart: [ restartCount:: restartCount + 1. node resume: nil. 'restarted' out ]
    progress: readingChan.
sup start.  node start.

(* A: healthy *) jobChan out: '' value: #sample. space drain.
                jobChan out: '' value: #done.   space drain.       (* -> reading 235 *)
(* B: wedged  *) jobChan out: '' value: #sample. space drain.
                platform advanceMillis: 51.      space drain.       (* -> crash -> restarted *)
```

The full cycle sequence (A healthy, B wedged, C healthy-and-resets-counter, D/E/F wedged) proves
the load-bearing property: without C's progress-reset, E would be the third *cumulative* crash
and quarantine early. Final tallies — `readings 2` (A and C only), `restarts 3` (B, D, E — F
quarantines instead) — confirm the whole story. This is Erlang-style supervision with **no stack
to unwind**.

---

## 9. Not here yet

> _Outline — the phase-2 roadmap. Authoritative sources: `oevm/review/reliability-gaps.md`
> (G1–G11), `oevm/review/lib/exceptions.md`, the milestone specs under `docs/design/`,
> and the open GitHub issues on `davidg238/{oevm,nsl-tuvm}`._

Phase 1 proves the model on the host. Phase 2 targets real hardware. Known gaps, to be written
up as they land:

- **Full `on:do:`/`ensure:`** — a scoped handler surface layered *on top of* the crash boundary
  (`exceptions.md` constraint 4: "unwind to a marked frame"). Let-it-crash + supervision stays
  the default; `ensure:` is for resource cleanup, not control flow.
- **The Zephyr seam** — replace the host virtual clock (§6) and mock sensors with real Zephyr
  timers, an **ISR → channel pulse** primitive, and OpenThread/UDP transport. The `seam` object
  `Timers` already talks through is the injection point.
- **On-device drivers** — real i2c (the TMP117 the demo-node mocks), the `sensors`-package
  interfaces, alert posting.
- **On-target measurement** — the RAM/footprint numbers here are host proxies; re-measure on the
  nRF52840 (tuvm#5). A soak/heap gauge exposed to nsl (`oevm_heap_stats` exists C-side, no prim
  yet).
- **CBOR off-device messaging** — `cbor.md` (tags 27/39) for wire-format messages to a gateway.

---

## Appendix — running a program

Programs in this guide live in `nsl-tests/` and run on the **both-path** conformance harness —
Path A (solidified image compiled into `oevm_runner`) and Path B (`.oimg` layout). A program is
correct only if **A and B agree** with the `expect:` block at the top of the file.

Full-suite run (sequential, > 2 min — **not** concurrency-safe, one at a time):

```bash
cd nsl-tests
OEVM_DIR=<abs>/oevm OEVM_PATH=AB python3 <abs>/nsl-tuvm/tools/run_tests_oevm.py
```

The authoritative harness is **`nsl-tuvm/tools/run_tests_oevm.py`** (not the stale copy under
`oevm/tools/`). Each test file is a class with `expect:` at the top; the harness compiles it down
both paths, runs it, and compares stdout to `expect:` verbatim. That is the whole contract — the
programs in this guide are exactly these files, and their expected output is what you see quoted
throughout.
