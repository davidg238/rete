# `Cbor>>encode:` — what shape is the cost? (#163)

**Date:** 2026-08-09 · **Issue:** #163 (and #161) · **Status:** measured on host,
predicts the device. Spike tree: `docs/spikes/2026-08-09-cbor-encode-shape/`.

**Answer up front: the cost is PER BYTE, it is ~9 activations per byte for flat data,
and almost none of those activations are the CBOR walk.** 99.5% of a flat encode is the
byte accumulator moving bytes; the recursive descent that decides what CBOR to emit is
half a percent.
Two mechanisms multiply the per-byte rate: `ByteBuilder`'s doubling growth (+ its
exact-size freeze) costs **as much again as the emit itself**, and `encodeMap:` re-copies
every entry's encoded bytes through a per-entry buffer, once per level of map nesting.

It is not per element (64× the elements costs +45%), and it is not allocation or GC
(400 encodes of a 425 B frame move the GC counter by one).

## Why this exists

#163 says plainly: *do not optimise from the source reading.* It lists four candidate
causes and refuses to pick one, because the 230 µs/byte in its title is a single frame's
total divided by a bound on its size. This is the falsifiable precondition it names —
"a bench that encodes one known record N times and divides."

## Method

`docs/spikes/2026-08-09-cbor-encode-shape/CborEncodeShape.nsl`, run by `run.sh`.
Payloads vary **one axis at a time** and every byte count is measured, never assumed.

Two arms in **one binary**, which is what keeps the cross-binary code-layout roulette
(#164's first, wrong measurement) out of this:

- **Timing** — `OEVM_TIMER_WALLCLOCK=1`, 20 span samples × 20 encodes, min sample / 20.
- **Activation counts** — `-DOEVM_METHOD_PROFILE` (#165), one profile window per payload
  wrapping exactly one encode. Exact, deterministic, and identical on host and device
  because an activation count is a property of the algorithm, not of the machine.

The profiler is built in and OFF until `platform profileStart`, so both arms share a code
layout. Offsets are resolved against the image's own `.oimg.methods.json` sidecar by
`resolve.py` — a sidecar from another build resolves to plausible wrong names.

## The numbers

Raw output: `report-baseline.txt` in the spike tree.

| payload | bytes | activations | act/byte | host ns/encode | ns/byte | gc |
|---|---:|---:|---:|---:|---:|---:|
| `str16` | 17 | 136 | 8.00 | 9 200 | 541 | 0 |
| `str64` | 66 | 581 | 8.80 | 38 950 | 590 | 0 |
| `str256` | 259 | 2 322 | 8.97 | 155 450 | 600 | 0 |
| `ints64` | 66 | 843 | 12.77 | 70 000 | 1 060 | 0 |
| `map8` | 41 | 940 | 22.93 | 66 750 | 1 628 | 0 |
| `listNest1` | 260 | 2 338 | 8.99 | 157 350 | 605 | 0 |
| `listNest4` | 263 | 2 386 | 9.07 | 160 900 | 611 | 0 |
| `mapNest1` | 264 | 4 713 | 17.85 | 301 250 | 1 141 | 1 |
| `mapNest4` | 279 | 12 036 | 43.14 | 733 950 | 2 630 | 1 |
| **`batch`** | **425** | **9 624** | **22.64** | **620 500** | **1 460** | **1** |

`batch` is the real 425 B push frame, lifted verbatim from `22_spacesync/09_batch_size.nsl`
so the anchor is the frame the fleet actually sends.

### Per byte — linear, with no quadratic term

`str16` → `str64` → `str256` is a 15× range of payload size at a flat 8.0 → 8.8 → 8.97
activations per byte and 541 → 590 → 600 ns per byte. **`ByteBuilder` is not quadratic**,
which retires one of #163's four candidates outright: the doubling growth is real cost
(below) but it is amortised, not superlinear.

### Not per element

`ints64` and `str64` encode to the same 66 bytes. `ints64` has **64 elements** where
`str64` has one, and costs 843 activations against 581 — **+45% for 64× the elements**,
i.e. ~4 activations per element against ~9 per byte. Per-element cost exists and is minor.

### Not per allocation, and not GC

The `gc` column is the GC-cycle delta across **400 encodes**. Flat payloads move it by
zero; the three largest move it by one. Allocation pressure is not what this is.

### The decisive pair: Maps re-copy, Lists do not

`listNest4` vs `listNest1` — the same 256-byte string wrapped in four Lists instead of
one — costs **+2%** (2 386 vs 2 338). `encodeList:` writes its elements straight into the
caller's accumulator, so depth is free.

`mapNest4` vs `mapNest1` costs **2.55×** (12 036 vs 4 713), and `mapNest1` already costs
**2.0×** `listNest1` at the same size. `encodeMap:` gives every entry its own
`ByteBuilder`, encodes into that, then copies those bytes into the accumulator **one
`add:` send per byte** (`appendColl:`). So a byte at map-nesting depth *d* is written
*d+1* times and read *d* times.

The predicted ratio for 1 → 4 levels is 5 passes / 2 passes = 2.5. **Measured 2.55.**
The `appendColl:` block's own activation count is the direct read: 263 for `mapNest1`,
1 082 for `mapNest4`, 704 for `batch` — i.e. **`batch` re-copies 704 bytes to emit 425.**

### Where a flat encode's activations actually go

`str256`, 259 bytes out, 2 322 activations, decomposed exactly (the terms sum to the
total, so this is arithmetic and not attribution):

| term | activations | share | what it is |
|---|---:|---:|---|
| emit | 777 | 33.5% | `ByteBuilder>>add:` ×259, its `byteSize` grow-check ×259, `u8At:put:` ×259 |
| **grow** | **1 012** | **43.6%** | five doublings 16→512 copying 496 bytes, each byte a read *and* a write |
| **freeze** | **522** | **22.5%** | `frozenBytes` copies all 259 bytes into an exact-size array |
| the CBOR walk, + setup | 11 | 0.5% | `encode:into:depth:`, `emitHead:len:into:`, `encodeString:`, `rankOf:` |

**Two thirds of a flat encode is the accumulator's own copying.** The codec's actual
decisions — majors, heads, shortest-form, the depth bound — are half a percent.

One `acc add: b` costs **four** activations, because `ByteBuilder>>add:` and the
`ByteArray` accessors it calls (`byteSize`, `u8At:put:`) are all nsl methods, and this
system's interpreter charges an activation for each. That is the same finding as
`nsl-per-byte-accessor-cost` (`u8At:` is a method, so every byte buys an activation),
reaching the codec.

## Both arms validate against numbers measured elsewhere

This is a host run making a claim about a device, so it is worth stating what independently
confirms it.

**1. Host time is activations.** Dividing host ns/encode by activations across the whole
table gives **61–83 ns per activation**, and 61–71 over the eight largest payloads — a 90×
range of payload size and four different payload shapes. #164 measured this host
interpreter's baseline at **58 ns/activation** on `fib`, a completely different workload.

**2. The activation count predicts the hardware number #163 already measured.** #164 also
measured the C6's baseline at **11.83 µs/activation**. `batch` is 9 624 activations:

> 9 624 × 11.83 µs = **113.9 ms predicted** vs **118.571 ms measured** on the bridged C6
> for `pushEncode` — a **4% miss**, against a frame of "at most 512 B" whose exact size
> the device never reported.

So `pushEncode`'s 118 ms is not mysterious and it is not the radio, the GC, or a quadratic
buffer. It is **9 600 method activations to emit 425 bytes**, on an interpreter that costs
11.83 µs each. The right way to state #163's rate is not "230 µs per byte" but
**22.6 activations per byte × 11.83 µs per activation** — the same number, decomposed into
the part a fix can move and the part it cannot.

## What a fix is worth — one term, measured

The largest single term is `ByteBuilder`'s growth. Sizing it away is one line, so the arm
was run: `ByteArray new: 16` → `ByteArray new: 512` in `ByteBuilder`, same bench, exact
activation counts (`report-nogrow.txt`).

| payload | baseline | no-grow | delta |
|---|---:|---:|---:|
| `str256` | 2 322 | 1 310 | **−43.6%** |
| `mapNest4` | 12 036 | 6 976 | −42.0% |
| **`batch`** | **9 624** | **7 456** | **−22.5%** |

`batch`'s predicted C6 `pushEncode` falls from 113.9 ms to **88.2 ms**. Host time agrees
(620 → 471 µs, −24%), though the count is the authority there — the two arms are separate
binaries.

**That 512-byte constant is a probe, not a proposal.** `encodeMap:` allocates two
`ByteBuilder`s *per map entry*, so a 512 B floor turns a `batch` encode into tens of
kilobytes of transient buffer on a 128 KB C6 heap — visible in the run as `batch`'s gc
delta going 1 → 2. What the arm establishes is the **size of the term**, which is what
#163 asked for: ~22% of a real push frame is the accumulator growing, and it is recoverable
by *some* policy that does not pay for it per map entry.

## Step 1, implemented the same day: `ByteBuilder` holds the raw indexable

`ByteArray>>u8At:` is `( ^ raw at: i )` and `u8At:put:` is `( raw at: i put: v. ^ v )` —
**one layer of nsl method over a prim.** The profile is what proves the layer is the whole
cost: `str256` counted 1 014 `ByteArray>>u8At:put:` activations and **zero** for the
`at:put:` beneath it, so the prim underneath is free and the wrapper is not.

`ByteBuilder` is private to `Cbor` (three construction sites, all inside `encode:`) and its
buffer never escapes except through `frozenBytes`, so it has no type discipline to enforce.
Holding `platform basicNewBytes:` directly instead of a `ByteArray` is **mechanism removal,
not addition** — no new primitive, no new selector.

| payload | before | after | |
|---|---:|---:|---|
| `str256` | 2 322 | **274** | **8.5×** |
| `mapNest4` | 12 036 | 2 618 | 4.6× |
| **`batch`** | **9 624** | **3 285** | **2.9×** |

Emitted bytes are unchanged — `14_cbor/02_golden_hex`, `03_map_order`, the round-trips, and
both encoded-size guards (`19_latency/05_record_size` = 140/195 B,
`22_spacesync/09_batch_size` = 87/195/425 B) all pass untouched.

### And this is where the uniform-cost model stops applying

Activations fell **66%** on `batch`; host time fell **33%** (620 → 415 µs). The gap is the
finding: the activations removed were *one-line accessors*, which cost well under an average
activation. `host ns/act` — added to the report for exactly this reason — was a flat
**67–68 across every payload** in the baseline and is **126 for `batch`** and **312 for
`str256`** after. The mix has shifted, so `activations × 11.83 µs` is now a **floor**, not
an estimate:

- uniform-cost model (a floor): `pushEncode` 118.6 → **38.9 ms**
- host-relative scaling (a ceiling): 118.6 × 0.669 → **79.3 ms**

The truth is bracketed at **39–79 ms**, and the argument favours the lower half: #164 found
the host could not resolve the mprof hook *at all* while the C6 resolved it to three
significant figures, because an in-order core gives frame push/return no out-of-order shadow
to hide in. Removing frame pushes should therefore pay *more* on the C6 than on this laptop.

**This does not retract the 4% validation above** — that was measured on the unmodified
codec, whose activation mix happened to match the Hanoi average. It says the model does not
survive a change that removes a non-representative slice of activations, which is precisely
what a fix does. **Any real number here needs the board**, the way #161 got one.

## Step 3, same day: `encodeMap:` buffers only the keys and streams the values

The sort needs encoded **key** bytes and nothing else — `sortPairsBytewise:` permutes two
parallel collections in lockstep and never inspects the second — so the second one can
carry value *objects* and each can be encoded straight into `acc` at emit time, in sorted
order, at the depth it would have had anyway. Encoding is a pure function of
(value, depth), so the bytes are identical. This is the difference between buffering a
Symbol and buffering a whole nested record.

| payload | baseline | step 1 | step 3 | total |
|---|---:|---:|---:|---:|
| `str256` | 2 322 | 274 | 274 | 8.5× |
| `mapNest1` | 4 713 | 845 | **319** | 14.8× |
| `mapNest4` | 12 036 | 2 618 | **454** | **26.5×** |
| **`batch`** | **9 624** | **3 285** | **2 325** | **4.1×** |

Same golden and size-guard evidence as step 1: emitted bytes unmoved.

### The allocation arm — and it answers a different question than the speed arm

The `gc` column is meaningless on the 16 MB host default. Re-run at a **fixed 256 KB
arena** (`gc-arms.sh`), where GC cycles per 400 encodes are proportional to bytes
allocated:

| payload | baseline | step 1 | step 3 | total |
|---|---:|---:|---:|---:|
| `str256` | 8 | 7 | 7 | −13% |
| `map8` | 29 | 26 | 18 | −38% |
| `mapNest1` | 57 | 56 | **10** | **−82%** |
| `mapNest4` | 215 | 210 | **20** | **−91%** |
| **`batch`** | **198** | **183** | **89** | **−55%** |

**The two changes do not overlap — they trade off against each other's blind spot.**
Step 1 is a time fix and moves allocation by 8%: it deletes one small `ByteArray` wrapper
object per buffer, but the buffer itself was always the bytes. Step 3 halves allocation on
the real frame and cuts it by ~90% on nested maps, because it deletes the *per-value*
buffer entirely.

That is the direct answer to the failure `review/lib/cbor.md` records: a WROOM-32 died with
an **uncatchable** `VM GC Error: Out of Memory -> abort()` on a 949-byte span push, because
`encodeMap:` held an accumulator for every key **and** every value simultaneously. The
value half — the big half — is now gone; what stays live is one buffer per key holding a
4–10 byte Symbol.

Caveats: this is the host allocator at 256 KB, not the C6's 128 KB. The bench does not boot
at 128 KB, for a reason unrelated to the codec — a single boot-time allocation of the whole
arena + 24 B, identical in all three arms and not chased here. Cycles at a fixed arena are
proportional to bytes allocated, so the **ratios** transfer; the absolute counts do not.

## What this picks, in order

Everything below is now sized rather than argued, which was the whole point of measuring
first. Step 1 is implemented (above); 2–4 are not.

1. ~~**Stop paying four activations per byte.**~~ **DONE** — see the step-1 section above.
   `add:` now costs one activation instead of three, and `grow`/`frozenBytes` became prim
   loops. `str256` 8.5×, `batch` 2.9×.
2. ~~**Stop the map re-copy.**~~ **DONE** — see the step-3 section above. Only the keys
   are materialised now; the values stream. `batch` 3 285 → 2 325 activations and −51%
   allocation; nested maps −83% activations and −90% allocation.
3. **A bulk byte-range copy (`copyFrom:to:`, already scoped as #28) — now RE-RANKED, and
   worth less than it looked.** After steps 1 and 3, `batch`'s remaining 2 325 activations
   are 683 `add:`, 284 `ByteBuilder>>at:` and 284 `OrderedCollection` sort operations
   (all **key** work, which cannot be streamed — the RFC 8949 §4.2 order is only knowable
   after the keys are encoded), 258 `appendColl:` block invocations for those key bytes,
   and ~460 in the walk. A bulk copy takes the 258 and part of the 683, i.e. **the
   remaining upside is roughly 25–30%, not the ~2× it would have been before step 3** —
   against a new prim id. Measure before building.
4. **#161's fix (1) is unaffected and still true.** Carrying `valueFits:`'s bytes into the
   row deletes a whole *extra* encode of every value; this measurement is about what one
   encode costs. The two multiply rather than overlap.

## What this does NOT settle

- **Nothing was re-measured on hardware.** The device claim rests on 11.83 µs/activation
  from #164 and lands within 4% of #163's measured `pushEncode`; that is one point of
  agreement, not a calibration. Any fix must be re-measured on the C6 the way #161 was.
- **`pushCollect` and `pushTrim` are untouched here.** #163 models them with the same rate;
  this measurement neither confirms nor refutes that.
- **The decode half was not measured at all.** It has its own per-byte walk
  (`rawBytesOf:` copies every input byte through `u8At:` before parsing begins) and no
  issue yet.

## Gate

Not a suite test, deliberately: it prints timings, and a suite test that cannot fail is
worse than none. It builds its own VM and layout under `/tmp/cbor-encode-shape/` rather
than the suite's `/tmp/oevm_img`, because the suite has no lock (#167) and a clobbered
binary reads as a code regression.

The `ByteBuilder` probe above was reverted; `nodus/nsl/lib/kernel.nsl` is unchanged by
this work.
