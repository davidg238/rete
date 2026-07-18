# Hanoi A/B — ESP32 results (native PrimitiveClosure vs bytecode interpreter)

**Date:** 2026-07-17 · **Board:** ESP32-DevKitC V4 (WROOM-32E), `/dev/ttyUSB0` · **Model:** B (`.oimg`)
**Build:** `OEVM_HANOI_NATIVE=1 OEVM_IRAM_HOT=1`, Zephyr OS v4.2.0, `zephyr.bin` 314004 B
**Node:** `zephyr/HanoiAB.ns` — same binary sends the identical `hanoiMove:from:to:temp:`
selector to (a) `self` (bytecode, interpreted) and (b) the native receiver from
`installNativeHanoi` (C `PrimitiveClosure` in a vtable). Means-of-3.

## Raw serial capture

```
*** Booting Zephyr OS build v4.2.0 ***
hanoi-bc 16: 770
hanoi-bc 16: 770
hanoi-bc 16: 770
hanoi-native 16: 7
hanoi-native 16: 6
hanoi-native 16: 7
hanoi-moves 16: 65535
hanoi-bc 20: 12315
hanoi-bc 20: 12316
hanoi-bc 20: 12316
hanoi-native 20: 103
hanoi-native 20: 104
hanoi-native 20: 103
hanoi-moves 20: 1048575
done
```

## Results

| n  | moves (2ⁿ−1) | bytecode mean (ms) | native mean (ms) | **speedup** | bytecode µs/move | native ns/move |
| -: | -----------: | -----------------: | ---------------: | ----------: | ---------------: | -------------: |
| 16 | 65,535       | 770.0              | 6.67             | **~116×**   | 11.75            | ~102           |
| 20 | 1,048,575    | 12,315.7           | 103.3            | **~119×**   | 11.74            | ~99            |

- **Correctness / dispatch fidelity:** native `hanoiMove:` returned exactly `2ⁿ−1`
  (65535, 1048575). Because that value is produced *inside* `prim_hanoi`, the match
  proves the send dispatched through the `PrimitiveClosure`-in-vtable fork
  (`dispatch.c:1673`), not a DNU or the bytecode path.
- **Bytecode baseline confirmed:** ~11.75 µs/move matches the standing 11.43 µs/move
  Hanoi figure (`~/workspaceToit/stic/README.md`); the small delta is build/measurement,
  not method divergence (the two `hanoiMove:` bodies are byte-identical).
- **Native is linear, GC-free:** native ns/move is constant (~100 ns) across a 16×
  work increase (n=16→20), exactly as predicted — `prim_hanoi` allocates nothing and
  holds no oops, so there is no GC and no arena pressure.

## Interpretation — the AOT ceiling

The native path **fully inlines the recursion in C** (`hanoi_c` recurses directly; the
inner sends are collapsed to raw C calls with a volatile counter). So **~117× is the
optimistic ceiling** of bespoke AOT on this workload — *not* what a mechanical
bytecode→native transliteration that still dispatched each recursive self-send would
achieve. That was the deliberate first question: is the ceiling high enough to ever
justify a compiler? **Yes, overwhelmingly.**

Framed against the ~12× Toit gap (Toit compiled vs tuvm bytecode, same board): native
tuvm at ~100 ns/move is ~117× faster than tuvm bytecode, i.e. it lands roughly an order
of magnitude *past* Toit's compiled engine on this inlined-recursion workload — expected,
since it is essentially optimal Xtensa machine code for the recursion, not a VM at all.
The realistic bespoke-AOT number for a compiler preserving message-send semantics and
GC frames would sit between the interpreter and this ceiling.

## What this proves about OEOM

The spike's real point (David's framing): OEOM's uniformity makes native substitution a
*drop-in*. A native method is just a `Binding` whose `method` is a `PrimitiveClosure`;
the existing dispatch fork invokes it transparently, with no change to callers or the
interpreter. The whole mechanism — install a native binding for a hot selector, send to
it, run at ~117× — required only `src/hanoi_native.c` + one `strcmp` gate, all behind a
flag, `.oimg` format untouched. This is the substrate for the "collect months of usage
data, then push a bespoke-AOT patch for a proven hot method" path.

## Caveats / non-goals (unchanged from the design)

- Hanoi is all-heat-in-one-trivial-method; DeltaBlue-style spread-heat workloads would
  dilute the win. This number does not generalize to whole-program speedup.
- The native path skips the confinement choke point (`check_access_allowed` runs only on
  the bytecode branch, `dispatch.c:1667`) and is opaque to the in-image debugger — fine
  for a leaf arithmetic method, load-bearing considerations for any real AOT design.
- Experiment only: `OEVM_HANOI_NATIVE` is off by default; production defaults untouched.
