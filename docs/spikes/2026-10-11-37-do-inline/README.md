# rete#37 spike — a literal `do:` lowered by the compiler behind a real class guard (2026-10-11)

> Draft design: `docs/design/2026-10-11-37-do-inline-design.md`. The code is on branch `37-do-inline-spike`:
> `OP_IS_VT` (0x20) in `vm.c`/`oevm.h`, `compile_do_inline` in `ns_compile_oevm.py` behind `NSL_DO_INLINE`,
> and the #296 bench node with the rows below.

**Question.** The 2026-10-11 hand-patched row (`docs/spikes/2026-10-11-37-ocdo-lowered/`) reached 1219 ms with a
stand-in for the class guard. What does the compiler-emitted lowering cost with a real guard on a hit, and what
does a miss cost?

**Method.** The compiler lowers `recv do: [:e | …]` in the methods that `NSL_DO_INLINE` names as follows: `IS_VT`
against OrderedCollection's vtable, then the `ITER_AT` loop over `count`/`contents` (slot indices looked up
by name), then the same against Array (slot 0, the receiver), then today's send. Each `*Inline` row has the
same source as its control. Receivers: a 10-element OrderedCollection, a 10-element Array, and
`Interval from: 1 to: 10` (both guards miss). WeAct RP2350B, `rpi_pico2`, `-O2`, blob in flash,
`OEVM_HOT`, built on msi-7d43 (`~/37-inline-build.sh`), three boots with
`docs/spikes/2026-10-10-296-image-in-sram/run_412.sh` (`logs/`, `logs.driver.log`).

## Result

| row (ms, WeAct, 100,000 × 10 elements) | boot 1 | boot 2 | boot 3 | vs control | vs MicroPython (1491) |
|---|---:|---:|---:|---:|---:|
| ocDo (send + kernel `do:` on `ITER_AT`) | 5681 | 5681 | 5681 | | 3.8× |
| **ocDoInline** | **1087** | **1087** | **1087** | **0.19** | **0.73×** |
| arDo | 6393 | 6393 | 6393 | | 4.3× |
| **arDoInline** | **1156** | **1155** | **1155** | **0.18** | **0.78×** |
| ivDo (Interval, no lowering) | 7339 | 7340 | 7340 | | |
| ivDoInline (both guards miss → send) | 7507 | 7506 | 7506 | **+2.3 %** | |

- **The compiler's lowering with the real guard beats the hand patch:** 1087 against 1219 ms. That is a
  cross-image comparison, and placement moves rows by a few percent (below), but the guard is no worse than
  the stand-in. `IS_VT` + `JMP_IF_FALSE` is two dispatches, where the stand-in was three.
- **The guard is cheap on a hit.** The miss row prices about six extra dispatches (two guards, two copies)
  at +167 ms per 100,000 calls, ~1.7 µs a miss, ~0.28 µs a dispatch. So the hit's guard is ~0.55 µs a call,
  ~55 ms of ocDoInline's 1087 (~5 %). The Array row pays one failed OC guard on top: 1155 − 1087 = 68 ms.
- **A miss costs 2.3 % on a 10-element Interval.** The cost is per call, not per element.
- **Static knowledge (dropping the guard) would buy ≤ 5 % on this row.** It does not justify itself on these
  numbers.
- Placement: the ocDo control is 5681 here and 5594 in the iter-at image, with its code unchanged; richards is
  8582–8595. Compare within one image only.

## Correctness and size

- Full nsl suite with `NSL_DO_INLINE=all` on opti7040: **293 passed, 0 failed.** Every aux gate that runs
  there passed. `portability-32` fails for lack of 32-bit glibc headers on that host, and the pytest/cmake gates
  SKIP for lack of the tools. That is a host limitation, not this change, but the aux line is not green here.
- One semantic hole, found by a probe before the suite run: a closure inside the inlined body that captures
  `e` read a reused register (`6.37e-322` where the send gives `6`). The spike keeps the send for any block whose
  nested closures name its params or temps. Arms of always-inlined selectors (`ifTrue:` …) do not count as
  closures. Plain `to:do:` has the milder form of the same bug today (**#417**).
- Size (C6 layout): the bell +0 B (no site in its app code, and none in any board node under
  `nodus/oevm/zephyr/`); DeltaBlue 7 sites, +1,328 B (+0.7 %); perfstex 1 site, +352 B.
