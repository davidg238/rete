# Why nsl-tuvm

*A capability-secure OO scripting layer for Zephyr-class devices — the one-page story.
Written 2026-07-03; companion to the first-step design spec and the slide deck in this
directory.*

---

## The itch

Toit got the model right: an ergonomic OO language on the device, supervised jobs,
crash-isolation, immutable messages between them, and a deployment story (`jag`) where
you push code to a live node over the network instead of reflashing firmware. Working in
Toit is what embedded development should feel like.

But Toit makes two bets that exclude a hardware class that matters:

1. **It needs ESP32-scale RAM.** Toit's runtime does not fit the 256 KB nRF52840 class —
   the class where Thread/802.15.4 radios, coin-cell budgets, and years-long deployments
   live.
2. **"Write your device drivers in Toit."** The language is the whole world; the enormous
   existing driver ecosystems (Zephyr's in particular) are not reachable. Every sensor,
   display, and radio needs a from-scratch in-language driver.

Meanwhile Zephyr already has what Toit lacks: hundreds of maintained drivers behind
uniform APIs, devicetree-based hardware description, OpenThread integration, power
management, and support for essentially every Cortex-M part. What Zephyr lacks is what
Toit has: an ergonomic, safe, remotely-serviceable programming model. C application code
on Zephyr is tedious, unsafe, and needs a flash cycle for every change.

**The wedge: put a tiny OO scripting layer *on top of* Zephyr. Reuse a Zephyr display
driver — don't write one.** The language is the coordinator; C is the datapath.

## What was learned the hard way

This project is the fifth attempt, and the graveyard is the design rationale:

- **toit-zephyr** (tree-sitter for Toit): incomplete, no static typing — the language
  surface was the wrong place to start.
- **st-berry** (Smalltalk on the Berry VM): proved keyword-message syntax over a small
  register VM works, and proved that **giving up non-local return (NLR) is livable** —
  collection combinators cover real early-exit uses, and `^` in an escaping closure can
  simply be a compile error.
- **ns-berry** (Newspeak on Berry): Newspeak's capability/module model is exactly right
  for confining driver access — but full Newspeak requires NLR. Abandoned.
- **ns-oevm / oevm** (custom VM on Piumarta's object model): a clean message-dispatch
  micro-kernel VM, fine single-threaded. Then two days of fibers + NLR + reify-on-escape
  + growable stacks produced a cascade of interacting complexity that ballooned the VM
  far past its intent. That balloon was not incidental — it is what *full*
  Smalltalk/Newspeak semantics cost on a tiny cooperative VM.

The lesson: **cut the ambition to fit the substrate, instead of stretching the substrate
to fit the ambition.** nsl-tuvm keeps Newspeak's capability model, keyword messages, and
message-based everything — and deletes the two features whose implementation cost
dominated everything else: non-local return and fibers.

## The model

```
┌────────────────────────────────────────────────────────────┐
│  nsl (policy):  supervisor · reactor · jobs · capabilities │
│                                                            │
│   job ──out:──▶ channel ──whenever:──▶ job    (run-to-     │
│                (capability = rooted     completion         │
│                 path prefix)            handlers)          │
├────────────────────────────────────────────────────────────┤
│  C seam (data, not control): VM · GC · codec · drivers    │
├────────────────────────────────────────────────────────────┤
│  Zephyr: OpenThread · I2C/SPI/GPIO · display · sensors ·  │
│          power management · devicetree                     │
└────────────────────────────────────────────────────────────┘
```

- **A ~24 KB-class run-to-completion VM.** No fibers, no NLR, no growable stacks. One
  heap-side language stack, O(1) C-stack use (Zephyr threads have 2–8 KB C stacks), a
  precise GC, and an in-image debugger.
- **Jobs are event handlers, not threads.** A job registers standing reactions
  (`whenever:do:`) on channels; each firing runs to completion. This matches Zephyr's own
  idiom (workqueues, run-to-completion callbacks) and OTP's (`gen_server` handles one
  message at a time).
- **Channels are capabilities.** A channel is a URL-style path; a capability is a channel
  namespace rooted at a prefix, attenuated by extending the prefix. A job can only speak
  where it has been granted. Drivers surface as granted capabilities, not ambient
  globals.
- **Messages are immutable values** over a closed algebra (null, bool, int, float,
  string, bytes, symbol, list, map, record) — Toit's proven design. On the single shared
  heap they pass **by reference, zero copy** (beating Toit's per-message copy); only the
  network/flash/driver boundary serializes.
- **Let it crash.** A faulting handler unwinds to the reactor; the supervisor logs,
  applies restart policy, and the rest of the node keeps running. Supervision policy
  lives in nsl, not C.
- **porta/nodus deployment.** Nodes are provisioned, updated, debugged, and profiled over
  the LAN through the porta gateway — the `jag` experience, on Thread-class hardware.

## The model, named: actors at the grain, Linda at the joints

A reviewer who knows the literature will recognize the reactor as a hybrid, and it is
deliberate: **jobs are actors in everything but addressing; channels are their tuple
space.**

A job carries the essential actor contract — private state, one message processed at a
time to completion, contained crashes, supervisor restart policy. It is `gen_server`
minus the PID. What is swapped out is only the rendezvous: actors mail an *address*;
jobs meet at a *channel path*. That is Gelernter's generative communication, and the
verbs are Linda's almost verbatim — `out:` (publish into the space), `take:`
(destructive read), `whenever:` (standing reaction; the extension later tuple-space
systems added as notify/events).

The swap buys exactly what a fleet device needs:

- **Restart transparency.** An actor address dies with its actor — Erlang grew
  registries (registered names, gproc, pg) to paper over PID brittleness. A channel
  path survives the job's crash, restart, quarantine, even a code push that replaces
  the job entirely; producers never learn anything happened. For let-it-crash plus
  remote deployment, decoupling identity from lifetime is the requirement, not a
  concession.
- **Attenuable capability.** An actor address is a capability, but attenuating one
  means building proxy actors. A path prefix attenuates by extension —
  `sensors/` → `sensors/temp/` — cheap enough to *be* the security model.
- **The substrate agrees.** Zephyr's own recommended IPC, zbus, is a channel-based
  pub-sub bus. The architecture converged on the host OS's idiom one level above
  where "compose with Zephyr's grain" was originally aimed. (The same convergence
  shows up across the field: JavaSpaces, ROS topics, MQTT — wherever producers and
  consumers must outlive each other independently.)

The layering is clean: **within** a job it is Newspeak all the way down — synchronous
message sends among objects (Bracha's world). **Between** jobs, where isolation must
hold, it is immutable values through the space (Gelernter's world). Two communication
semantics, each at exactly the boundary where it belongs — the same line BEAM draws
between in-process calls and inter-process messages.

The remaining actor piece is behavior *across* events — without fibers, a multi-step
protocol cannot be straight-line code — and that is the FSM library's role
(`lib/fsm.ns`): an explicit state machine is the actor's behavior function, its state
held in ordinary objects. Which is the same property that makes deep sleep work:
explicit state rehydrates; wake is just another event.

Honest ledger: BEAM isolates by construction (per-process heaps and mailboxes); tuvm
isolates by policy (immutable messages, prefix grants, supervision, per-source
overflow policy). The guarantee is weaker and the RAM cost is 256 KB-affordable — and
the reliability-gaps work (G2 overflow vocabulary, per-job heap attribution) is
precisely what closes the operational distance.

**Paths extend across the network.** Nothing in the model says the space behind a
prefix is local — and that is deliberate, not accidental. Remote reach needs zero VM
changes: a bridge job holds the radio capability, registers `whenever: node42/**`,
serializes outbound, and `out:`s inbound frames into the local space. The remote
space is proxied by an ordinary reaction (the porta gateway *is* this pattern), and
the capability story composes untouched: a grant of `node42/env/` is permission to
speak to that machine, attenuable by prefix like anything else.

The one law to hold when the wire work starts: **location transparency must not
become failure transparency** (Waldo, Wyant, Wollrath & Kendall, *A Note on
Distributed Computing*, Sun Labs TR-94-29 — the canonical argument for why systems
that make remote look identical to local break down over latency, partial failure,
and concurrency). The resolution is that **remoteness lives in the grant, not the
syntax**: producers keep the uniform verbs and never care where a prefix leads, but
a remote prefix is granted *with a delivery contract* — best-effort `out:`,
subscription-semantics `whenever:`, and no `take:` across the network (a distributed
destructive read is a consensus problem; this is where JavaSpaces grew transactions).
The bridge job is the single ingress where G2's overflow policy, G5's auth, and G8's
version skew are enforced — one door, same rules as any other event source.

## Why no fibers is a feature, not a concession

The obvious objection: without fibers, "start conversion, wait 10 ms, read result" can't
be straight-line code — it becomes a state machine or chained handlers. Three reasons
this is the right trade for *this* hardware class:

1. **It is Zephyr's grain.** ISR bottom halves, workqueue items, and OpenThread callbacks
   are all run-to-completion. The scripting layer composes with the host OS instead of
   building a second scheduler on top of it.
2. **It is the deep-sleep-correct model.** A suspended fiber's stack cannot survive
   power-down without heroic checkpointing. A reactor whose inter-event state lives in
   explicit objects and channels rehydrates naturally: wake-from-sleep is just another
   event. Always-on and deep-sleep nodes run the *same* programming model.
3. **It is what makes robustness cheap.** Run-to-completion dispatch means one language
   stack in one known state — which is exactly what makes the crash boundary, the
   watchdog safe-points, and precise GC rooting simple enough to trust at 24 KB.

The ergonomic gap is closed in the library and compiler (a state-machine idiom now, CPS
sugar if ever justified) — never by putting suspension back in the VM.

## The RAM payoff — XIP + run-to-completion, measured

No-fibers and execute-in-place imaging are what make the footprint small, and on real
silicon the free space is large:

- **The image executes from flash (XIP).** Classes, methods, bytecode, and literals live
  in the `.oimg`/solidified blob and run in place (`is_in_blob` → const, never marked,
  never copied); RAM holds only the *mutable* object graph plus VM working state. A
  deliberate flash-for-RAM trade — cheap on a 1 MB/256 KB part (ROM ~26 %) — and the
  reason a bytecode VM can leave most of RAM free.
- **Run-to-completion means one language stack, reused.** With no fibers there is exactly
  one heap-side `Stack` (`g_active_stack`): each handler runs to completion, unwinds
  fully, and the next event reuses it. The stack is therefore a *single* fixed allocation
  sized for the deepest single handler — the largest single arena object (16 KB at 4096
  words on the DK) but paid **once**. A fiber model would multiply it by the live task
  count; that is the RAM line "no fibers" deletes, not merely a stack that happens to be
  big.
- **A fixed, non-growable stack is safe because of let-it-crash.** Overflow is a clean
  abort that unwinds to the reactor's crash boundary; the supervisor quarantines that
  handler and the node runs on — a contained fault, not a smashed C stack. So the stack
  can be sized tight without risking the node.

Measured on the nRF52840-DK (2026-07-09, `nsl-tests/00_perf/RESULTS.md`): the recommended
config runs full perfstex at **45 % RAM (55 % free)**, an aggressive profile at **37 %
(63 % free)** — for a scripting VM with drivers and OpenThread linked. The free region is
genuine headroom (larger object graphs, more channels, wire/DMA buffers), tunable via
`OEVM_HEAP_SIZE`; today the fixed object arena and the newlib float heap split it, so an
object-heavy node should spend some of the free RAM on a bigger arena.

## Where this sits

| | Toit | MicroPython/Zephyr | AtomVM | **nsl-tuvm** |
|---|---|---|---|---|
| Fits 256 KB + OpenThread | no | barely, weak Thread story | partial | **yes (design target)** |
| Reuses host-OS drivers | no (drivers in-language) | partially | partially | **yes (the product)** |
| Capability confinement | no | no | no | **yes** |
| Supervision / let-it-crash | yes | no | yes (BEAM) | **yes** |
| Remote provision/debug | yes (jag) | REPL only | no | **yes (porta)** |
| Static types, raw speed | **yes** | no | no | no — conceded |

Toit wins types and performance; that is conceded up front. The bet is that for
Thread-class fleet devices, **operability** — confinement, supervision, remote
serviceability, and access to Zephyr's driver catalog — is what actually decides whether
a scripting layer survives production.

## Status

First step (host-only, in progress): seed the `tuvm`/`nsl-tuvm`/`nsl-tests` repos, delete
the fiber/NLR/reify balloon, prove the immutable message algebra, prefix-capability
channels, and the supervised reactor end-to-end on the desktop, and bank the footprint
delta. Zephyr bring-up, real drivers, the wire codec, and porta integration follow — see
§8 of the first-step spec for the fence.
