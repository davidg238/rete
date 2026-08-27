# The space-sync durability trio: the replay guard, the RAM cursor, and what must-deliver means

2026-08-26. Resolves #119 and #117, closes #262, decides #114 (implementation rides #232).
Answers the first hazard of `2026-08-01-porta-space-sync-rewrite-design.md` §8. #121 is the
opposite edge (a cursor *behind* retention) and is deliberately untouched here.

## 1. What exploration found, and how it reshaped the rung

The queue line said "NVS-backed cursor/bootId + replay guard". Two of those three halves
turned out to already exist:

- **bootId persistence landed with #162/#215.** `platform_backend_boot_id()` persists an
  incrementing counter under the settings key on every Thread board; the bridge latches it
  once at construction; porta's `NoteCheckIn`/`Ingest` already use it for restart detection
  with the `!=`-not-`>` rewind. #117's bootId half was closed in practice and only its
  cursor half remained open.
- **The cell half of the replay guard landed with the rewrite itself.** `OutboundSince`
  reads cells from the cell tree, so cursor-0 already means *latest value per cell path*,
  bounded by tree size. Only the **stream** half was missing — which is exactly the half
  #262 hit in the field: two verification boots each replayed cursor 0→3 and re-ran a 60 s
  `dbg/req` profile window, on every boot, forever.

So the trio reduces to **one porta-side mechanism and two recorded decisions**. This rung
touches no node code and no wire shape.

## 2. Decisions

### D1 — The replay guard is an acked high-water, uniform over streams.

Porta keeps, per node, the maximum out-seq the node has ever **acked as applied** —
`nodes.cursor_hw`, monotonic. The pull path never serves a stream row with
`seq <= cursor_hw`. Cells are untouched (already latest-value). The node's pull cursor is
documented load-bearing on the node as "the last seq already APPLIED"
(`kernel.nsl`, PortaBridge header) — with one honest qualifier the final review added:
the node advances its cursor past an entry it counted-and-dropped or shed under
pressure, so the receipt attests delivered-and-offered rather than executed. Surfacing
those drops is node-side work, tracked on #232 alongside #114's implementation.

The invariants, stated so they can be falsified:

- **At-least-once within a boot.** A lost BATCH never advanced the node's cursor, so it
  never advanced `cursor_hw`; the re-pull re-serves. The exchange's no-retransmit posture
  is unchanged.
- **At-most-once across boots.** Re-execution of an applied `do/`-shaped action is
  impossible. #262's rows stay permanent in the append-only log but are *served* once per
  application, ever.
- **Delivered-while-down still delivers.** A row written during an outage has
  `seq > cursor_hw` and reaches the next boot exactly once.
- **Push-direction independence.** `node_seq_hw` and the #162 rewind govern the node→porta
  sequence space and are untouched. A node-side NVS erase restarts `boot_id` and has no
  bearing on `cursor_hw`, which lives entirely in porta's out-space.

**This supersedes #119's proposed wording** ("serve `do/` streams only above the node's
current bootId"). The bootId rule needed per-row boot stamping and silently dropped a
request written while the node was down; the high-water needs neither and its semantics
are strictly better: *an applied stream row is never served again; an unapplied one is
served until acked.* bootId stays what #162 made it — restart **detection** — and is
deliberately not part of the guard predicate.

**Uniform, not classed.** The guard applies to every outbound stream alike, not only
`do/`-class paths. Re-serving an *applied* stream entry is by definition a duplicate on
every path; there is no known beneficiary of the distinction, so the manifest's class axis
is not consulted and no new class value is minted (#262 sketched one; this is simpler).

### D2 — The clamp: `cursor_hw = max(cursor_hw, min(cursor_in, served_hw))`, and the door refuses what it cannot clamp. (CORRECTED at the rung's final review)

As first approved, the clamp was `min(cursor_in, out_seq)` — and the final review proved
that version CREEPS: re-evaluated on every check-in against a growing `out_seq`, a stale
impossible cursor (porta store reset under a still-running node) gradually marks
never-served rows as applied — check-in at cursor 500 against `out_seq` 0 records 0, the
operator writes two rows, the next check-in at cursor 500 records 2, and the node's
eventual reboot silently never receives them. The exact inversion of what this decision
existed to prevent.

The correction is two parts, both from the same principle — porta only ever records what
it can vouch for:

1. **The receipt is bounded by what was SERVED.** `served_hw` is the highest seq actually
   delivered to the node in a capped batch (`NoteServed`, from the pull door, post-capping,
   only when entries rode the batch — an empty batch's `hi` is the node's cursor echoed,
   not a delivery). A node cannot have applied what was never served, so
   `cursor_hw <= served_hw` always.
2. **An impossible cursor is refused, not recorded.** A pull declaring
   `cursor_in > out_seq` names a history this store never issued — the node is synced to
   a different store (a reset, a restore). The door answers `#err` (the node's rete#199
   `refused`/`lastErrMsg` machinery makes it operator-visible) and records nothing; the
   node's next reboot resyncs from 0. Refuse-don't-guess is the same posture as closed
   decoding. This also closes the second-order case the served-bound alone leaves open:
   without the refusal, once `out_seq` outgrows the stale cursor, rows above it would be
   served and acked, and the monotonic watermark would then blanket the never-served rows
   below it.

### D3 — The node's cursor stays RAM, deliberately.

With D1 in place, cursor-0 is *safe*; persisting the cursor buys only boot-time economy —
skipping a re-pull of the goal cell tree, today a handful of cells in one or two 512 B
batches. No measurement says that cost is real, and the posture rule applies: ship the
degenerate case, let data earn the knob back.

Against that, persistence is not "one settings key". Two hazards were traced:

1. **Stranding.** The node's cursor is forward-only (`hi > cursor` only) and porta serves
   only `seq > cursor`, so a porta store reset with persisted node cursors leaves every
   node permanently ahead, receiving nothing, until reflash. Today's cursor-0-per-boot is
   what self-heals that.
2. **Naive rewind is unsafe.** The obvious fix — make BATCH `hi` authoritative in both
   directions — reintroduces a re-execution hazard the forward-only rule quietly prevents:
   the bridge's pull is send-then-wait with a 2 s budget, and a reply that misses its
   window can arrive during the *next* pull's wait and be consumed as its answer. Today
   the max-rule makes that stale batch harmless; authoritative-`hi` would let it rewind
   the cursor mid-boot and re-deliver — for a stream row, re-execute. Do not "simplify"
   into this later.

**The earned follow-on, recorded so the deferral is falsifiable:** if a measured boot
re-pull cost becomes real (a large goal tree, or a duty-cycled battery node where every
radio second is budgeted), the shape is: the NVS cursor key, plus an **explicit reset
signal** — porta detects `cursor_in > out_seq` at the pull door and answers a
distinguished reset indication; the node zeroes both the RAM cursor and the NVS key on
it. Not authoritative-`hi`.

### D4 — Must-deliver means re-offered until acked from a durable source. (#114)

Must-deliver is not a property a stream capture can provide: a `whenever:` reaction dies
with the bridge job, and #114's loss window (an entry written between crash and restart)
is structural to capture. It is the property of a **durable source re-offered until
acked** — and for `sys/panic`, that source already exists: the #215 RebootNote is C
state (survives a bridge crash), survives the node rebooting (proven by `wedge-note`),
and is cleared only by explicit `panicNoteAck:`.

So: **no C-side outbound journal is built, and no schema change routes panic through a
cell.** The pattern is — while the note is unacked, (re-)publish the Panic record onto
`sys/panic` at check-in; call `panicNoteAck:` only once porta's ACK covers that entry. A
duplicate panic row in an append-only log is acceptable; a lost one is not. Re-offering
per check-in (rather than once at boot) is what survives the bounded pending queue
evicting the entry under a long disconnection (`maxPending` 64, oldest-first).

RECORDS §4.4's "must-deliver" class *means* this pattern from now on. `tel/**` stays
lossy by policy; nothing else changes class.

**Decided here, implemented with #232** — the publication is node-side kernel/app work,
any kernel.nsl edit needs a board, and #232 is the next board session. This rung stays
porta-only. #114 remains open carrying this decision until that lands.

### D5 — No new operator surface, no tunables.

Suppression is derivable from the exchanges log (`cursor_in` vs the served range). A
counter, a log line, or a class knob waits for a demonstrated need.

## 3. The mechanism

- **Schema.** `nodes.cursor_hw INTEGER NOT NULL DEFAULT 0`, added on open by the existing
  `addColumns` migration path (the same shape `boot_id`'s arrival used).
- **Writer.** `NoteCheckIn` — the pull door, the leg a node always sends — applies D2's
  clamp-and-max. Nothing else writes it; `Ingest` is the other sequence space.
- **Reader.** `OutboundSince` keeps its two-half query, but the halves diverge by kind,
  which is the §3 amendment made mechanical: cells `seq > cursor_in` from the cell tree;
  streams `seq > max(cursor_in, cursor_hw)` from the log.

## 4. Edges traced

| edge | outcome |
|---|---|
| store reset under a running node | clamp (D2) keeps `cursor_hw` honest; node self-heals at next reboot (D3); pre-existing serve-nothing window unchanged |
| node NVS erase (`west flash --erase`) | `boot_id` restarts; `cursor_hw` unaffected — it lives in porta's out-space |
| stale/reordered pull datagram (old low cursor) | `cursor_hw` is monotonic; cells re-serve (idempotent), streams stay guarded |
| lost BATCH within a boot | cursor and hw both unmoved; full re-serve; at-least-once holds |
| row written while node is down | `seq > cursor_hw`; delivered exactly once to the next boot |
| push-direction restart (#162) | untouched; different sequence space, different column |
| stale cursor persists across new writes (the creep) | refused at the door (D2 pt 2); nothing recorded, nothing served, `obs/health.ref` rises |
| restore from an older backup | the store forgot acks it once held; those streams re-serve and re-execute — no watermark survives a store's own amnesia. Documented, not mechanised |
| node down across the upgrade that adds the guard | its first cold boot replays old streams ONCE more (served_hw/cursor_hw backfill 0, guard latches from the next ack) — expected, not a failed fix |

## 5. Documents of record and issue fates

Amendments made in this rung, not after (the promotion rule):

- `protocol/PROTOCOL-NSL.md` §3 — "cold boot = cursor 0 = full replay" amended to state
  replay-from-0 per kind: cells as latest-value-per-path; streams only above the node's
  acked high-water.
- `protocol/PROTOCOL-NSL-RECORDS.md` §4.4 — must-deliver defined as D4's pattern, with
  `sys/panic`'s durable source named.
- `2026-08-01-porta-space-sync-rewrite-design.md` §8 — one-line pointer here; the dated
  record stays historical.

| issue | fate |
|---|---|
| #119 | **closes** — guard built, §3 amended; releases #266's hard gate |
| #117 | **closes** — bootId half landed via #162; cursor half resolved *deliberately RAM* (D3, falsifiable deferral) |
| #262 | **closes** — rows permanent, served once per application |
| #114 | **open** — decision D4 commented on the issue; implementation rides #232 |
| #121 | **open, untouched** — cursor *behind* retention is the opposite edge; do not conflate |

## 6. Raised and deferred: backup gateways (David, mid-design)

"Must-deliver, re-offer indefinitely" raises the question: OpenThread allows multiple
border routers — should there be backup gateways? Two different things wear that name:

- **Multiple BRs are IP redundancy** and are already compatible: they keep the mesh↔LAN
  route alive and porta does nothing to benefit. They do not cover the porta *host*
  dying — porta is an application endpoint at a configured `peerHost`, not the BR.
- **A backup porta breaks D5 unless it shares the store.** The node's cursor and
  `cursor_hw` are scoped to one porta's out-seq space; a second store is a different
  sequence space, and multi-master is a project. If HA is ever needed, the
  contract-preserving shape is one logical store behind one address (failover at the ops
  layer), not two portas.

Two properties of this design already carry the load: D4 makes a gateway outage cost
latency and never loss (the durable source waits for whichever gateway eventually acks),
and D3 + node-relative paths (rewrite D4.5) make re-pointing a node at a replacement
porta exactly the cursor-0 case D1 makes safe. **Deferred, nothing built**; what would
earn it: a deployment with a stated gateway-availability requirement, or a measured
outage cost the node-side re-offer does not already absorb.

## 7. Testing and gates

Go tests carry the whole burden — no kernel.nsl edit, so the host-suite blindness rule
does not bite and no board time is spent:

- store: `cursor_hw` monotonic across pulls; the D2 clamp under `cursor_in > out_seq`
  asserting the fresh store's rows still serve after; cursor-0 pull serves cells but not
  acked streams; lost-batch re-serve within a boot; outage-written row delivered exactly
  once across a reboot; migration adds the column to a pre-`cursor_hw` store.
- wire: #262's field shape end-to-end — seed `dbg/req` rows, ack them, reboot (new
  bootId, cursor 0) → BATCH carries cells only.
- Gates: `go test ./...` and `nodus/oevm/tests/porta_sync_host.sh` stay green (node
  behaviour unchanged by construction). An on-board re-run of the `ReplayProbeC6` recipe
  (commit `0d70b12`) piggybacks on the #232 session as confirmation, not as this rung's
  close condition.
