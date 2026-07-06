# Milestone C — capability-scoped channels + reactor (design)

**Status:** DRAFT 2026-07-06, awaiting David's ratification (esp. §5 D-C3/D-C4, §7 strings, §9 Q1).
**Parents:** `2026-07-03-nsl-tuvm-first-step-design.md` (§Coordination, §Milestone C, §6),
`tuvm/review/2026-07-03-first-step-design-addendum.md` A1,
`tuvm/review/test-suites-spec.md` T-C, `tuvm/review/demo-node.ns` (the target program +
its WHAT THIS FILE PROVED findings 2/5), `tuvm/review/lib/errors.md` (L5),
`tuvm/review/lib/fsm.ns` (L4 — lands this milestone per `lib/README.md`).
Program position: phase 1 milestone C of 4, `tuvm/review/program-sequence.md`.

## 1. Scope

Implement, host-only, all policy in nsl over a thin C substrate (first-step design
"the C seam carries data, not control"):

- **TupleSpace** — the degenerate (exact-channel) space: registry of channels, each with
  its standing reactions and a global FIFO event queue. Degenerate case ONLY (§6 of the
  first-step design): no wildcards, no associative matching, no persistent tuples —
  additive later, forbidden now.
- **Channel capability** — rooted-prefix addressing per addendum A1 (segment-aligned,
  canonicalized at grant, monotonic attenuation).
- **Three verbs** — `out:value:`, `whenever:do:`, `take:`. No blocking `in`/`rd`.
- **Reactor dispatch** — dequeue one event, run its reactions to completion, repeat.
  Host driver: `space drain` (run until queue empty — demo-node `main:` uses it). The
  supervisor's idle-wait loop, crash boundary, grants-at-setup and restart policy are
  Milestone D; C builds the machine D wraps.
- **fsm.ns Protocol lands** (its only outstanding dependency was the channels API).

Out of scope (pinned so it isn't re-litigated): crash containment (D), timers.ns (D —
needs the host seam), `on:do:` (phase 2 §8), Zephyr/ISR (phase 2 — but see §6:
the C substrate must not foreclose the ISR→channel pulse).

## 2. Concepts and naming

- **Space**: one per node, owned by the supervisor (D). `platform space` answers it in
  host mode (demo-node spelling).
- **Channel path**: URL-style, `/`-separated segments (`node7/reading`). Paths are data
  (Strings) — a path is NOT authority.
- **Capability**: an object rooting a prefix into the space — the ONLY way to touch
  channels. `space channel: 'node7'` derives a capability at that root (space itself is
  the root capability, supervisor-held). All verbs take paths RELATIVE to the
  capability's root; `''` means the root itself (demo-node: `chan whenever: '' do:`,
  `chan out: 'reading' value:`).
- **Attenuation**: `cap channel: 'sub'` answers a narrower capability rooted at
  `<root>/sub`. Monotonic per A1 — a derived capability can never address what its
  parent could not. There is no widening operation.
- **Capabilities are constructor arguments, not messages** (demo-node finding 5, the
  spec line program-sequence.md demands): a job receives its capabilities as object
  references at construction (`TempNode i2c:bus:chan:timers:power:`); capabilities are
  NOT values in the message algebra (valueRank -1 → posting one crashes `not a value`),
  so authority cannot leak through the space. Structural, not conventional.

## 3. The three verbs (surface frozen on ratification)

| Verb | Contract |
|---|---|
| `cap out: relPath value: v` | Validate `v` is a value (algebra `checkValue:` — crash `not a value`), resolve `relPath` against the root (crash on A1 violation), enqueue the event (channel, v) at the FIFO tail. Non-blocking; answers the capability. If the channel has no reactions at DISPATCH time, the signal is dropped (transient — T-C C.7), not queued for later registrants. |
| `cap whenever: relPath do: aBlock` | Register a standing reaction on exactly that channel. Takes effect for events enqueued AFTER registration completes (C.7; see D-C5). `aBlock` receives the posted value (`[:e | …]`). Answers the capability. No unregister verb in this milestone (§9 Q2). |
| `cap take: relPath` | Remove-if-present-now: answers the oldest queued-but-undispatched value on exactly that channel and removes it from the queue, or nil immediately if none (never blocks). nil-return is the single sanctioned sentinel (errors.md L5 — "empty now" is the meaning, not a failure). |

`out:` posting and `take:` operate on the same FIFO queue the reactor drains — `take:`
is how a non-reactive reader (demo-node `main:` reading `node7/reading`) intercepts an
event before the reactor would drop it for lack of reactions.

## 4. Dispatch semantics — pinned by prior documents (restated, with provenance)

- **P1 Exact-channel dispatch.** A posting on channel C fires only reactions registered
  on exactly C. Registration at a prefix does NOT observe subpath postings —
  `whenever: ''` on `node7` never sees `node7/reading` traffic (so demo-node's job
  can't feedback-loop on its own publishes). The capability prefix scopes what you MAY
  address; each registration/post names ONE canonical channel. (First-step design
  §Coordination "exact-channel dispatch"; wildcard matching forbidden.)
- **P2 Run-to-completion.** A firing runs the handler to completion on the single stack;
  nothing preempts or suspends it. This is what makes D's single `setjmp` boundary
  sufficient (first-step design §Milestone D) and A3's "no VM suspension" hold.
- **P3 No re-entry + FIFO.** A handler that posts (to its own or any channel) does not
  re-enter dispatch recursively; new postings go to the queue tail and dispatch only
  after the current firing completes. Multiple postings dispatch FIFO. (T-C C.6 — "the
  reactor's core semantic promise".)
- **P4 Transient signals.** Posting to a channel with no reactions is a defined no-op —
  dropped, not stored. (T-C C.7.) `take:` (§3) is the only buffering, and only until
  dispatch reaches the event.
- **P5 Segment-aligned confinement.** A1 verbatim: capability at `P` addresses `C` iff
  `C == P` or C starts with `P` + separator (`sensor` ≠ `sensors/temp`, `a/b` ≠ `a/bc`);
  canonicalization at grant time rejects (never silently normalizes) `a//b`, `./a`,
  `a/../b`, trailing separator; attenuation monotonic. A matching bug here is a
  capability escape, not an ordinary defect — the confinement tests must include the
  collision cases (C.1–C.4).

## 5. Dispatch semantics — NEW decisions this design adds (David to ratify)

- **D-C1 One event queue, node-wide.** Events dispatch in global posting order, not
  per-channel order only. Rationale: single-stack reactor, deterministic replay,
  matches C.6's cross-posting FIFO test shape. Rejected: per-channel queues with a
  scheduler (ordering policy with no first-step consumer; additive later if fairness
  ever matters).
- **D-C2 Event = (channel, value) pair, dequeued exactly once.** `take:` competes with
  dispatch for queued events (§3); once dispatch dequeues an event it is gone from
  `take:`'s view. Rejected: rd-style peeking (forbidden verb family).
- **D-C3 Multiple reactions on one channel: broadcast fan-out, registration order.**
  All reactions standing on exactly that channel fire for the event, sequentially, in
  registration order, each as its own run-to-completion firing. The whole fan-out for
  event N completes before event N+1 dispatches (P3 extends: postings made anywhere in
  the fan-out queue behind it). When D's crash boundary lands, it wraps EACH firing —
  reaction 1 crashing does not rob reaction 2 of the event (consistent with D.1's
  "other reactions keep running" and the first-step §7 criterion "other reactions
  unaffected"). NOTE the common idiom stays single-reaction: demo-node registers ONE
  `whenever:` and demultiplexes in the Protocol FSM; fan-out exists for observer
  patterns (a logger beside porta's bridge on `node7/reading`).
  Rejected: one-reaction-per-channel, second `whenever:` crashes (simplest, but
  forecloses observers and makes channel topology rigid for a constraint no document
  demands). Rejected: first-registered-consumes / competing consumers (turns
  `whenever:` into a work queue; `take:` already owns consume semantics; wrong default
  for a reactor over transient signals).
- **D-C4 Duplicate registration is two reactions.** The same block (or the same job)
  registering twice on one channel fires twice, in registration order — the degenerate
  case of D-C3; the space does not identity-dedupe. Rejected: dedupe (requires an
  identity notion for closures the language deliberately doesn't surface).
- **D-C5 Registration visibility boundary is the EVENT, not the queue.** A reaction
  registered during a firing sees events ENQUEUED after its registration — including
  events already in the queue behind the current one? No: pinned the simpler way —
  it sees events enqueued after registration; events enqueued before (even if not yet
  dispatched) were addressed to the channel's then-standing reactions… REJECTED as
  bookkeeping (per-event reaction snapshots). PINNED: dispatch consults the channel's
  CURRENT reaction list at the moment the event (or, within a fan-out, nothing — the
  fan-out list is snapshotted when the event dequeues) begins dispatch. Concretely:
  (a) register during firing of event N → fires for any event dispatched after N,
  including one already queued; (b) never fires for the in-flight event N (C.7's
  "subsequent postings", tightened to "subsequent dispatches"); (c) a reaction added
  to channel X while X's own fan-out is mid-flight joins from the NEXT event (the
  fan-out snapshot). Rationale: one rule ("snapshot at dequeue"), no per-event state.

## 6. The C seam (substrate, not policy)

Per first-step design §"Channel/space core in C": the C primitive owns (1) the channel
registry (exact-match string→id table), (2) the FIFO event queue, (3) enqueue/dequeue/
take operations, (4) the reaction-list storage per channel. nsl owns everything with a
decision in it: the verbs' validation (value-check, A1 path canonicalization + prefix
check), the reactor loop (dequeue → snapshot → fire), registration, and later (D) the
supervisor. Sizing/tuning (queue depth, OOM behavior on enqueue) is a work-order detail
for the implementing task, with one requirement pinned now: **enqueue must be callable
from the C side without entering nsl** — that is the ISR→channel pulse seam
(demo-node finding 2, phase-2 work order `onAlertPost:to:`); Milestone C proves it by
having the host seam (or a test prim) post an event from C.

## 7. Error strings (frozen on ratification — errors.md discipline)

| Condition | Tier | String |
|---|---|---|
| posting a non-value | 2 (crash) | `not a value` (already frozen, Milestone B) |
| grant/derive path fails canonicalization (A1) | 2 | `bad channel path` |
| relative path escapes the capability prefix (A1) | 2 | `outside capability prefix` |
| `take:` on empty channel | contract | answers nil (pinned, errors.md) |
| posting to reaction-less channel | contract | no-op (dropped, C.7) |

## 8. Tests (T-C contract + this design's additions)

T-C C.1–C.7 land as written (`test-suites-spec.md`), including A1's collision cases in
C.1–C.4. This design adds: **C.8** fan-out order (two reactions, one channel: both fire,
registration order, one event) — pins D-C3/D-C4; **C.9** snapshot-at-dequeue (register
during firing: not fired for in-flight event, fired for already-queued next event) —
pins D-C5; **C.10** `take:`-vs-dispatch race ordering (D-C2: taken event never fires).
Suite runs solo/foreground as always.

## 9. Open questions for David (blocking the plan, not the design direction)

- **Q1 `'%power'` (demo-node `main:`).** The `%`-prefixed root smells like a reserved
  supervisor namespace (job-visible channels the supervisor listens on). Is `%` a
  convention to pin (canonicalization must then permit it in segment heads), or a
  placeholder to respell? A1's canonical grammar needs the answer.
- **Q2 No unregister verb** in this milestone: a standing reaction lives until the space
  (or, at D, the job's restart teardown) drops it. OK, or does C need `cancel` now?
  (demo-node cancels TIMERS, never channel reactions — evidence for deferring.)
- **Q3** D-C3/D-C4/D-C5 ratification (the §5 decisions carry the design's only new
  semantic surface).

## 10. Why this stays the degenerate case (§6 guard)

Everything above is expressible as "one-field exact template, transient tuples":
wildcard/associative matching would generalize P1's registry lookup; persistence would
generalize P4; ownership-transfer buffers ride the same queue. None of D-C1..D-C5
forecloses those — fan-out order and snapshot-at-dequeue are properties of the
dispatcher, not the matcher. The reactor is built first; the tuple space grows over it
only when real multi-job workloads justify it (first-step design §6).
