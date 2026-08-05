# Node metrics over porta — the run-up, not the dying breath

**#56 capability (3), designed 2026-08-04.** The out-of-band metrics leg of the device
observability rung, riding the space rather than a side channel.

## 1. What this is, and what it is not

#56 asked for "out-of-band periodic metrics — push VM stats on a cadence instead of
pulling over HTTP: they survive the crash and give the run-up. Mailbox depth, heap,
handles, drop counters."

This design does **not** build a metrics channel, a metrics record, or a metrics
schema. All three already exist and are green. What it builds is the part nobody
built: **a trigger**, and the one protocol correction that makes the existing record
sendable at all.

It is also explicitly **not** a crash-boundary mechanism. David's framing, 2026-08-04:

> want to get out as much info BEFORE a crash, rather than definitive message that
> could not be sent because sys had crashed

That is a design posture, not a preference. A last-gasp message is a promise made by
the half of the system that is already failing. Everything below follows from taking
it seriously: the node's job is to have already said it.

## 2. What already exists

Counted with a command, not from memory — the standing lesson from the porta capability
audit.

- **The record.** `kernel.nsl:2970` `Kernel>>metrics` composes prim 104 (`heapStats`)
  and prim 109 (`sysStats`) into one flat immutable `Record tag: #Metrics` — 11 fields:
  `heapSize, liveBytes, heapHighWater, largestFreeBlock, gcCycles, handlesLive,
  handlesHighWater, queueHighWater, overruns, pulseDrops, pulseCoalesced`.
- **The schema**, pinned at `PROTOCOL-NSL-RECORDS.md` §4.4, **195 B encoded, MEASURED
  2026-07-21** (`nodus/tests/19_latency/05_record_size.nsl`), 61 B under the 256 B
  value cap.
- **The issue.** tuvm#24 is **CLOSED**, with a DONE row in `review/tests/CLAIMS.md` and
  a host test at `nodus/tests/15_heap_stats/02_sys_metrics.nsl`.
- **The signals #56 asked for by name are in it**: `queueHighWater` and `pulseDrops`
  are the tuvm#54 mailbox-starvation counters; `handlesLive`/`handlesHighWater` are the
  handle table; `largestFreeBlock` is the fragmentation signal that says a years-long
  node is dying slowly, which `heapFree` alone never will.

**And nothing publishes it.** Not one node app. That is not neglect — see §3.

## 3. The finding: `sys/metrics` is unsendable by construction

The two protocol documents contradict each other, and the contradiction is load-bearing.

- `PROTOCOL-NSL.md:248` — **the conformance target**, §§1–4 — defines the push as
  *"send cell `obs/**` values changed since the last acked push, plus queued `tel/**` /
  `sys/**` / `dbg/resp` **stream** entries"*. Cells are scanned from `obs/` **only**;
  `sys/` participates as streams.
- `PROTOCOL-NSL-RECORDS.md` §4.4 pins `sys/metrics` as **kind C (cell)**.

`kernel.nsl:1705` implements the conformance target exactly:

```
cells:: (space channel: 'obs') changedSince: acked max: self maxRows.
```

So a cell written at `sys/metrics` is never scanned, never batched, never pushed. The
value would sit in the node's space forever. **There was nowhere for it to go**, which
is the honest reason no app publishes it, and it is invisible from either document read
alone.

**D1 — widen the cell scan to `sys/` and correct §4.2, rather than move the record.**
The alternatives were to re-pin the record at `obs/metrics` (zero bridge change) or to
make it a stream (zero protocol change). Both resolve the contradiction by moving the
record to fit an implementation accident, and both re-open the `obs/` vs `sys/`
boundary that §5 settles on evidence. Widening the scan is a second scan in
`collectOutbound` — merged into the same seq-sorted row list, since both scans draw
seqs from the one space counter — plus one corrected line in the conformance target,
after which the two documents agree.

Verified before choosing, not after:

- **The scan cannot leak reactor traffic.** `space.c:486` — `if (c->kind !=
  SPACE_KIND_CELL) continue;` — the changed-since scan filters to cells, so `sys/tick`
  and `sys/timer` (streams, and the node's own reactor plumbing) cannot enter a push.
  This was the flood risk, and it does not exist.
- **porta needs no change.** `porta/internal/space/store.go:215` upserts any cell
  regardless of path; there is no `obs/` restriction on the inbound side.

**Cost, stated:** `maxRows = 8` is shared, so a `sys/` scan competes with `obs/` for the
same eight rows per push. With `obs/health` and `sys/metrics` as the only cells this is
nowhere near binding, but it stops being free on a node with many `obs/` paths.

## 4. The run-up survives without any new mechanism

**Verified, because "should" is how a false green happens.**
`porta/internal/space/store.go:220` calls `appendEvent` for **every** accepted entry,
cell or stream, unconditionally — there is no value dedup. The only skip is
`e.Seq <= hw`, a replayed unacked batch, which is correct.

So every distinct `sys/metrics` push lands as its own `events` row with `value_cbor` and
`ts_ms`. **The history is already retained and queryable.** Nothing in this design needs
to build, store or ship a run-up buffer; it needs only to cause the pushes to happen at
the right moments. This is the single largest simplification available here, and it was
found by reading the store rather than assuming a cell keeps only its latest value.

## 5. Two records, two jobs

The overlap is the bug. Three sources disagree today about what `obs/health` even is:

| source | `obs/health` |
|---|---|
| `PROTOCOL-NSL.md:327` (conformance target) | `{uptimeUs, wakes, pollTimeouts}` — 3 fields |
| `PROTOCOL-NSL-RECORDS.md:231` | `+ bootId, heapFree, heapLargest, overruns` — 7 fields |
| `PortaBridgeNodeC6.nsl:110` (what runs) | `+ streamDropped` — 8 fields, one of them unpinned |

`obs/health` began as pure liveness and accreted VM internals at every layer. Every
accreted field already has a home in `sys/metrics`.

**D2 — `obs/health` is liveness and delivery. `sys/metrics` is VM internals.**

- **`obs/health` — "is this node there, and is it keeping up?"**
  `{uptimeMs, bootId, wakes, pollTimeouts, streamDropped}`. It lives under `obs/`
  because it is the observed half of the goal-vs-obs convergence diff.
  `streamDropped` belongs here rather than with the VM internals: it counts
  *information lost on the way out*, which under §1's posture is the number that says
  the observability itself is lying.
- **`sys/metrics` — "why is this VM in trouble?"** The pinned 11 fields, unchanged.
  Forensics, never part of convergence — nobody writes a `goal/` for `gcCycles`.

`heapFree`/`heapLargest`/`overruns` **leave** `obs/health`; they are not duplicated,
they are deleted from it. The record drops 8 fields → 5.

**D3 — `PROTOCOL-NSL.md:327` is corrected to match**, including `uptimeUs` → `uptimeMs`.
It is the conformance target and it currently describes a record no implementation has
ever sent, in units nothing downstream uses (D4.2 makes milliseconds the rule).

**Noted, not fixed here:** `bootId` is the source constant `1` (`kernel.nsl:1540`) —
"honest for a node that cannot yet count its own boots". porta stores it to order
observations across reboots, which a constant makes degenerate. Out of scope; it needs
NVS.

## 6. Where the policy lives

**D4 — the escalation state machine lives in `PortaBridge`, not in each app.**

`PortaBridge` (`kernel.nsl:1525`) is constructed with `space:` handed to it by the app —
the capability is *granted*, not conjured, so this does not breach the rule that the
kernel never reaches into the space on its own. The bridge already owns the check-in,
which is the wake everything must ride.

The alternative is that each app publishes, which puts the same state machine in
`PortaBridgeNode`, `PortaBridgeNodeC6` and every node after them — the exact duplication
D2 is removing from `obs/health`, recreated one layer up.

The app keeps control through constructor knobs: the floor, and off.

## 7. Triggers

**D5 — the floor is counted in check-ins, never in seconds.** The current 2 s cadence
(`PortaBridgeNodeC6.nsl:38`) is a bring-up number for the hardware gate, and #115's
adaptive controller will change it. A floor in seconds silently becomes a different
policy the day the cadence moves; a floor in check-ins rides whatever the controller
picks. This is the "reporting rides existing wakes" rule (G9) applied to the floor
itself.

**D6 — escalate on degradation, with hysteresis.**

| trigger | signal | why this shape |
|---|---|---|
| **floor** | every *N*th check-in | cheap baseline when healthy |
| **edge** | *any* movement in `pulseDrops`, `overruns`, `streamDropped` | these are supposed to be zero, so movement **is** the event; no threshold to tune |
| **threshold** | `largestFreeBlock` below a mark | always non-zero, so only a level carries information |
| **decay** | *K* consecutive clean check-ins | one transient must not pin the node at maximum forever, and a flapping counter must not oscillate the cadence |

Levels: `every N` → `every M` → `every check-in`. An edge or threshold breach moves up
one level; *K* clean check-ins move down one.

**Defaults, so the plan does not have to invent them** — all overridable by the app:

| knob | default | reasoning |
|---|---|---|
| *N*, healthy floor | **30 check-ins** | 60 s at the 2 s gate cadence, 15 min at a 30 s production cadence — a floor that stays sane across the range #115 will pick |
| *M*, degraded | **5 check-ins** | fine enough to see a trend, still 6× cheaper than maximum |
| critical | **every check-in** | the D7 ceiling |
| *K*, clean check-ins to decay one level | **10** | long enough that a flapping counter cannot oscillate the cadence |
| `largestFreeBlock` threshold | **`largestFreeBlock * 2 < (heapSize − liveBytes)`** | expressed as a ratio of *free* bytes, not an absolute count: fragmentation means "the largest block is small **relative to what is free**", and an absolute byte mark would mean different things on a 53 KB device heap and a host heap |

**Dialect constraint for the plan: nsl has no modulo.** "Every *N*th check-in" is a
down-counter reset to *N*, never `checkIns \\ N = 0`. Stated here because the natural
expression of this policy is the one the language does not have.

**D7 — escalation may change what rides a check-in. It may never add a check-in.**

This is the rule that makes escalation safe, and it exists because the objection to
escalation is real: `queueHighWater` and `pulseDrops` rise precisely when the reactor is
not pumping fast enough, so a cadence that reacts to them adds work to a starving
reactor — the tuvm#54 failure mode wearing a helpful hat. Under D7:

- The worst case is bounded and budgetable: **one `sys/metrics` per check-in**, which is
  the simplest possible policy, not a pathological one.
- The marginal cost of escalating is composing a record and adding one row to a batch
  that was being sent anyway — no extra wake, no extra radio round-trip, no extra power.
- An edge detected between check-ins publishes on the **next** check-in. It does not
  wake the node.

A consequence worth naming: because `sys/metrics` is a **cell**, writing it more often
than the check-in gains nothing — the bridge re-reads the latest value at check-in and
the intermediate values are coalesced away. The one-per-check-in ceiling is therefore
not merely a policy, it is what the transport can express. Sub-check-in resolution would
require the stream kind, which §3 D1 rejected for other reasons; the delivered
difference is about 2 records per batch versus 1, and the real lever on run-up
resolution is the check-in rate, which G9 and #115 govern.

Node-side state cost: three previous counter values, the level, and the clean-run
count — about five slots. This matters against the DRAM ceiling (#53).

## 8. Failure modes

- **The batch does not fit.** `pushFrame` (`kernel.nsl:1686`) trims from the tail and
  the trimmed entries **ride the next check-in** — they are not lost, whereas an
  oversize datagram is lost on the mesh. A metrics record that does not fit is deferred,
  not dropped.
- **The value exceeds 256 B.** `valueFits:` drops it and counts a `rejected`, rather
  than letting one bad cell stall every good one behind it. At 195 B measured there is
  61 B of headroom; a future field addition eats it silently, which is why §9 keeps the
  size test as a regression guard.
- **The gateway is unreachable.** Cells are re-read at check-in, so `sys/metrics` cannot
  be evicted from `pending` the way a stream entry can. Under sustained disconnection
  the node reports the latest state on reconnect and the intervening run-up is lost —
  bounded and honest, and the same answer the space gives for every other cell.
- **Escalation stuck at maximum.** Bounded by D7: maximum is one record per check-in.

## 9. Testing

- **Host, deterministic:** a test under `nodus/tests/` that drives the state machine by
  injecting counter movement and asserts the published cadence — floor, escalation on
  each of the three edges, the `largestFreeBlock` threshold, and decay after *K* clean
  check-ins. The state machine must be testable without a radio.
- **Size, measured not estimated:** extend the method of
  `nodus/tests/19_latency/05_record_size.nsl` to the new 5-field `obs/health` and assert
  `obs/health + sys/metrics` against the 512 B batch cap with device-realistic values.
  **This measurement is OWED and is not yet taken** — see §12. The house rule is that a
  size in a protocol document is measured through the real `Cbor>>encode:` path, because
  CBOR is shortest-form and placeholder values encode to a different number than the
  fleet actually sends.
- **Wire, end to end:** extend `nodus/tuvm/tests/porta_sync_host.sh` to assert
  `sys/metrics` arrives in porta's cell tree **and** that repeated pushes accumulate as
  distinct `events` rows — the §4 claim, asserted rather than assumed.
- **Hardware:** on the second C6, not the bell (see §11).

## 10. Out of scope, recorded so it is deferred rather than forgotten

- **Retained RAM across a reboot (the v2 refinement).** Writing the run-up through
  RTC/LP retained memory — no flash wear — so it survives into the next boot. The
  mechanism is already proven in this codebase (`.rtc.data` on the MagTag for the bell
  ratchet; the region is undefined on cold boot and needs a magic word).
  **The caveat, and why it is a refinement rather than the answer:** retained RAM
  survives a VM panic, an OOM or a watchdog reset, and does **not** survive a brownout —
  the rail dropping is exactly what clears it. That makes it well aimed at the failure
  class in scope here (David, 2026-08-04: *"worried about software issues, hardware is
  out of scope"*) and useless for the one that cost this bench an afternoon (#149, a
  power-integrity reset loop). Escalation and retained RAM cover different deaths;
  neither substitutes for the other.
- **`bootId` as a real counter** (§5) — needs NVS.
- **#56 capability (4)**, the device test harness. This design is what makes its
  assertions possible.
- **The crash boundary itself**, which is less naked than it was: `log_udp.c` now
  streams panic output over Thread as it is printed, and `sys/panic` is pinned
  **must-deliver** in RECORDS §4.4.

## 11. Scope: which node

**D8 — build it on the bridge node; the bell inherits it as a separate small change.**

`ShipsBellAlarmNodeC6.nsl` — the app running unattended on the bench — has no
`PortaBridge`, no `checkIn` and no space writes. Neither does `SoakNode`. So "reporting
rides existing wakes" has no wakes to ride on the one node that most needs it: the node
with metrics is the gate node, and the node that runs for days has none.

Landing on `PortaBridgeNode`/`PortaBridgeNodeC6` first proves the mechanism where
reflashing is free, and leaves the bell's soak intact — it is currently the only witness
for the midnight 47→0 slot wrap and the post-midnight NTP sync, neither of which has
ever run on target. Because D4 puts the policy in the bridge, giving the bell metrics
afterwards costs a constructor call, not a second implementation.

## 12. Closes / touches / owed

- **Advances #56** — capability (3). Unblocks capability (4).
- **Touches `PROTOCOL-NSL.md`** §4.2 (the cell scan, D1) and §4.2's `obs/health` row
  (D3) — both in the conformance target, both corrections to text no implementation has
  ever matched.
- **Touches `PROTOCOL-NSL-RECORDS.md`** §231 — `obs/health` drops to 5 fields.
- **Touches `kernel.nsl`** — `PortaBridge>>collectOutbound` (D1) and the escalation
  state machine (D4).
- **Touches `PortaBridgeNode.nsl` / `PortaBridgeNodeC6.nsl`** — `publishHealth` loses
  the heap fields; a change to the body of either file is a change to both.
- **OWED before implementation is called done:** the §9 size measurement. Until it is
  taken, the claim that `obs/health + sys/metrics` fits one 512 B batch is an estimate,
  and this project has already paid once for an estimate in a protocol document
  (§4.4's own history, discharged by tuvm#61).
