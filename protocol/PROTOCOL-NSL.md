# Porta ↔ nsl node protocol — the space-sync model

**Status: design direction — *ratified* 2026-07-14, pre-implementation on the porta
side.** The node/VM half of the model shipped as oevm **rung 9** (tuvm#32 closed,
option A: the node's space owns the value-holding channel kind — see §3.1); the
porta half (bridge job, store, wire) is unbuilt and lands with the phase-3 rungs in
`review/program-sequence.md`. This document is the conformance spec for
`kind: "nsl"` nodes.

**Companions:**
- Record schemas per path prefix + the radio size law: [`PROTOCOL-NSL-RECORDS.md`](PROTOCOL-NSL-RECORDS.md).
- **The machine-readable contract: [`MANIFEST.cddl`](MANIFEST.cddl)** (rete#243,
  2026-08-25) — NORMATIVE for path → kind, delivery class, direction and record shape,
  enforced at the build/test gates (zcbor) and at porta's store (all three write
  doors). This document holds the *model*; the manifest holds the machine facts; the
  RECORDS catalogue holds the why.
- The Toit-era protocol this one replaced, archived 2026-10-02: [`archive/PROTOCOL.md`](archive/PROTOCOL.md). Legacy; read as evidence only.
- Background / derivation: `review/WHY.md` (§"The model, named", §"Paths extend across the network").
- How this doc reached its current form: [`archive/PROTOCOL-NSL-draft-2026-07-11.md`](archive/PROTOCOL-NSL-draft-2026-07-11.md) — the superseded genesis draft, kept as the reasoning trail.

---

## 0. Scope — one gateway, two node kinds

Porta carries two node protocols side by side, selected by the existing per-node
`kind` column (the heterogeneity seam built for exactly this):

- **`kind: "toit"` nodes speak `PROTOCOL.md` (v1), unchanged.** The nodus fleet
  conforms to it and it works; v1 is *frozen for that kind*, not deprecated. No Toit
  node changes because of this document.
- **`kind: "nsl"` nodes (nsl-tuvm / nRF52840 / Zephyr) speak the protocol in this
  document** — the **space-sync** model.

The v1 `st` kind ("Smalltalk → Berry `.bec`") predates the nsl-tuvm pivot and is
superseded by this document for the Zephyr node class.

> **North star (conditional, not a commitment).** If paths-over-verbs proves out as
> nsl-tuvm deploys under porta, the intent is to revisit the Toit `nodus` fleet and
> try to align it onto this same space model — ultimately *replacing* v1's verb
> protocol rather than carrying both forever. That is an aspiration to be earned by
> the nsl deployment, not a scheduled migration; §6 treats coexistence as a bridge
> toward it, not a permanent state.

---

## 1. Goals and non-goals

### Goals

- **Extension is additive, not multiplicative.** New node state = a new *path* + a
  pinned record schema. v1's unit of extension is the verb/schema, which touches the
  command codec, the report shape, echo rules, self-heal, the conformance section
  and every node kind at once. A path touches none of them. (The full v1→space
  convergence argument is §2.)
- **Disconnected, duty-cycled operation is a requirement, not a degraded mode.** A
  node sleeps on a duty cycle, loses 2–5 % of datagrams, and must keep running while
  partitioned. The protocol *replicates* state and reconciles on reconnect; it never
  fetches on demand.
- **Full-state resync by construction.** A reflashed or amnesiac node resyncs from
  cursor 0 with no special rules — cold boot replays the whole `goal/**` CELL tree.
  Streams are exempt by the replay guard (§ "seq, cursors, and changedSince:"):
  an entry the node has already acked as applied is never served again, so a
  resync cannot re-execute old `goal/do/*` actions. v1's boot-echo drift-healing
  dissolves.
- **Convergence is a query, not a mechanism.** `goal/X` versus `obs/X` is a tree
  diff; the UI's desired-vs-observed view and the self-heal loop are the *same*
  query. There is no separate self-heal engine.
- **One store replaces the per-feature tables** (commands queue, report cache,
  config-desired projection, debug_response, …) — see §4.1.
- **Remoteness lives in the grant.** Node jobs never learn that `goal/**` values
  arrive from porta; the bridge is the single ingress where overflow policy, auth
  and version skew are enforced — one door, same rules as any other event source.

### Non-goals

- **No distributed destructive read.** `take:` never crosses the wire. A remote
  destructive read is a consensus problem (where JavaSpaces grew transactions);
  neither side destructively reads the other's space. Each side publishes into its
  own; the other syncs (§2, "the one law").
- **No bulk payloads on the space.** Firmware images and profiler blobs are *data
  plane*: delivered by a bulk transfer selected by content hash, merely *referenced*
  from a control-plane record. Big bytes never ride channels (the same law as
  drivers). See §3.3.
- **Auth does not ride the space it guards.** Node identity + session auth wrap the
  exchange at the transport layer; prefix authority is enforced *inside* (§3.3).
- **This is not a synchronous mount.** We are not building 9P: there is no fetch
  from a server that must be up *right now*. We replicate, precisely because the node
  must survive partition (§2, "which prior art").
- **No request/reply between jobs.** The space has `post`/`take`/`peek`, and no
  *call* — a job cannot ask another job for a value. This is load-bearing: it is
  *why* the value-holding kind had to live in the space and not in the bridge's
  private map (§3.1).
- **No server-initiated push — CoAP Observe is out of scope.** The exchange is
  always node-initiated. There is no standard way to deliver server push reliably to
  a *sleeping* node (RFC 7641 Observe presumes a reachable observer); the standards'
  answer for sleepy endpoints — LwM2M **Queue Mode**, the CoAP **mirror-server /
  Resource Directory** pattern (RFC 9176) — is client-initiated drain to an always-on
  mirror, which is exactly what porta *is*. Freshness and responsiveness are bought
  with adaptive **cadence** (§4.4), not push. Observe would be a mains-only
  micro-optimization that also makes the gateway stateful; it is deliberately unused
  (transport note, §5.3).

---

## 2. Why this shape — the convergence argument

This is not a redesign for taste. Read with the nsl node's tuple-space model in
hand, **v1 has been converging on a space protocol one revision at a time**, and
each revision was the rediscovery of a rule the space model has natively:

| v1 mechanism (and its bespoke rules) | What it is in space vocabulary |
|---|---|
| "Commands are declarative and absolute, last write wins" | a **cell** per target — the name *has* a value |
| `reboot`'s exception paragraph (imperative, exactly-once, terminal on delivery, no convergence) | a **stream** entry — the state/event split, native |
| `node_config` echo: "only on cold boot and on change; absent never clobbers cache" | a cell with publish-on-change; per-path values need no clobber rules |
| `chip`/`sdk`/`kind` "absent never clobbers known identity" | same — per-path cells |
| gateway self-heal (diff desired vs reported config, re-enqueue divergent `set`s) | **anti-entropy between a `goal/` tree and an `obs/` tree** |
| `set-mode` atomicity rule (accept whole or reject whole) | one record at one path is atomic **by construction** |
| `set-forward` "the command is the whole policy, not a patch" | a cell simply *is* the whole value |
| FATAL/panic "must-deliver subset" | a per-stream delivery class |
| `commands?id=` drain-until-empty; `debug?id=` "same drain pattern" | `take:ifEmpty:`-until-empty — the transport surface was already channels |

The structural diagnosis: **v1's unit of extension is the verb/schema, which is
multiplicative** — each new piece of node state touches the command codec, the
report shape, echo rules, self-heal, the conformance section and every node kind. **A
space's unit of extension is the path, which is additive**: new state = new path +
a pinned record schema. v1's own evolution rule ("add optional fields only") becomes
"add paths" — the version of that rule that scales.

The nsl node makes this nearly free on its end: the node's space already exists
(`out:` / `take:ifEmpty:` / `whenever:do:` / `put:value:` / `peek:ifAbsent:`,
prefix-granted capabilities). The porta client on the node is an ordinary **bridge
job**, not a protocol stack.

### The one law

**Location transparency must not become failure transparency** (Waldo, Wyant,
Wollrath & Kendall, *A Note on Distributed Computing*, Sun Labs TR-94-29). The wire
does not make remote look local; it makes remote *nameable* while keeping its
delivery contract explicit:

- `out:` across the wire is **best-effort, batched, eventually delivered**;
- `whenever:do:` across the wire is a **subscription** (sync of changes since a cursor);
- **`take:` never crosses the wire** (non-goal §1). Each side publishes into its own
  space; the other syncs.

### Which prior art — and which to ignore

Recorded because it tells us what to steal and what to leave:

- The **naming** model is Plan 9's — the namespace *is* the interface, `walk` ≈
  `resolve:`, and Plan 9's `fid` is precisely our `Channel` capability. Its lesson
  for "two kinds": keep the verbs uniform and let whoever serves a name supply the
  semantics.
- **But we cannot use Plan 9's remote model.** A mount means every read is a round
  trip to a server that must be up *right now*; our node sleeps, loses datagrams, and
  runs while partitioned. Disconnected operation is a requirement → we cannot fetch
  on demand → we must **replicate** — and *that* is where `seq`, cursors and the
  stopped-state discipline come from. They are forced by the radio, not chosen for
  elegance.
- Which means **we are building etcd, not 9P**: their `revision` is our per-path
  `seq`, their *watch-from-revision* is our `changedSince: cursor`, and **their
  compaction hazard is ours too** (#121 — a node whose cursor falls behind
  porta's retention point is silently stranded; §7). Steal their vocabulary *and*
  their scars.
- **LOCUS** is the cautionary bookend: Plan 9 got away with far more transparency
  than LOCUS largely because its interface was *file I/O, which everyone already
  knows can fail*. Our contract keeps failure visible on purpose.

---

## 3. The model — one picture

```
        PORTA (gateway)                            NSL NODE
  ┌──────────────────────────┐             ┌──────────────────────────┐
  │  per-node space mirror   │   check-in  │  the node's own space    │
  │                          │  exchange   │                          │
  │  node/<id>/goal/**  ─────┼────────────▶│  goal/**   (in)          │
  │  node/<id>/obs/**   ◀────┼─────────────│  obs/**    (out)         │
  │  node/<id>/tel/**   ◀────┼─────────────│  tel/**    (out)         │
  │  node/<id>/sys/**   ◀────┼─────────────│  sys/**    (out)         │
  │  node/<id>/diag/**  ◀────┼─────────────│  diag/**   (out)         │
  │  node/<id>/dbg/req  ─────┼────────────▶│  dbg/req   (in)          │
  │  node/<id>/dbg/resp ◀────┼─────────────│  dbg/resp  (out)         │
  │  node/<id>/gw/**         │   (never)   │  — not present —         │
  └──────────────────────────┘             └──────────────────────────┘
       porta writes ONLY goal/** and dbg/req;    the bridge job holds the
       the node writes ONLY its own node/<id>/** radio capability + these grants
       gw/** is porta's own verdict ABOUT a node: gateway-authored, never on
       the wire, and excluded from the exchange by the store's direction
       column rather than by a filter (design 2026-08-10 §6.1)
```

**The picture carries direction and authority, not kind** — deliberately, because kind is
not a property of the prefix.

Every name resolves to one of **two kinds**, and **kind is a property of the leaf, set by
its first write** (D6). This replaces v1's per-verb rules *and* an earlier draft of this
document that fixed kind by path prefix — a rule the node's own families already refute:
`dev/thermal0/sample` is a cell while `dev/thermal0/tick` is a stream under one prefix,
and `sys/**` carries both `sys/panic` (stream) and `sys/metrics` (cell). The wrong verb on
an existing name is an ordinary, catchable error (enforced in nsl `Channel`, not in the C
space — which is storage, not policy):

- **cell** — a pigeonhole that *holds* the latest letter. `put:value:` overwrites it;
  `peek:ifAbsent:` reads it **without removing it**. State lives here (`goal/**` apart
  from `goal/do/*`, `obs/**`, the `sys/` gauges, `diag/spans/**`). Sync transfers cell
  *values* changed since a cursor.
- **stream** — a pigeonhole you *empty* by reading. `out:value:` appends;
  `take:ifEmpty:` removes the oldest, once. Each letter is delivered once. Actions
  and telemetry live here (`tel/**`, the `sys/` events, `dbg/*`, `goal/do/*`). Sync
  transfers entries once, in order, deduped by sequence number.

**A party that does not own a name must ask its kind before it touches it**, and must
not use the raising verb. Kind is pinned by the first write and re-created by replay,
so one wrongly kinded write would otherwise disable a reader or a writer permanently,
with no recovery short of a new path. The two seams where this bites are both on the
node: reading a gateway-written leaf (`Channel>>peekCell:ifStream:`) and applying an
inbound entry whose kind the *operator* chose (`Channel>>kindAt:`, used by
`PortaBridge>>applyEntry:`). See the sixth bridge rule in §4.1.

Every value on the wire is a **CBOR record** from the nsl message algebra (null,
bool, int, float, string, bytes, symbol, list, map, record — the L6/`cbor.md`
encoding). Schemas are pinned **per path prefix** in `PROTOCOL-NSL-RECORDS.md`;
evolution is additive (new paths, new optional record fields).

### No `clear:`, no tombstone

There is no delete verb and no tombstone. This is a deliberate deletion of a whole
mechanism, on the industrial-control rule: *there is no "clear" — there is a start
button and a separate stop button, and stop always wins.*

- **Absence means only "nobody has ever said anything about this name,"** and the
  safe answer to absence is the fail-safe default.
- A stopped loop is `auto: false`; an un-deployed app is `runlevel: #stopped`. The
  operator **writes the stopped state explicitly** — it is a value, not a deletion.
- If a record must ever be genuinely *removed* (not stopped — removed), that is an
  **action**: a letter on a `do/` stream, not a property of the pigeonhole.

### `seq`, cursors, and `changedSince:`

Replication needs three things the raw event queue never had, and rung 9 gave the
space: a latest-value slot per cell, a monotonic per-path `seq`, and a
changed-since-cursor query. The node's cursor is RAM, deliberately (design
2026-08-26 D3): persistence would buy only a bounded boot-time re-pull and
would cost a stranding mode after a gateway store reset. Cold boot = cursor 0,
and what replay-from-0 MEANS is per kind (design 2026-08-26 D1):

- **cells** — the latest value per path, read from the cell tree, so the replay
  is bounded by tree size and idempotent by construction;
- **streams** — only entries above the node's acked high-water. The gateway
  keeps, per node, the maximum cursor the node has ever declared applied
  (`cursor_hw`, monotonic, clamped to what the gateway has actually SERVED —
  `served_hw`, the highest seq delivered in a capped batch — because a node
  cannot have applied what was never served), and never serves a stream row
  at or below it — to any boot, on any path, uniformly. An applied
  `goal/do/*` action therefore cannot re-execute across reboots; an
  UNAPPLIED entry is served until acked, so at-least-once within a boot and
  delivery of rows written during an outage are both unchanged. Two honest
  edges: the receipt attests DELIVERED-AND-OFFERED, not executed — a node
  that sheds or counts-and-drops an entry still advances its cursor past it,
  and since the #232 rider that count is visible as `obs/health.rej`
  (RECORDS §4): a rejected entry is a PERMANENT loss under this guard, so it
  must be seen, not merely counted — and a pull
  declaring a cursor beyond the store's own out-seq is REFUSED (`#err`,
  visible in `obs/health.ref`) rather than recorded: that node is synced to
  a different store's history, and its next reboot resyncs from 0.

**A cell's `seq` is not a liveness signal across a reboot.** The counter is RAM state and
restarts at 0 on every boot — porta logs the restart and adopts the node's declared
high-water rather than reading the rewind as duplicates (§4.1, the fifth rule) — so a
node rebooting every few seconds reaches a *lower* seq than one wedged since its last
boot, which is exactly backwards. **Progress and liveness are read from the append-only
event log**, whose rows only ever accumulate; the #25 host-OTA gate found this by writing
its first liveness assertion on `seq` and watching it fail against a healthy node.

---

## 4. The node end

### 4.1 The bridge job

An ordinary supervised nsl job, granted: the radio/transport capability, write
access to `goal/**` and `dbg/req` (inbound), and read/reaction access to `obs/**`,
`tel/**`, `sys/**`, `dbg/resp` and `diag/**` (outbound). A crash of the bridge is a
contained fault: porta sees a late check-in, nothing worse.

`diag/**` joined the outbound grant with tuvm#107 (2026-08-05). It is outbound ONLY:
the node reports what it measured. Driving a capture from porta would need an
*inbound* path, and §3's inbound authority is deliberately two paths wide — see the
note under RECORDS §4.6.

**The VM did change — and had to.** The value-holding kind lives in the *space*, not
in the bridge's private map. tuvm#32 settled this (rung 9, option A): because the
space has `post`/`take`/`peek` and **no call**, a cell held privately in the bridge
would be invisible to the supervisor reading `goal/apps/**` — and §4.2/§4.3's whole
claim ("the protocol *is* the space, filtered by grant") would be false. So the space
gained: a value slot on `SpaceChannel`, prims 105–107 (`spaceKind:` / `spacePeek:` /
`spacePut:value:`), and the nsl `Channel>>put:value:` / `peek:ifAbsent:` verbs (with
`take:ifEmpty:` as the sole stream read, tuvm#21). The bridge is a *consumer* of that
kind, not the owner of it.

On each check-in (cadence per `obs/mode`, exactly as v1 derives liveness):

1. **Pull** — request `goal/**` (+ `dbg/req`) entries with `seq > cursor`, declaring
   the `cursor` applied so far, the `hw` the node believes porta has accepted
   from it (see the fifth rule below), and **`wak`** — the count of this boot's
   check-ins, required; porta judges progress from it and from nothing else
   (rete#272 D3.3). It rides the pull and not the push because the pull is built
   fresh every tick, while the push frame is re-sent unchanged until it is acked
   (rete#199) and would carry a stale count exactly when it mattered. Apply each
   to the local space (cell paths become local cell values via `put:value:`;
   `goal/do/*` stream entries are `out:`ed once); advance the RAM cursor only after
   apply (the cursor is deliberately not persisted — §3 and design 2026-08-26 D3;
   a crash mid-apply therefore re-pulls rather than skips, and the gateway's
   acked high-water keeps the re-pull from re-executing applied stream entries).
2. **Push** — send cell `obs/**`, `sys/**` **and `diag/**`** values changed since the
   last acked push, plus queued `tel/**` / `sys/**` / `dbg/resp` stream entries, each
   batch tagged with a node-side sequence for gateway dedup. All three cell prefixes
   are the node's own authority per §4.1; scanning only `obs/**` (as this said until
   2026-08-04) left RECORDS §4.4's `sys/metrics` — a pinned cell — unsendable by
   construction, and scanning only those two (until 2026-08-05) did the same to the
   span cells tuvm#107 had just made small enough to send. A value that fits the cap
   but is never scanned is unsendable in exactly the same way.

On the node these are the *same paths* its local jobs already use — the supervisor
reacts to `goal/apps/**` exactly as it reacts to any channel. There is no separate
"protocol handler": **the protocol is the space, filtered by grant.**

**What landed on the node (2026-08-02 — plan 2 of the space-sync rewrite, host-green).**
The VM gained ONE monotonic counter per space plus a `last_seq` per channel (D5),
reached by prims 138–140 (`spaceSeq`, `spaceStamp`, `spaceChangedSince:under:max:`) and
the nsl verbs `Channel>>changedSince:max:` / `Space>>seq` / `Space>>stamp`. The bridge
is `class PortaBridge` in `kernel.nsl`, beside `NetBridge`; the node holds ONE instance.
Six rules were decided in the writing and are part of the contract — the fifth added
2026-08-05, after hardware found what its absence cost, and the sixth 2026-08-09, after
a review of the #25 branch found the same shape on the write side:

- **The changed-since scan answers CELLS ONLY.** A stream's `seq` advances on every
  write, but its value is killed on delivery, so a stream row would carry a
  stale-or-nil value under a fresh seq. Streams reach the wire by reaction capture in
  the bridge, stamped with `spaceStamp` so both halves share ONE ascending sequence.
  Reading the counter rather than bumping it would let two captures collide on one seq,
  which the gateway drops as a duplicate — silent loss, not a repeated delivery.
- **Ascending order is load-bearing.** The bridge sends a *prefix* of the scan and
  advances its cursor to the last entry it sent, so any other order would strand
  everything it skipped. The same rule governs trimming a batch from the tail to fit
  the 512 B cap: what is dropped rides the next check-in.
- **Inbound authority is enforced on the node,** because the node is the thing that
  would be lied to. porta may write `goal/**` and `dbg/req`; an entry at any other path
  is counted and dropped, never applied and never raised. An accepted write to `obs/**`
  would forge the node's own report of what it observes, which is the one thing the
  convergence view rests on.
- **`diag/spans/**` is syncable** as of tuvm#107 (2026-08-05): the per-slot rows are sibling
  cells at `diag/spans/<span>` (140 B each) and `diag/spans/result` carries the window
  summary `{ n, ms }` only, so no value in the subtree approaches the 256 B cap. The rows
  are written BEFORE the summary — a reader that reacts to `diag/spans/result` is entitled
  to find every row already in place (RECORDS §4.6).
- **A RESTART is declared by the pair (`cursor`, `hw`) going backwards together, and it
  rewinds porta's high-water** (added 2026-08-05). porta's stored high-water for a node
  is a *cache* of the node's own `acked`, and both numbers a pull carries are RAM state
  that only advances within a boot. When **both** go backwards, the node lost that RAM,
  so it will never issue those seqs again and the cache is unusable: porta adopts the
  declared `hw`.

  Without it a **rebooted node is indistinguishable from a duplicate** — the space
  counter restarts at 0 and porta's high-water does not, so every cell the node writes
  is skipped as already-applied, and the ack carries the stale high-water, which
  `applyAck:` adopts. After that the node believes everything it writes has been
  accepted and porta holds observations hours old. Neither end can tell.

  **Both halves of the test are load-bearing, and `hw` alone is NOT enough.** A missed
  ack also leaves the node declaring a lower `hw`, and that case must not rewind: porta
  committed those rows and the node is legitimately re-offering them, so a rewind
  deletes the `seq <= hw` dedupe and manufactures the duplicate it was meant to prevent.
  `bootId` is what separates them, and it is evidence rather than inference: it is a
  counter the node increments once per boot and persists in NVS (RECORDS §3), so only a
  reboot can change it and a swallowed ack cannot. **It rides the wire as `bid`** since
  rete#197; this document says `bootId` where it means the mechanism and `bid` where it
  means the field.

  **This replaced a cursor heuristic on 2026-08-09 (rete#162).** That test — rewind when
  the declared cursor *and* watermark both went backwards — was sound but incomplete: a
  node porta has never written to has a cursor of 0 on both sides, so `0 < 0` was false
  and its restart carried no signal at all. The gap was named in the code for four days
  before a node fell into it. A node's own boot counter needs no `goal/` to have been
  written, so the heuristic has no remaining case and was deleted rather than kept beside
  the replacement.

  **That gap was witnessed in the field on 2026-08-06 and cost `c6-bell` ten hours of
  telemetry (#162).** The node had only ever written `obs/build` and `sys/metrics` — porta
  had never written to it — so its stored cursor was 0, `cursor < storedCursor` was
  `0 < 0`, and the rewind could not fire. It rebooted, pushed against a stale high-water of
  3981, had every row discarded, adopted 3981 from the ack, and then reported
  `misses 0` on its console every 30 s while delivering nothing.

  **Closed 2026-08-09 by making `bootId` real.** The `porta put <node> goal/name <node>`
  inoculation this section used to prescribe is no longer needed for new nodes: it worked
  by making `storedCursor` nonzero, which was the precondition the *cursor* rewind was
  missing, and that mechanism is gone. Two operational notes survive it:

  - A node that has been running since before this landed still needs **one power cycle**
    to clear its in-RAM `acked` — `applyAck:` raises that value and never lowers it — and
    a second one before porta has two different boot ids to compare.
  - A node reporting `bootId 0` has **no persistent boot identity** and is not protected
    by any of this. porta says so on every check-in; that line is the signal to check why
    its settings/NVS backend is unavailable, not noise to filter.

  Found on hardware 2026-08-05 by the `bridge-health` target gate — hidden until then
  for as long as the C6 gate wiped porta's store before every run — and the `hw`-only
  version of the fix was caught within one leg by `porta_sync_c6.sh`, which saw the
  dedupe stop firing. porta side is `Store.NoteCheckIn`.

- **An inbound entry whose kind disagrees with the leaf's PINNED kind is counted,
  reported once on `tel/log`, and SKIPPED — the batch completes and the cursor advances**
  (added 2026-08-09, rete#174). The wire carries a kind the *operator* chose, and kind is
  pinned by the first write, so `put:` onto a pinned stream and `out:` onto a pinned cell
  are both reachable by one ordinary `porta put --kind …`. Both raise in `Channel`, which
  is correct for a caller that owns the name and fatal for the bridge, which does not:
  the raise escaped `applyBatch:` **before the high-water was adopted**, so the cursor
  never advanced, the same batch re-pulled, and it raised again every tick forever. The
  node went on exchanging and applied nothing, and neither end could see it — the same
  silent-permanent shape the read side had at `goal/apps/main` before
  `peekCell:ifStream:`. The bridge now asks `Channel>>kindAt:` first.

  **This stops the wedge; it does not make the two ends agree.** porta then believes the
  leaf is one kind and the node keeps the other, permanently, marked only by one log row
  per replay. porta's `Store.Write` pins no kind at all — D6 is unenforced on the gateway
  side, and that is **rete#180**, not this rule.

Still owed, each filed rather than assumed: the **cursor** lives in RAM (`bootId` no
longer does — rete#162, 2026-08-09; the cursor half of #117 is deliberately still open,
because persisting it changes what a reboot MEANS and is entangled with replaying
`goal/do/*`), the bridge holds the root space rather than a `goal`-write + `obs`-read pair
(G5), cadence is a constant rather than §4.4's controller, and a stream entry written
while the bridge is down is lost — bounded and honest for `tel/**`, which is lossy by
policy, but **not** for `sys/panic`, which RECORDS marks must-deliver.

**What hardware added (2026-08-01, plan 3 — the C6 gate).** The exchange was witnessed on
an ESP32-C6 over a real Thread mesh against `porta serve` on the border router, including
a deliberate post-commit ack drop. Two facts belong to the protocol rather than to the
bench:

- **Exactly-once is two claims.** No duplicates *and* no gaps. On loopback only the first
  can fail, so a gate that checks only duplication is green for the wrong reason.
- **A 512 B batch is not one radio frame.** The 802.15.4 PHY payload is 127 B, so a
  full-size datagram is five or six 6LoWPAN fragments and losing any one kills the whole
  datagram. Batch sizing trades against loss probability on a mesh in a way it does not on
  a LAN, and a node's `misses` being non-zero is normal operation, not a fault.

**A third fact corrects, rather than adds to, §4.1 above.** N7 held that a missed
check-in "costs latency and nothing else, which is the whole reason there is no
retransmit layer" — that was wrong on real hardware. With the gateway unreachable, an
unacked bridge's `pending` queue grew without bound and OOM-panicked the node at tick 48
(~96 s) on a 128 KB heap: a miss also costs memory, and costs it unboundedly, which no
loopback run could show because loopback's gateway always answers. The fix bounds
`pending` at 64 rows, oldest-first eviction, with a counted `dropped` published as
`#streamDropped` — so a node that discarded telemetry is distinguishable from one that
had none to send. This narrows, but does not resolve, the "still owed" item above:
`sys/panic` must-deliver semantics remain undecided, and "drop the oldest" is the wrong
answer for that path specifically — the decision belongs with #114.

### 4.2 Node-side path map (what replaces the nine verbs)

| v1 verb / report field | nsl path | kind | record shape (sketch) |
|---|---|---|---|
| `run` | `goal/apps/<name>` | cell | `{crc:, size:, triggers:, runlevel:, lifecycle:, arguments:}` |
| `stop` | `goal/apps/<name>` | cell | write `runlevel: #stopped` — a value, not a delete; reconcile uninstalls |
| `set-mode` | `goal/mode` | cell | `{mode:, minAwakeS:, maxAwakeS:, maxAsleepS:, loopSleepS:}` — atomic because it is one record |
| `set-name` | `goal/name` | cell | string |
| `set-forward` | `goal/forward` | cell | whole-policy record, as v1 already demands |
| `set` (per-app key) | `goal/config/<app>/<key>` | cell | scalar; per-key last-write-wins is per-path, native |
| `reboot` | `goal/do/reboot` | **stream** | `{}` — exactly-once via cursor; v1's exception paragraph dissolves |
| `debug` (the breakpoint set) | `goal/debug/<app>` | cell | `{sha:, breaks:}` — the whole set for one image, empty = detach (rete#375); the session itself is the `dbg/req`/`dbg/resp` stream pair; the node reports the hold on `obs/debug` |
| `profile` | `goal/profile/<app>` | cell | `{action:, durationS:, continuous:}` |
| report `apps` | `obs/apps/<name>` | cell | observed `{crc:, runlevel:, lifecycle:, triggers:}` |
| report `config` echo | `obs/config/<app>/<key>` | cell | scalar echo |
| `node_config` echo | `obs/mode`, `obs/name`, `obs/forward` | cell | publish-on-change is what cells do; no echo-cadence rules |
| `health` | `obs/health` | cell | `{up:, bid:, wak:, pto:, sdr:, ref:}` — liveness and delivery; `ref` counts REFUSALS (`#err` replies) distinctly from `pto`'s timeouts, because a refused node is answered on every exchange and would otherwise read as perfectly healthy (rete#199); VM internals are `sys/metrics`. The labels were `uptimeMs`/`bootId`/`wakes`/`pollTimeouts`/`streamDropped` until rete#197 shortened every label on the frame (2026-08-11) |
| `liveness` (declare) | `obs/liveness` | cell | `{misses:, telCad:}` — the node's own cadence and miss allowance; porta judges `now - last_seen > telCad × misses` and infers nothing (rete#231, rete#273 D2). The app-liveness deadline `appCad` is declared to the on-node observer and never crosses the wire (D1) |
| `liveness` (request) | `goal/liveness` | cell | `{misses:, telCad:}` — the operator **requests**; the node clamps `telCad × misses` to 24 h on `misses` and publishes what it adopted. The `goal`/`obs` diff is the answer; there is no grid to learn, because a budget shorter than a cadence is unrepresentable |
| `health.reset` (fault) | `sys/reset` | stream | `{category:, code:}` — v1's "data_log on first fault" is just an entry |
| telemetry `print`/`log` | `tel/print`, `tel/log` | stream | `{text:}` / `{lvl:, txt:}` (`{level:, text:}` until rete#197) |
| telemetry `metric` | `tel/m/<name>` | stream | `{value:, ts:}` |
| telemetry `panic` | `sys/panic` | stream, **must-deliver** | bounded inline summary + `BlobRef` (RECORDS §2.4) |
| debug lines | `dbg/req` ↓ / `dbg/resp` ↑ | stream | `{line:}` — porta does not interpret or own the session (v1 §8); scope of that claim: RECORDS §4.5 |

### 4.3 What stays OFF the space

- **Bulk image payload** — data plane. Delivery is a bulk transfer selected by
  content hash, referenced from the `goal/apps/<name>` record. What rides is raw image
  bytes verified against that hash; v1 §5's CRC32 does **not** carry over, because the
  `BlobRef`'s hash is already the integrity check. **The transport today is the block
  frame of §5.3** — exchange port + 1, stop-and-wait, resumable by block number — with
  CoAP blockwise the ratified direction for Thread. Not TFTP: that was v1's surface and
  it was deleted with v1.
- **Auth / identity (G5)** — the grant machinery cannot ride the thing it guards.
  Node identity + session auth wrap the exchange at the transport layer; prefix
  authority is enforced *inside*: porta may write only `goal/**` and `dbg/req`; the
  node may write only its own `node/<id>/**` mirror.
- **Profiler blobs** — opaque and potentially large; delivered like payloads
  (referenced, pulled in bulk), not as stream entries.

`sys/panic` is the one apparent exception, and it is not one: the *report* is a
bounded ~60-byte inline summary (reason, bootId, monoMs, truncated detail) plus a
`BlobRef` for the core dump. Truncation is legal here and nowhere else — a node
dying of OOM can still **report** that it died of OOM, because the report needs no
buffer to build (RECORDS §2.4).

### 4.4 Check-in cadence — liveness, freshness, load

Because the exchange is always node-initiated (non-goal §1), one node-side variable —
the **check-in cadence** — governs four concerns that are usually tangled. They pull
that one knob in different directions, and the knob is **nsl policy**, not a transport
setting (the space is mechanism; when to sync is policy — same split as everywhere
else in this stack):

| Concern | What it is | Direction | Pulls cadence… |
|---|---|---|---|
| **Request latency at the server** | Time from porta writing a `goal`/command to the node acting on it — downstream *actuation* responsiveness as seen from porta | porta → node (pull) | **faster**, on demand (active debug, pending goal) |
| **Data acquisition freshness** | How stale `obs`/`tel` is at porta, and `goal` at the node | both (push / pull) | **faster**, per application need |
| **Server / mesh ping load** | Aggregate check-in traffic across the whole fleet | fleet aggregate | **slower** — idle nodes must back off |
| **Node-alive detection** | How fast a dead node must be noticed | node → porta | sets the **floor** (check in *at least* this often) |

The structure that falls out:

- **Liveness sets the floor** — the slowest allowed cadence (`maxAsleepS`): miss it and
  porta declares the node offline (`offline = k × cadence`, §5.2).
- **Battery sets the ceiling** — the fastest allowed cadence on a sleepy node. An
  always-on node has effectively *no* ceiling; the `mode` lifts it, and cadence policy
  fills the room. **This is why always-on nodes are not stuck on an artificial slow
  poll** — nothing forces a fixed period; on a radio LAN 2–10 Hz is real-time for
  anything short of a hard control loop.
- **Freshness and request-latency pull up** toward the ceiling; **fleet-load pulls
  down** toward the floor. The node runs a small **adaptive cadence controller**
  between the two bounds, keyed off `mode` plus application signals (observed change
  rate, a pending goal, an active debug session).

Two deliberate choices:

1. **Node-alive is *not* a separate message.** An idle check-in *is* the heartbeat
   ("cursor=N, nothing to push" → "nothing new"), so liveness comes free from the sync
   exchange. The only case that would justify a separate fast liveness ping is a node
   that must be *known-dead fast* while syncing *data slowly* — named here, not built,
   until a use case needs it.
2. **The server never dictates cadence — it may only *hint*.** porta cannot push to a
   sleeper, so its only lever on fleet load is an optional **back-pressure hint** in
   its check-in response ("you may slow to X") that a node's policy may honor. This
   keeps porta passive and stateless. Idle backoff on the node alone likely suffices
   first; the hint is future (§7).

---

## 5. The porta end

### 5.1 Store

One mechanism replaces the per-feature tables (commands queue, report cache,
config-desired projection, debug_response, …):

- `space_cells(node_id, path, seq, value_cbor, ts)` — latest value per path,
  monotonic per-node `seq` assigned on write. No tombstone column: a "stopped" record
  is an ordinary value; a genuinely removed path is driven by a `do/` action.
- `space_events(node_id, path, seq, value_cbor, ts, direction)` — append-only;
  inbound (node→porta) rows are the telemetry/forensics log, outbound (porta→node)
  rows are the command/debug queues.

Convergence is **structural**: `goal/X` vs `obs/X` is a tree diff — the UI's
desired-vs-observed view and the self-heal loop are the same query. Self-heal as a
*mechanism* disappears: a node whose `obs` diverges simply hasn't applied the `goal`
seq yet (visible), and a node that lost state resyncs from cursor 0 (automatic —
cells; applied stream actions are never re-served, per §3's replay guard).

### 5.2 Operator surface

CLI/API verbs become path writes and tree reads — the vocabulary stays
operator-friendly while the plumbing unifies:

- `porta app run <node> <name> …` → write `goal/apps/<name>` cell (+ stage image)
- `porta app stop <node> <name>` → write `goal/apps/<name>` with `runlevel: #stopped`
- `porta mode <node> deep-sleep …` → write `goal/mode` cell
- `porta reboot <node>` → append `goal/do/reboot` stream entry
- `porta get <node> [prefix]` → dump the cell tree (goal + obs, diffed)
- `porta tail <node> [prefix]` → follow `tel/**` / `sys/**` streams
- `porta debug send/poll` → `dbg/req` append / `dbg/resp` read-after-cursor (v1 §8
  semantics preserved verbatim)

Liveness derivation is unchanged from v1: `offline = k × cadence`, cadence read from
the `obs/mode` cell. The heartbeat *is* the exchange — and this `cadence` is the
liveness *floor* of the node's own adaptive policy (§4.4), not a fixed period.

### 5.3 Wire framing (first cut — ratified at phase-3 design)

Keep the node-initiated, gateway-passive shape (deep-sleep-correct and
transport-agnostic):

- **Pull** — node requests `space?id=<id>&cursor=<n>` → body = CBOR array of
  `[seq, path, kind, value]` for outbound entries with `seq > n`, plus the new
  high-water. Empty body = up to date.
- **Push** — node sends CBOR array of `[nodeSeq, path, kind, value]`; gateway dedupes
  on `nodeSeq` high-water and acks it.
- **The `id`** (rete#118, 2026-10-02) — the node's factory address as lowercase hex, most
  significant byte first, no separators: 12 characters for a board with a radio (its
  48-bit link address, the sticker value), 16 for one without (its flash serial). It is
  read from silicon at boot and never configured, built in or defaulted; a node that
  cannot read it does not check in. It is a name within one porta's store, not a
  credential. The human-facing name is `goal/name` ↔ `obs/name`, and the bench name of
  the board is the optional `board` field of `obs/build`. Definition and rationale:
  `porta/docs/ARCHITECTURE.md` §3.8.
- Transport — whatever the node class carries. **Today it is plain UDP carrying
  CBOR**; the v1 TFTP surface this bullet once named as the day-one carrier was
  deleted with v1 and carries nothing. **The ratified direction is CoAP** — logged in
  §8 under its original name, TFTP → CoAP, from when TFTP was the carrier — for
  `kind: "nsl"` nodes (Zephyr ships a native CoAP client/server, so this rides the
  same BSD-socket seam already proven for UDP in rungs 5–7). CoAP earns the switch
  for its **resource model** (paths = URIs), **ETag-conditional GET** (our `seq`/cursor
  in-protocol), **DTLS** (a real answer to G5), **CBOR content-format**, and
  **resumable blockwise** image transfer — *not* for throughput (blockwise is
  stop-and-wait, ≈ TFTP-with-blksize) and *not* for Observe, which is out of scope
  (non-goal §1). Only the **client-initiated** subset is used, so one profile serves
  sleepy and always-on nodes alike. Framing is transport-independent by construction;
  the one thing to retire before committing is the **DTLS RAM footprint** on the most
  constrained target (§7).

**The bulk plane, as actually built (rete#25, 2026-08-08).** Images never ride the
exchange; they are pulled over a second port, **exchange port + 1** — a convention, not a
knob, so a node that knows where to check in knows where to fetch. The framing is fixed
binary and the server is stateless: the node asks `0x01 ‖ hash[32] ‖ blockNo u32 BE`,
porta answers `0x02 ‖ blockNo ‖ more ‖ data` with `data` ≤ 256 B (the value cap, and
radio-honest), and every refusal is `0x7F ‖ text`. Stop-and-wait, one datagram in and
one out, nothing kept per client — so **resume is the client re-asking from any block number**,
and a killed transfer costs only the blocks it had not got. This is deliberately the
shape of a CoAP blockwise GET, so the ratified **TFTP → CoAP** direction above is
unchanged and the later move is framing, not redesign — still gated on the §7 q7 DTLS
footprint measurement. Binary rather than CBOR because the fetch loop lives in the node's
C seam, which cannot reach the nsl `Cbor` class, and a C encoder for one fixed 37-byte
request would be more mechanism than the frame it encodes — CoAP, the successor, is
itself binary-framed. **CBOR stays the law of the space planes; this is the data plane's
one binary surface.**

**The size law is a real constraint, not a free choice.** The binding limit is the
*radio*, not the heap: 6LoWPAN fragments above ~102 B, and a datagram survives only
if every fragment does, so at the bench fleet's measured worst-node loss
(4.6 %/datagram) a 1 KB batch arrives ~54 % of the time. This governs whether
`obs/**` pushes go per-path or snapshot-batched (§7 open question) — measure it in
the fleet-sim (RECORDS "the size law").

---

## 6. Coexistence & migration

- Porta serves v1 and this protocol side by side, selected by node `kind`. No Toit
  node changes.
- **First implementation target: the G10 host fleet-sim** (N host oevm VMs + porta
  over localhost UDP) — prove the exchange, cursor resync, and kill-mid-transfer
  recovery there before any radio is involved.
- The nsl node's conformance doc is this file; the bridge job + C seam land with the
  phase-3 (CBOR wire) rungs in `review/program-sequence.md`.
- **The bridge toward the north star (§0).** Coexistence is the mechanism that lets
  the nsl model earn its keep in production *before* any Toit node is touched. If it
  earns it, the follow-on is a v1→space alignment for the `nodus` fleet — the verb
  protocol retired, not perpetually carried. That step is out of scope here and gated
  entirely on the nsl deployment going well.

---

## 7. Open questions (for the phase-3 design doc)

1. **Cursor persistence granularity** — ANSWERED 2026-08-26 (design D3): the cursor
   is deliberately not persisted at all; the question of write granularity dissolves
   with it. What would reopen it (a measured boot re-pull cost) is recorded in the
   design record.
2. **Porta retention window + the compaction-vs-cursor hazard** (#121, *still
   unanswered*): how long porta keeps cell history, and what happens to a node whose
   cursor falls behind that point — etcd's compaction scar, ours too (§2). This is the
   one hazard the cell/stream decision did *not* close.
3. **Event-queue overflow at the *gateway* mirror** (a chatty node vs porta storage)
   — per-prefix retention policy, mirroring the node's own G2 overflow vocabulary.
4. **`dbg` session flow-control** (v1 relies on poll pacing; keep, or add a window?).
5. **Auth handshake, concretely (G5)** — what wraps the exchange on localhost UDP for
   the sim vs Thread in the field; where the node's porta-identity pairing lives (G7).
6. **`obs/**` push shape** — per-path or snapshot-batched on constrained MTUs (Thread
   fragmentation vs chattiness); decided by measurement against the §5.3 size law.
7. **DTLS RAM footprint on the most constrained target** — the one gate on the
   TFTP → CoAP transition (§5.3). CoAP message handling is cheap; mbedTLS DTLS
   handshake/session state is not. Measure it against the dongle's arena budget (a
   G-series footprint gate) before committing CoAPs; CoAP-without-DTLS is nearly free.
8. **Porta back-pressure hint** — whether porta's check-in response carries an optional
   "you may slow to X" cadence hint (§4.4) or idle backoff on the node alone suffices.
   Keeps porta passive either way; measure fleet load in the sim first.

*(The former open question "pin the per-prefix record schemas" is now **done** —
`PROTOCOL-NSL-RECORDS.md`.)*

---

## 8. Provenance & decision log

Distilled from the 2026-07-11 architecture review (actors-at-the-grain /
Linda-at-the-joints framing; `review/WHY.md`). The §2 convergence table cites v1
behaviours verbatim from `PROTOCOL.md`.

Decisions folded into this edition (full reasoning trail in
[`archive/PROTOCOL-NSL-draft-2026-07-11.md`](archive/PROTOCOL-NSL-draft-2026-07-11.md)):

- **tuvm#32 CLOSED — the space owns the value-holding kind** (rung 9, option A). The
  bridge is a consumer, not the owner; "no VM changes" was wrong, and the VM changed
  (§4.1).
- **Vocabulary: cell / stream**, never "retained". "Retained" is MQTT's word for a
  broker-side delivery trick; we mean a *state semantics* — *this name has a value*.
  Every system that solved this uses the first word or a synonym (Plan 9 *file*,
  ZooKeeper *znode*, etcd *key*, REST *resource*).
- **No `clear:`, no tombstone** — stopped state is written explicitly; removal is a
  `do/` action (§3).
- **(2026-07-15) Transport direction: TFTP → CoAP**, client-initiated subset only,
  **Observe out of scope** (§5.3, non-goal §1). No standard delivers server-push to a
  sleepy node; the standards' sleepy answer *is* our client-initiated-to-a-mirror
  model. Gated on a DTLS footprint measurement (§7, open question 7).
- **(2026-07-15) One adaptive check-in cadence, as nsl policy** (§4.4), bounded
  [liveness floor ≤ cadence ≤ battery ceiling], pulled up by freshness/request-latency
  and down by fleet load. Dissolves the "always-on nodes stuck on a slow poll" worry
  without a new mechanism; node-alive stays coupled to the sync heartbeat.
- **(2026-09-16) Wire versioning, #200 ratified as amended: the two closures are
  already separate, the catalogue already has a version, and the envelope joins the
  manifest.** Field closure is relaxed and tag/type closure kept on BOTH ends today, by
  construction and unstated — porta looks envelope fields up by name
  (`porta/internal/wire/message.go:22`), the store's manifest gate is a first-byte check
  with deep validation at the build-time zcbor gates, and the node builds
  `Record tag:fields:` from whatever map arrives (`kernel.nsl:1068`). What lands under
  #253 is the statement (RECORDS §1) and a gate on each end (extra key tolerated, unknown
  tag refused). The catalogue's version is the MANIFEST.cddl hash pinned per node at
  `obs/build.man` (#243 D4); **the five envelope shapes go into `MANIFEST.cddl`** so that
  one hash covers the whole wire (David, (b) over a `v` field). The flag-day era ends at
  #253's freeze line, which IS that hash published as 1.0 — not at #197. A `v` in the
  envelope stays deferred on #200's trigger: the first rollout that cannot complete inside
  one maintenance window. #199 (a refused node halts) is closed, so "reject" is useless
  for a staged rollout rather than destructive.
- **(2026-09-17) rete#273 BUILT.** `Liveness { cad, bud }` → `{ misses, telCad }`; the budget
  is derived, `bud = telCad × misses`, so the grid snap and the `bud < cad` guard are deleted
  rather than replaced. `gw/liveness` may read `#neverSeen` and `#unknown` (rete#285 — the
  manifest now says so). The node's app-liveness deadline (`appCad`) is declared to the
  observer through one primitive and never crosses the wire. Every node built from here
  pins the manifest hash (`obs/build.man`); the hash itself moved with the next entry.
- **(2026-10-02) rete#118/#242 BUILT: the node id is the factory address.**
  §5.3 defines the `id`; the build-time id is deleted, not defaulted. `obs/build` gains an
  optional `board` text field between `chip` and `build`, which is the only manifest
  change. Manifest sha256 `d7eefc871e538b77…`, armed on the gateway the same day; every
  node built from here pins it. The bench-named spaces are historical from this date.
  Record: `docs/design/2026-09-11-node-identity-hardware-rooted-design.md` and its
  addendum; board proof `docs/results/2026-10-02-118-node-id-board-proof.md`.
