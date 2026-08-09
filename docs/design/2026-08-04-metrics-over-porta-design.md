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
they are deleted from it. The record drops 8 fields → 5, and **130 B → 87 B measured**
(§9.1) — which doubles the slack in a full push frame, from 44 B to 87 B under the
512 B cap. The split earns its keep in bytes, not only in clarity.

**D3 — `PROTOCOL-NSL.md:327` is corrected to match**, including `uptimeUs` → `uptimeMs`.
It is the conformance target and it currently describes a record no implementation has
ever sent, in units nothing downstream uses (D4.2 makes milliseconds the rule).

**D9 — there is no backward compatibility to keep, and no migration path to design.**
David, 2026-08-04: *"there is ZERO need to maintain backward compatibility or 'the
fleet': we will reflash as necessary."* Both ends of the wire are reflashed together,
so a record shape change is an edit to two documents and a rebuild — not a versioned
field, not a dual-read, not a deprecation window. This is what lets D1, D2 and D3 be
corrections rather than accretions, and it is the difference between this design and
the one that produced the three-way `obs/health` disagreement in the first place: every
one of those extra fields was added *beside* what was there rather than *instead of*
it.

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

**D5 — the unit of cadence is the check-in, never the second.** The current 2 s cadence
(`PortaBridgeNodeC6.nsl:38`) is a bring-up number for the hardware gate, and #115's
adaptive controller will change it. Any policy expressed in seconds silently becomes a
different policy the day the cadence moves; one expressed in check-ins rides whatever
the controller picks. This is the "reporting rides existing wakes" rule (G9) applied to
the cadence itself, and it is why D6 can be as simple as it is — "every check-in" is
already adaptive, for free, because the check-in is.

**D6 — v1 publishes `sys/metrics` on every check-in. Escalation is staged, not built.**

*Revised 2026-08-04 under David's standing guidance: limit scope, get something
working, run spikes and gather data rather than making complex decisions early — and
remember the lineage, `st/berry → ns/berry → ns/oevm → nsl/tuvm`. This system has been
rewritten wholesale four times. Nothing here is built to last; it is built to teach.*

The escalating design (a healthy floor of every *N*th check-in, edge triggers on
`pulseDrops`/`overruns`/`streamDropped`, a fragmentation threshold on
`largestFreeBlock`, and decay after *K* clean check-ins) needs **five invented
constants** before it can run once. None of them can be chosen from evidence today,
because nobody has measured what a metrics push costs on this mesh.

**And D7 already caps escalation's worst case at one record per check-in — so the
ceiling of the elaborate design is the simple design.** Publishing unconditionally is
not a lesser version of escalation; it is escalation with the dial at maximum, which is
the only setting whose cost we can measure directly.

So v1 is: **compose `platform metrics`, `put:` it at `sys/metrics`, every check-in.**
No levels, no thresholds, no hysteresis, no counters, no tunables, no new node state.

What that buys beyond simplicity: cadence policy then lives in exactly one place
(#115's adaptive controller) instead of two mechanisms interacting, and the run-up
resolution is simply the check-in rate.

**What earns escalation back**, recorded so the deferral is falsifiable rather than
vague — any one of:

- the measured per-check-in cost (radio time, power, or batch pressure) turns out to
  matter at the cadence #115 picks;
- `maxRows = 8` or the 512 B batch starts binding as `obs/` paths multiply (§9.1 says
  there is 87 B of slack today);
- a real degradation event shows that check-in-rate resolution was too coarse to see
  the run-up.

The escalation design is preserved above in outline for whoever picks that up. It was
not wrong; it was unearned.

**Dialect note kept for whenever a cadence divisor does arrive: nsl has no modulo.**
"Every *N*th check-in" is a down-counter reset to *N*, never `checkIns \\ N = 0` — the
natural expression of that policy is the one the language does not have.

**D7 — reporting may change what rides a check-in. It may never add a check-in.**

The standing rule, and the reason D6 could collapse to "every check-in" without
argument. It exists because the objection to any reactive cadence is real:
`queueHighWater` and `pulseDrops` rise precisely when the reactor is not pumping fast
enough, so a policy that reacts to them adds work to a starving reactor — the tuvm#54
failure mode wearing a helpful hat. Under D7:

- The cost is bounded and budgetable: **one `sys/metrics` per check-in**, measured at
  195 B in a 425 B frame (§9.1).
- The marginal cost is composing a record and adding one row to a batch that was being
  sent anyway — no extra wake, no extra radio round-trip, no extra power.
- Nothing metrics-related may ever wake the node. If a future escalation returns, it
  publishes on the **next** check-in, never on its own schedule.

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
- **Cost runaway.** Cannot happen in v1: the rate is the check-in rate, one record per
  check-in, and D7 forbids metrics from ever adding a wake. The failure mode this
  design has instead is the opposite one — resolution too coarse to see a fast
  degradation — which is the observation that would earn escalation back (D6).

## 9. Testing

- **Host, deterministic:** a test under `nodus/tests/` asserting that a check-in
  publishes `sys/metrics` and that the published record is the `#Metrics` shape §4.4
  pins. With D6 staged there is no state machine to drive, which is most of why v1 is
  worth shipping first: the test is an assertion about one behaviour, not a matrix over
  five constants.
- **Size — MEASURED 2026-08-04, see §9.1.** The measurement is taken; what remains for
  implementation is landing it in `nodus/tests/` as a **regression guard**, the same job
  `19_latency/05_record_size.nsl` does for `#Metrics`. 61 B of value headroom is eaten
  silently by one field addition.
- **Wire, end to end:** extend `nodus/tuvm/tests/porta_sync_host.sh` to assert
  `sys/metrics` arrives in porta's cell tree **and** that repeated pushes accumulate as
  distinct `events` rows — the §4 claim, asserted rather than assumed.
- **Hardware:** on the second C6, not the bell (see §11).

### 9.1 The measurement, taken 2026-08-04

Not a sum of record sizes. The thing that must fit 512 B is the **push frame** —
envelope and rows included — so this builds the frame `PortaBridge>>encodePush:` would
build and encodes it through the real `Cbor>>encode:` path. Values are chosen for a
**long-running** node (30 days of uptime, 1,296,000 wakes), because CBOR is
shortest-form and a fresh node encodes narrower than one that has been up for weeks:
measuring the easy case is exactly how an estimate becomes wrong in the field.

| encoded | bytes |
|---|---|
| `obs/health` as sent today (8 fields) | **130** |
| `obs/health` proposed (5 fields, D2) | **87** |
| `sys/metrics` (11 fields) | **195** |
| `tel/log` one entry | **45** |
| **push frame: proposed health + metrics** | **365** — 147 B under the cap |
| **push frame: + `tel/log`** | **425** — 87 B under the cap |
| push frame: today's health + metrics | 408 |
| push frame: today's health + metrics + `tel/log` | 468 — 44 B under the cap |

**It fits, with room.** Three findings worth keeping:

1. **`sys/metrics` reproduces at exactly 195 B**, the value §4.4 pinned on 2026-07-21.
   That is the control: had it come out otherwise, every other number here would be
   suspect. The method is validated, not merely used.
2. **D2 doubles the headroom.** The full three-row batch is 468 B with today's
   `obs/health` and 425 B with the proposed one — 44 B of slack versus 87 B. Slimming
   the record is not tidiness; it is the difference between one future field being
   affordable and not.
3. **The batch was never in danger, but it was never checked either.** 468 B fits
   today, which means this design could have shipped on an assumption and been right —
   and the next field added to any of the three records is the one that finds the cap.

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
- ~~**`bootId` as a real counter** (§5) — needs NVS.~~ **LANDED 2026-08-09 (rete#162).**
  The NVS was never the obstacle: every `prj_*_thread.conf` already mounts it for
  OpenThread's dataset, so this cost one settings key. See `protocol/PROTOCOL-NSL-RECORDS.md`
  §3.
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
- **The size measurement is DISCHARGED** (§9.1, 2026-08-04): the full three-row push
  frame is **425 B** against the 512 B cap, and `sys/metrics` reproduces the pinned
  195 B exactly as a control. What implementation still owes is landing that
  measurement as a **regression guard** in `nodus/tests/`, because the number protects
  nothing if nothing re-checks it.

---

## 13. Hardware witness, 2026-08-04 — everything above, on silicon

Written the same evening the design landed, because a design gated only on the host is a
design whose radio assumptions are untested.

A fresh C6 devkit running `PortaBridgeNodeC6`, checking into porta on `gw85224-01` over a
real Thread mesh:

```
obs/build     seq=1     #Build{#chip: #esp32c6}
obs/health    seq=1985  #Health{#wakes: 331, #bootId: 1, #uptimeMs: 664140,
                                #pollTimeouts: 6, #streamDropped: 0}
sys/metrics   seq=1990  #Metrics{#gcCycles: 211, #heapSize: 131072, #liveBytes: 89592,
                                 #largestFreeBlock: 37356, #queueHighWater: 3,
                                 #pulseDrops: 0, ...}

tick 326 misses 6 cursor 0 acked 1954 dropped 0
```

Confirmed, in order of how badly each would have hurt if false:

1. **D1's widened scan carries `sys/` over the radio.** The record that was unsendable by
   construction is in porta's cell tree.
2. **§4's claim holds on real traffic** — 134 `sys/metrics` rows in the event log within
   minutes, accumulating rather than collapsing. At the 2 s bring-up cadence, ~1,800
   rows/hour. The node stores nothing; the series lives gateway-side, which is the answer
   to #56's "metrics die with the node".
3. **D2's slimmed `obs/health` is what actually goes on the wire** — five fields, no heap
   fields, `streamDropped` present.
4. `dropped 0` and `misses 6` of 326 ticks: the batch is nowhere near the cap in practice,
   consistent with §9.1's measured 425 B of 512.

**The escalation measurement (D6) is now collectable** from this run. It was not taken
tonight; the run exists to make it takeable.

### 13.1 What the bring-up cost, recorded so it is not re-paid

The node was silent for seven diagnostic probes and the cause was **not** in anything this
design touches. A C6 image built without
`--conf-extra "prj_esp32c6_thread.conf;prj_esp32c6_thread_ftd.conf"` contains **no
OpenThread at all** — no `CONFIG_NET_L2_OPENTHREAD`, no settings, no dataset — and the only
symptoms are the absence of the `thread:` boot lines and the app printing a bare `bind`,
which is `udpBindPort:` answering nil because there is no interface. The correct invocation
was already in `nodus/tuvm/tests/porta_sync_c6.sh:173`.

**The compounding error is the more useful record.** Seven "zero bytes" reads were reported
as evidence the board was dead, from a serial reader that had never once been shown to work.
The C6 console does not reset on open — unlike the WROOM, and the bench notes say so — so
attaching to an already-booted, quiet node yields nothing, and that silence means nothing.
The control that resolved it was entering bootloader mode deliberately: `esptool --before
no_reset chip_id` succeeds *only* if the chip is already in the ROM loader, which separates
"not booting" from "booted and quiet" in one command. **Validate the instrument against a
known-present signal before believing an absence.**
