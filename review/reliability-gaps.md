# Reliability gaps — the questions not yet asked (2026-07-03)

Everything specced so far covers the *language and node runtime*. "A reliable scripting
language over Zephyr" is decided just as much by the topics below — none of which the
first-step spec, the addendum, or the review kit yet addresses. Ordered by how much each
decides success, with when-to-decide. None block Milestones A–D; three (G1, G2, G3)
should be settled before the Zephyr bring-up step is designed.

---

## G1 — The code-push lifecycle: OTA with rollback (the #1 unasked question)

Remote provisioning is the product's core promise, and it is also the node's biggest
self-destruction risk: a bad image pushed over Thread to 50 ceiling-mounted nodes must
not brick them.

- **Deploy watchdog / self-rollback:** a newly activated image must positively check in
  (reach the supervisor's healthy state, contact porta) within N minutes or the
  bootloader reverts to the previous image. Toit ships this; it is the single most
  important fleet-reliability feature. Requires **A/B image slots** — budget the flash
  now (nRF52840 has 1 MB: MCUboot + 2 app slots + NVS is comfortable, but only if
  planned).
- **MCUboot** is the Zephyr-native answer (signed, versioned, revert-capable). Decide
  early whether the *whole firmware* swaps (VM + .oimg fused) or the **.oimg swaps
  independently** (smaller OTA, VM stable across script pushes — strongly preferable
  over Thread's bandwidth; implies the .oimg slot gets its own A/B + checkin logic).
- Thread is slow and lossy: OTA needs chunked, resumable transfer (CoAP blockwise fits
  the CBOR/CoAP direction) and, later, image **deltas**.
- **Decide:** at Zephyr-step design time. **Flash map + "oimg swaps independently?"
  decide before any flash layout is committed.**

## G2 — The seam under pressure: ISR safety and event overflow

**Overflow half RESOLVED 2026-07-14 (tuvm#20, David's calls on the three forks):** the
event-ring `abort()` is gone; a full ring sheds per the channel's **kind** (rung 9's
declaration doubles as the per-source policy, zero new API) — a CELL's poke
**coalesces** into its own newest queued event (the latest-value case; `peek:` always
answers the newest value), a STREAM's post **drops newest + counts** (already-queued
events keep causal order — the crash that started the storm must not be the record
lost). Never silent: per-channel + node-total counters (`spaceOverruns:`, prim 108;
first overrun per channel prints one line). `abort()` in space.c now means only
structural invariants. Design:
`nsl-tuvm/docs/design/2026-07-14-tuvm20-overflow-policy-design.md`; gate
`nsl-tests/17_overflow/`. Still open here: the ISR-half rules below at each new
event source.

**ISR half RESOLVED 2026-07-21 (tuvm#34/#55, surfaced by #54).** The seam in FRONT of
the ring kept the node-kill this section retired one layer later: `pulse_post_isr`
answered a full mailbox with `k_oops`/`abort` from ISR context. It now **sheds and
counts**, and **coalesces per source** — a source with a post already pending absorbs
the new one. Two findings worth carrying:

1. **The kind vocabulary does not transfer to this seam, and does not need to.** The
   ring carries VALUES, so coalesce-vs-drop is a real semantic choice there. The pulse
   mailbox carries a `(chan_id, handle_idx)` **doorbell token** with no payload, so two
   posts from one source are byte-identical and "coalesce into newest" and "drop newest"
   are the same operation. The seam's rule is therefore simply *never kill, drop and
   count*; coalescing is an optimisation that keeps slots free for OTHER sources, not a
   per-kind policy.
2. **Coalescing, not shedding, is the load-bearing half.** Once one post per source is
   the maximum, a full mailbox requires more than `OEVM_PULSE_MAILBOX_DEPTH` **distinct
   live sources** — load cannot produce one. Shedding survives as a genuine-anomaly
   backstop. Measured: 100 Hz fault injection drove `pulseCoalesced` to 13563 with
   `pulseDrops` **0** through a 16-slot mailbox.

Placed at the seam, not in `timer_zephyr.c` as #55 first proposed: the host timer
backend posts straight through `space_post` and never touches the mailbox, so a timer-side
fix would have been target-only code with no host test. At the seam it is host-tested and
covers timer and GPIO together.

Never silent, per this section's law: `pulseDrops`/`pulseCoalesced` ride `sysStats`
(prim 109, 4→6 slots) into the `sys/metrics` record (9→11 fields) and the node's
`/stats` page. A shed nobody can see would be strictly worse than the crash it replaced.

**Caveat carried forward:** the two backends share the policy but not the mechanism —
posix scans its array mailbox (the queue IS the pending set) while zephyr cannot inspect
a `k_msgq` and keeps a shadow ring under `irq_lock`. A missed `shadow_pop()` would wedge
one source's delivery silently and permanently. Untested; tracked as **tuvm#62**.
GPIO **signal conditioning** (debounce for doorbells, a counting API for pulse trains) is
deliberately NOT here — it belongs at the pin, before this seam: **tuvm#60**.

**Queue high-water + push side CLOSED 2026-07-14 (tuvm#24):** ring-occupancy
high-water (`q_high_water` in `tuvm/src/space.c`, monotonic max of `q_count`
bumped at post time — note it counts ring SLOTS including lazily-dead
not-yet-compacted ones, so it measures ring-slot **pressure**, not logical
queue depth) is pulled via prim 109 `sysStats` and pushed as the
`queueHighWater` field of the `sys/metrics` record (G4's "queue high-water
marks are a first-class metric" is now landed, not just named). Overruns
(this section, above) were already counted by #20; both numbers now travel
together in the one record. Design:
`nsl-tuvm/docs/design/2026-07-14-tuvm24-sys-metrics-design.md`.

The C→nsl event path is the reactor's front door, and its failure mode under load
was previously undefined.

- **ISR rules:** an interrupt context may never allocate on the VM heap or touch the
  space. The seam primitive is a **fixed-size ring of small tokens** (channel id +
  scalar payload); the reactor thread drains it and does the real `out:` on the VM
  side. `onAlertPost:to:` (demo-node finding) must be specified this way from day one.
- **Overflow policy, per event source:** when events arrive faster than handlers run,
  something must give — pin it: *coalesce* (counter/latest-value semantics — right for
  sensor ticks, GPIO edges), *drop-oldest*, or *drop-and-count*. Never unbounded queue
  (that is the OOM path), never silent drop (a `sys/overrun` counter record makes it
  observable). Transient-signal semantics (T-C.7) already point at coalescing as the
  default.
- **Backpressure telemetry:** queue high-water marks are a first-class metric (G4).
- **Decide:** the ring + per-source policy vocabulary at Milestone-D seam design
  (the host seam should already have the shape); the Zephyr ISR half at bring-up.

## G3 — Long-uptime memory health: fragmentation is the silent killer

Years-long uptime on a ~100 KB heap with a **non-moving** GC dies slowly of
fragmentation; restart-the-job cannot fix it (shared heap), and reboot-as-cure is a
fleet-wide symptom by then.

- **Decide now (cheap while the GC is being touched in Milestone A): is the GC moving
  (compacting) or not?** If non-moving, adopt mitigations deliberately: size-classed
  allocation, and the message algebra helps (most objects are small and die young) —
  but big long-lived buffers (OTA staging, radio buffers) must live OUTSIDE the VM heap
  in C arenas, by rule.
- **Heap telemetry from day one:** live bytes, largest free block, allocation
  high-water — exported as `sys/heap` records. T-D.5's plateau test catches leaks;
  only the largest-free-block trend catches fragmentation.
- **Decide:** moving-vs-not before Milestone B lands lots of allocation paths; the
  "big buffers live in C" rule can be adopted immediately.
- **Status at Milestone D close (2026-07-07):** the C-side counters exist
  (`oevm_heap_stats()`, `tuvm/src/gc.c`/`oevm.h`, live_bytes/high_water/
  largest_free_block, landed pre-D for G3/M2) but are still NOT exported as a
  `sys/heap` record or any nsl-visible prim — `tuvm/src/prim_ids.h`'s
  append-only list (59-89) has nothing heap-related. `nsl-tests/11_supervision/
  10_soak_leak.ns` (T-D.5, landed) therefore asserts the weaker
  completion+responsiveness invariant, not a strict plateau — this gap is
  still open and is exactly what closing it would unblock.
- **CLOSED 2026-07-14 (tuvm#24):** the gauge is now exported end-to-end — prim
  104 (`heapStats`) + the new prim 109 (`sysStats`: handle live/high-water,
  queue high-water, overrun total) compose in `kernel metrics` into a 9-field
  flat `Record tag: #Metrics`, published on a `sys/metrics` **cell** (rung 9
  `put:`/`peek:`) so any grantee reads current state by name without draining
  a stream another consumer counts on. Schema pinned
  `protocol/PROTOCOL-NSL-RECORDS.md` §4.4. `nsl-tests/11_supervision/
  10_soak_leak.ns` (T-D.5) now asserts a STRICT live-bytes plateau, not the
  completion proxy — the gap above is closed. Design:
  `nsl-tuvm/docs/design/2026-07-14-tuvm24-sys-metrics-design.md`.
  **Gauge-correction story (execution finding, keep — the next person who
  reaches for `highWater` will hit this too):** the originally planned gauge
  — "highWater plateaus unless there's a leak" — is FALSE on this VM: GC
  fires only on arena exhaustion (no on-demand collect prim yet; tuvm#35
  filed), so a healthy soak against the 16 MB host arena never collects and
  `highWater` is just a monotonic max over cumulative allocation (measured: a
  deterministic +9,121,280 B climb across 90k iterations, three identical
  healthy runs — pure noise, no leak). Corrected gauge: force a sweep at each
  sample point (spin allocation until `gcCycles` increments —
  `sweptLiveBytes:` in the test) and sample `liveBytes` at that sweep
  boundary. Tuning gave delta 1240/1240/1240 B across three runs → tolerance
  8192 B (~176x below the smallest leak the test must catch); an
  injected-leak run falsified correctly (`plateau false` at delta 217,888 B).
  **Bonus fragmentation datum, filed against tuvm#19 (idle-time slide
  compaction, gated on exactly this kind of soak evidence):** the same soak
  workload independently showed a doubling `OrderedCollection`
  fragmentation-OOMs the non-compacting arena well before `liveBytes`
  exhausts — a large contiguous block becomes unavailable mid-run even though
  the sum of free bytes would suffice. A second, distinct data point (beyond
  years-long uptime) that a non-moving GC dies of fragmentation before it
  dies of volume.

## G4 — Field forensics: what do you hold after a node dies at 3am?

Let-it-crash is only a virtue if crashes are *observable after the fact*.

- **Retained-RAM "last words":** a small noinit region surviving warm reboot: reboot
  reason (hard fault / WDT / deploy-rollback / panic), last error string, restart
  counters, FSM states. First thing porta reads when a node reappears.
- **Crash records need source positions:** `self error:` with no line info makes field
  debugging archaeology. Method + line table in the .oimg (or a porta-side symbolizer
  keyed by image hash) — decide the format early; retrofitting line tables into an
  image format is painful. This also fixes T-A.1's diagnostics story.
- **Metrics channel:** heap (G3), queue high-water (G2), restart counts, radio RSSI /
  parent, battery voltage — periodic `sys/metrics` record; porta persists. This IS
  the "profile" in provision/debug/profile — currently the least-specified of the
  three.
- **Rate-limit remote logging** at the source (token bucket per channel) — a
  crash-looping node must not DoS its own Thread network.
- **Decide:** retained-RAM layout + reboot-reason enum at Zephyr bring-up; line-table
  decision **belongs to the .oimg format now** (Milestone A keeps the loader —
  check nothing being deleted forecloses it).

## G5 — The OUTER security boundary: who may push code?

The capability model confines jobs *inside* the node. The outer boundary — remote code
push is remote code execution *by design* — is unspecced.

- Signed images (MCUboot gives firmware signing; an independently-swapped .oimg needs
  its own signature check), porta↔node mutual auth (Thread gives link-layer crypto;
  session auth on top for porta protocol), and a commissioning story (how a factory
  node joins the network and trusts its porta — Thread's commissioner flow answers
  half; pairing the node to a porta identity answers the rest).
- **Decide:** before ANY over-the-network push path exists — the wrong moment to add
  auth is after the unauthenticated path ships.

## G6 — Time: nodes have no clock

- Pin the discipline now: **on-node time is monotonic-only** (`monotonicMillis`,
  already in timers.ns); wall-clock is porta's business. Readings are stamped with
  monotonic time + boot-epoch counter; porta maps to real time using its own clock at
  ingest. Nodes never make wall-clock decisions (no "at 6pm do…" on-node — that is a
  porta-pushed event).
- Deep-sleep wakes come from the RTC domain; timer drift across sleep is fine for
  cadence, fatal if anyone assumes precision — document ±.
- **Decide:** adopt the discipline now (it is free); revisit only if a use case truly
  needs on-node wall time.

## G7 — Identity, config, calibration: the NVS story

Node id, Thread credentials, porta pairing, sensor calibration, FSM snapshots that must
survive power loss: Zephyr NVS/settings is the store. Needs: a versioned schema (CBOR
map with a version field — reuse L6), remote config update via a `sys/config` channel
with the same rollback thinking as G1 (a bad config must not brick), and flash-wear
awareness (batch writes; snapshots on sleep-entry, not per-event).
**Decide:** at Zephyr bring-up; the CBOR-schema choice can be pinned with L6 now.

## G8 — Fleet version skew

porta will face a mixed fleet: VM vX, image vY, compiler vZ, protocol vW. Pin the
policy: every hello/checkin carries all four versions; porta owns the compatibility
matrix and refuses/upgrades accordingly; the wire protocol (CBOR records) evolves by
**adding optional fields only** — never re-keying. Registry/oimg versioning discipline
already exists in-repo; this extends it across the LAN. **Decide:** with porta wire
integration.

## G9 — Power budget as a tested number, not a hope

Coin-cell viability = sleep current × duty cycle. The architecture is sleep-friendly by
design (that argument is won); what is missing is **measurement discipline**: a power
budget line (µA sleeping, mA-ms per wake-sample-publish cycle, target battery-years)
banked and ratchet-tested like RAM once hardware exists — plus the one design rule that
protects it: **no periodic work on the node that isn't user-requested** (the VM idles
in `k_sleep`/WFI via the space's idle-wait; no housekeeping tick). SED/SSED poll period
is the other big lever — expose it as config (G7), not code.
**Decide:** rule now (costs nothing); measurements at first hardware.

## G10 — System-level CI: simulate the fleet on the host

Everything so far tests one node. The reliability claims that matter (porta reconnect,
node reboot mid-transfer, version skew, crash-loop containment) live BETWEEN boxes.
Because the first step is host-only anyway, the pieces exist to run **N tuvm VMs +
porta in one CI job** (localhost UDP): kill nodes mid-flight, drop packets, replay.
Build this as the porta-integration step's test harness from day one — retrofitting
system tests after the wire works is how they never happen.

## G11 — When the supervisor itself dies

Let-it-crash protects jobs; the supervisor + reactor are the node's kernel and their
failure mode is: soft-watchdog trip or hard fault → **hard WDT reboot**. That makes
reboot a *designed* recovery path, which imposes: boot must be fast and idempotent
(cold boot ≡ Nth boot), a boot-loop breaker (K reboots within M minutes → safe mode:
radio + porta + no user jobs — so a poisonous image/config can always be recovered
remotely, closing the loop with G1's rollback), and reboot-reason forensics (G4).
**Decide:** safe-mode concept at Zephyr bring-up; it shapes the boot sequence.

---

## Sequencing the decisions

| Now (free or Milestone-A/B adjacent) | Zephyr-step design time | porta-wire step |
|---|---|---|
| G3 moving-vs-non-moving GC · big-buffers-in-C rule | G1 flash map, A/B, independent .oimg swap | G8 version skew policy |
| G4 line-table provision in .oimg format | G2 ISR ring + overflow vocabulary (host shape at Milestone D) | G10 fleet-sim harness |
| G6 monotonic-only discipline | G4 retained-RAM layout · G5 auth · G7 NVS · G11 safe mode | |
| G9 no-unrequested-periodic-work rule | G9 power measurement rig | |
