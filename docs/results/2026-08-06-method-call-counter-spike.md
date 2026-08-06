# Exact per-method call counts — spike result (#164)

**Date:** 2026-08-06 · **Issue:** #164 · **Status:** spike. Host **and** C6 measured.

**Answer up front: 0.90% on a C6** — 106 ns against an 11.83 µs/activation interpreter.
For comparison, the debugger's three per-bytecode checks cost 10–16% and are compiled out
of every Zephyr build for it (`zephyr/CMakeLists.txt`, #40).

## What this answers

Spans measure hand-placed brackets, so "which code inside the box is expensive" costs a
guess, a build and a reflash per question — the method that produced #161 and #163 and
consumed 14 of 16 span slots. This spike asks whether the deterministic alternative is
affordable: count **every** method activation exactly, at the one chokepoint every
activation already passes through, with no bracket, no sampling rate and no ISR.

## What was built

~180 lines across three files, entirely behind `-DOEVM_METHOD_PROFILE` (off by default).

- **The hook is one line** in `push_frame` (`nodus/tuvm/src/vm.c`). That function already
  loads `struct CompiledMethod *m = closure_entry(cl)` for `num_registers`, so the table
  key costs nothing to obtain — the added work is a hash, a probe and an increment.
- **The key is the CompiledMethod pointer**, which is already this system's method identity:
  it is what the breakpoint table keys on (`bp_method_key`) and what `mirror methodOf:`
  answers. It is stable because the GC is non-moving mark-sweep. A count field *inside*
  `CompiledMethod` would be cheaper and is not available — solidified methods are `.rodata`.
- **256-slot open-addressed table**, zero allocation — the same mechanic `span.c` already is.
  Overflow is **counted, never absorbed**, so a saturated table cannot read as "this method
  was never called".
- **Nameless in C**, exactly like a span slot: readback is `mirror callsOf:` / `callsStats:` /
  `callsReset:`, and the name↔method table would live in nsl via `methodsOf:`/`selectorsOf:`.
  **These consume no prim id** — `mirror_dispatch` branches on the selector *name*, so
  `prim_ids.h` is untouched and nothing shifts.

## Correctness — the counts are exact, and the number is the proof

`fib(n)` makes exactly `2·fib(n+1) − 1` activations, so the workload predicts its own answer.

| workload | predicted | counted | methods seen | overflow |
|---|---|---|---|---|
| `fib(32)` | 7,049,155 | **7,049,155** | 3 | 0 |
| `fib(35)` | 29,860,703 | **29,860,703** | 3 | 0 |

Exact, not approximate — which is the whole point versus a sampling profiler. It also
identified the hot method with no bracket and no guess, and incidentally confirmed
`ifTrue:ifFalse:` is compiler-inlined: three methods activated, no block activations.

## Cost — below the measurement floor on host

**The first measurement was wrong, and the way it was wrong is the finding.** An
OFF-build vs ON-build comparison of `fib(35)`, min of 15 interleaved runs, put the **ON
build 4% faster** (1.56 s vs 1.63 s). That is the cross-binary code-layout roulette the
benchmark discipline warns about, not a result. Adding work does not make the VM faster.

Redone the way the discipline prescribes — **one binary, a runtime gate**
(`-DOEVM_MPROF_RUNTIME_GATE`, spike scaffolding only), so both arms share a code layout:

```
fib(35), 29,860,705 activations, 25 interleaved reps, user CPU, ms resolution
  gate-off   min 1.738  median 1.787  max 1.823
  gate-on    min 1.748  median 1.782  max 1.845
  min    delta +0.58%   = +0.335 ns/activation
  median delta -0.28%   = -0.167 ns/activation
```

The two estimators **straddle zero**. On the send-densest workload available — `fib`, where
sends are essentially the whole program — counting 29.8 million activations is not
measurably slower. Against an interpreter baseline of **58 ns per activation**, the hook is
under ~0.6% and cannot be resolved further by this method.

That baseline is why: at 58 ns of interpreter work per activation, ~7 independent
instructions disappear into the shadow of an out-of-order core.

## Cost on the C6 — 0.90%, and the device is where it is measurable

Host could not resolve this hook at all; the C6 resolves it to three significant figures,
because an in-order 32-bit RISC-V has no out-of-order shadow for those instructions to hide
in and its millisecond clock is 0.03% of a 3.1 s run.

Workload: `MethodProfileNodeC6.nsl`, Hanoi depth 18 — **exactly 2¹⁸−1 = 262,143 activations
per run**, 20 identical runs, one binary, one boot. The counter flips on at a fixed
activation count (`OEVM_MPROF_FLIP_AT=2621430`, exactly 10 runs), so runs 1–10 are the OFF
arm and 11–20 the ON arm with the same code layout and the same cache state.

```
run   1..10  (counter off)   3101 3101 3101 3101 3101 3102 3101 3101 3101 3101  ms
run  11..20  (counter on)    3129 3129 3129 3129 3129 3129 3129 3129 3129 3129  ms
```

The device is essentially noise-free — one millisecond of scatter across ten runs on the
OFF arm and **zero** on the ON arm.

| | |
|---|---|
| interpreter baseline | **11.83 µs / activation** (204× the host's 58 ns) |
| counter cost | **+27.9 ms per 262,143 activations = +106 ns / activation** |
| **relative** | **+0.900%** |

The scaffolding decrement is in *both* arms, so it cancels out of the delta; it inflates the
denominator very slightly, making 0.900% a hair's-breadth understatement.

The counter was also exact on device: the ON arm reported **2,621,430 activations, overflow
0**, split 2,621,420 to `hanoiMove:from:to:temp:` and exactly **10** to `runHanoi:index:` —
one per run. Both keys are `0x428…`, i.e. XIP flash, confirming the `.rodata` reasoning above.

Incidental finding: only **two** methods were activated in the whole window. The reporting
path (`,`, `printString`, `out`) costs no method activations at all — it is prims end to end.

## Footprint (`vm.o`, x86-64 host, `-O2`)

| | off | on | delta |
|---|---|---|---|
| `.text` | 19,560 | 20,344 | **+784 B** |
| `.bss` | 1,160 | 4,264 | **+3,104 B** |
| `.text.exit` | — | 250 | +250 B (spike-only exit dump) |

On a 32-bit target the table is `256 × (4 + 4) + 12` = **2,060 B**. The ESP32 has 816 B of
dram0 spare (#53), so it cannot carry this unconditionally; the C6 has room. Hence the
build knob rather than an unconditional facility.

## Gate

`cd nodus/nsl && python3 tools/run_tests_oevm.py` → **246 passed, 0 failed**, aux line
`grant / debugger-integration / portability-32 / boot-gc-asan / deepsleep` all PASS,
`footprint(T0.3): SKIP` (pre-existing, no arm-zephyr-eabi). The knob is off by default, so
the default build is unchanged.

## What this does NOT settle

1. **The readback grant.** The mirror is handed to the image's **Supervisor**
   (`oimg_loader.c:238`), not to the app — an ordinary node app cannot send `mirror callsOf:`
   and therefore cannot publish `diag/calls/**` today. The spike sidesteps this with a
   C-side exit dump under `OEVM_MPROF_DUMP`. Deciding the real grant (supervisor publishes,
   or supervisor attenuates and hands down) is separate work and belongs with the
   capability posture, not inside a spike.
2. **Time attribution.** Counts only. Counts × an existing span sum already gives µs/call
   without inventing a sampling rate; per-method *time* (entry/exit stamping) is a
   different and more expensive mechanism, deliberately not attempted.
3. **Whether 0.90% is cheap enough to be unconditional.** That is a judgement, not a
   measurement, and it is David's. What the measurement supports: it is **an order of
   magnitude cheaper than the per-bytecode debugger checks** that were judged too expensive
   for Zephyr (10–16%), and 2 KB of `.bss` remains the binding constraint on an ESP32
   (816 B spare, #53) rather than the time.
4. **Table capacity under a real app.** Hanoi touches two methods; a bridged node will touch
   far more. 256 slots at a ≤0.7 load factor is ~180 distinct methods, and overflow is
   counted so saturation is visible — but nobody has yet run this under `PortaBridgeNodeC6`
   to see the real number.

## Bench state

Measured on **c6spare** (console `d2e21074…`, JTAG `40:4C:CA:44:6E:60`) — the disposable
board and current `TARGET_DEFAULT_BOARD`. The ship's bell was not on the cable and is
refused by by-id hash on the deploy path regardless. **c6spare is left running
`MethodProfileNodeC6`**, which finishes its 20 runs and returns; reflash it before using it
as a gate target.
