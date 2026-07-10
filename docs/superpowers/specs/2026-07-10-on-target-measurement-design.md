# On-Target Measurement — design

> **Status:** design (approved 2026-07-10). Phase-2 rung following rung-3 (CBOR + Int64
> wire seam). Tracks **tuvm#5** (true target-config RAM line) and the G3/M2 "soak/heap
> gauge exposed to nsl". The follow-on rung is UDP/OpenThread transport (separate spec).

## 1. Purpose & scope

Phase-1 footprint/RAM numbers are host proxies (`tools/footprint.sh` default path: multi-MB
gc arena, host dispatch tables). This rung replaces the proxies with **real numbers** and
exposes **runtime heap telemetry** to nsl, so both the *static* image cost and the *dynamic*
arena behaviour can be observed on the real nRF52840-DK.

Three separable strands, one rung:

- **A. `heapStats` prim (104) + nsl surface** — expose the existing C-side `oevm_heap_stats()`
  to nsl as a dynamic gauge.
- **B. Layered footprint gate** — a CI-gated MCU static ratchet plus a banked, DK-captured
  real image-size reference.
- **C. DK on-device capture** — real `ram_report`/`rom_report` and a live `heapStats` soak
  under a workload, captured from the plugged-in DK.

**Out of scope (explicit):**
- UDP/OpenThread transport — the *next* rung; this rung is the measurement predecessor.
- Any GC / arena / allocation-behaviour change. This rung only measures and reports.
- Wrapping arithmetic / bitop LargeInteger path (tuvm#4) — unrelated, stays deferred.
- New runtime semantics beyond a single **read-only** prim.

## 2. Background (as-built facts this design rests on)

- `oevm_heap_stats()` already exists (`tuvm/src/gc.c:873`, decl `tuvm/src/oevm.h:414`) and
  returns `struct OevmHeapStats { size_t heap_size, live_bytes, high_water,
  largest_free_block, gc_cycles; }`. `live_bytes/high_water/gc_cycles` are maintained
  incrementally on the hot path; `largest_free_block` is computed on demand. **No prim
  exposes it today.**
- Prims are **append-only**; the last is **103** (`asSymbol:`). This rung adds **104**.
- `tools/footprint.sh` already has a `FOOTPRINT_CONFIG=mcu` branch that cross-compiles the
  engine with `arm-zephyr-eabi-gcc` and prints real `text/data/bss` via
  `arm-zephyr-eabi-size -t`. It is **not gated** (no ratchet / baseline).
- The authoritative whole-image numbers come from the DK west build's `ram_report`/`rom_report`.
  Config-D (`OEVM_HEAP_SIZE=49152 METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256`, 4k stack,
  512 mark) = **ROM 25.8 % / RAM 45.1 %** of the nRF52840, banked ad-hoc in
  `tuvm/zephyr/README.md` + `nsl-tests/00_perf/RESULTS.md`. Also **not gated**.
- On the 32-bit DK the arena is ≤ 48 KB, so all five `size_t` fields are ≪ 2²⁹ and fit in an
  SMI on both host and target — **no LargeInteger path needed**.

## 3. Strand A — `heapStats` prim (104) + nsl surface

### 3.1 C leaf prim 104 `heapStats`
- Append-only id **104**, platform-dispatched (same shape as the CBOR leaves 97–103;
  mirrored in `prim_ids.h`, counted — not computed — as the 104th `PRIM_NAMES` entry).
- No arguments. Calls `oevm_heap_stats()`, then builds **one** `indexable_new(5)` List and
  writes 5 SMIs into it: `[heapSize, liveBytes, highWater, largestFreeBlock, gcCycles]`
  (fixed order = the struct field order). `indexable_new(n)` layout: `slots[0]=count`,
  values in `slots[1..n]`.
- **GC-safety:** exactly one allocation (`indexable_new`); the struct is read into C locals
  *before* the alloc, and SMIs are immediates (no alloc), so there is no heap-read-after-alloc
  and no un-rooted oop across an allocation — the recurring hazard does not arise. No
  `gc_temp_root_push` needed (documented as such in the leaf comment).

### 3.2 nsl `HeapStats` value class (`kernel.ns`)
- Constructed `HeapStats fromList: (platform heapStats)`; stores the 5 values.
- Named accessors: `heapSize`, `liveBytes`, `highWater`, `largestFreeBlock`, `gcCycles`.
- `printString` → a compact one-liner, e.g. `live 12456 / hw 18240 / free 30112 / gc 3 / size 49152`.
- `platform heapStats` returns the raw List; the class names it. Mirrors the rung-3 pattern
  (raw C leaf + nsl class over it). A `Platform`/kernel convenience `heapStats` may return the
  `HeapStats` instance directly.

## 4. Strand B — layered footprint gate

### 4.1 CI ratchet (every-commit, host-only, gated)
- Extend the existing `FOOTPRINT_CONFIG=mcu` path in `tools/footprint.sh` with a **ratchet
  check**: bank baseline `text` and `data+bss` totals in a checked-in file
  `tuvm/tools/footprint-mcu-baseline.txt`; a regression beyond a small, documented tolerance
  **fails**, exactly as tuvm#5 requires ("a RAM regression is a gate failure like a text
  regression").
- Runs as a **named gate** in the standard suite (alongside `grant`, `portability-32`, …), so a
  RAM/text regression fails the same run everything else does. Requires the NCS SDK on PATH
  (`source zephyr/tools/env.sh`); when the cross toolchain is absent the gate **skips with a
  loud SKIP line** rather than passing silently.
- This is a **static `.o`-size** proxy (engine core only; `src/backend/*` excluded, measured in
  the west build instead) — deterministic and board-free.

### 4.2 Ground-truth reference (banked, DK-captured)
- Capture the real whole-image `ram_report`/`rom_report` from the config-D west build into a
  canonical `tuvm/zephyr/FOOTPRINT_DK.md`: RAM + ROM in bytes and as % of the nRF52840, the
  exact config-D knob set, the commit, and the date. Cross-linked from
  `nsl-tests/00_perf/RESULTS.md`.
- **Not** every-commit gated (needs SDK + board); it is the authoritative snapshot the §4.1
  host ratchet is a proxy for. Refreshed when the engine or config-D materially changes.

## 5. Strand C — DK capture + testing / acceptance

### 5.1 Automated both-path test (gated)
- `nsl-tests/NN_heap_stats/…` (number picked at implementation time) asserting **invariants**,
  not raw numbers (which vary run-to-run), so the `expect:` block is **deterministic across
  Path A and Path B**:
  - `heapSize > 0`
  - `highWater >= liveBytes`
  - `largestFreeBlock <= heapSize`
  - `gcCycles >= 0`
  Prints `true` per invariant; A and B must agree with `expect:` verbatim.
- Keeps the full both-path suite green (178 → **179** passed / 0 / 12, + the 4 named gates,
  now 5 with the footprint gate).

### 5.2 C coverage
- Light `tuvm/tests/test_heap_stats.c` (or an extension of an existing gc test): assert the
  struct→List field mapping (order + values) and the SMI-safety bound; run under
  `OEVM_GC_STRESS=1`. Registered in `run_c_tests.sh`.

### 5.3 DK acceptance (Task-8 style, manual bar)
- Flash **config-D** a measurement node: reuse **perfstex** (already runs to `done` on the DK;
  floatMath 215 ms) with a trailing `heapStats printString` emitted over VCOM (`/dev/ttyACM0`).
- Capture, under load, the real `highWater` + `largestFreeBlock` + `gcCycles`, and grab
  `ram_report`/`rom_report` from the same build. Bank all into `FOOTPRINT_DK.md`.
- This is the on-hardware exercise of prim 104. Acceptance = node runs to `done` and prints a
  well-formed `HeapStats` line with `highWater ≤ heapSize` and `largestFreeBlock ≤ heapSize`.

## 6. Repos, sequencing, footprint

Same leaf-prims-before-kernel-before-tests order as rung-3:

1. **`tuvm`** — prim 104 + `prim_ids.h`; `test_heap_stats.c`; footprint ratchet + baseline +
   named-gate wiring; `zephyr/FOOTPRINT_DK.md`; measurement-node change; DK capture.
2. **`nsl-tuvm`** — `HeapStats` class in `kernel.ns`.
3. **`nsl-tests`** — both-path invariant test.

Footprint delta banked per strand (rung vs master, clean per-branch regen), as in rung-3. The
new prim adds one `PRIM_NAMES` entry + one leaf body (small); the `HeapStats` class adds a
handful of `kernel.ns` lines (image/ROM only, no RAM/bss).

## 7. Acceptance summary (definition of done)

- Prim 104 `heapStats` lands, append-only, GC-safe; `HeapStats` nsl class wraps it.
- Both-path invariant test green on A and B; full suite **179/0/12**; C `test_heap_stats` green
  under `OEVM_GC_STRESS=1`.
- Footprint named gate active: MCU static ratchet fails on regression, skips loudly without the
  cross toolchain.
- `FOOTPRINT_DK.md` banked with real config-D `ram_report`/`rom_report` **and** a live
  under-load `HeapStats` readout, both captured from the plugged-in DK.
- Footprint deltas banked; docs (`FOOTPRINT_DK.md`, `RESULTS.md` cross-link, program-sequence if
  applicable) reconciled to as-built.
</content>
</invoke>
