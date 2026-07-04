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
  `%sys/metrics` record piggybacks on every porta checkin; an always-on node reports
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
histograms (porta can bucket; nodes can't afford the slots), anything requiring
allocation to record. If a debugging session needs more, that is what the remote
debugger and a temporarily-pushed diagnostic job are for — the standing profile stays
under 200 bytes.

## Sequencing

Slots + reporter shape: **Milestone D** (the supervisor owns the reporter; host metrics
subset = memory + reactor + supervision groups, which is also exactly what T-D.5 wants
to read). Radio/power/flash groups: Zephyr bring-up. porta derivations: porta-wire step.
