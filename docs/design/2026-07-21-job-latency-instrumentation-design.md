# End-to-end job latency instrumentation — Design

**Date:** 2026-07-21
**Status:** approved (design), unimplemented
**Tracking issue:** **#61** (relates #34, #55, #60; constrained by #53)
**Constrains / is constrained by:** `~/workspace/tuvm/protocol/PROTOCOL-NSL-RECORDS.md` §2.2
(the 256 B `MAX_VALUE_BYTES` cap) and §4.4 (which explicitly forbids a 12th `#Metrics`
field and names this issue as the reason to split).
**Standing rules honoured:** `~/workspace/tuvm/review/metrics-spec.md` ("collection is a
fixed C array of counters, zero allocation on any hot path"; "node reports raw, porta
computes trends"), `~/workspace/tuvm/review/reliability-gaps.md` G3 (buffers live in a C
arena, never the VM heap) and G9 (no unrequested periodic work).

---

## Goal

Measure how long a real **job** takes start to finish, **segmented**, on the two job
shapes `ThermalHttpNode` already runs:

- **timer → thermal → cell** — a timer fire acquires an 8×8 Grid-EYE frame and publishes
  it to a cell
- **request → format → response** — an HTTP request is routed, an SVG is rendered, and
  the bytes go out the socket

The deliverable is not the mechanism; it is a **published table of segmented latency** for
both shapes, measured on hardware, in `review/program-sequence.md` and
`review/tests/CLAIMS.md`: p50 and p95 as bucket **decades**, exact max, sample count, and
failure count — the reporting rules are §4. The mechanism exists to produce that table and to keep
producing it for every job added later.

### Why this is the stronger benchmark story

`richards`/`deltablue` answer *"how does your VM compare to other VMs"*. Job latency
answers *"is this fast enough for my use case"*. It also reframes the VM as one line item
in a budget rather than the protagonist — "timer→thermal→cell is 340 ms, of which 12 ms is
VM execution" tells a skeptic more than any richards number, and it is a confident move
rather than a defensive one.

It feeds the OEOM native-substitution story directly (#45): segment timings say *which*
`Binding` is worth a native C drop-in, which turns the measured ~117× from a party trick
into a targeted decision.

## Non-goals (YAGNI)

- **No per-sample trace, ever.** No sample array, no ring of timestamps. `metrics-spec.md`
  already ruled per-message tracing out, and GC is collect-when-full — a growing list
  pegs heap high-water to the arena ceiling (see the VM memory-model rules).
- **No on-node percentile computation.** The node ships raw buckets; the reader reduces
  them to decades. Decided in §4.
- **No interpolated percentile, anywhere.** A percentile is the decade it lands in, not a
  number inside that decade — §4.1.
- **No clock synchronisation with the client.** §3 explains why none is needed.
- **No new periodic publisher.** The record is built on request, like `#Metrics` (G9).
- **No instrumentation of the VM interpreter itself.** Spans bracket *jobs*, not opcodes.

---

## 1. What the clock can actually do — measured, not assumed

The issue's first open question was whether `monotonicMillis` can resolve a ~1 ms segment.
It cannot, and this was checked against the built configs rather than guessed:

| target | `CONFIG_SYS_CLOCK_TICKS_PER_SEC` | `k_uptime_get()` step | `CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC` | `k_cycle_get_32()` step | u32 wrap |
|---|---|---|---|---|---|
| ESP32-C6 | 1000 | 1 ms | 16 MHz | **62.5 ns** | 268 s |
| ESP32 WROOM-32 | 10000 | 1 ms (the API is ms) | 240 MHz | **4.2 ns** | 17.9 s |

*(Read from `zephyr/build-esp32c6-B/zephyr/.config` and `zephyr/build-esp32-B/zephyr/.config`
on 2026-07-21.)*

So `platform monotonicMillis` — `k_uptime_get()` on target
(`src/backend/zephyr/timer_zephyr.c:88`) — is genuinely 1 ms granular and cannot resolve
the VM-compute segment, which is expected to be in the hundreds of microseconds.

**A microsecond clock is free on both targets.** `k_cycle_get_32()` is a register read.
Its 32-bit wrap is irrelevant to this design because spans only ever use **unsigned
32-bit differences**, which are correct across wrap provided the span is shorter than the
wrap period — 17.9 s on the WROOM, the tighter of the two. A span that long is a hung job,
and §5 says what happens to it.

`CONFIG_TIMING_FUNCTIONS` is **not** set on either build, so Zephyr's `timing_*` API is
unavailable; `k_cycle_get_32()` is the right primitive and needs no new Kconfig.

## 2. The seam — C holds nameless slots, nsl owns the names

### 2.1 The C side carries data only

A new backend leaf beside `timer_backend_millis()`, in `src/span_backend.h`:

```c
uint32_t span_now(void);   /* free-running microsecond counter; wrap-safe under u32 delta */
```

| backend | implementation |
|---|---|
| `backend/zephyr/span_zephyr.c` | `k_cyc_to_us_floor32(k_cycle_get_32())` |
| `backend/posix/span_posix.c` | `clock_gettime(CLOCK_MONOTONIC)` → µs, **except** under the default virtual clock, where it answers `timer_backend_millis() * 1000` so the suite stays deterministic |

Storage is one fixed static array — the `metrics-spec.md` mechanic, unchanged:

```c
struct SpanSlot {
    uint32_t start;      /* span_now() at spanBegin:, 0 when not open */
    uint32_t n;          /* samples closed */
    uint32_t min, max;   /* microseconds */
    uint64_t sum;        /* microseconds */
    uint32_t b[6];       /* log-spaced decade buckets, see §4 */
};                       /* 48 bytes: 4 u32 = 16, u64 sum at offset 16, b[6] at 24..48;
                            8-byte struct alignment from the u64, no padding waste */
static struct SpanSlot spans[OEVM_SPAN_SLOTS];   /* default 8 -> 384 B */
```

Zero allocation, no locking (cooperative fibers, the `prim_cache` precedent), safe to bump
from any context. `OEVM_SPAN_SLOTS` is a compile-time knob so a DRAM-tight target can trim
it and the C6 can go wide — the same shape as the seven existing `OEVM_*` env knobs.

**C never learns a slot's name.** Slots are `0..N-1`. Which slot means "acquire" lives
entirely in nsl. This is not a style preference; it is the rule `src/timer_backend.h`
already states for the timer seam — *"The C side carries DATA ONLY (a token, a millisecond
count). Which channel, what message … live in nsl."* It also keeps the name strings out of
the WROOM's DRAM, which matters given §6.

### 2.2 Four prims

Appended **after** `PRIM_pf_sensorArmEvent` (currently the highest id, 117), so no
pre-existing id shifts — the binding constraint on this table. Ids are the 1-based
position in `PRIM_NAMES`, counted not computed, per the standing rule in `prim_ids.h`:

| id | selector | args | answers |
|---|---|---|---|
| 118 | `spanBegin:` | SMI slot | the platform capability |
| 119 | `spanEnd:` | SMI slot | the platform capability |
| 120 | `spanStats:` | SMI slot | an 11-slot raw Array of SMIs: `[n, minUs, maxUs, sumUsLo, sumUsHi, b0..b5]` |
| 121 | `spanRecord:us:` | SMI slot, SMI µs | the platform capability — **test seam**, §8 |

`spanRecord:us:` is compiled unconditionally rather than gated behind a test define: a
prim is a `PRIM_NAMES` entry plus a switch case, and gating it would fork the parity
guard of §2.2 between build flavours — a worse trade than shipping it. It is harmless by
construction, since the only thing it can do is add a sample to a slot.

**There is no `fails` slot in C.** A failed job is an app-level concept the C seam cannot
detect, so the failure counter of §4 lives in nsl, on `Spans`, and is folded into the
record by the nsl record builder. This keeps the slot at 48 B and keeps C dumb.

`spanBegin:` stamps `start`. `spanEnd:` computes `span_now() - start` as a u32, folds it
into `n/min/max/sum` and the matching bucket, and clears `start`. Both are a handful of
instructions with no branch on the common path.

Three notes that are easy to get wrong:

- **There is no ceiling at 256 prims.** Prim ids are resolved at runtime by selector
  *name* — `selector_prim_id()` (`src/dispatch.c:823`) strcmps the interned symbol against
  `PRIM_NAMES[]` and answers `index + 1`, then caches it pointer-keyed. Nothing in the
  `.oimg` format or the bytecode carries a prim id (`src/oimg.h` has no prim field). `pid`
  is a plain C `int`. The real constraints are append-only numbering and the compile-time
  parity guard at `src/dispatch.c:811`, which must be updated to the new last id.
- **Cold-path cost.** Adding four names lengthens the linear scan run once per *unseen*
  selector. The cache is `PRIM_CACHE_SIZE` = 4096 on host, **256 on embedded**
  (`zephyr/CMakeLists.txt:192`), so the hot path is unaffected.
- **`sum` cannot cross the seam as one SMI.** `TAG_SMI` is `val << 1 | 1`
  (`src/oevm.h:62`), so an SMI is 31-bit signed — max 1,073,741,823 on a 32-bit target.
  A microsecond sum passes that after **17.9 minutes of accumulated span time**, which a
  long-lived node reaches in a day. `spanStats:` therefore answers `sum` split as
  `sumUsHi = sum / 1000000000` and `sumUsLo = sum % 1000000000`, both SMI-safe, recombined
  by the reader. Reporting `sumMs` instead was rejected: at ~800 µs per sample it truncates
  every sample to zero.

### 2.3 The nsl side

A `Spans` class in `~/workspace/nsl-tuvm/lib/kernel.nsl`, next to `HeapStats`. The app
registers each name **once at setup** and holds the answer, so the hot path only ever
passes an SMI — no Map lookup, no symbol compare, no allocation inside a measured span:

```
(* setup *)
spans::   platform Spans new.
acquire:: spans register: #acquire.        (* answers an SMI slot index *)
compute:: spans register: #vmCompute.

(* the measured path *)
spans begin: acquire.  f:: seam sensorFrame.  spans end: acquire.
```

`Spans` also owns the name↔slot table, so it — not C — builds the records in §4.

## 3. The honesty constraint: the node cannot see ingress wire time

The issue asks for request→format→response **including wire time**. The node physically
cannot measure the whole of that. `http_accept_request` (`src/http_core.c:129`) is the
earliest on-node moment; everything before it — client send, mesh or WiFi transit, TCP
handshake — happened somewhere the node has no clock for. Timestamping at the client and
differencing against the node would require clock sync, which this design refuses to build.

So the measurement is deliberately split into two independently honest halves:

| quantity | measured where | how |
|---|---|---|
| **node service time** | on-node | `http_accept_request` → `http_end`, segmented (§5) |
| **client-observed RTT** | this workstation | a small harness issuing N requests and recording its own wall clock |
| **network** | derived | `RTT − service time` |

No clock sync is needed because both halves are measured **with a single clock each**, and
only their *durations* are compared. This is also the more useful presentation: it shows a
skeptic exactly how much of the number is the VM and how much is the radio.

The timer→thermal→cell path has no client, so it is fully on-node.

## 4. Buckets, and how percentiles are reported

**The node ships raw buckets. The reader reduces them.** This follows
`metrics-spec.md`'s standing division of labour ("node reports raw; porta computes
trends"), keeps the distribution shape visible, and lets bucket edges be re-cut fleet-side
without touching a node.

Bucket edges are log-spaced decades, chosen to straddle the expected range from a
~100 µs VM segment to a ~1 s retransmit tail:

| bucket | range |
|---|---|
| `b[0]` | < 100 µs |
| `b[1]` | < 1 ms |
| `b[2]` | < 10 ms |
| `b[3]` | < 100 ms |
| `b[4]` | < 1 s |
| `b[5]` | ≥ 1 s |

Six buckets is the largest count that keeps eight slots inside §6's budget. Bucketing is a
comparison chain, not a division.

### 4.1 Percentiles are reported as decades, never as interpolated values

**p50** is the median — half the samples came in faster. **p95** is the value 95% of
samples came in under; it is the number that exposes retransmit tails and GC pauses, which
is why the issue asks for it and not a mean.

**A percentile is reported as the bucket it falls in, not as a number inside that bucket.**
"p95 falls in the 1–10 ms decade" — never "p95 = 4,200 µs".

The reason is that interpolating within a decade bucket assumes samples are spread
uniformly across it, which is exactly what a log-scaled bucket is not: a bucket spanning
1 ms to 10 ms will in practice have most of its mass bunched at one end. An interpolated
figure would therefore carry three digits of precision the data does not contain, and
inventing precision is the single easiest thing for a critic to attack. Reporting the
decade costs nothing that matters — it still says which segment dominates and whether the
tail sits one decade or three above the median — and `max` is exact, so the worst case is
never approximated at all.

This supersedes the weaker "label it as interpolated" rule: with nothing interpolated,
there is no soft claim left to defend.

### 4.2 A p95 is not published below n ≥ 200

A percentile computed from too few samples is noise wearing a precise-looking label. At
n = 20, "p95" is simply the 19th value, and one unlucky retransmit relocates it entirely.

So: **n ≥ 200 before a p95 is published at all.** Below that threshold the report carries
`n`, `max`, and the full bucket histogram — which are all exact at any sample count — and
says nothing about p95. p50 is stable much earlier, but rides the same rule for
consistency. §8 sizes the client harness at 500 requests so the HTTP path clears the bar
without arranging the result; the thermal path clears it in under two minutes at 2 Hz.

### 4.3 The mean is never the headline

The issue's constraint 1 — percentiles, not means — is why `sum` exists at all: it is kept
for the mean only as a secondary figure. The 2026-07-20 C6 soak logged 9 misses in 2399
round-trips; a mean over that distribution is a lie, and the means-of-3 discipline that
governs the STIC benchmark table does not transfer to anything network-inclusive.

**Failure count is separate.** A job that fails does not produce a span sample, so a
failure would silently improve the percentiles. Each instrumented job therefore also
carries a plain failure counter, reported beside the latency — the issue's constraint 1
asks for exactly this. It lives on `Spans` in nsl, not in the C slot (§2.2).

## 5. What gets instrumented, and where the boundaries fall

Eight slots on `ThermalHttpNode`, which covers both job shapes in one node:

| job | slot | boundary | owner |
|---|---|---|---|
| timer→thermal→cell | `tickQueue` | pulse post → reaction entry — the reactor delay #34/#55 made observable | C + nsl |
| | `acquire` | `seam sensorFrame` — the I2C read | nsl |
| | `vmCompute` | the 64-element raw→List copy | nsl |
| | `publish` | `sample put:value:` | nsl |
| | `jobThermal` | the whole chain (outer, nested over the four above) | nsl |
| request→format→response | `httpQueue` | `http_accept_request` → `http_take_request` | C |
| | `render` | the route block | nsl |
| | `egress` | the `write()` inside `http_chunk` | C |

Two boundary decisions worth stating:

- **`render` and `egress` overlap by design.** `httpChunk:` both formats in nsl and writes
  the socket in C, so an nsl-bracketed `render` necessarily contains the writes.
  Render-compute is therefore **`render − egress`**, and the published table must show it
  as a derived column, not pretend it was measured directly. Splitting the prim to
  separate them was rejected as a larger change than the measurement justifies.
- **C-owned spans cannot be forgotten.** `httpQueue` and `egress` are stamped inside the
  prims that own the work, so no app can accidentally ship an uninstrumented HTTP path.
  The nsl-owned spans are the ones an app opts into, which is correct — they name
  app-level concepts C has no business knowing.

**Nesting** is supported because each slot holds its own `start`; `jobThermal` being open
while `acquire` opens and closes is just two independent slots.

**A span left open** (an exception unwinds past `spanEnd:`) leaves `start` set and never
records a sample. It self-heals: the next `spanBegin:` on that slot overwrites `start`. The
visible symptom is `n` lagging the job count, which the failure counter explains. No
watchdog, no cleanup hook — `ensure:` exists for a caller that wants the guarantee.

## 6. The DRAM budget — #53 is the binding constraint

The device leg targets the **classic ESP32 WROOM-32**, because it is the only board that
runs both job shapes: `CONFIG_AMG88XX` appears in `prj_esp32.conf` and
`prj_esp32_nowifi.conf` only, and HTTP-over-Thread on the C6 does not exist.

That board links at **dram0 99.58%, 816 bytes spare** (#53). The budget:

| item | bytes |
|---|---|
| `spans[8]` at 48 B | 384 |
| remaining headroom | ~432 |

The span prims themselves cost nothing in DRAM — four `const char*` entries land in
rodata/flash, and the `prim_cache` is already allocated at its embedded size of 256
entries. **The measured `dram0` delta is a required close-out artifact**, not an
afterthought: if the build does not link, `OEVM_SPAN_SLOTS` trims first and the finding
gets recorded against #53.

Bench state confirmed on 2026-07-21 by resetting each port and reading the ROM banner:

| port | board | evidence |
|---|---|---|
| `/dev/ttyACM0` + `/dev/ttyUSB0` | **one** ESP32-C6 DevKitC-1 (native USB + CP2102N UART) | `ESP-ROM:esp32c6-20220919`, 8 MB flash, MAC `40:4C:CA:45:47:A4` |
| `/dev/ttyUSB1` | ESP32 WROOM-32 rev v1.0 — **the thermal/HTTP board** | `ets Jun 8 2016`, 4 MB flash, `libc heap size 0 kB` (Model B), `wifi: connect` |

The two USB boards carry different bridge chips (CP2102N with a unique serial vs CP2102
with serial `0001`), so port order cannot scramble them; prefer the `/dev/serial/by-id`
paths. This supersedes the older note that the Toit/Jaguar board sits on `ttyUSB1` — no
Jaguar board is attached.

## 7. Surfacing — a new record, not a 12th `#Metrics` field

porta `PROTOCOL-NSL-RECORDS.md` §4.4 forbids appending here, and names this issue as the
reason: *"if per-job latency histograms ever want publishing (tuvm#61, where bucket counts
are many fields at once) — that wants its own record, split by subsystem."*

So: **one `#LatencySpan` record per slot**, on a new `sys/latency` cell.

```
#LatencySpan { span: sym, n: int, fails: int, minUs: int, maxUs: int,
               sumUsLo: int, sumUsHi: int, b0..b5: int }
```

Thirteen fields, all ints plus one symbol. It stays that size forever, because adding a
*job* adds a record, not a field — which is precisely the property `#Metrics` lacks. The
cell holds the latest snapshot; a reader `peek:`s it by name (§4.4 kind C).

**The short field names are deliberate.** §4.4 observes that symbol field names dominate
the encoded size, which is why `#Metrics` reaches ~185–230 B on eleven fields with names
like `largestFreeBlock`. Thirteen fields here total 46 characters of name against
`#Metrics`'s ~120. Rough estimate: ~59 B of keys, ~70 B of values, ~15 B of map and tag
framing — call it **~145 B**. That is an estimate of the same kind §4.4 warns about, which
is why measuring it is a task below and not a footnote.

`/stats` grows a second table rendering all eight slots: `n`, `fails`, exact `min`/`max`,
the six bucket counts, and the p50/p95 **decades** per §4.1 — with the p95 cell reading
`n<200` rather than a decade whenever the threshold of §4.2 is unmet. Rendering the
histogram alongside the decades is deliberate: it lets a human on the bench see the shape
that produced the summary, which is the whole reason the node ships raw buckets.

**One measurement is owed while we are here.** §4.4 flags that `#Metrics`'s ~185–230 B is
*an estimate, nobody has encoded the record and measured it*, and that it must be measured
before a 12th field. This work does not add that field, but it does build the encoder path
for a sibling record — so measuring the encoded size of both `#Metrics` and `#LatencySpan`
is nearly free here and discharges a standing debt. It is a task, not a hope.

## 8. Validation

**Host, in the nsl suite** — deterministic, no timing flake:

- `spanRecord:us:` (prim 121) feeds synthetic samples directly, so bucket edges are
  asserted **exactly** at 99/100/999/1000/… µs, and `n`/`min`/`max`/`sum` are asserted
  against known inputs. No clock is involved, so nothing can be flaky.
- `sumUsLo`/`sumUsHi` are asserted across the 10⁹ split, including a sum large enough to
  prove the SMI overflow of §2.2 does not occur.
- A separate test drives `spanBegin:`/`spanEnd:` under the **virtual** clock with
  `advanceMillis:`, proving the clock plumbing and that a span spanning an `advanceMillis:`
  lands in the expected bucket.
- An open-span test proves an unclosed span records nothing and self-heals on re-begin.
- The decade reducer of §4.1 and the n ≥ 200 gate of §4.2 are asserted against hand-built
  histograms — including the boundary cases n = 199 (p95 suppressed) and n = 200 (p95
  reported), and a histogram whose mass sits entirely in one bucket, which is the case an
  interpolating reducer would have fabricated a number for.
- Run with `nsl-tuvm/tools/run-one.py <file.nsl>` for the inner loop — the suite runner
  globs everything and takes 2–3 minutes. Path B (`.oimg`) only, per the standing rule.

**Device, on `/dev/ttyUSB1`:**

- Flash `ThermalHttpNode`, let it run long enough for both paths to clear §4.2's n ≥ 200:
  the thermal path needs ~100 s at 2 Hz, so a 5-minute run is ample.
- The client harness issues **500 requests** to `/thermal` and records its own RTT per
  request, which both clears the p95 threshold for the HTTP path and supplies the network
  column of §3. 500 is chosen to clear n ≥ 200 with margin — not tuned after seeing a
  result.
- Report the `dram0` delta against #53's 99.58% baseline.

**Anti-goal for the numbers:** these are *not* means-of-3 like the STIC table. The
benchmark discipline that governs `richards`/`deltablue` does not transfer to
network-inclusive numbers. p50/p95 decades, exact max, and a failure count, or it is not
published — and no p95 at all below n ≥ 200.

## 9. Close-out — which mainline document absorbs this

Per the promotion rule, this issue is not closed until:

- **`review/program-sequence.md`** — the ladder gains the rung and the measured table.
- **`review/tests/CLAIMS.md`** — the latency claims, each with its witness, its sample
  count, and percentiles stated as decades per §4.1.
- **`review/metrics-spec.md`** — the catalog gains the latency group; today it says
  *"histograms (porta can bucket; nodes can't afford the slots)"* under **Deliberately NOT
  collected**, and that line is now wrong. It must be amended to say per-job latency
  buckets are collected, with the slot budget that made it affordable — otherwise the
  spec contradicts the code.
- **`~/workspace/tuvm/protocol/PROTOCOL-NSL-RECORDS.md` §4.4** — `#LatencySpan` added to the
  `sys/**` table, plus the measured encoded sizes from §7.
- **`src/timer_backend.h` / `src/span_backend.h`** — #52's footgun note gains a third
  clock. There are now `oevm_millis`, `timer_backend_millis`, and `span_now`; the seam
  headers must say which is which and that only `span_now` is wrap-limited.

## 10. Sequencing

1. `span_backend.h` + both backends + the slot array + prims 118–121, with the parity
   guard updated.
2. `Spans` in `kernel.nsl`; the nsl suite tests of §8.
3. C-owned spans in `http_core.c` (`httpQueue`, `egress`) and the pulse path (`tickQueue`).
4. nsl spans in `ThermalHttpNode`; the `#LatencySpan` record and the `/stats` table.
5. Device run on `ttyUSB1`; the client RTT harness; the `dram0` measurement.
6. Promotion per §9.

Steps 1–2 are independently mergeable and leave the tree green with no behaviour change,
which is the natural review checkpoint.

---

## Addendum — implementation findings, 2026-07-21 (Tasks 1–7)

Recorded here rather than only in the tracker, because each of these corrects or confirms
something the design above asserted.

**§2.1's 384 B was right; the plan's 388 B was wrong.** The implementation adds a 4-byte
open-ness bitmask (a per-slot `uint8_t` flag would have rounded the struct from 48 B to
56 B under the `u64`'s alignment). The plan therefore predicted 388 B and flagged it for
reconciliation. Measured on the WROOM-32 with both builds `--pristine`, the delta is
**384 B exactly** — the mask landed in existing padding. No correction needed; the figure
above stands.

| build | dram0 | spare |
|---|---|---|
| `master` 63fcf64 | 195,936 B (99.66%) | 672 B |
| with spans | 196,320 B (99.85%) | 288 B |

**§6's baseline citation was stale.** #53 records 195,792 B at commit 54f055e; `master`
today is 195,936 B. The +144 B drifted in independently. Cite a commit alongside any dram0
figure, or re-measure.

**§5's `tickQueue` (plan Task 12) is dropped from this rung.** David's call, on the 288 B
remaining: spending the last of the DRAM on the one span that touches the ISR mailbox —
the seam #34/#55 just stabilised, and which #62 notes has no automated Zephyr coverage —
is a bad trade. The published table is complete without it.

**The nsl grammar has no `#( … )` literal array.** §4's decade names are a comparison chain
instead, which also allocates nothing per call. Same family as the literal-array trap
already known from the STIC benchmark work.

**§8's host validation understated the reach of the change.** `src/span.c` had to be wired
into **seven** enumerated source lists across two repos; patching the main runner's four
left `grant`, `debugger-integration` and `portability-32` failing to link while the suite
still printed `0 failed`. Filed as #63. The upside: `portability-32` is a `-m32` build, so
§2.2's 31-bit SMI reasoning — the whole reason `sum` is split lo/hi at 1e9 — is now
verified on an actual 32-bit target rather than argued.

**§2.1's posix backend needs `_POSIX_C_SOURCE 200809L`** for `clock_gettime` under
`-std=c99`, matching `timer_posix.c:5`. And `tests/test_span.c` supplies its own
`timer_backend_millis` stub rather than linking `timer_posix.c`, which drags in `space_post`
and from there the entire VM. That turned out better than a link fix: because the test
controls the virtual clock, the ms→µs scaling the whole nsl suite's determinism rests on is
asserted exactly.

---

## Addendum — implementation findings, 2026-07-21 (Task 8)

**Seven source lists was an undercount; it is nine.** Task 8 found two more that carry
`http_core.c` and `platform.c` but not `span.c` — `tests/http_roundtrip_host.sh` and
`tests/hanoi_ab_host.sh`. Both had been failing to link since **Task 2**, not Task 8: the
span prims put `span_begin`/`span_end`/`span_record`/`span_stats` into `platform.c`, and
neither script was patched. Neither is invoked by `run_tests_oevm.py`, so the suite's `0
failed` never saw them. This sharpens #63's real lesson: the count is not the point, the
absence of a single list is — a new engine `.c` file has no one place to be declared.

Fixed in the same rung. `hanoi_ab_host.sh` also turned out to be missing
`sensor_posix.c` from an *earlier* rung entirely, and had been red for longer; committed
separately (`a77db43`) so it is not buried in #61's history.

**§5's egress bracket covers all three writes of a chunk**, not the payload write alone —
size line, payload, trailing CRLF are one chunk's transport cost, and a caller tuning page
size acts on the total. `http_begin`/`http_end` stay outside: their framing is fixed-size
and per-*response*, so folding it in would smear a constant across a per-chunk
distribution.

**§5's httpQueue stamp rides `struct RingItem`, not the in-flight table.** The plan named
the in-flight array. `http_take_request` dequeues from the *ring*; the in-flight slot is
allocated at accept and freed at respond, so it outlives the interval being measured and a
stamp there would have measured the wrong thing. Per-request either way — several requests
can be queued at once, and one shared slot start would have them overwrite each other.

**Two stale build scripts, left alone deliberately.** `tools/footprint.sh` omits
`src/span.c`; it compiles with `-c` only, so nothing breaks, but the gated `mcu` branch now
under-reports the engine by span.c's text and its 384 B of bss. Adding it needs a
`FOOTPRINT_UPDATE_BASELINE=1` rebase, which is a deliberate act, not a side effect of this
rung. And `bench.sh` still `cd`s to `~/workspace/oevm` and takes `.ns` files — it builds a
different repo entirely and is unaffected by #61, but it is stale enough to mislead.

---

## Addendum — implementation findings, 2026-07-21 (Tasks 9–11, device run)

### §2.1's wrap reasoning was WRONG as written, and the device proved it

This is the correction that matters most in this document.

§2.1 and the original `span_backend.h` both argued that a 32-bit counter is safe here
because "spans use unsigned 32-bit DIFFERENCES, which are correct across a wrap provided
the interval is shorter than the wrap period." That argument is **only valid when the
counter wraps at 2^32 in the units the subtraction happens in.** The Zephyr backend
converted cycles to microseconds *before* returning, squeezing its value domain to
0..17,895,696 — so it no longer wrapped at 2^32, and a u32 subtraction across the wrap
produced ~4.28e9 instead of a small delta.

The first WROOM-32 `/stats` read showed `jobThermal` max = **−17872490** (a u32 of
4,277,094,806 printed as a signed SMI) with a phantom sample in the ≥1 s bucket. The
arithmetic pins the mechanism exactly:

    4,277,094,806 = 23,207 − 17,895,697 + 2^32

i.e. an entirely ordinary 23 ms thermal job. **Silent on the host**, where the backend
unit really is the microsecond and the counter really does wrap at 2^32 — so no host test
could have caught it, and none did.

Fixed by giving the seam two functions and moving the subtraction into the backend:
`span_stamp()` answers an opaque counter in whatever unit is cheapest (cycles on Zephyr,
microseconds on posix) and `span_elapsed_us()` differences and converts. Subtracting in
cycles is correct across the real hardware wrap; converting a small difference cannot
overflow. The stamp was **renamed** from `span_now` deliberately — a function named "now"
returning cycles is the trap that made the original bug look correct.

`tests/test_span.c` gained a direct wrap assertion. Read `src/span_backend.h` before
touching that seam; it carries the full reasoning.

### §5's C-owned slots moved to the BOTTOM of the range

Task 8 pinned them at `OEVM_SPAN_SLOTS - 2` and `- 1`, which reads tidier but makes both
indices depend on a **build knob** — and §7 itself contemplates dropping `OEVM_SPAN_SLOTS`
to 6 if DRAM runs short. nsl has to name those slots to render them, so it would have had
to mirror the ceiling, and a `-DOEVM_SPAN_SLOTS=6` build would silently point those names
at the wrong data.

They are now **0 and 1**. The reserved *count* is stable where the ceiling is not, so
`kernel.nsl` seeds its name table with the pair and `register:` hands out 2 upward by
construction. The no-collision property became structural instead of arithmetic, and no
constant is duplicated across the seam.

### §4.4's record-size estimate is discharged

`#Metrics` **195 B**, `#LatencySpan` **140 B**, measured through the real `Cbor>>encode:`
path (`nsl-tests/19_latency/05_record_size.nsl`, both paths agreeing). The estimate was
sound — 195 lands inside ~185–230 — but the headroom is now a number: **61 B** under the
256 B cap, roughly two more integer fields, not five.

Method note worth keeping: the figures were built from values **measured on the device**,
not round numbers. CBOR is shortest-form, so a sum of 7,820,873 costs five bytes where
52,746 costs three; a size taken against placeholders is not the size the fleet sends.

### The node does not compute the mean, by design

`/stats` publishes `min`, `max`, `sumUsLo` and `sumUsHi` raw. Reassembling
`hi * 1e9 + lo` on-node would overflow a 31-bit SMI the moment `hi` reaches 2 — which is
the very reason `span.c` splits the sum. Accumulate on the device, divide off it. This is
also `metrics-spec.md`'s "node reports raw; porta computes trends" rule applied literally,
and it means every published figure is derived from the accumulator rather than from
something the node rounded first.

### What the device run found that the host could not

Beyond the wrap defect: the node **leaves the network entirely under an unpaced request
burst** (#65) — `EHOSTUNREACH`, no panic, no console output, recovery only by hardware
reset. `pulseCoalesced` and `pulseDrops` were 0 throughout, so it is *not* the #54
blocking-render-starves-the-pump pattern. Every HTTP figure in the published table
therefore comes from a run **paced at 4 req/s**, and that qualifier is mandatory wherever
those numbers appear.

### Where the knowledge now lives

Per the promotion rule this document is the historical *why*; the current statements of
record are `review/program-sequence.md` (the rung and the table),
`review/tests/CLAIMS.md` (three rows), `review/metrics-spec.md` (the latency group, and
the removal of its now-false "nodes can't afford the slots" line) and
`protocol/PROTOCOL-NSL-RECORDS.md` §4.4 (`sys/latency` plus both measured sizes).

---

## Addendum, 2026-08-06 — the slot budget doubled, and six slots became kernel-owned

Two statements in this document are now superseded by `nodus/tuvm/src/span.h`, which is the
authority. Recorded here rather than edited above, because this document is the historical
*why*.

**`OEVM_SPAN_SLOTS` is 16, not 8** (§2.1's "default 8 → 384 B" is now 16 → 772 B, including
the 4 B open-mask). The knob's *character* is unchanged — §7's "a DRAM-tight target can trim
it" still holds — but the default moved, and it moved because the budget ran out at exactly
the point the interesting question became askable. It deliberately stops at half of the hard
ceiling of 32 (`span_open_mask` is one `uint32_t`), so the next node wanting a span does not
have to re-argue the DRAM.

**Six slots are now owned by the kernel, not by the app.** `PortaBridge` registers
`pullSend` / `pullWait` / `applyIn` / `pushBuild` / `pushSend` / `pushWait` at construction,
and `spans` is a mandatory constructor argument — the same reasoning §5 gives for pinning the
two C-owned HTTP slots, one level up: a node must not be able to ship an uninstrumented
bridge by forgetting a bracket. So the reserved count this document describes as *two* is
effectively *eight* for any bridged node, and an app's own spans start above them.

This does not disturb the §5 decision that made those two slots the **bottom** of the range.
That decision was taken precisely so the reserved indices would not depend on the ceiling —
and it is what let the ceiling move today with no constant duplicated anywhere and no test
edited. `nodus/tests/19_latency/01_span_slots.nsl` still passes unmodified, because it seeds
the reserved pair rather than deriving it from the knob. The design note warning that a
`-DOEVM_SPAN_SLOTS=6` build "would silently point those names at the wrong data" is the same
property that made an 8 → 16 build safe.

**What the slots bought.** `docs/results/2026-08-05-c6-checkin-decomposition.md`: the six
partition `checkIn` to 0.16%, the brackets cost nothing measurable against the previous
capture, and a porta check-in is **70% VM / 30% radio** with `PortaBridge>>pushFrame` alone
at 62% of the check-in. The expected culprit — 6LoWPAN fragmentation on the 512 B push frame
— measured 3.1 ms, the cheapest span in the table. Follow-on: #161.

Current statements of record for this addendum: `nodus/tuvm/src/span.h`,
`review/tests/CLAIMS.md` (the bracketing row), `protocol/PROTOCOL-NSL-RECORDS.md` §4.6 (the
eight non-app span names and the per-window cell-count consequence).
