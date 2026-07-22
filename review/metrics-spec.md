# Node health metrics — the long-term profile (G4/G9 companion)

**Status:** proposed 2026-07-03. Companion to `reliability-gaps.md` G4 (forensics) and
G9 (power). Principle: **every metric must answer a question about future failure** —
leading indicators over lagging ones. A node that only reports what already broke is a
coroner; the profile's job is prediction (fragmentation trend, battery slope, crash
rate) so porta can flag a node weeks before it dies.

## Mechanics (decided once, cheap forever)

- **Collection is a fixed C array of counters/gauges** — one slot per metric, increment
  is one instruction, **zero allocation on any hot path** (GC, dispatch, ISR ring all
  bump slots freely). The reporter snapshots the array, emits one record, resets the
  since-last-report values.
- **Three slot types:** `counter` (monotonic since boot), `gauge` (sampled at report
  time), `hiwater` (max since last report, reset on report).
- **Reporting rides existing wakes** (the G9 rule — no unrequested periodic work): a
  `sys/metrics` record piggybacks on every porta checkin; an always-on node reports
  every N minutes. One CBOR map, short symbol keys, ints only, add-only schema
  (G7/G8); target < 200 bytes.
- **Node reports raw; porta computes trends.** Slopes, rates, and alert thresholds are
  porta-side (updatable fleet-wide without touching nodes). The node never
  self-diagnoses beyond the boot-loop breaker (G11).
- **A tiny subset lives in retained RAM** (the G4 last-words struct): reboot reason,
  reboot count, crash counts — so death itself becomes a data point on the next boot.

## The catalog — grouped by the question each answers

### "Is memory dying slowly?" (the shared-heap killers, G3)
| metric | type | notes |
|---|---|---|
| `heapLive` | gauge | live bytes **after** GC (honest number) |
| `heapHiWater` | hiwater | worst since last report |
| `largestFree` | gauge | **the fragmentation signal** — porta trends this; if GC is non-moving this is the single most predictive metric on the node |
| `gcCount` | counter | collections since boot |
| `gcPauseMaxUs` | hiwater | worst pause; also validates radio-timing assumptions |
| `allocBytes` | counter | churn rate; rising churn under a constant workload = code drift or retry loops |
| `stackHiWater` | hiwater | language-stack frames — headroom trend toward the fixed cap predicts T-A.4-style crashes *before* they happen |
| `cArenaUsed` | gauge | seam/OTA buffers outside the VM heap (the G3 rule made observable) |

### "Is the reactor healthy?" (responsiveness, G2)
| metric | type | notes |
|---|---|---|
| `eventsDispatched` | counter | denominator for every rate |
| `overruns` | counter | per-source coalesce/drop tallies (G2) — nonzero = producer outrunning consumer |
| `queueHiWater` | hiwater | ISR-ring depth — headroom before the overflow policy engages |
| `handlerMaxUs` | hiwater | longest run-to-completion dispatch — the "who is hogging the reactor" number; the soft-watchdog budget's little brother |
| `timersActive` | gauge | a monotonically climbing count is a timer leak |

### "Are jobs crashing, and is it getting worse?" (supervision)
| metric | type | notes |
|---|---|---|
| `crashCount` (per job) | counter | plus hash of last error string (maps to errors.md vocabulary — porta groups fleet-wide by cause) |
| `restarts` / `quarantines` | counter | restart-intensity in action |
| `uptime` | gauge | seconds since boot |
| `lifetimeUptime` | gauge | NVS-accumulated — distinguishes "reboots daily" from "first boot" |
| `rebootReasonTally` | counter[reason] | WDT / hard fault / deploy rollback / safe mode / power-on (retained-RAM sourced, G4/G11) |

### "Is the network stable?" (Thread/porta)
| metric | type | notes |
|---|---|---|
| `parentRssi` / `parentLqi` | gauge | link quality now |
| `parentChanges` | counter | **flapping predicts placement/interference problems** better than RSSI itself |
| `txFail` / `txRetries` | counter | vs `txCount` — congestion/interference rate |
| `roleChanges` / `partitionChanges` | counter | Thread topology churn |
| `checkinFails` | counter | porta reachability from the node's side |

### "Will the battery last?" (power, G9 — the duty-cycle truth)
| metric | type | notes |
|---|---|---|
| `battMv` | gauge | porta computes the **discharge slope** — the health number |
| `wakeCount` / `awakeMs` | counter | duty cycle actually achieved vs designed |
| `radioOnMs` | counter | the dominant energy consumer, measured not assumed |
| `dieTempC` | gauge | environment + hardware anomaly correlate (free on nRF52840) |

### "How long do jobs actually take, and where does the time go?" (latency, tuvm#61)

Implemented 2026-07-21 and measured on the WROOM-32. A **span** is a fixed slot in the
same nameless C array the mechanics section describes — `n`, `min`, `max`, a sum split
lo/hi at 1e9 (an SMI is 31-bit, and a microsecond sum passes 2^30 after ~17.9 minutes of
accumulated span time), and six log-spaced decade buckets. 8 slots cost **384 B** of
DRAM, measured. Zero allocation on any measured path, so a span may be opened inside a
prim, inside HTTP egress, or inside the thing it is timing.

| metric | type | notes |
|---|---|---|
| `span.n` | counter | samples; also the denominator that gates whether a p95 may be reported at all |
| `span.fails` | counter | jobs that produced NO sample. Counted in nsl, because a failure is an app-level concept the C seam cannot see — and an uncounted failure silently *improves* the percentiles by removing exactly the slow cases |
| `span.min` / `span.max` | gauge | exact microseconds, at any sample count |
| `span.sumUsLo` / `span.sumUsHi` | counter | the raw accumulator. The node does **not** divide: reassembling `hi*1e9 + lo` on-node would overflow a 31-bit SMI at `hi >= 2`. Accumulate on the device, compute the mean off it — the `raw / porta derives` rule above, applied literally |
| `span.b0..b5` | counter | the histogram: <100µs, <1ms, <10ms, <100ms, <1s, >=1s |

**Two spans are C-owned** (`httpQueue` = accept→take, `egress` = the writes in one HTTP
chunk) and stamped inside the core functions that own the work, so no node can ship an
uninstrumented HTTP path by forgetting a bracket. They occupy slots 0 and 1 by
convention; nsl seeds those two names so its own `register:` allocates from 2 upward.
Everything else is named and bracketed by the app.

**Percentiles are reported as bucket decades, never interpolated**, and no p95 is
reported below n ≥ 200. A log-scaled bucket's mass is not uniform, so interpolating
inside one manufactures precision the data does not contain; and at small n a p95 is
simply the k-th sample, which one retransmit relocates. `max` is exact at any n and
carries the tail on its own.

### "Is anything quietly corrupt?" (integrity canaries)
| metric | type | notes |
|---|---|---|
| `decodeErrors` | counter | CBOR rejections at the boundary (L6 rule 3) — peer misbehavior or corruption |
| `pathRejects` | counter | channel-path validation failures — a buggy (or probing) sender |
| `nvsWrites` | counter | flash wear tracking (G7) |
| `otaAttempts` / `otaFails` | counter | the G1 lifecycle observed |

## What porta derives (the actual "profile")

Per node, trended over weeks: **fragmentation trend** (`largestFree` vs `heapLive`),
**battery slope** (mV/day, projected empty date), **crash rate** (crashes per 1k events,
by error-hash), **parent flap rate**, **duty-cycle drift** (`awakeMs`/hour vs design),
**stack/queue headroom trends**. Fleet-wide: error-hash leaderboard, version-cohort
comparisons (G8 — did the new image raise crash rates?), outlier detection (one node's
churn 10× its cohort = investigate placement or hardware).

## Deliberately NOT collected

Per-message tracing, per-channel byte counts, wall-clock timestamps on-node (G6),
anything requiring allocation to record. If a debugging session needs more, that is
what the remote debugger and a temporarily-pushed diagnostic job are for — the standing
profile stays under 200 bytes.

> **Amended 2026-07-21 (tuvm#61).** This list previously read *"histograms (porta can
> bucket; nodes can't afford the slots)"*. That is now false and has been removed: the
> latency group above ships six decade buckets per span, and it turned out nodes *can*
> afford the slots. The budget that made it affordable: **8 spans × 48 B + a 4 B
> open-mask = 384 B**, measured on the WROOM-32, against 672 B of spare dram0 (#53).
> A spec that contradicts the code is worse than no spec, so the line is gone rather
> than softened.
>
> The reasoning the line got *right* is still worth keeping: porta bucketing beats node
> bucketing whenever the node can ship raw samples cheaply. Spans are the case where it
> cannot — a sample per job at 2 Hz is an unbounded stream, and the whole point is to
> pay a fixed 48 B per span instead. Porta still owns every *trend*; the node owns only
> the accumulator.

## Sequencing

Slots + reporter shape: **Milestone D** (the supervisor owns the reporter; host metrics
subset = memory + reactor + supervision groups, which is also exactly what T-D.5 wants
to read). Radio/power/flash groups: Zephyr bring-up. porta derivations: porta-wire step.
