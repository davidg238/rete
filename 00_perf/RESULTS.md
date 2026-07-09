# oevm vs PrimordialSoup — performance comparison

## fib(32) — pure dispatch + SmallInt arithmetic

`fib.ns` — recursive `fib: 32` (= 2178309), 7,049,155 calls. Identical source
compiled for each VM. oevm built `gcc -O2`; Soup is ReleaseHost.

| VM | fib(32) wall | ns/call | notes |
|---|---|---|---|
| PrimordialSoup | ~140 ms | ~20 | reference |
| oevm (-O2), 2026-06-15 baseline | 4149 ms | 588 | **~30× slower** |
| oevm (-O2), 2026-06-17 | ~332 ms | ~47 | **~2.4× slower** |

The block-inlining + dispatch optimizations (see `oevm/docs/optimizations.md`)
closed the fib gap from ~30× to ~2.4×.

## Steady-state comparison (2026-06-17) — the honest headline

The conformance benchmarks run `innerIterations: 1`, so the small ones finish in
1–5 ms and are dominated by process startup, not VM speed. To get a true
interpreter-vs-interpreter ratio, each benchmark's verified kernel is run K times
(the standard are-we-fast-yet inner loop) so it does ≥75 ms of real work. Scale
factors: queens ×100, sieve ×150, towers ×80, nbody innerIterations 30000,
richards ×8, mandelbrot size 750 (already ~2.8 s), fib(34). Median of 5,
subprocess wall, oevm `gcc -O2`, Soup ReleaseHost.

| Benchmark | Soup | oevm | oevm / Soup |
|---|---|---|---|
| richards | 367 ms | 433 ms | **1.18×** |
| towers | 161 ms | 267 ms | 1.66× |
| queens | 92 ms | 163 ms | 1.76× |
| sieve | 76 ms | 145 ms | 1.91× |
| mandelbrot | 2811 ms | 6288 ms | 2.24× |
| fib(34) | 366 ms | 938 ms | 2.57× |
| nbody | 217 ms | 573 ms | 2.64× |

**Geomean ≈ 1.9× slower than Soup.**

This *corrects* the single-iteration table (`../benchmark_comparison.md`, from
`../run_benchmarks.py`): there oevm looked at parity or faster on queens /
large_class / many_locals, but that was a **startup artifact** — oevm has no
snapshot-deserialization step, so its faster startup masked slower steady-state
compute on the 1–5 ms runs. At matched workload oevm is uniformly slower
(1.2×–2.6×). The single-iteration table still reflects the *as-shipped
conformance workload*; this table reflects *VM speed*.

The worst ratios are the two that lean hardest on the unbuilt optimizations:
fib/nbody (~2.6×) hammer dispatch (no inline cache) and float allocation (no
unboxed doubles). richards (1.18×) is closest because it is allocation/branch
heavy rather than arithmetic heavy. (deltablue and deep_nesting still EXEC_ERR
on oevm — not yet passing.)

## Global method cache — before/after + cache stats (2026-06-17)

Task 3 (`feat/global-method-cache`, commit e940e4a) wired a 4096-entry
direct-mapped cache in front of `lookup_defining_vtable`. Task 4 adds a gated
`-DOEVM_CACHE_STATS` shutdown report and records the numbers below.

### Before / after timing (best of 5, `-O2 -DOEVM_APPLIANCE`)

| Benchmark | BEFORE (30db3db) | AFTER (e940e4a+) | speedup |
|---|---|---|---|
| fib(32) | 0.45 s | 0.42 s | **1.07×** |
| richards | 0.06 s | 0.05 s | **1.20×** |
| deltablue | did not run (EXEC_ERR) | did not run (EXEC_ERR) | — |

### Cache stats per workload (`-DOEVM_CACHE_STATS`, single run)

| Benchmark | hits | misses | fills | collisions | flushes | hit_rate |
|---|---|---|---|---|---|---|
| fib(32) | 7 049 154 | 4 | 4 | 0 | 0 | **100.0%** |
| richards | 940 257 | 96 992 | 96 992 | 96 934 | 0 | **90.6%** |

### Verdict: single-table interference

**fib**: 100% hit rate, 0 collisions — cache is nearly ideal for this
pure-dispatch workload (only 4 distinct (vtable, selector) pairs; all fit
without eviction).

**richards**: 90.6% hit rate is good, but collisions (96 934 out of 96 992
fills ≈ 99.9% of fills evict something) shows the 4096-entry table sees heavy
hash collisions on this workload. However, the 10% miss rate is still producing
a useful cache — the 1.20× speedup is real even with heavy collision pressure.
The single-shared-table concern is therefore **partially real for richards-class
workloads** (diverse vtable/selector pairs thrash a 4096-entry direct-mapped
cache), but not a blocking issue at current table size: the speedup is positive
and the miss rate is bounded. Increasing to 8192 or switching to 2-way
associativity is a viable next lever if richards becomes the bottleneck.

**Flushes = 0 for both benchmarks** — neither fib nor richards calls `bind:to:`
after boot, so the cache is never invalidated mid-run. Flush frequency is a
non-issue for these workloads.

## Remaining perf levers

- **Inline caching is NOT enabled.** The VM has the machinery (`struct
  InlineCache`, `send_ic`, `OP_SEND_IC` handler in `vm.c`), but the ns-oevm
  compiler never emits `OP_SEND_IC` — every call site emits plain `OP_SEND`
  and does a full vtable lookup. Wiring up the (currently monomorphic) inline
  cache is the largest untapped lever, especially for fib/richards.
- **Boxed floats.** nbody/mandelbrot allocate a `Float` per arithmetic op;
  unboxed or tagged doubles would cut the float gap.

---

## 2026-06-18 — sober reference refresh: Model A vs Model B vs Soup (post-float-arena)

Re-measured after conformance closed (suite 47/0/1) and the float arena shipped.
Three configs, **both oevm paths built `gcc -O2`** for a fair throughput compare:

- **oevm Model A** — image baked into the binary (`-DOEVM_APPLIANCE`); gcc
  optimizes VM + image together. The appliance build.
- **oevm Model B** — resident image VM (`/tmp/oevm_img`: `vm.c gc.c dispatch.c
  oimg_loader.c kernel_boot.c oevm_image_main.c`) + a separate read-only `.oimg`
  blob, executed in place. **This is the OTA-deployable build for Zephyr.**
- **PrimordialSoup** — `out/ReleaseHost/primordialsoup`, the reference peer.

Both oevm "models" are the **same bytecode interpreter** (`switch(op)` in
`vm.c:181`, no JIT / no native codegen). A vs B is *image packaging only*, not
compiled-vs-interpreted.

### Two methodologies — don't conflate them

| Methodology | A/Soup | B/Soup | What it measures |
|---|---|---|---|
| Single-iteration (`run_benchmarks.py`, conformance workload) | 1.06× | 1.30× | **startup-dominated** for the small benches — oevm has no snapshot-deserialize step, so its fast startup flatters it. NOT VM speed. |
| Steady-state (scaled to ~1–8 s, startup negligible) | **1.84×** | **2.07×** | true interpreter throughput |

### Steady-state throughput (median of 3, desktop x86-64, scaled so wall ≈ compute)

| Benchmark | Soup | oevm A | oevm B | A/Soup | B/Soup | B/A |
|---|---|---|---|---|---|---|
| queens (×500) | 0.45 s | 0.95 s | 1.09 s | 2.11× | 2.42× | 1.15× |
| sieve (×700) | 0.33 s | 0.74 s | 0.84 s | 2.24× | 2.54× | 1.13× |
| towers (×400) | 0.77 s | 1.57 s | 1.85 s | 2.04× | 2.40× | 1.18× |
| mandelbrot (@750) | 5.57 s | 8.19 s | 8.41 s | 1.47× | 1.51× | 1.03× |
| nbody (×150) | 1.04 s | 2.20 s | 2.50 s | 2.12× | 2.41× | 1.14× |
| richards (×30) | 1.30 s | 2.01 s | 2.30 s | 1.55× | 1.77× | 1.14× |
| deltablue (×400) | 0.29 s | 0.45 s | 0.50 s | 1.53× | 1.73× | 1.13× |
| **geomean** | | | | **1.84×** | **2.07×** | **1.13×** |

**Model A vs Model B:** B costs **~13% throughput** at steady-state — 3% on
compute-heavy mandelbrot, up to 18% on object/field-heavy benches
(queens/towers), where B's execute-in-place field-read indirection bites. Plus a
fixed ~1 ms `.oimg`-load startup (only matters for sub-second tasks). Memory
identical. Shipping the OTA-friendly Model B costs ~13% — a sound trade.

**vs Soup:** ~1.84× (A) / 2.07× (B) slower, steady-state. This *confirms* the
~1.9× figure in the steady-state table above; the float arena pulled mandelbrot
2.24→1.47× and nbody down, but the geomean barely moved because the non-float
benches (queens/sieve/towers ~2.1×) dominate and the arena doesn't touch them.

### ⚠️ These oevm numbers include NO GC penalty

**GC never triggers (GH #3).** Every float op bump-allocates a `Float` cell that
is never collected, so the heap grows unbounded. Peak RSS under sustained load:

| Benchmark | Soup (has GC) | oevm (no GC) |
|---|---|---|
| mandelbrot (@750, ran 2×) | 6 MB | **4,558 MB** |
| nbody (×150) | ~6 MB | **749 MB** |
| sieve (×700) | ~5 MB | 29 MB (and climbing) |
| queens/towers/richards | 4–6 MB | 2 MB |

Consequences: (1) on an nRF52840 (~256 KB RAM) **any sustained program OOMs**,
regardless of speed — GC (GH #3) gates the Zephyr target, not throughput; (2)
**the throughput numbers above will regress once GC lands** — they currently pay
zero collection cost. Treat 1.84×/2.07× as a *no-GC* upper bound.

### VM footprint — Soup vs oevm Model B (the embedded-fit metric)

VM source only (each VM's compiler is a separate host-side toolchain — Soup's
`CompilerApp.vfuel`, oevm's `ns_compile_oevm.py` — and is excluded). Both
binaries dynamically linked (libc/libm), x86-64.

LOC counted *authoritatively* for the Linux build: Soup selects platform code
via `#if defined(OS_LINUX)` guards *inside* each `.cc` (the SConstruct compiles
every `vm_ccs` file unconditionally), so the dead variants
(`os_macos`/`thread_win`/`message_loop_kqueue`/…) resolve to zero. Active Linux
set = neutral core + `os_linux`/`thread_linux`/`message_loop_epoll`/
`virtual_memory_posix` + vendored `double-conversion/`.

| Metric | Soup VM | oevm Model B VM | ratio |
|---|---|---|---|
| source files (own VM, Linux-active) | 46 (19 `.cc` + 27 `.h`) | **8** (6 `.c` + 2 `.h`) | 5.8× |
| LOC (own VM, Linux-active) | 15,664 | **3,369** | 4.6× |
| + vendored double-conversion | +20 files / +6,776 LOC | — | |
| **total LOC compiled on Linux** | **22,440** (66 files) | **3,369** (8 files) | **6.7×** |
| executable, stripped | 215 KB | **60 KB** | 3.6× |
| executable, unstripped | 3.08 MB (debug_info) | 68 KB | — |
| `.text` (code) segment | ~205 KB | **~48 KB** | 4.3× |

oevm Model B is **4.6× smaller than Soup's own VM code** (6.7× counting the
double-conversion float-printing library Soup vendors and oevm does without), and
3.6× smaller as a stripped binary. (The 3 MB Soup file is almost all
`debug_info`; stripped it is 215 KB — that's the fair comparison.) Each VM's
*compiler* is a separate host-side toolchain (Soup's `CompilerApp.vfuel`, oevm's
`ns_compile_oevm.py`) and is excluded from both. These are x86-64 host builds;
absolute sizes differ on Cortex-M but the ratio is indicative.

### Reproduce

`/tmp/sober_bench.py` (natural workload + RSS) and `/tmp/steady_bench.py`
(iteration-scaled + RSS). Model B VM built once with
`gcc -O2 -std=c99 vm.c gc.c dispatch.c oimg_loader.c kernel_boot.c
oevm_image_main.c -o /tmp/oevm_img -lm` after `dump_layout > /tmp/oevm_layout.json`.

---

## 2026-07-08 — Perfstex baseline: **tuvm** VM (post-Zephyr-pulse-primitive)

First perfstex baseline for the current **tuvm** VM generation (the fork that
carries the Zephyr rung-2 pulse primitive + M6 handle table — all additive, off
the perfstex hot paths, so this is a clean VM baseline, not a measure of those
changes). `perfstex.ns` is the classic Smalltalk SpeedTester port, self-timed via
the `millis` primitive. Comparison columns: the prior **ns-oevm** VM's M3 device
run and the published **Berry** numbers (`st-zephyr/.../RESULTS.md`, 2026-04-11),
all on the **same nRF52840 @ 64 MHz Cortex-M4**, identical iteration counts.

### On-device — nRF52840-DK (Model A, single run, console over J-Link VCOM)

| Sub-test       | Iterations | Berry (ms) | ns-oevm (ms) | **tuvm (ms)** | tuvm ÷ Berry |
|----------------|-----------:|-----------:|-------------:|--------------:|-------------:|
| integerMath    |      1,000 |        118 |           95 |        **87** |    **0.74×** |
| floatMath      |      1,000 |        141 |          284 |     **OOM***  |          —   |
| arrayWrite     |     10,000 |      3,611 |        4,751 |     **5,128** |        1.42× |
| arrayIterate   |      1,000 |      2,764 |        3,407 |     **3,920** |        1.42× |
| stringCompare  |     10,000 |        460 |        1,609 |     **1,215** |        2.64× |
| hanoi(18)      |    262,143 |     15,837 |       14,719 |    **15,154** |    **0.96×** |

Footprint (96 KB arena build): FLASH 25.8%, RAM 86.3% (226.2 KB / 256 KB).

**Reading it.** tuvm is **competitive-to-faster on integer dispatch + SmallInt
arithmetic** — integerMath 0.74× Berry (and faster than ns-oevm's 95 ms), hanoi(18)
0.96× (deep method-send recursion, essentially at parity). stringCompare improved
markedly over ns-oevm (1,215 vs 1,609 ms) but is still 2.6× Berry — the clearest
non-float optimization target. Array ops are ~1.4× Berry (slightly behind
ns-oevm). Net: on non-float work tuvm is within ~1.4× of Berry and ahead of it on
the two dispatch-bound tests.

**\*floatMath OOMs on device (`VM Float Arena Error: Out of Memory`).** The boxed-
Float arena grows **unbounded** — raising the slab budget (4→6) and the newlib pool
(48→64 KB) changed nothing, so it is a **reclaim** gap, not a sizing one: dead
floats (floatMath reassigns its accumulator every iteration) are not collected, so
its ~20k allocations exceed any arena that fits in 256 KB. This is the on-device
face of the documented boxed-float limitation (the "no GC penalty / floats never
collected" caveat above, GH #3). It runs fine on the **host** only because the
16 MB default arena never triggers collection. ns-oevm's M3 recorded floatMath 284
ms, so bounded-memory float execution is achievable — **follow-up: make the float
arena reclaim under the slab-budget cap** so float-heavy programs run on the DK.
(To get the other five numbers, floatMath was moved last in a scratch
`perfstex_dk.ns` so its abort didn't truncate the run.)

### On-host — x86-64 desktop (Model A, `gcc -O2`, best-of-10)

| Sub-test | integerMath | floatMath | arrayWrite | arrayIterate | stringCompare | hanoi18 |
|----------|------------:|----------:|-----------:|-------------:|--------------:|--------:|
| tuvm host (ms) | 0–1 | 0–1 | 6 | 5 | 1–2 | 20 |

Sanity/regression check only: at the **device-calibrated** iteration counts, a
~GHz desktop collapses the quick tests to 0–1 ms (below ms resolution), so the host
run is not a meaningful per-test baseline — it just confirms the VM runs perfstex
correctly and, notably, floatMath completes on host (big default arena, no
collection). The device column above is the Berry-comparable baseline.

### Reproduce

- **Host:** compile `perfstex.ns` → `tuvm/src/solidified_image.c` with
  `nsl-tuvm/tools/ns_compile_oevm.py`, build the Model A appliance
  (`gcc -O2 -DOEVM_APPLIANCE -std=c99 -Isrc <engine .c set> src/oevm_main.c -o
  oevm_bench -lm`), run `./oevm_bench` (self-timed stdout). Restore
  `solidified_image.c` after (`git checkout --`).
- **Device (nRF52840-DK):** reuse the Task-8 DK build (`tuvm/zephyr`,
  `-DCONF_FILE=prj_dk.conf`) — `main.c` already wires `oevm_millis = k_uptime_get`.
  Bake `perfstex.ns`, `west build -b nrf52840dk/nrf52840`, `west flash
  --skip-rebuild --runner jlink`, capture `/dev/ttyACM0` @ 115200. Env for the
  float-heavy build: `OEVM_HEAP_SIZE=98304 METHOD_CACHE_SIZE=512 PRIM_CACHE_SIZE=512
  OEVM_FLOAT_ARENA_SLAB_CELLS=512 OEVM_FLOAT_ARENA_SLAB_BUDGET=6` +
  `CONFIG_HEAP_MEM_POOL_SIZE=65536` (the `zephyr/CMakeLists.txt` env→`-D`
  passthrough forwards the float-arena knobs).
