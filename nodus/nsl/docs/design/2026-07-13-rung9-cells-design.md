# Rung 9 — Cells: a name that has a value

*Design. Written 2026-07-13, the day tuvm#32 was decided. Owner of the decision:
`tuvm/review/cells-and-streams-v3.html` (deck v3) + `tuvm/review/cells-sketch.ns` (the
same scenario written under option A and option C). Ratified by David: **option A**.*

---

## 1. What this rung is

The space today has exactly one kind of name: a **queue**. This rung adds the second:
a name that **has a value**.

> a **stream** — a pigeonhole you *empty*. Each letter delivered once; taking removes it.
> a **cell** — a pigeonhole that holds *the latest letter*. Reading does not remove it.

That is the whole feature. It is small on purpose.

## 2. Why it is forced (and why nothing else was)

Three facts about `src/space.c`, **verified, not assumed**:

1. **every name is a queue** — `space_post` / `space_take` / `space_register`. No slot
   holds a value.
2. **an event with no reaction registered is DRAINED AND DROPPED.** `space_dequeue`
   snapshots the channel's reaction list *at drain time* and then marks the event dead
   unconditionally (`space.c:262`, `queue[idx].live = 0`). **The stream does not wait
   for a subscriber who arrives later.**
3. **there is no request/reply.** There is `post` and `take`; there is no *call*. A job
   cannot ask another job — or the bridge — to re-send anything.

A supervised job crashing is **doctrine** here (Milestone D), not an exception. Put those
three facts together and a restarted PID's setpoint is not *slow to recover*, it is
**unrecoverable** — short of a round trip to porta, over a radio that may be unreachable,
on the read path, which is the one thing that must never happen (deck v3 §10).

A cell is the only thing that fixes that, because **a cell outlives the job that reads
it.** The space is C; it survives every job restart in the node.

## 3. What the mailroom settled (and what it deleted)

David's model, and the design follows it exactly:

- **Everything on the wire is a letter.** The bridge reads the **address** on the
  envelope (`path`), never the contents. The addressee's job opens it. So *nothing in
  this rung teaches C what a setpoint is.*
- **The space enforces nothing.** Which combinations of `sp`/`auto`/`out` make sense is
  the PID's business. The space holds the last letter per name and knows nothing else.
- **There is no `clear:`, and no tombstone.** *"In industrial control there is no clear.
  There is a start button and a separate stop button, and stop always wins."* A stopped
  loop is `auto = false`, not an absent `auto`. An un-deployed app is
  `runlevel: #stopped`, not a missing record. **Absence means exactly one thing —
  nobody has ever said anything about this name** — and the safe answer to that is the
  fail-safe default. If a record must ever be *removed* (not stopped — removed), that is
  an operator **action**: a letter on a `do/` stream, not a property of the pigeonhole.

That deletion is what makes this rung small: **a cell is a `value` slot. Nothing else.**

## 4. The namespace it has to serve (David's two-PID node)

```
/node5/pid2/sp      cell     the operator — or a cascade loop's `out`
/node5/pid2/pv      cell     the AI driver, WITH QUALITY: {value: 54, quality: #good}
/node5/pid2/out     cell     the PID in auto; the OPERATOR in manual. one cell, LWW.
/node5/pid2/auto    cell     either end. one cell, LWW.
       pid2/alarm   stream   the AI driver. an occurrence; survives the repair.
```

Three properties this must not break:

- **`/node5` is the bridge's business, not the node's.** It is truncated on delivery and
  prepended on push. The node does not know it is node5 → **the same app image runs on
  any node**. Nothing in this rung introduces a node id anywhere.
- **Two PIDs = two rooted capabilities and one piece of source.** `pid2` holds
  `space channel: 'pid2'` and says `peek: 'sp'`; so does `pid1`. Isolation is by name,
  through the capability each was constructed with. This already works — cells must
  simply resolve through the same `resolve:` path as `out:value:` does, and get it for
  free.
- **The alarm stays a stream.** It is an occurrence: it must be *delivered*, and it
  outlives the repair that makes `pv` good again. This is the first place cell-vs-stream
  earns its keep **entirely inside the node**, with porta nowhere in the picture.

## 5. The surface

### nsl (`lib/kernel.ns`, class `Channel`)

```
public put: rel value: v     (* write the cell. creates it. fires whenever: reactions. *)
public peek: rel ifAbsent: aBlock
                             (* read, non-destructively. aBlock on "never written". *)
```

Two methods. `peek:ifAbsent:` — not a `nil` sentinel — is the kernel's existing
expected-miss convention (`review/lib/errors.md`; `demo-node.ns` header: *"expected-miss
-> ifAbsent:; everything else crashes"*). It is also why **tuvm#21 is no longer on this
rung's critical path**: a cell needs two states (*has a value* / *never had one*), not
three, and `ifAbsent:` distinguishes them at the language level without asking `take:`'s
nil-vs-empty ambiguity to be fixed first. #21 stays open, on its own merits.

### C prims (append-only after 104 `heapStats`)

| id | prim | answers |
|---|---|---|
| 105 | `spaceHasValue:` | `true` iff this path is a cell that has been written |
| 106 | `spacePeek:` | the cell's value (undefined if `spaceHasValue:` is false) |
| 107 | `spacePut:value:` | writes the slot, then enqueues an event so reactions fire |

A predicate + a reader, rather than one prim answering a `{found, value}` pair, because
`peek:` is on a **control loop's hot path** — the PID reads `sp` every tick — and an
allocation per read is not acceptable on a 48 KB arena. Two prim calls, zero allocation.

### The verb declares the kind

`put:value:` makes a name a **cell**; `out:value:` makes it a **stream**. A name's kind
is fixed at first write. Using the wrong verb on an existing name is an **error**, raised
as an ordinary nsl condition via `vm_raise` (Milestone E — so it is catchable with
`on:do:` and, uncaught, reaches the supervisor as a crash):

- `out:value:` or `take:` on a cell → error
- `put:value:` on a stream → error
- `peek:ifAbsent:` on a stream → error
- `whenever:do:` works on **both**, and is the one verb that does not care.

This is deliberate and was argued in deck v2: mixed namespaces are ordinary. `read()` on
a directory returns `EISDIR` and nobody has ever called that a design flaw.

## 6. Mechanism

`struct SpaceChannel` (a **32-entry static table**, `OEVM_SPACE_MAX_CHANNELS`) gains:

```c
oop     value;       /* the cell's value; NULL when never written */
uint8_t kind;        /* 0 = unset, 1 = stream, 2 = cell — set by the first write */
uint8_t has_value;   /* distinguishes "wrote nil" from "never written" */
```

**`space_put` enqueues an event.** It writes the slot *and* pushes onto the same ring the
stream path uses, so `whenever:` reactions fire through the **existing, unchanged**
reactor — `space_dequeue` → `Space>>drain` → the block. There is no second delivery
mechanism, no new reactor path, and no change to `Space>>drain`. The difference between
a cell and a stream, from the reactor's point of view, is only that **the cell also kept
a copy**.

Two consequences, both intended and both documented rather than papered over:

- **`whenever:` on a cell fires on every WRITE, not on every CHANGE of value.** We do not
  compare the new value to the old one (deep record equality on a hot path, to save a
  reaction that is idempotent anyway). "Fires on change" in deck v3 means *a write
  happened*. De-duplicating identical writes is the writer's business.
- **A `put` with no reactions registered still leaves the value in the slot.** The event
  is drained and dropped, exactly as fact (2) above says it must be — and *that is
  precisely the case a cell exists to survive.*

**GC: `space_gc_trace` must mark `channels[i].value`.** This is the one edit in this rung
that is silently catastrophic if missed — the cell's value is a new GC root, reachable
from nothing else in the heap. It gets its own test (§7, test 05), because a GC bug here
would otherwise show up as an occasional wrong setpoint on a running plant.

**Deferred to rung 10 (the bridge), deliberately:** `seq` and `changedSince:`. They have
no consumer until the bridge pushes `obs/**` to porta, and building a mechanism with no
customer is the same mistake I argued against for option B. They are additive — a struct
field and a prim, no image-format impact. Recorded here so their absence is a *decision*,
not an omission.

**RAM:** +6 bytes × 32 channels ≈ **192 B of BSS** (padding may round it to 256). Static,
not arena. The MCU footprint ratchet is a text gate and should not move; if the RAM line
(tuvm#5) moves, it gets re-banked with the reason.

## 7. Gates

**The gate that decides the rung** — host only, no radio, no bridge, no porta:

> **write a setpoint · crash the job · let the supervisor restart it · peek the correct
> setpoint back.**

It passes under option A and **cannot be written under option C**. That asymmetry *is*
the argument, made executable in about forty lines.

`nsl-tests/15_cells/`:

| test | proves |
|---|---|
| `01_put_peek.ns` | put, peek non-destructively (twice — same answer), `ifAbsent:` on a never-written name |
| `02_survives_crash.ns` | **THE GATE.** Supervisor + a job that crashes; the restarted job peeks its setpoint back |
| `03_kind_mismatch.ns` | wrong verb on each kind raises, catchable with `on:do:` |
| `04_whenever_on_put.ns` | a `whenever:` reaction fires on put **and** the value is still in the slot afterwards (a stream's `take:` would have removed it) |
| `05_gc_root.ns` | put, force GC, peek — the value survives (guards the `space_gc_trace` edit) |
| `06_two_loops.ns` | David's two PIDs: one class, two rooted caps, isolated `sp`s |

Plus the standing five: suite green **both paths** (Path A solidified / Path B `.oimg`),
MCU footprint ratchet, portability-32, C tests. Floor moves 183 → 189.

## 8. What this rung does not do

- **No bridge.** Rung 10.
- **No `seq` / `changedSince:`.** Rung 10, with the bridge that needs them.
- **No option B.** The radio remains a `dev/` driver; served prefixes stay unbuilt and
  unblocked. If B ever lands, **A's cell becomes B's first server and no job's code
  changes.**
- **No tombstone, no `clear:`.** Deleted by §3, and not coming back through the side door.
