# performance.nsl — an nsl column for the STIC ESP32 benchmark

**Status:** design, 2026-07-16. Approved for a first pass (96 KB heap, hanoi 8).
**Goal:** port the STIC benchmark to nsl so tuvm earns a fourth column beside Toit,
uLisp and CircuitPython in the published ESP32 table (`~/workspaceToit/stic/README.md`,
originally <https://ekorau.com/some-numbers/>).

Now possible because tuvm runs on ESP32 at all — see `tuvm/zephyr/docs/results.md`
(E1–E3) and the ladder's rung-2 entry. The Model B `.oimg` loop (44 s) is what makes
iterating on an on-target benchmark practical.

## 1. Why this is worth doing

The published table compares three VMs **on the same class of hardware**:

| Test | Toit | uLisp | CircuitPython |
| --: | --: | --: | --: |
| alloc | 16.0 | 21.6 | 11.4 |
| array write | 19.4 | 98.1 | 63.3 |
| dictionary write | 5.7 | 1.4 | 0.9 |
| float math | 41.1 | 124.3 | 20.9 |
| integer math | 9.4 | 122.5 | 14.4 |
| collection iterate | 12.7 | 42.5 | 29.2 |
| collection write | 9.5 | 6.7 | 15.4 |
| string compare | 1.5 | 12.5 | 2.9 |
| hanoi @ 8 | 0.0000 | 0.01 | 0.01 |

(seconds; Toit v2.0.0-alpha.180, uLisp v4.7, CircuitPython 9.2.7)

Extrapolating from E3 (perfstex on ESP32), tuvm looks **competitive on floats** and
well off on array write — i.e. the column is interesting, not embarrassing:

| test | Toit | tuvm (extrapolated from E3) |
|---|---:|---:|
| integer math | 9.4 s | ~18 s |
| float math | 41.1 s | **~37 s** |
| array write | 19.4 s | ~143 s |
| string compare | 1.5 s | ~37.6 s |

These are extrapolations, not results. The point of the work is to replace them.
`stringCompare` is the one to watch: Toit's `s1 == s2` on two identical literals is
likely an interned-pointer compare (1.5 s / 1M ≈ 1.5 µs), and tuvm has its own literal
dedup + `String=` identity fast path (`oevm#17`, `cee55d4`) — yet E3 extrapolates to
~25× slower. Either the fast path is not firing on this shape, or the two VMs are not
measuring the same thing. Worth understanding before the column is quoted.

## 2. Lineage — three files, and which one is authoritative for what

This matters more than it looks; getting it wrong silently invalidates the column.

- `~/workspaceToit/stic/performance.toit` — the Toit implementation. **Byte-identical**
  to `~/workspace/toit-zephyr/tests/performance.toit`. Defines
  `ITER_10K/100K/300K/1000K`.
- `~/workspaceToit/stic/performance.{py,lsp}` — CircuitPython and uLisp sources. Both
  declare the **same four constants with the same nesting**. Verified.
- `~/workspace/st-zephyr/samples/benchmark/performance.st` — a Smalltalk **sibling**, not
  the 90s original. Its comments already encode the Smalltalk↔Toit scale factors
  (`"100K × 10 ... (x3 toit: 300K × 10 = 3M)"`) and its `run` prints `alloc x1:`,
  `arrayWrite x10:`, `floatMath x3:`, `integerMath x3:`, `collectionIterate x10:`,
  `stringCompare x1:`.

**Therefore:**
- **Counts** come from `performance.toit`. All three published columns share them, so
  they define the table. The `.st`'s own 100K/10K counts do **not** match it, and
  porting the `.st` verbatim would produce numbers that silently belong to a different
  benchmark. (David: *"stic was a desktop benchmark, adjust the iterations to match
  performance.toit iterations."*)
- **Structure** comes from `performance.st` — Smalltalk→nsl is near-mechanical.
- **Semantics** come from `performance.toit`. See §4.

## 3. The nine tests

`performance.st` has only seven — it lacks `dictionaryWrite` and `collectionWrite`,
presumably because that Smalltalk lacked the collections. nsl's `lib/kernel.ns` has
`Dictionary` (line 282) and `OrderedCollection` (129), so **all nine are portable** and
the column can be complete.

| # | test | iterations (from Toit) | nsl construct |
|---|---|---|---|
| 1 | alloc | `ITER_100K` × 10 | `Array new: 10` |
| 2 | arrayWrite | `ITER_1000K` × 10 | `Array` + `at:put:` (10M writes) |
| 3 | dictionaryWrite | `ITER_10K` × 10 keys | fresh `Dictionary`, string keys |
| 4 | floatMath | `ITER_300K` × 10 | `e:: (e * a + b) * c + d` |
| 5 | integerMath | `ITER_300K` × 10 | same, SmallInteger→LargeInteger |
| 6 | collectionIterate | `ITER_100K` × 10 | `OrderedCollection` (20) `do:` |
| 7 | collectionWrite | `ITER_100K` × 10 | fresh `OrderedCollection`, `add:` |
| 8 | stringCompare | `ITER_100K` × 10 | `s1 = s2`, two equal long strings (1M compares) |
| 9 | hanoi | 8 disks | recursive `move:from:to:temp:` |

Collection mapping: Toit `List` (fixed size) → nsl `Array`; Toit `[]` (growable) → nsl
`OrderedCollection`; Toit `{:}` → nsl `Dictionary`.

## 4. The literal-array trap — the one real correctness hazard

`performance.st`'s `allocTest` and `arrayWrite` both use:

```smalltalk
a := #(0 0 0 0 0 0 0 0 0 0).
```

In Smalltalk `#(...)` is a **literal**: a compile-time constant, shared, **not allocated
per evaluation**. Ported line-for-line, `allocTest` would measure assignment and report a
spectacular, meaningless number — while Toit's `List 10 --initial=0` genuinely allocates
1M arrays. (`arrayWrite` additionally *mutates* a literal, which is illegal or
implementation-defined in many Smalltalks.)

**Decision: `performance.nsl` matches Toit's semantics, not the `.st`'s text.** Every
allocation is real (`Array new: 10`). Any place the `.st` and the `.toit` disagree about
*what is being measured*, the `.toit` wins — it is what produced the published numbers.

## 5. Shape

Mirrors `nsl-tests/00_perf/perfstex.ns`, which is already proven to boot unmodified on
ESP32 (E3) — no structural risk:

```
class PerformanceBench packageUsing: manifest = () (
  public class Performance usingPlatform: platform = () (
     public alloc = ( ... ^ t1 - t0 )
     ... nine tests, each self-timed with `0 millis` ...
  )
  public main: platform args: args = (
     ('alloc: ' , p alloc printString) out.
     ...
     ('done') out.
  )
) : ( )
```

`public main: platform args:` is the node entry the Model B loader bootstraps
(`oimg_bootstrap(oimg_root())`), so it boots with no wrapper.

**Units.** Tests are timed with `0 millis` (`k_uptime_get()`, 1 ms tick) and return ms.
The published table is in **seconds**, so ms are converted at report time. At these
iteration counts every test runs for seconds, so the 1 ms tick is immaterial — unlike
perfstex, where `integerMath: 30` carried ~3% quantisation.

**THREE RUNS, MEANED — non-negotiable for comparability.** `performance.toit`'s `main`
does not report a single run. It runs the whole suite **three times, each with a fresh
`SpeedTester`**, and reports the arithmetic mean:

```toit
one := tester.run          # fresh SpeedTester each time
two := tester.run
three := tester.run
results[i] = (one[i] + two[i] + three[i]) / 3.0
```

The published Toit/uLisp/CircuitPython numbers are therefore **means of three**. A single
nsl run would be a *different statistic* and not comparable no matter how faithful the
tests are. `performance.nsl` runs three times with a fresh `Performance` each time and
reports the mean, formatted to one decimal in seconds, matching Toit's `%.1f`.

**Deviation, deliberate:** Toit prints only the final mean. `performance.nsl` prints each
run's nine values **as that run completes**, then the mean. Same measurement, but a
crash or truncated capture in run 3 does not destroy runs 1–2 — worth it when a full
suite is ~7–9 min and the whole job is ~21–27 min. It also yields per-run spread for
free, which the published table does not have.

Toit also prints `system.process-stats --gc` full-GC counts between runs. nsl's
`platform heapStats` exposes `gcCycles` (`kernel.ns:1000`), so the same can be reported
between runs — useful given the alloc row is GC-bound, and free to include.

**Location:** `~/workspace/nsl-tests/00_perf/performance.nsl`, beside `perfstex.ns`. It
is an nsl test program, not tuvm code. This makes the work cross-repo: benchmark in
`nsl-tests`, results in `tuvm/zephyr/docs/results.md` — exactly how perfstex already
spans the two.

## 6. Config for the first pass

**Heap: 96 KB** (`OEVM_HEAP_SIZE=98304`), and this ceiling is not arbitrary. The arena is
a *static* array inside `dram0`; the libc heap is the leftover, and the float slabs
malloc from it:

- `dram0` = 192 KB; currently 135,024 B used with a 64 KB arena → base ≈ 71 KB
- libc heap ≈ 60 KB today; float slabs need ≥ ~8.3 KB (512 cells × 16 B + markbits)
- so `heap ≲ 105 KB` before `floatMath` starts aborting again (§4.1 of results.md)

96 KB leaves ~28 KB of libc heap — comfortably above the slab. **Verify empirically**
rather than assume: if `floatMath` aborts, the heap is too big, not the arena too small.

All seven knobs, per `results.md` §4.1:

```
OEVM_HEAP_SIZE=98304 METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256
VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512
OEVM_FLOAT_ARENA_SLAB_CELLS=512 OEVM_FLOAT_ARENA_SLAB_BUDGET=4
```

Model B, `prj_esp32.conf`, `esp32_devkitc/esp32/procpu`, board as attached
(ESP32-D0WDQ6 rev v1.0 in a WROOM-32 module, 240 MHz, unicore).

## 7. Verification — a free correctness gate

Four tests overlap perfstex E3 at known counts, so the port can be checked **before any
number is quoted**:

| test | E3 (perfstex) | expected scale | predicted |
|---|---|---|---|
| integerMath | 30 ms @ 5K evals | ×600 | ~18 s |
| floatMath | 62 ms @ 5K evals | ×600 | ~37 s |
| arrayWrite | 1431 ms @ 100K writes | ×100 | ~143 s |
| stringCompare | 376 ms @ 10K compares | ×100 (1M) | ~37.6 s |

If a test misses its prediction by more than ~2×, **the port is wrong or the counts are
wrong** — investigate before reporting. A too-fast result is the dangerous one: it means
work is being optimised away (the literal-array failure mode of §4).

## 8. Risks

- **alloc on any heap** — 1M live-then-dead 10-element arrays is heavy GC churn. It may
  be slow or thrash. That is partly the experiment; it is also why 96 KB is the starting
  point rather than 64 KB.
- **`Dictionary` performance is entirely unknown** — never exercised on target. It is the
  one row with no perfstex precedent and no prediction in §7.
- **`integerMath` overflows into `LargeInteger`** (boxed, allocating), as perfstex's does.
  Both tuvm and Toit wrap at 64-bit, so the semantics match, but tuvm boxes.
- **Runtime is the big one: ~21–27 min.** `arrayWrite` alone ≈ 143 s and `stringCompare`
  ≈ 38 s; a suite is ~7–9 min, and the mean-of-three (§5) triples it. The console is
  ~520 B/s (`results.md` §5) so the capture window must be generous — budget ≥ 30 min.
  Printing each run as it completes (§5) is the mitigation: a truncated capture costs one
  run, not the job.
- **`run.sh` cannot drive out-of-tree nodes** (`--node` resolves under `zephyr/`), so the
  first pass uses the manual recipe in `results.md` §5.

## 9. Out of scope for the first pass

- **The 64 KB column.** Decide after seeing 96 KB. (The two-column GC-sensitivity study
  is still wanted; it is deferred, not dropped.)
- **hanoi at 16/20/21/22/24.** Depth 8 only. Deeper rows also probe VM stack depth, which
  is its own question given the derecursify work.
- **Publishing.** First pass is banked internally as **E4** in
  `tuvm/zephyr/docs/results.md`. David: *"I simply will not push performance_stic until
  ready."*
- **Fixing `run.sh --node` to take a path.** Wanted, but separable.

## 10. Open questions, deliberately parked

- **Which board produced the Toit numbers.** The README says *"DevKitC V4 WROOM-32E"*,
  but a WROOM-32E carries an ESP32-D0WD-V3 reporting **revision v3.0**, while the
  attached board is **D0WDQ6 revision v1.0**. Either the Toit board was a different,
  newer module or the README mislabels it. Perf impact is small (both 240 MHz LX6; rev3
  is ECO fixes, not speed), but the claim "same silicon" is not yet earned. **Plan: rerun
  Toit and nsl on a definitely-identical board later**, which removes the confound
  outright rather than footnoting it.
- **RTOS confound.** Toit/uLisp/CircuitPython run on ESP-IDF/FreeRTOS; tuvm runs on
  Zephyr. This cannot be eliminated by re-running on one board and must be disclosed
  whenever the column is published.
