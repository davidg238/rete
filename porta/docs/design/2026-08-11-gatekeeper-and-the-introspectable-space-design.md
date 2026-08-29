# Gatekeeper, and the introspectable space — design note (2026-08-11)

> **RENAMED 2026-08-29: `gatekeeper` is now `custos`** (Latin *guardian*, to sit with
> *porta*, the gate). The package is `porta/internal/custos`, the binary
> `porta/cmd/custos`. This document keeps the old name throughout because it is a
> dated record and the issues and commits that cite it resolve by that name; read
> "gatekeeper" below as "custos". The current picture is
> `porta/docs/ARCHITECTURE.md`.

**Status: CLOSED 2026-08-22. Superseded on every open fork by
`porta/docs/design/2026-08-22-gatekeeper-first-cut-design.md`.** This note is kept as the
*why* — the reasoning that produced the questions. It is no longer the *what*; where the two
disagree, the successor wins. Dispositions are marked inline below (§5, §6, §9, §10).

When opened, this was: a decision record with D1 and D2 taken, D3 the open fork it existed
to frame, and D4–D6 following from D1.

**Not a plan.** Nothing here is scheduled. The last section names the cheapest thing that
would replace the guesswork with fact, and that is deliberately the only next step proposed.

---

## 1. The question

rete is an **agent-first** system. An agent arriving cold needs to discover what the fleet
is, what it records, and how to ask it things — and to get answers that are *right*, not
merely plausible. Today none of that exists: the field catalogue is prose in
`protocol/PROTOCOL-NSL-RECORDS.md`, the rules for reading each field are scattered through
issue comments and design addenda, and the store answers only "which path, which node, which
time".

The prompt for this note was narrow — after rete#197 shortens the wire labels, what does
`porta get` show a reader? — but the honest answer dissolved the question. If a reader cannot
say what `pulseCoalesced` *means*, the long name was never buying intelligibility; it was
buying familiarity to whoever named it. A dictionary is needed either way. So the real
question is what that dictionary is, where it lives, and who is allowed to ask it.

## 2. Why Smalltalk is the right precedent, and not merely an evocative one

Smalltalk's introspection was not a tool bolted onto the image. It worked because of three
structural properties:

1. **Uniformity** — everything was an object, reachable from a root, answering the same
   protocol.
2. **The query language was the implementation language** — you did not learn a separate
   dialect to ask about the image; you wrote Smalltalk.
3. **Metadata was data** — a class was an object, inspectable exactly like an instance.

There was no LLM. There was a knowledgeable human in a chair, facing something uniform. The
modern substitution puts an agent in that chair, and it only works if the chair still faces
something uniform.

**rete already has the substrate, and it has not been named as one.** The space *is* the
image: every node owns a uniform tree of paths, porta holds a mirror, values are tagged
records, and convergence is already a tree diff rather than a mechanism. Introspection here
is therefore not a layer to add — it is the space describing itself, answered by the same
read that answers everything else. That is the same "one mechanism, not two" preference this
project applies everywhere else.

## 3. D1 — porta is OPERATIONS. Gatekeeper is the query and authority surface.

**Taken (David, 2026-08-11).**

**porta** is dedicated to running a network of devices and supporting the developer
workstations that flash them:

- the space mirror, the exchange, the event log, the history
- device lifecycle: what is running where, what is stale, what has been reflashed
- **node assets held for later profile and debug** — the `.oimg` sidecars, `.stmap` source
  maps and build-id archives that already accumulate under `~/.local/state/rete/elf/` on a
  workstation today, and which are exactly what a post-mortem needs and a workstation loses
- porta stays **sheltered inside a VPN** and stays **language-neutral** (CLAUDE.md), i.e. it
  does not learn the nsl record catalogue

**Gatekeeper** is the thing questions are addressed to. **Identity, privilege and capability
all rest here.** It is the exposed surface; porta is not exposed.

Two consequences worth stating now, because they are easy to lose:

- **This is where G5 (auth) lands.** The space-sync rewrite deliberately deferred auth as
  "out of this rung". It now has a home rather than a deferral: it is gatekeeper's whole
  reason to exist, and it is *not* a thing to retrofit into the node↔porta wire.
- **The capability vocabulary already exists in this system.** nsl capabilities are rooted
  path prefixes that can only ever be attenuated (narrowed), never widened. That is precisely
  the shape a query authority wants: a caller holds a rooted view of the fleet's tree and can
  hand out strictly narrower ones. Gatekeeper should reuse that model rather than invent a
  second one. Whether it reuses the *implementation* is a separate question; the *model* is
  already ours and already proven.

**The interface between porta and gatekeeper is the load-bearing piece**, and it is a real
design task rather than a detail — it is the boundary between "sheltered, operational,
schema-blind" and "exposed, interrogable, schema-aware".

## 4. D2 — the meta level is answered by the same read as the object level

**Taken.** Whatever gatekeeper serves, an agent should discover it the way it discovers
anything else: by reading paths. A separate discovery protocol beside the space would be a
second mechanism describing the first, and the two would eventually disagree — which is the
failure this project has already paid for more than once (three inconsistent definitions of
`obs/health`; a guard edited into agreement with the code it was guarding).

## 5. D3 — THE OPEN FORK: values are opaque CBOR, so the store cannot be queried by field

This is the gap behind "beyond simple SQL", and it is a **design consequence, not a tooling
weakness**. D4.1 says every value on the wire and in the store is raw deterministic CBOR,
decoded only for display. So `cells` and `events` can filter by path, node and time — and
nothing else. `SELECT median(liveBytes)` is not a query the store can express, because
`liveBytes` is inside a blob.

Three options, with the argument each has to answer.

### (a) Project at ingest — decode values into queryable columns/rows as they arrive

**For:** SQL works immediately; aggregation is pushed down; a month of history is cheap.

**Against, and this is the strong objection:** it forces **porta** to understand every record
shape. That directly contradicts D1 (porta stays operational and language-neutral) and it
breaks the rewrite's unit-of-extension rule — "adding a feature adds a path, not a table, a
verb or an endpoint". Under (a) a new record type on a node requires a porta change before
its data is answerable. porta's CBOR codec is also deliberately **closed**, so that a
mis-versioned peer cannot smuggle a value through; teaching it field *semantics* widens
exactly the surface that was narrowed on purpose.

### (b) The query layer decodes — the store stays opaque, gatekeeper decodes on read

**For:** porta stays schema-blind and language-neutral. Schema knowledge lands in the one
place that must hold it anyway (§6), so there is exactly one place to update. A new record
type is answerable the moment gatekeeper knows it, with no porta change and no migration.

**Against:** aggregation cannot be pushed into SQL; gatekeeper scans and decodes.

**That objection is weaker than it sounds, and the arithmetic should be done rather than
assumed.** At the bell's 30 s cadence a month is ~86 400 rows *per node*; at the bench 2 s
cadence, ~1.3 M. Tens of nodes, not thousands. Scanning and decoding that is ordinary work
for a Go process, and the cost is bounded by history retention rather than by fleet size.
**Measure it before designing around it.**

### (c) Hybrid — raw is truth, a derived projection is a rebuildable cache

**For:** correctness of (b), speed of (a); the projection can be dropped and rebuilt from raw
because raw is authoritative.

**Against:** it is a third mechanism, and it is the classic shape that quietly becomes the
source of truth once something reads it directly.

### Recommendation — (b), with (c) held in reserve and (a) rejected

(b) is the only option consistent with D1, and it puts decode where the schema and the
reading rules must live regardless. (a) is rejected on architecture, not on performance: it
would make porta the thing this project spent a whole rewrite making it stop being.

(c) should be built **only if a measurement shows (b) is too slow**, and the measurement is
cheap. Recording that here so the deferral is falsifiable rather than vague: *the trigger for
(c) is a real query, over a real month of a real node's history, that (b) cannot answer
inside an interactive latency budget.* No such query has been run.

**RATIFIED (David, 2026-08-22): (b), with (c) held in reserve and (a) rejected.** The
measurement the reserve was conditioned on has since been run — 404,790 `sys/metrics` records
decode in **419 ms**, 1.16 s including I/O, against the worst slice that exists. (c) is not
needed. Successor §7.

## 6. D4 — the payload is the READING RULES, not the schema

**Follows from D1, and it is the part most likely to be under-built.**

A schema that expands `pco` to `pulseCoalesced` leaves the reader exactly as stuck. Worse, a
help desk that returns *data* without the rule for reading it industrialises error at machine
speed. This is not hypothetical — every one of these is already recorded:

- **`largestFreeBlock` is meaningless instantaneously.** The GC fires only on arena
  exhaustion, so the sample before a collection reports whatever the *failing* allocation
  left behind. Over one real record, 26.1% of samples sit below 9260 B and the global minimum
  is 8 B; adjacent samples 30 s apart read 57644 → 2624 → 20816. Any single low reading is
  unfalsifiable as evidence, and quoting one is how rete#153 was opened. The readable gauge is
  the **per-GC-cycle envelope**.
- **`liveBytes` sawtooths with every GC cycle**, so two samples at different phases yield any
  trend you like — including, once, a confident and entirely wrong prediction of imminent
  arena exhaustion.
- **Short windows manufacture trends.** A known-flat 10 h run yields ±7σ trends in *both*
  directions when sliced into 1.5 h pieces.
- **A node id is a slot, not a board.** One space has carried 19 builds from ≥2 boards under
  one id, so any aggregate is silently meaningless unless segmented by `obs/build` first.
- **`allocs − frees` is not universally "outstanding blocks"** — `oevm_realloc` bumps `allocs`
  and never `frees`, so a realloc-heavy workload drifts upward while perfectly clean.

An agent asking "median free memory for XYZ last month" today gets a plausible, wrong number.
So a field descriptor must carry more than a name:

| facet | example |
|---|---|
| short key | `pco` |
| name | `pulseCoalesced` |
| meaning | pulses merged because the mailbox was full |
| unit / kind | count, **cumulative since boot** (not a rate, not instantaneous) |
| validity rule | — |
| segmentation | segment by `obs/build` before aggregating across time |

and for `lfb`: *"valid only at a sweep boundary; use the per-GC-cycle max, never an
instantaneous sample."*

**This knowledge exists today and is unreachable**, living in issue comments, `CLAIMS.md`
rows and design addenda. Making it addressable is most of the value of the whole idea.

**UNDERSTATED, corrected 2026-08-22.** The fourth rule above says "a node id is a slot, not a
board". The store demonstrates the converse as well — **a board is not one id**: boot counters
prove `c6-bench` → `c6-timerless` → `c6spare` are one device, and `c6-bell` → `c6` another. An
aggregate keyed on a node id is wrong in *both* directions, and the fix is a derived lineage
rather than a segmentation rule. Successor §5.

## 7. D5 — schema and rules are per-BUILD, held northbound, generated not written

- **Per-build, not per-node.** Every node on a given build has the same record shapes. porta
  already knows each node's `obs/build`.
- **Nodes do not publish it.** A 512 B frame spending bytes on identical self-description is
  the wrong trade, and rete#197 exists because that budget is already tight. The node
  publishes *data*; the northbound half supplies *meaning*.
- **Generated from the documents of record**, so it cannot drift. `PROTOCOL-NSL-RECORDS.md`
  §4.4 already carries most of the prose per record; `CLAIMS.md` and the design addenda carry
  the reading rules. Hand-maintaining a second copy would recreate the drift problem this
  project keeps paying for.

**The documents stay prose.** They are the provenance and the reasoning. The generated
artifact is the interface. Those are different jobs and should not be merged.

## 8. D6 — the agent is the client, not the help desk

"Is the help desk a wad of `.md` files, or itself an agent?" is a false choice.

- `.md` files are **provenance** — where the reasoning lives. They stay.
- The **interface** is the structured, generated substrate of §7, served through gatekeeper.
- The **agent is the client**, sitting in the knowledgeable human's chair. Smalltalk had no
  help-desk agent; it had uniform access. Adding a conversational layer over a substrate that
  does not exist would produce fluent, unfalsifiable answers — the exact failure §6 is about.

Build the substrate. The agent is then whatever asks it, and needs no privileged position.

## 9. What this note does NOT decide

**All five were decided on 2026-08-22. Dispositions in brackets.**

- Whether gatekeeper is a porta subsystem or a separate process that reads porta. (Instinct:
  separate, because D1's whole point is that porta is sheltered and gatekeeper is exposed —
  but that is an instinct, not a decision.)
  **[Separate process, same Go module so it shares `cval`. The instinct was right.]**
- The porta↔gatekeeper interface shape.
  **[A snapshot FILE, not a database handle — `porta snapshot` + `immutable=1`. This also
  dissolves the sheltered/exposed tension, because a file can live anywhere. Successor §2.]**
- Whether "agent-first" means the compact machine surface is primary and the human view is
  derived from it. **If yes, it settles rete#197's display question in the same stroke** —
  short keys are the surface, and the annotated human view is a rendering of the descriptor
  table rather than a separate concern.
  **[Yes. rete#197 is settled: the keys are the surface, the human view is a rendering.]**
- Retention: how much history porta keeps, which bounds every aggregate query.
  **[Retain until there is a reason to delete. No bound, so §7's arithmetic is what keeps
  unbounded aggregates honest.]**
- Anything about identity mechanism, transport or deployment.
  **[Identity partly decided: device is `platform nodeId`, ROLE is `goal/name` and optional,
  lineage is derived. Whether the device id becomes hardware-rooted is rete#242. Auth (G5)
  and deployment remain undecided.]**

## 10. The only next step proposed: five questions against the store as it stands

Do not design further before this. Take five questions someone actually wants answered and
try to answer them against today's store:

1. Median free memory for node XYZ over the last month.
2. Which nodes disagree with their `goal/` tree right now.
3. Did any node's GC arena grow over its last soak.
4. What changed between build A and build B on the same node.
5. Which nodes are stale, and why — never checked in, checked in and rejected, or wedged.

**RUN 2026-08-22.** The five questions became the successor's acceptance tests (§10 there),
and the prediction below is recorded as a test expectation rather than as prose — so it fails
loudly if it was wrong.

**What blocks each one is the design input.** The prediction, recorded so it can be wrong:
(2) and (5) are already answerable by tree/exchange queries; (1) and (3) need value
projection **and** the §6 reading rules, and are wrong-but-plausible without them; (4) needs
build segmentation. If that prediction survives contact, D3 has its measurement and §6 has
its first five descriptors. If it does not, this note needs revising before anything is built.

---

**Refs:** rete#197 (wire labels; the prompt for this note), rete#153 (the `largestFreeBlock`
reading rule), rete#150 (frame budget), `porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md`
(D2 verbs-at-the-CLI, D4.1 raw CBOR, D8 no silently-deleted capability),
`protocol/PROTOCOL-NSL-RECORDS.md` §4.4, `review/tests/CLAIMS.md`.
