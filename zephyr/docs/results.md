# tuvm on-device results — ESP32

On-silicon milestones for the ESP32 port. Shape deliberately mirrors
`ns-zephyr/docs/results.md` (the nRF52840 predecessor's ledger) so the two read
side by side; `E` numbers here, `M`/`R` numbers there.

Board: **ESP32-D0WDQ6 / WROOM-32 devkit, rev v1.0 @ 240 MHz**, `/dev/ttyUSB0` (CP210x),
`esp32_devkitc/esp32/procpu`, Zephyr **4.2.0** upstream via the `~/zephyr-esp32`
workspace (not NCS — ESP32 is absent from NCS's manifest). Config `prj_esp32.conf`.

Status/progress lives in the ladder (`tuvm/review/program-sequence.md`, Phase 2 rung 2),
not here. This file is measurements + the evidence behind them.

| Milestone | Program | Heap cap | Expected | Result | RAM (build report) |
|-----------|---------|---------:|----------|--------|--------------------|
| E1 | `zephyr/TimerNode.nsl` (**Model A**, solidified) | 64 KB | `TIMER OK`, then alive | PASS (2026-07-16) | not captured † |
| E2 | `zephyr/TimerNode.nsl` (**Model B**, `.oimg`) | 64 KB | `TIMER OK`, then alive | PASS (2026-07-16) | 135024 B / 192 KB (68.68%) |
| E3 | `nsl-tests/00_perf/perfstex.nsl` (6 sub-tests, **Model B**) | 64 KB | six timings + `done` | PASS (2026-07-16) | 135024 B / 192 KB (68.68%) |
| E4 | `nsl-tests/00_perf/performance.nsl` (STIC, 9 tests, **Model B**) | **96 KB** | nine timings + `done` | PASS (2026-07-16) | 168816 B / 192 KB (85.86%) |
| E5 | `nsl-tests/00_perf/blockcost.nsl` (7 sub-tests, **Model B**) | 96 KB | seven timings + `done` | PASS (2026-07-16) | 168816 B / 192 KB (85.86%) |
| E6 | blockcost + STIC on branch `perf/activation-plus-ops` (**Model B**) | 96 KB | same, faster | PASS (2026-07-17) | 168816 B / 192 KB (85.86%) |
| E7 | `nsl-tests/00_perf/richards.nsl` (3 runs, **Model B**, baseline AND branch) | 96 KB | 3 timings + `verified: true` | PASS (2026-07-17) | 168816 B / 192 KB (85.86%) |
| E8 | richards + blockcost on `perf/iram-interp` knobs (IRAM / NODBG / both) | 96 KB | same, production config | PASS (2026-07-17) | 168816 B dram; IRAM 16.6→20.1% |
| E9 | `nsl-tests/00_perf/deltablue.nsl` (n=50, 3 runs, **Model B**) | **112 KB** | 3 timings + `verified: true` | PASS (2026-07-17) | 185712 B / 192 KB (94.46%) |

† E1's green run predates the Model A/B knob and its build was never re-run under
plain `prj_esp32.conf` — the only surviving Model A elf carries the `coredump.conf`
overlay, so quoting its RAM would misreport the shipping config. The **program**
evidence is the captured log, not a build report. Re-running Model A costs ~50 min and
~4.9 GB (see §4.2); it has not been judged worth it, since Model B supersedes it.

Green logs for all three: `tuvm/zephyr/esp32-debug-artifacts/`.

---

## E1 — TimerNode on ESP32 (Model A): rung-2 gate 1

The real on-target `k_timer` ISR path (`timer_zephyr.c`), no radio, no GPIO. Green after
**two stacked bugs** — the ROM `fflush` fault (`596b208`) and, once that was unmasked, an
uncancelled `every: 200` repeater filling the 16-deep pulse mailbox (`883cdc5`). The full
three-state timeline is on the ladder; the short version is
`dead 0.891s → dead 4.290s → ALIVE at 20s`.
Log: `esp32-debug-artifacts/tuvm-esp32-GREEN-both-fixes.log`.

## E2 — TimerNode on ESP32 (Model B `.oimg`): the build knob's gate

Same node, same output, via the **Model B** path: a generic VM (`oimg_loader.c` +
`kernel_boot.c`) plus the program as a `.oimg` blob `.incbin`'d into flash `.rodata`
(`zephyr/blob.S`), with **no `src/solidified_image.c` in the link**. This is the
`ENG_A`/`ENG_B` split `tools/run_c_tests.sh:28-29` has always run on the host, ported to
Zephyr behind `-DTUVM_MODEL=A|B`.

Verified in the ELF: `tuvm_blob_start=0x3f400884`, `tuvm_blob_end=0x3f41b31c`, span
**109208 B** — exactly the `.oimg` size — both in **DROM** (flash-mapped `.rodata`,
XIP'd in place, never copied to RAM), 4-aligned per `blob.S`'s `.balign 4`.
`run_solidified_entry_point`/`init_solidified_vtables` are absent from the image.

**Why this milestone exists — the loop, not the node:**

| | Model A | Model B |
|---|---:|---:|
| whole pristine loop (bake→build→flash→capture) | ~50 min, **OOM** | **44.0 s** |
| pristine build alone | ~46 min | **11.6 s** |
| `.incbin` of the blob (real Xtensa `as`) | — | **0.007 s** |

Log: `esp32-debug-artifacts/tuvm-esp32-GREEN-modelB-oimg.log`.
Recipe: `zephyr/run.sh --model B --node TimerNode.nsl --flash --capture 20`.

**A and B are not byte-identical on the console — B is cleaner.** Both print `TIMER OK`
and sit alive, but Model A also emits 48 lines of the non-fatal ROM-`setjmp`
`<err> os: ** SYSCALL` register-window spill dumps (PC `0x4005626f`); **Model B emits
zero**. B never enters the ROM `setjmp` path. That is new information about the ladder's
own "setjmp soundness on Xtensa unverified" loose thread — the noise is Model-A-specific,
not intrinsic to the VM on this part. Not investigated; do not assume benign.

## E3 — Perfstex on ESP32 (Model B): first on-target benchmark

The classic Smalltalk SpeedTester, ported to OEVM/Newspeak
(`nsl-tests/00_perf/perfstex.nsl` — already a node, `public main: platform args:`, so it
boots unmodified). Six sub-tests + `done`:

```
integerMath: 30      floatMath: 62       arrayWrite: 1431
arrayIterate: 1026   stringCompare: 376  hanoi18: 4248      done
```

### ESP32 vs nRF52840 — same VM, different silicon

The honest comparison is OEVM-on-ESP32 against OEVM-on-nRF52840 (`ns-zephyr` **M3**,
PASS 2026-06-28): identical VM, identical iteration counts, so this measures **the chip**,
not the implementation. Clock ratio **240 MHz ÷ 64 MHz = 3.75×** is the number to beat.

| Sub-test | Iterations | OEVM/nRF52840 (ms) | **OEVM/ESP32 (ms)** | speedup |
|---------------|-----------:|-----------:|----------:|--------:|
| integerMath   |      1,000 |         95 |    **30** |   3.17× |
| floatMath     |      1,000 |        284 |    **62** | **4.58×** |
| arrayWrite    |     10,000 |      4,751 | **1,431** |   3.32× |
| arrayIterate  |      1,000 |      3,407 | **1,026** |   3.32× |
| stringCompare |     10,000 |    1,163 † |   **376** |   3.09× |
| hanoi(18)     |    262,143 |     14,719 | **4,248** |   3.46× |

† the **post-`oevm#17`** nRF figure (identity fast path in `String=`, `oevm cee55d4`),
not the 1,609 in M3's own table. tuvm carries that fix, so quoting M3's pre-fix number
would flatter the ESP32 by ~40%.

**Reading it.** Five of six land at **3.09–3.46×**, consistently *just under* the 3.75×
clock ratio — expected for a part executing XIP out of external flash through a cache,
against the nRF's tightly-coupled internal flash. The tightness of that spread is itself
the signal: nothing is pathological, and the VM scales with the clock.

**`floatMath`'s 4.58× is above the clock ratio and is NOT explained — do not quote it**
until someone separates the candidates. It is *not* "the ESP32 has an FPU": the LX6 and
the Cortex-M4F FPUs are both single-precision, so **both parts emulate `double` in
software**. That leaves soft-float codegen and/or the memory system — and `floatMath` is
also the one sub-test whose working set is the float arena (§4.1), a confound to rule out
first.

**Berry is deliberately absent.** M3 ran a Berry head-to-head, but those numbers are
nRF52840 silicon. Placing them beside an ESP32 column would measure the chip and report
it as a VM difference. For a Berry comparison on this part, Berry must run on this part.

### Footprint

| Region | Bytes | % of ESP32 |
| ------ | ----- | ---------- |
| FLASH  | 294,684 | 7.03 % (of 4 MB) |
| dram0  | 135,024 | 68.68 % (of 192 KB) |

M3's nRF52840 build was 216,584 B / 256 KB (**82.6%**) for comparison. The blob is
**112,832 B** of `.oimg` in DROM — XIP'd from flash, never copied to SRAM. That is the
point of Model B: **the program costs flash, not RAM.**

## E4 — STIC on ESP32 (Model B): the first cross-VM column

The STIC benchmark (Smalltalk Industry Council, late '90s), ported to nsl as
`nsl-tests/00_perf/performance.nsl` (`0e0715a`). Design + lineage:
`tuvm/docs/design/2026-07-16-performance-nsl-stic-column-design.md`. Log:
`esp32-debug-artifacts/tuvm-esp32-STIC-96k-run1.log`. Ran clean in **14.9 min** — no
fault, no arena abort, no oops.

Reference columns are the published table in `~/workspaceToit/stic/README.md`
(<https://ekorau.com/some-numbers/>): Toit v2.0.0-alpha.180, uLisp v4.7, CircuitPython
9.2.7. **Seconds; smaller is better.**

| Test | Toit | uLisp | CircuitPython | **tuvm/nsl** | rank |
| --- | ---: | ---: | ---: | ---: | :--: |
| alloc | 16.0 | 21.6 | **11.4** | 24.1 | 4th |
| array write | **19.4** | 98.1 | 63.3 | 138.9 | 4th |
| dictionary write | 5.7 | 1.4 | **0.9** | 43.3 | 4th |
| float math | 41.1 | 124.3 | **20.9** | 35.1 | 2nd |
| integer math | **9.4** | 122.5 | 14.4 | 15.3 | 3rd |
| collection iterate | **12.7** | 42.5 | 29.2 | 532.6 | 4th |
| collection write | 9.5 | **6.7** | 15.4 | 97.0 | 4th |
| string compare | **1.5** | 12.5 | 2.9 | 5.2 | 3rd |
| hanoi @ 8 | **0.0000** | 0.01 | 0.01 | 0.004 | 2nd |

### Read it honestly

**tuvm is last on 5 of 9 and wins nothing.** That is the result. Do not quote "beats
Toit on float math" without the rest of the row: it does (35.1 vs 41.1) — and
CircuitPython does it in 20.9.

The *shape*, however, is consistent and diagnostic rather than diffuse:

- **Competitive on raw compute** — float 2nd, integer 3rd (level with CircuitPython),
  hanoi 2nd. Notable because tuvm **boxes every float** (`struct Float`, 16 B on Xtensa)
  and `integerMath` overflows into boxed `LargeInteger`, so it is allocating throughout
  and still keeping pace.
- **Badly behind wherever collections or blocks appear** — collectionIterate 41.9×,
  collectionWrite 10.2×, dictionaryWrite 7.6×, arrayWrite 7.2× (all vs Toit).

### The one number that matters: 26.6 µs per block activation

`collectionIterate` does 20M block evaluations in 532.6 s = **26.6 µs each**. Against
`integerMath`'s 3M four-op chains in 15.3 s = **5.1 µs per chain**: *a single block
activation costs ~5× a complete four-operation arithmetic chain.*

`kernel.nsl:132` shows why it cannot be optimised away at the call site:

```smalltalk
public do: aBlock = ( 1 to: count do: [ :i | aBlock value: (contents at: i) ]. ^ self )
```

The outer `to:do:` is compiler-inlined; `aBlock value:` **cannot** be — `aBlock` is a
parameter holding a real block object. oevm's literal-block inlining (which took fib from
~30× to ~2.4×, `nsl-tests/00_perf/RESULTS.md`) only fires on blocks literal at the send
site, so it never reaches this one. **This is a VM finding.** Every `do:`/`collect:`/
`select:` in `kernel.nsl:54-61` pays it.

`dictionaryWrite` and `collectionWrite` are a different, cheaper problem — **library
growth policy**, not the VM: `Dictionary` (`kernel.nsl:282`) starts at an 8-slot table and
grow-rehashes on every 10-key iteration, and `OrderedCollection add:` (`kernel.nsl:135`)
calls `ensureCapacity` per element, recopying as a fresh collection reaches 10.

### What these numbers are NOT

- **A single run against published means-of-three.** `performance.toit`'s `main` runs the
  suite 3× with a fresh `SpeedTester` and reports the mean; every reference column is a
  mean of 3. This is one run (David: *"run the test once for starters ... lets see what
  we have"*). **Not comparable; orientation only. Not publishable.**
- **Same-RTOS.** tuvm is on Zephyr; the other three are on ESP-IDF/FreeRTOS. This
  confound cannot be removed by re-running and must always be disclosed.
- **Confirmed same-silicon.** See the ledger's † note and §10 of the design: the README
  says WROOM-32E (rev v3.0); the attached board is D0WDQ6 **rev v1.0**. Plan is to rerun
  Toit *and* nsl on a definitely-identical board.
- **Heap-matched.** tuvm ran a 96 KB arena; the other three have their own GC policies.

### Port fidelity — the gate passed

Predictions from E3, checked before quoting anything (design §7). A **too-fast** result
was the danger signal (the literal-array trap):

| test | predicted | actual | error |
|---|---:|---:|---:|
| arrayWrite | ~143 s | 138.9 s | 3% |
| floatMath | ~37 s | 35.1 s | 5% |
| integerMath | ~18 s | 15.3 s | 15% |
| stringCompare | ~7.5 s | 5.2 s | 30% |
| collectionIterate | ~410 s | 532.6 s | 30% |

Nothing came in suspiciously fast; the two highest-count rows landed within 5%. The port
and the iteration counts are sound.

`floatMath` also proves the arena at the tightest config yet: a 96 KB heap leaves only
~23 KB of libc heap, and ~3M boxed floats cycled through 512-cell slabs via W2a reclaim
without aborting — the first real workout that path has had on target.

## E5 — blockcost on ESP32 (Model B): decomposing E4's block-activation tax

E4 left one number demanding explanation: 26.6 µs per `collectionIterate` element
visit, 41.9× Toit. `nsl-tests/00_perf/blockcost.nsl` decomposes it: seven self-timed
loops, each visiting **10M elements**, differing by exactly one mechanism. The emitted
bytecode was verified per variant before running: `baseLoop`'s body is pure inlined
adds/jumps, `blockValue` adds exactly one `OP_BLOCK_INVOKE` per iteration, `methodSend`
one `OP_SEND`, `closureMake` one `OP_CLOSURE`, and `blockCapture` builds its
closure+Context once and pays only `LD_CTX`/`ST_CTX` per iteration.
Log: `esp32-debug-artifacts/tuvm-esp32-blockcost-96k-run1.log` (~16.5 min run).
Host column: x86-64 desktop, `gcc -O2`, median of 4 runs. Per-op figures are
**within-binary deltas** (`variant − baseLoop`, or − `blockValue` for capture), ÷ 10M.

| variant | host ms | ESP32 ms | derived per-op cost (host / ESP32) | ratio |
|---|--:|--:|---|--:|
| baseLoop (inlined `to:do:`) | 413 | 76,856 | 41 ns / 7.7 µs per inlined iteration, all-in | 186× |
| blockValue | 695 | 135,137 | **28 ns / 5.8 µs per block activation** | 207× |
| methodSend | 676 | 147,960 | 26 ns / 7.1 µs per monomorphic send | 270× |
| blockCapture | 1,038 | 158,365 | 34 ns / 2.3 µs per captured-var write | 68× |
| closureMake | 829 | 100,078 | 42 ns / 2.3 µs per closure alloc (incl. GC) | 56× |
| ocDo (10-elem `oc do:`) | 1,719 | 407,131 | 172 ns / 40.7 µs per element | 237× |
| ocAt (manual `oc at:` loop) | 1,882 | 735,069 | 188 ns / 73.5 µs per element | 391× |

### What it says

- **A block activation costs the same as a method send** (5.8 vs 7.1 µs on target;
  28 vs 26 ns host). `OP_BLOCK_INVOKE` skips the lookup but `push_frame` — header
  setup, arg copy, nil-clearing the register file — dominates both. Frame activation
  is the unit cost; blocks are not specially broken.
- **Allocation is not the villain.** Closure creation and captured-variable access are
  the *cheap* rows on target (2.3 µs each) and scale best from host (56–68× vs 200–400×
  for send/frame paths). The collections gap is send/frame machinery, not GC.
- **The hand-written index loop is WORSE than `do:`** — 73.5 vs 40.7 µs/element on
  target (1.8×; host agrees, 1.09×). Per element, kernel `at:` costs a full method
  send + bounds-check + an inner `contents at:` send — two frame activations — where
  `do:` pays a block invoke + one `contents at:` send. "Avoid combinators, write
  index loops" is a losing trade in this VM.
- **Per-element anatomy of `do:`** (bytecode-verified): one direct slot load (cheap —
  own-slot reads compile to a direct load, not a send; `count` is hoisted once by
  `to:do:`), one `#at:` **send** (~7.1 µs), one block invoke (~5.8 µs), plus the
  inlined loop skeleton (~7.7 µs). But note the component sum (~21 µs) under-predicts
  the measured 40.7 µs by ~2× — and the same holds on host (95 ns of components vs
  172 ns measured) and for `ocAt` (~25 µs of components vs 73.5 µs). The microbench
  deltas are measured with ONE hot selector and a tiny interpreter working set;
  real combinator loops alternate several send paths and lose cache locality. Treat
  the per-op numbers as lower bounds that compose non-linearly.
- **Cross-binary absolutes carry large noise; deltas don't.** The same 10-element
  `ocDo` loop measured 164 ns/elem and 130 ns/elem in two different host binaries
  (~25% swing from code layout / I-cache), and a one-binary 10/20/40-element sweep
  shows **no** collection-size effect above noise. E4's 26.6 vs E5's 40.7 µs/elem is
  the same phenomenon writ large (flash XIP cache on Xtensa) — quote the *range*, not
  either endpoint, and trust within-binary deltas only.

### Where the leverage is

The two per-element costs in `do:` are the `#at:` send and the block invoke; both are
mostly `push_frame`. Three concrete directions, in rough order of reach:
(1) cheapen `push_frame` (the nil-clear loop and header stores);
(2) an `OP_AT`/`OP_AT_PUT` fast-path opcode for SMI-indexed `Array` access, exactly as
`OP_ADD` already bypasses the full send for SMI `+` — this deletes the biggest single
cost in `do:` *and* in every index loop;
(3) callee-side block inlining is off the table at the call site (`aBlock` is a
parameter — the whole reason this benchmark exists), so the remaining big swing is a
customized `do:`-family, e.g. compiling collection combinators against the block's
`CompiledMethod` at first call (polymorphic inline caching / specialization).

Benchmark source: `~/workspace/nsl-tests/00_perf/blockcost.nsl`. Reproduce: E3 recipe
(§5) with `blockcost.nsl`, 96 KB heap knobs as E4; budget a ~17 min capture.

## E6 — the E5 findings acted on: two trial branches, measured on target

**Branch results, NOT master.** E5's decomposition named the costs; E6 is the trial
that attacks them, exactly in the "obvious optimizations, clean code" posture:

- `perf/activation-trim` (tuvm only, no ISA change): `is_truthy` identity fast path
  (was `strcmp("true")` per conditional jump), send_resolve's three capability
  gates cached as pointers (were cross-TU calls per send), push_frame arg-copy
  clamp, OP_BLOCK_INVOKE selector load moved to its fallback.
- `perf/op-cmp-at` (tuvm + nsl-tuvm, paired): six opcodes `OP_LT/LE/GT/GE/AT/AT_PUT`
  (0x19–0x1E) in OP_SEND's exact layout — a fast-path miss GOTOs into the OP_SEND
  handler, so fallback semantics are the send's, verbatim. SMI×SMI compares inline
  to the boolean symbols (Float/LargeInteger/String compares take the old path);
  `basicAt:`/`basicAt:put:` use PRIM_ix_basicAt's exact shape gate. The compiler
  lowers loop-bound compares (`to:do:`, `to:by:do:`, `timesRepeat:`), generic
  explicit-receiver sends of the six selectors, and literal-array construction.
  Kernel: OrderedCollection `do:/at:/at:put:/add:` index `contents` via `basicAt:`,
  deleting the Array>>at: frame per element.
- Combined: `perf/activation-plus-ops` (tuvm merge 8e70ba5 + nsl-tuvm
  `perf/op-cmp-at` cc30a99-amended).

**Gates** (all on the combined branch): C units 26/26 incl. the new hand-assembled
`tests/test_op_fast.c` (TDD: watched it fail "Unknown opcode 26" on the base VM);
nsl suite 195 passed / 0 failed / 12 skipped — identical to baseline — plus
grant/debugger-integration/portability-32/boot-gc-asan PASS. (Two integration
harnesses had `OEVM = ../tuvm` hardcoded and ran branch bytecode on the master VM
— fixed in `nsl-tuvm` to honor `OEVM_DIR`; that fix is part of the branch.)

**blockcost on ESP32** (same board/knobs as E5; ms; log
`esp32-debug-artifacts/tuvm-esp32-blockcost-AB-96k-run1.log`):

| variant | E5 baseline | E6 branch | change |
|---|--:|--:|--:|
| baseLoop | 76,856 | 49,640 | **−35%** (7.7→5.0 µs/iter) |
| blockValue | 135,137 | 104,753 | −22% |
| methodSend | 147,960 | 119,743 | −19% |
| blockCapture | 158,365 | 128,065 | −19% |
| closureMake | 100,078 | 72,945 | −27% |
| ocDo | 407,131 | 156,058 | **−62%** (40.7→15.6 µs/elem) |
| ocAt | 735,069 | 375,641 | **−49%** (73.5→37.6 µs/elem) |

The mechanism deltas the branches did NOT touch are unchanged (activation 5.5 vs
5.8 µs, captured-var 2.3, closure 2.3) — the wins are exactly the deleted
per-element sends/frames and the cheaper loop skeleton. Note the host/device rank
flip persists: on device `ocAt` is still 2.4× `ocDo` (OC>>at:'s kernel-method frame
per element), so on target the combinator remains the fast way to iterate an OC.

**STIC on ESP32, branch vs E4** (seconds; references as in E4; log
`esp32-debug-artifacts/tuvm-esp32-STIC-AB-96k-run1.log`):

| Test | Toit | uLisp | CircuitPython | E4 tuvm | **E6 branch** | change |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| alloc | 16.0 | 21.6 | **11.4** | 24.1 | 23.7 | −2% |
| array write | **19.4** | 98.1 | 63.3 | 138.9 | 117.0 | −16% |
| dictionary write | 5.7 | 1.4 | **0.9** | 43.3 | 45.9 | +6% † |
| float math | 41.1 | 124.3 | **20.9** | 35.1 | 34.3 | −2% |
| integer math | **9.4** | 122.5 | 14.4 | 15.3 | 14.6 | −5% |
| collection iterate | **12.7** | 42.5 | 29.2 | 532.6 | **274.8** | **−48%** |
| collection write | 9.5 | **6.7** | 15.4 | 97.0 | 81.3 | −16% |
| string compare | **1.5** | 12.5 | 2.9 | 5.2 | 4.8 | −8% |
| hanoi @ 8 | **0.0000** | 0.01 | 0.01 | 0.004 | 0.004 | flat |

Read it honestly, as E4 demanded:
- **collectionIterate halves** (41.9× Toit → 21.6×; 18.2× CircuitPython → 9.4×) —
  still the worst row, but the single biggest movement the port has seen.
- **integerMath is now at parity with CircuitPython** (14.6 vs 14.4).
- floatMath −2% on device **refutes the host's +15%** — that was host code-layout
  noise, exactly the E5 caveat in action. Never quote a host absolute for this VM.
- † dictionaryWrite +6%: Dictionary probes send `=`, which is deliberately NOT
  lowered (object equality is too broad); the delta is within this target's
  cross-binary noise band, but the row is untouched by design and says so.
- The rows still 4th (arrayWrite, collectionWrite, dictionaryWrite) share one
  remaining shape: a public kernel-method send + frame per element (`at:put:`,
  `add:`'s ensureCapacity chain, Dictionary probe loop). That is the next ledge,
  and it is push_frame's cost, not lookup's.

## E7 — Richards on ESP32 (Model B): the first program-shaped benchmark

David's question after E4–E6: the STIC collection rows are pure per-element
mechanics — what does a *non-synthetic* workload show? Richards (the classic
OS-task-scheduler simulation: polymorphic sends, linked task/packet structures,
branchy control flow, no single hot loop) is the canonical answer, and the nsl
port already existed and passes in the suite on both VMs
(`nsl-tests/05_somns_benchmarks/06_richards.nsl`, SOMns lineage).
`nsl-tests/00_perf/richards.nsl` (uncommitted, alongside blockcost.nsl) wraps that
body **verbatim** in a self-timed node: 3 runs, each a fresh
`RichardsBenchmarks new start` verified against the canonical counts
(queuePacketCount 23246, holdCount 9297).

Same board, same seven knobs / 96 KB heap as E4–E6; dram 168816 B (85.86%)
verified identical for both builds. Logs:
`esp32-debug-artifacts/tuvm-esp32-richards-96k-run1.log` (baseline),
`tuvm-esp32-richards-AB-96k-run1.log` (branch).

| config | run 1 | run 2 | run 3 | mean |
|---|--:|--:|--:|--:|
| baseline (`feat/zephyr-model-ab`) | 17,549 | 17,658 | 17,677 | **17,628 ms** |
| `perf/activation-plus-ops` | 18,387 | 18,496 | 18,514 | **18,466 ms** |

(Host, `gcc -O2`, same `.oimg`s: baseline 117/82/75 ms, branch 79/71/67 ms —
the host's apparent −10% is refuted on device, layout noise again.)

### What it says

- **The E6 branch is flat on Richards** (+4.8%, below the cross-binary noise
  band E5 established). Expected, and diagnostic: Richards' hot code is `=`
  compares (`NoWork = …`, `NoTask = …` in every task function and the scheduler
  loop) and parameter-block invokes (`function value: message value: handle`) —
  precisely the two things the fast-path design deliberately did NOT lower
  (Decisions 2 and 3). No OrderedCollections, few `to:do:` bounds, public `at:`
  only on the 6-slot taskTable. E6's wins are collection-mechanics wins; the
  general send+activation floor is untouched, and Richards runs on that floor.
- **Within-binary spread is <1%** across the 3 runs in each config; the ±5%
  between configs is cross-binary. Same discipline as ever: deltas within a
  binary, ranges across.
- **Host→device ratio is ~220×** (17.6 s vs ~80 ms), squarely inside E5's
  send-heavy band (207–270×) — Richards composes the same machinery the
  microbenches decomposed, at realistic proportions.
- **Relative standing.** No cross-VM ESP32 Richards column exists yet — that
  requires Richards on Toit/CircuitPython **on this board** (a Toit port exists
  at `~/workspaceToit/toit/tests/type_propagation/richards-test.toit`; Python
  Richards is standard in pyperformance). What is already on record: on host,
  oevm runs Richards at **1.18× primordialsoup**
  (`nsl-tests/00_perf/RESULTS.md`) — on program-shaped work this VM is closest
  to a mature Smalltalk-family VM, vs 41.9× Toit (E4) on the worst synthetic
  collection row. The real-world gap sits far nearer the Richards end than the
  collectionIterate end for any workload that is not itself a collection loop.

### Caveats

- Branch-vs-baseline here is a cross-binary comparison; ±5% is below signal on
  XIP (E5's caveat). The 3 runs within each binary are the trustworthy part.
- Both columns are tuvm on the same board/RTOS — E4's cross-VM confounds do not
  apply, but there is no external-VM column to rank against yet.

## E8 — production-config knobs: IRAM placement and debugger hooks off

David's post-E7 direction: correctness and simplicity over absolute perf;
try the two cheap production-config knobs, document, defer the rest. Branch
`perf/iram-interp` (worktree `~/workspace/tuvm-wt-iram`), two independent
env-knob-gated commits, both default-off, no semantic change when off:

- **`OEVM_IRAM_HOT`** (90ae47e): `oevm.h OEVM_HOT` routes `run_interpreter`,
  `push_frame`, `send_resolve`, `is_truthy` (~8 KB) into `.iram.text`
  (+`-mtext-section-literals` so Xtensa literal pools travel with the code).
  IRAM 16.63% → 20.09%; dram unchanged.
- **`OEVM_NO_DEBUG_HOOKS`** (333f4f3): compiles out the three per-bytecode
  debugger checks (bp gate, OP_BREAK dispatch, single-step hook). A build so
  configured cannot host the on-target debugger — disclosed semantic knob,
  never a silent default.

**Gates** (branch, both knobs off = default): C units 25/0 (+2 pre-existing
skips); nsl suite **195/0/14** — the +2 skips vs baseline are exactly the new
expectation-less perf nodes richards.nsl/deltablue.nsl; grant /
debugger-integration / portability-32 / boot-gc-asan all PASS
(debugger-integration passing proves the hooks still work when the knob is off).

**Richards on device** (ms, 3 runs, same board/96 KB knobs; baseline = E7):

| config | runs | mean | vs baseline |
|---|---|--:|--:|
| baseline | 17,549/17,658/17,677 | 17,628 | — |
| IRAM | 17,418/17,648/17,689 | 17,585 | −0.2% |
| NODBG | 16,539/16,648/16,665 | 16,617 | **−5.7%** |
| both (production) | 16,444/16,554/16,575 | 16,524 | **−6.3%** |

**blockcost on device** (ms; baseline = E5):

| variant | E5 base | IRAM | Δ | NODBG | Δ | both | Δ |
|---|--:|--:|--:|--:|--:|--:|--:|
| baseLoop | 76,856 | 76,148 | −0.9% | 64,895 | **−15.6%** | 64,311 | −16.3% |
| blockValue | 135,137 | 133,887 | −0.9% | 116,756 | −13.6% | 115,673 | −14.4% |
| methodSend | 147,960 | 146,709 | −0.8% | 130,789 | −11.6% | 129,704 | −12.3% |
| blockCapture | 158,365 | 157,115 | −0.8% | 138,859 | −12.3% | 137,776 | −13.0% |
| closureMake | 100,078 | 99,411 | −0.7% | 89,450 | −10.6% | 88,867 | −11.2% |
| ocDo | 407,131 | 286,806 | **−30%** | 439,374 | +7.9% | 260,157 | **−36%** |
| ocAt | 735,069 | 495,532 | **−33%** | 815,419 | +10.9% | 457,988 | **−38%** |

**The combination is additive**: mechanism rows track NODBG (−11…−16%), the
combinator rows better either knob alone (ocDo 40.7→26.0 µs/elem, ocAt
73.5→45.8). Note ocDo's 26.0 µs/elem equals E4's best-ever cross-binary
endpoint (26.6) — but now **by construction** (deterministic IRAM placement),
not by layout luck. This pair of default-off env knobs is the recommended
production configuration: −6.3% on program-shaped work, −36% on combinator
iteration, cost ~8 KB IRAM and no on-target debugger.

### Read it together — the two knobs are complementary

- **The E5 hypothesis "the ~370 cyc/bytecode floor is interpreter I-fetch" is
  refuted**: IRAM placement leaves every single-hot-path row and Richards flat
  (≤1%). The interpreter loop was already flash-cache-resident there.
- **But IRAM wins exactly where paths alternate**: ocDo −30%, ocAt −33%, with
  the five flat mechanism rows as the in-run control against layout luck. This
  resolves E5's open puzzle (oc rows cost ~2× their component sums): alternating
  send paths evict the interpreter's own code from the flash cache, and IRAM
  placement ends that fight — and, because placement becomes deterministic,
  removes the cross-binary layout roulette on combinator rows.
- **NODBG is a genuine per-bytecode win**: −10…−16% on every mechanism row and
  −5.7% on Richards — the first knob that moves Richards at all. Its oc rows
  going +8…+11% is the same layout roulette rolling badly in that binary
  (ocAt 81.5 µs/elem is a new worst endpoint of the known band) — precisely the
  noise IRAM placement eliminates.
- Richards flat under IRAM says its residual bottleneck is D-side: bytecode and
  kernel objects fetched from the DROM-mapped blob — Model B's "program costs
  flash, not RAM" trade seen from its latency side.

Logs: `tuvm-esp32-richards-IRAM-96k-run1.log`,
`tuvm-esp32-blockcost-IRAM-96k-full.log`,
`tuvm-esp32-richards-NODBG-96k-run1.log`,
`tuvm-esp32-blockcost-NODBG-96k-run1.log` (+ `-prod` logs for the combined
config, below). Assessment + deferred list:
`docs/design/2026-07-17-collection-performance-assessment.md`.

## E9 — DeltaBlue on ESP32, and the cross-VM program-shaped family

The RESULTS.md "deltablue EXEC_ERR on oevm" note is **stale** — the somns port
(`nsl-tests/05_somns_benchmarks/07_deltablue.nsl`) runs clean on both the
baseline and `perf/activation-plus-ops` VMs today. `nsl-tests/00_perf/
deltablue.nsl` wraps it in the richards.nsl-style timed node: 3 runs of
`chainTest: 50` + `projectionTest: 50` (SOM shape; self-verifying — any wrong
plan raises).

**Device result (n=50, 112 KB heap):** `deltablue: 1160 / 1164 / 1159 ms` —
mean **1.161 s**, verified. Log:
`esp32-debug-artifacts/tuvm-esp32-deltablue50-112k-run1.log`.

**Why n=50 and 112 KB — two findings on the way to green:**

- **n=100 does not fit a 96 KB arena at all** (OOM in the first chainTest,
  before any timing; log `tuvm-esp32-deltablue-96k-run1.log`), and at 112 KB
  completes exactly one run (2.959 s) then OOMs
  (`tuvm-esp32-deltablue-112k-run1.log`).
- **At n=50 / 96 KB it completes two runs (1169/1167 ms) then OOMs on the
  third** — reproduced on host with a matching compile-time heap. A 12-run
  host trial on a roomier heap completes clean, so this is **not unbounded
  cross-run retention**: the GC does reclaim prior runs. The failing shape —
  a few-hundred-byte allocation refused while "used" reads ~heap-size — points
  at **free-list fragmentation in the non-moving heap** reaching a plateau a
  bit above what 3 back-to-back graph rebuilds need at 96 KB. **Loose thread
  for the VM**: characterize fragmentation behavior for long-running nodes
  that periodically rebuild object graphs; this is a correctness-adjacent
  concern for always-on devices, beyond benchmarking.

**Cross-VM family authored** (David's ask): `~/workspaceToit/stic/` now
carries `richards.toit`, `richards.py`, `deltablue.toit`, `deltablue.py` —
Toit classes verbatim from the Toit repo's V8-lineage ports; Python files are
direct translations; DeltaBlue chain tests matched to the SOM shape so all
columns do identical, self-verified work; all print the nsl nodes' exact
output format. All four verified on host (host toit + CPython). The stic
README's STIC table gained a caveated tuvm/nsl column (E4 numbers) and a new
Richards/DeltaBlue table with the nsl device figures; Toit/CircuitPython
device columns await runs on that toolchain (marked tbr).

---

## 4. Two embedded gotchas these milestones surfaced

### 4.1 The float arena — a bug found twice, on two chips

`floatMath` aborts on its **first boxed float** unless the arena is shrunk:

```
VM Float Arena Error: Out of Memory
abort()
```

Arithmetic, not shortage. `gc.c`'s slab is `OEVM_FLOAT_ARENA_SLAB_CELLS` cells, default
**4096**. On Xtensa `struct Float` is **16 B** (8-byte `double` alignment — the same ABI
fact `zephyr/layout-esp32.json` records), so one `struct FloatSlab` is
`4 + 4096*16 + 512` = **66,052 B ≈ 64.5 KB** against this build's **~60 KB libc heap**.
The first `malloc` can never succeed.

`ns-zephyr` hit and fixed this *identical* bug on nRF52840 in June 2026 — `results.md`
M3: *"OEVM allocated boxed floats in 4096-cell (~64 KB) slabs — larger than free RAM, so
`floatMath` aborted on the first box"*; `ZEPHYR_NODE_GUIDE.md` §3 pins the values.
`zephyr/CMakeLists.txt` already forwarded both knobs; only `run.sh`'s export list was
short.

**So "the five load-bearing knobs" — repeated by the ladder, the plans and `run.sh` —
was never true. It is seven.** Five is correct only for nodes that never box a float,
and TimerNode never does:

```
OEVM_HEAP_SIZE=65536 METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256
VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512
OEVM_FLOAT_ARENA_SLAB_CELLS=512 OEVM_FLOAT_ARENA_SLAB_BUDGET=4    # <- not optional
```

`run.sh` now exports all seven. Values are ns-zephyr's, proven on nRF52840.

### 4.2 Model A's build is a memory bug, not just a slow one

`cc1` on `src/solidified_image.c` reaches **~4.9 GB RSS** and has been **OOM-killed by
the kernel**, taking the terminal's cgroup with it (`oom-kill: task=cc1`,
`anon-rss:4865120kB`; 15 GB box, ~9 GB desktop, 4 GB swap full). Model A's build cannot
be casually scheduled — free RAM first, or use Model B. Note the cost is **`cc1`**,
*before* `as` ever runs: docs blaming the ~50 min on `as` chewing 941.7 MB of generated
asm are at best incomplete.

---

## 5. Reproduce

E2 (and any in-tree node):

```sh
zephyr/run.sh --model B --node TimerNode.nsl --flash --capture 20
```

E3 — **`run.sh` cannot drive this yet**: `perfstex.nsl` lives in `nsl-tests/00_perf/` and
`run.sh` resolves `--node` relative to `zephyr/` only (a papercut worth closing; every
other node is in-tree). Until then:

```sh
source ~/zephyr-esp32/.venv/bin/activate
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.17.4
source ~/zephyr-esp32/zephyr/zephyr-env.sh
export OEVM_HEAP_SIZE=65536 METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256 \
       VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512 \
       OEVM_FLOAT_ARENA_SLAB_CELLS=512 OEVM_FLOAT_ARENA_SLAB_BUDGET=4

python3 ~/workspace/nsl-tuvm/tools/ns_compile_oevm.py \
  ~/workspace/nsl-tests/00_perf/perfstex.nsl /tmp/perfstex.oimg \
  --format=oimg --layout=zephyr/layout-esp32.json

cd zephyr && west build -b esp32_devkitc/esp32/procpu -d build-esp32-perf . -- \
  -DCONF_FILE=prj_esp32.conf -DTUVM_MODEL=B -DTUVM_BLOB=/tmp/perfstex.oimg
~/.espressif/python_env/idf5.4_py3.12_env/bin/esptool.py --chip esp32 --baud 921600 \
  write_flash --flash_mode dio --flash_freq 40m --flash_size 4MB 0x1000 \
  build-esp32-perf/zephyr/zephyr.bin
python3 capture_coredump.py 60        # hanoi18 alone is ~4.2 s; 60 s is generous
```

The console here emits **~520 B/s** (~20× under the 115200 line rate), so short capture
windows truncate output silently and mid-token. Budget generously.

## 6. Caveats

- **E3 is a single run, no repeats** — no median-of-N, no variance. `millis` is
  `k_uptime_get()` (1 ms tick), so `integerMath: 30` carries ~3% quantisation on its own.
  Treat the small sub-tests as indicative; `hanoi18`/`arrayWrite` are the trustworthy rows.
- Both ESP32 and nRF M3 numbers used the float arena at **512/4**, so that axis is matched.
- Nothing was tuned for the ESP32; no claim is made that these are its ceiling.
- E1's RAM is not captured (see the ledger's † note).
