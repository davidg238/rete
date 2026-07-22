# Bounded span-capture sessions on a `diag/` channel family — Design

**Date:** 2026-07-22
**Status:** approved (design), unimplemented
**Tracking issue:** **#72** (relates #61, #50, #63; constrained by #53; anticipates #32 option B)
**Constrains / is constrained by:** `~/workspace/porta/docs/PROTOCOL-NSL.md` — but **in the
reverse of the usual direction.** porta and the wire protocol are slated for a total
do-over (David, 2026-07-22): the command model is scrapped, channels win, and the node side
is designed first. This document therefore takes porta's *vocabulary* (cells vs streams,
convergence trees, "remoteness lives in the grant") and rejects its *naming constraints*.
§7 lists what porta must change to follow.
**Standing rules honoured:** `~/workspace/tuvm/review/metrics-spec.md` ("collection is a
fixed C array of counters, zero allocation on any hot path"; "node reports raw, porta
computes trends"), `~/workspace/tuvm/review/reliability-gaps.md` G9 (no unrequested
periodic work), and the Milestone-C rule (capabilities arrive as constructor args).

---

## 1. Goal

Make the job-latency figures **trustworthy to quote**, by measuring them over a window that
excludes the act of reporting them.

The deliverable is not the capture mechanism. It is a latency table whose numbers are not
partly about the table — specifically, one that can be re-taken for #61 and quoted in the
performance deck (#70) without a caveat, and a page that can then be made readable
(commas, a legend, column documentation) without those improvements moving a figure.

## 2. The defect that forced this

`egress` records **one sample per emitted chunk, for every route**, in
`~/workspace/tuvm/src/http_core.c:215-225`. It is not scoped to `/thermal`. Chunk counts:

| Route | `httpChunkId:` calls per fetch |
|---|---|
| `/thermal` | 12 (1 header + 8 SVG rows + 3) |
| `/stats` | ~24 (1 + 11 stat rows + 1 + 1 + one per span slot + 2) |

So **the latency table's `egress` row partly measures the rendering of the latency table.**

Magnitude, stated honestly: over a 300-request harness run that is ~24 samples against
~3,600 — irrelevant to `n` and to the decade percentiles, which is why nothing looked
wrong. It is **not** irrelevant to `max`, a single-sample statistic: a `/stats` chunk is a
much larger string than an SVG row, so the egress max is a plausible `/stats` artifact.
The 2026-07-22 addendum in `~/workspace/tuvm/review/program-sequence.md` already flags that
span's 727× max/min spread as the figure most likely to move — it is precisely the figure
most exposed here.

**The trigger.** Four wanted cosmetic improvements to `/stats` (a home link above the
`job latency` header, thousands separators, a `&middot;` between histogram buckets, and
column documentation below the table) all add chunks and bytes. Every one of them makes the
contamination worse. A page that cannot be improved without disturbing its own measurements
is the actual problem; the cosmetics are downstream of it.

## 3. Design

### 3.1 The channels

```
  diag/spans/ctl      cell     {state: #running | #stopped}          <- control in
  diag/spans/result   cell     {n:, ms:, slots: [ ... ]}             <- report out
```

`ms:` is the window duration. `n:` is the **total samples recorded across all slots**
during it — deliberately not "cycles", because the capture is generic and must not know
which span represents a job. The cycle count for any particular job is that job's row in
`slots`; the top-level `n` exists so a reader can tell at a glance whether a window caught
anything at all.

`slots` is a List of per-span records:

```
  {name:, n:, fails:, min:, max:, sumLo:, sumHi:, b0:, b1:, b2:, b3:, b4:, b5:}
```

`sumLo`/`sumHi` ship raw, as the two halves `src/span.c` keeps them in — reassembling
`hi*1000000000 + lo` would overflow a 31-bit SMI the moment `hi` reaches 2, which is why
the split exists. Accumulate on the device, divide off it.

### 3.2 The invariant

**Reset last on start. Snapshot first on stop.**

- **→ `#running`**: stamp `t0:: 0 millis`, *then* `spans resetAll`.
- **→ `#stopped`**: snapshot the C table into the record, *then* `put` it on `result`.

In the channel design this is **structural, not hand-maintained**. A reaction runs during
`space drain`; an HTTP response renders synchronously in its own request. The report is
therefore rendered on a *later* request than the one that stopped the capture, and a report
can never land inside the window it describes. The same guarantee in a bare HTTP toggle
would depend on getting two statements in the right order inside one handler, forever.

The corollary is a UX consequence, and it is the right one: **sliding the capture off does
not hand you the report.** It returns "captured"; the report is a separate read of
`diag/spans/result`. That is exactly the cell semantics — the report is a value you peek,
not a reply you receive, and the space has no request/reply at all.

### 3.3 Edge cases, decided

| Case | Behaviour | Why |
|---|---|---|
| `#running` while running | Restart the window (new `t0`, fresh reset) | Writing the cell states intent; "start again" is the only sane reading |
| `#stopped` while stopped | Ignored; no `result` write | Otherwise a stray refresh overwrites a good capture with a zero-length one |
| Supervised restart mid-capture | At startup the job **peeks** `ctl`; a retained `#running` is normalised by **writing `#stopped` back**, with no `result` published | The cell makes a *setpoint* recoverable (#32 option A). A timestamp is not a setpoint, and a window whose start cannot be named is not a measurement. Writing the correction back matters: registering a reaction does not replay a retained value, so without the explicit peek-and-normalise the cell would sit there claiming a capture is running forever, and a remote reader would believe it |
| `millis` wraps mid-capture | Publish `ms: -1` | `millis` tags into a 31-bit SMI (~12.4 days uptime). A negative elapsed must not be published as a plausible wrong number — silent-wrong is the failure mode this work exists to remove |

### 3.4 Where the numbers come from, afterwards

`/stats` keeps **two** tables: the live cumulative one (since boot, still self-polluting,
still useful for a glance) and the **last capture result** (clean). The four cosmetic items
land on the result table, where they are free.

### 3.5 The legend

Below the result table, and it documents **two** things — the columns, and **what each span
actually represents**. A column legend alone leaves `jobThermal` versus `acquire` as a
guess, which is the harder question and the one a reader at the bench actually has.

Span meanings are not this design's to invent; they are pinned in
`~/workspace/tuvm/docs/design/2026-07-21-job-latency-instrumentation-design.md` §5 and the
legend must not drift from it:

| Job | Span | What it brackets |
|---|---|---|
| timer→thermal→cell | `tickQueue` | pulse post → reaction entry — the reactor delay #34/#55 made observable |
| | `acquire` | `seam sensorFrame` — the I2C read |
| | `vmCompute` | the 64-element raw→List copy |
| | `publish` | `sample put:value:` |
| | `jobThermal` | the whole chain — **outer, nested over the four above** |
| request→format→response | `httpQueue` | `http_accept_request` → `http_take_request` |
| | `render` | the route block |
| | `egress` | the `write()` inside `http_chunk` |

Two things the legend **must** say, because a reader who does not know them will draw wrong
conclusions from correct numbers:

- **`jobThermal` is an outer span nesting the other three.** Its time is not additional to
  them, and summing the thermal rows double-counts.
- **`render` and `egress` overlap by design.** `httpChunk:` formats in nsl and writes the
  socket in C, so an nsl-bracketed `render` necessarily contains the writes. Render-compute
  is **`render − egress`**, a *derived* figure — the legend says so rather than letting the
  table imply it was measured directly.

`sumLo`/`sumHi` also need a line: the sum ships as the two halves C keeps it in, because
reassembling it on the node would overflow a 31-bit SMI at `hi ≥ 2`. Divide off the device.

**Rule, and it is the point of the whole exercise: figures quoted in `review/` or in the
deck come from the capture table only.**

## 4. The `diag/` family

### 4.1 Layout

The node already has a family convention (`~/workspace/tuvm/review/zephyr-driver-exposure.md`:
`dev/<device>/sample·event·ctl`). Diagnostics get the same **shape**:

```
  dev/<device>/{ctl, sample, event}        existing
  diag/<facility>/{ctl, result, event}     new — same shape, one word different
```

Populated lazily. Naming the family costs nothing; creating leaves nobody reads does.

| Facility | Leaves | When |
|---|---|---|
| `diag/spans` | `ctl`, `result` | now — this design |
| `diag/dbg` | `req`, `resp` | phase 3 item 2, unbuilt |
| `diag/prof` | `ctl`, `result` | Arc 2, unbuilt |
| `diag/stats` | `result` | **not yet** — `/stats` reads `platform metrics` synchronously and nothing remote wants it |

`diag/dbg` keeps a request/response **stream pair** rather than `ctl`/`result`. A debug
session is genuinely a different shape, and forcing it into the facility mould would be
uniformity for its own sake. It belongs to the family by *authority* — outside the
convergence tree, granted together — not by having matching leaves.

### 4.2 Closed leaf vocabulary

**Leaf names come from a small closed set — `ctl`, `result`, `sample`, `event` — not free
text.** This is the discipline that makes a served prefix and any generic tooling possible
later, and it is far cheaper to hold from the start than to retrofit.

`result` and `sample` are deliberately different words for deliberately different things:

- **`sample`** — periodic, sensor-driven, always current (`dev/**`).
- **`result`** — produced once, at the end of a bounded run (`diag/**`).

Names considered and rejected for the `diag/` out-leaf: `sample` (implies periodic
resampling — wrong direction); `snap` (a snapshot is a state at an instant, but this record
is an accumulation over a window — the same class of error one layer down); `last` (not a
measurement word); `val` (names the *kind*, not the content — a cell just is a value, so
every out-leaf on the node could take it); `run` (reads as a verb); `stats` (stutters
against `spanStats:` and the `/stats` page).

### 4.3 What grouping does and does not buy

**It does not buy channel budget.** `find_channel` is an exact string match
(`~/workspace/tuvm/src/space.c:130`) and `channels[]` is keyed by full path — a prefix does
not exist in the implementation. `diag/prof/ctl` costs exactly what `prof/ctl` would.

Budget is comfortable anyway: `channels[OEVM_SPACE_MAX_CHANNELS]` is a **static** 32-entry
array (`src/space.h:33`) with 6 in use, so the two new names cost **zero DRAM** — which
matters against #53. Full build-out of the table above reaches ~13.

What *would* threaten the cap is **per-instance naming** (`diag/spans/jobThermal/ctl`,
`diag/spans/egress/ctl`), which grows with the workload rather than the toolset. Hence:

**One `ctl`/`result` pair per facility, never per instance. The record carries the
instance, not the path.** This is already the house pattern — `dev/thermal0/ctl` takes
`{rate: N}` rather than spawning a channel per knob.

**What grouping does buy** is authority and future structure: diagnostics stay out of the
`goal/`/`obs/` convergence tree so fleet anti-entropy never walks them, and `diag/**` is
shaped to become a **served prefix** (#32 option B, recorded as unbuilt and unblocked) —
one channel entry backed by a server job instead of 2N leaves. Not now; 13/32 is not
pressure. But the leaf layout built here is exactly what B would serve, so being
compatible costs nothing today.

### 4.4 Authority

`*/ctl` is the only remote-writable leaf in either family. Everything else is node-pushed
and read-only from outside. One sentence, covers `dev/**` and `diag/**` both.

## 5. What it costs

| Piece | Cost |
|---|---|
| Reset reachable from nsl | One prim wrapping the **existing** `span_reset_all()` (`src/span.c:90`, currently "tests only"). Append-only prim id, counted-not-computed. **No new `.c` file**, so #63's nine-list footgun does not apply |
| Two channels | Zero DRAM (§4.3) |
| The accumulator | Already exists, already static C (`src/span.c:17`) |
| `SpanCapture` class | ~60 lines in `~/workspace/nsl-tuvm/lib/kernel.nsl`, beside `Spans` — **not** in the node file; see below |
| Result record | ~600–800 B, allocated **once, at stop** |

**GC pressure inside the measurement window is zero.** Nothing allocates until the capture
is stopped, and the stop allocation is by construction outside the window it reports.

### 5.1 Why `SpanCapture` lives in the kernel library, not the node

Corrected during planning (2026-07-22). An earlier draft put the class in
`zephyr/ThermalHttpNode.nsl`. That is **incompatible with §6**: `nsl-tests/` cannot reach a
class nested in a zephyr node file, so the host tests that pin both ordering invariants
could not be written, and the one real improvement this design offers over the status quo
would have been lost.

It is also the better home on the merits. `SpanCapture` knows nothing about thermal
sensors, HTTP, or any device — it takes a `Space` and a `Spans` and reacts to a cell. That
is exactly the profile of `Spans` itself, which already lives in `lib/kernel.nsl`.

**The cost to acknowledge:** the kernel is shared, so every node's image carries the class,
including Thread-only nodes that will never start a capture. This is image/heap cost rather
than static DRAM (the ceiling in #53), and `Spans` sets the precedent — but it is a real
cost and the implementation must **measure it rather than assume it**: check live bytes and
the footprint gate before and after, and record the delta.

## 6. Testing

`zephyr/ThermalHttpNode.nsl` has **no host coverage today** — the only way to check a
change to it is to flash. The capture logic breaks that pattern, because the span seam
already runs on host (`nsl-tests/19_latency/01..05`). New host tests:

1. start → record known samples → stop → `result` contains exactly those samples
2. samples recorded **before** start do not appear — the reset-last invariant, asserted
3. samples recorded **after** stop do not appear — the snapshot-first invariant, asserted
4. `#stopped` while stopped writes nothing
5. `#running` while already running restarts the window rather than extending it
6. startup with a retained `#running` reports stopped

**Not testable, and worth saying so rather than implying coverage:** the `millis`-wrap guard
(`ms: -1`) has no test. `t0` is private to the instance and the clock is a real prim, so
forcing a negative elapsed would need a clock seam that does not exist and that this work
does not justify building. The guard is one comparison; the risk of it being wrong is low
and the cost of the seam is not. Recorded here so a later reader does not mistake the test
list for complete coverage.

Both ordering invariants become assertions rather than comments, which is the part that
would otherwise rot.

**Device verification** stays what it is, and is honest about being manual: flash, slide
on, run `zephyr/tools/latency_harness.py` against `/thermal`, slide off, read the result
table. The suite passing proves the capture logic, not the page.

## 7. What porta/nodus must change to follow

Not in scope here — recorded so the do-over inherits it rather than rediscovering it.

- **`diag/**` is a new top-level prefix**, outside the convergence tree, with `*/ctl`
  remote-writable and everything else read-only.
- **`dbg/req`/`dbg/resp` become `diag/dbg/req`/`diag/dbg/resp`.** Nothing implements them
  on the nsl side (phase 3 item 2 is unbuilt), so this is the cheapest this rename will
  ever be.
- **Drop "kind is fixed by path prefix"** (`PROTOCOL-NSL.md:182`). It is already
  contradicted by the node's own `dev/**` family — `dev/thermal0/sample` is a cell and
  `dev/thermal0/tick` is a stream under one prefix. In the VM, kind is a property of the
  **leaf**, set by the first write: `put` makes a cell, `post` makes a stream, and the
  wrong verb raises. The implementation has enforced this since rung 9; the protocol
  document never caught up.
- **Pin the two record schemas** (`diag/spans/ctl`, `diag/spans/result`) in
  `PROTOCOL-NSL-RECORDS.md`, per the standing rule that per-prefix schemas are pinned
  before first implementation.

## 7a. Implementation addendum — 2026-07-22

Written after building and flashing it. The design above stands; these are the corrections
and additions reality forced.

**Added at David's request: a `lang` column** (C | nsl) between `span` and `n` on both
tables, from `Spans>>langAt:`. The #61 design §5 already carried this as its `owner` column,
and "is this the VM or the seam" is the first question a reader has. Deliberately **derived**
from the reserved C-owned pair (slots 0/1) rather than kept as a table, so it cannot drift;
and deliberately **not** added to `recordAt:`'s wire record, which is size-capped by porta's
256 B value law and whose readers can derive it.

**The shared-kernel RAM cost is zero, measured.** §5.1 required this be measured rather than
assumed: the flashed build reports `dram0 193,040 B / 98.19%` — 3,568 B spare, exactly the
figure the ladder recorded after #65. Moving `SpanCapture` into the shared kernel consumed no
static DRAM.

**The feature is not usable from a browser, and that is tuvm#73.** A single browser tab pins
the node's connection budget (`CONFIG_NET_MAX_CONTEXTS=5`, four usable after the listener)
with parallel and speculative sockets, and the node then answers nothing at all. Pre-existing
— but this design is its first victim, because it turns a one-page-view into a
start → load → stop → read workflow.

**A wrong fix was tried and measured wrong, and the reasoning is worth keeping:**
tightening the keep-alive probes 10/5/3 → 3/2/2 does **not** help, because **keep-alive reaps
a DEAD peer, not an IDLE one** — a browser answers the probes, so the connection is healthy
by definition. The original evidence ("the node recovered the instant Chrome navigated away")
was Chrome *closing its sockets*, not a reaper firing; attributing it to the reaper is what
produced the wrong fix. Caught by measuring: four held-open idle sockets, no recovery in 60 s.

**Standing rule this design establishes, for the ladder to absorb at close-out:**
figures quoted in `review/` or in the performance deck come from the **capture table only** —
never from the cumulative table, which partly measures its own rendering.

## 8. Out of scope

- The porta/nodus rewrite itself (§7 is input to it, not part of this work).
- `#32` option B, served prefixes — anticipated, not built.
- `diag/prof`, `diag/stats` — named, not built.
- #50 (eliminating per-rect `printString` and row concat on the render path). Adjacent and
  still open; this design does not touch the `/thermal` render.
