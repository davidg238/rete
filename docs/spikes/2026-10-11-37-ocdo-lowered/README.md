# rete#37 spike — `oc do:` lowered at the call site, by hand (2026-10-11)

**Question.** The #296 record (`docs/results/2026-10-10-296-image-in-sram.md` §9) priced lowering a literal
`do:` block into the caller at "~900 → ~300 cycles per element, the MicroPython number". Before anyone builds
the pass or a receiver-class story for it, does the hand-lowered bytecode get there?

**Method.** `Spike296Node.nsl` (the #296 node, on branch `37-ocdo-spike` over master after #412) gains a row
`ocDoLowered:` whose source is only room for a longer method; `patch_ocdo.py` replaces its bytecode in the baked
image with the lowering a compiler would emit: a class-guard stand-in of the same length (the ISA has no
class-test opcode; the guard runs once per call), `count` and `contents` read once from the receiver's slots,
then per element `LE`, `JMP_IF_FALSE`, `AT`, the body, `ADD`, `JMP` (10 bytecodes; the two copies before `LE`
and before `AT` are the ISA's, since both write over their window's first register). `run.sh` runs the patch
when `OEVM_OIMG_PATCH` names it (spike only). `ocDo:` stays in the same image as the control.

- **Host check** (`--check`): the patched body sums the elements; 100,000 × (1..10) answers **5,500,000**,
  so every element is visited exactly once. Host `-O2`: ocDo 95–97 ms, ocDoLowered 41–42 ms.
- **Board**: WeAct RP2350B, `rpi_pico2` image, `-O2`, blob in flash, master's `OEVM_HOT`, built on msi-7d43
  (`~/37-build.sh`), three boots with `run_412.sh` (`logs/`, `logs.driver.log`). Every row identical to the ms
  across the three boots.

## Result

| row (ms, WeAct) | boot 1 | boot 2 | boot 3 | row ÷ 1M elements | vs MicroPython (1491) |
|---|---:|---:|---:|---:|---:|
| ocDo (control: the compiler's send + kernel `do:`) | 7650 | 7649 | 7650 | 7.65 µs, ~1,150 cycles | 5.1× |
| **ocDoLowered** | **2808** | **2807** | **2808** | **2.81 µs, ~420 cycles** | **1.9×** |

- **Lowering is worth 2.7× on ocDo** (0.37 of the control), and the control reproduces #412's final 7620 within
  0.4 %.
- **It does not reach MicroPython.** Of the 2808 ms, about 0.5 s is per call, not per element: the outer
  loop (7 bytecodes) and the lowering's guard, slot reads, setup and final failed test (~11), at the model's ~41
  cycles each, 100,000 times. That leaves ~2.3 µs, ~350 cycles, for each element's 10 bytecodes: the cycle model
  holds (~35–41 per bytecode), and the record's ~300 assumed fewer bytecodes than this ISA allows.
- **What would close the last 1.9× (not measured):** four of the ten per-element bytecodes are window copies.
  A separate destination on `LE`/`AT` (as `ADD` already has) would make it ~6 bytecodes, ~210 cycles, and
  the row ~1.9 s with the 0.5 s per-call share (~1.3×). A fused iterate opcode (`k <= count`, `e := contents[k]`,
  `k++`, branch, in one dispatch, Python's `FOR_ITER`) would make it ~3, the row ~1.2 s (~0.8×). Both are ISA changes; the second is one
  more mechanism.
- **Placement moved again.** Adding the method shifted the flash layout: richards 8588 → 8138, blockValue 6301 →
  6355 against #412's table, with neither row touching the new code. The comparison above is within one image,
  so it is not affected; a cross-image comparison would have been.

## Second image: the fused iterate opcode, `OP_ITER_AT` (same day)

`OP_ITER_AT b, off` (0x1F, `vm.c`/`oevm.h`, spike only, never emitted by the compiler) does one step of the
lowered loop in one dispatch: R(b) next index, R(b+1) limit, R(b+2) the indexable, R(b+3) the element. Past the
limit it branches out; otherwise it loads the element and increments the index. A third row, `ocDoFused:`, is
patched to use it: per element `ITER_AT`, the body, `JMP` — 3 bytecodes. Host check 5,500,000 for both patched
rows; host `-O2`: ocDo 93–96, ocDoLowered 41, **ocDoFused 16–18 ms** (MicroPython unix on the same host: 21).

WeAct, one image with all three rows, three boots (`logs/weact-37-fused-*`, `logs-fused.driver.log`):

| row (ms, WeAct) | boot 1 | boot 2 | boot 3 | vs MicroPython (1491) |
|---|---:|---:|---:|---:|
| ocDo (control) | 7668 | 7669 | 7668 | 5.1× |
| ocDoLowered (today's ISA) | 2838 | 2838 | 2838 | 1.9× |
| **ocDoFused (`OP_ITER_AT`)** | **1220** | **1219** | **1219** | **0.82×** |

- **The fused lowering beats MicroPython by 18 %**, and lands on the model's estimate (~1.2 s, ~0.8×) made
  before the run. 6.3× faster than the control.
- Per element: ~0.7 µs, ~110 cycles for 3 bytecodes, once the ~0.5 s per-call share is taken out. The per-call
  share is now ~40 % of the row: with 10-element collections the outer loop and the guard and setup matter as
  much as the elements.
- Adding a case to the dispatch switch moved placement again: richards 8138 → 8361, ocDoLowered 2808 → 2838,
  ocDo 7650 → 7668, with none of their code changed. Within-image comparisons only.

## What this says for #37

1. A guarded lowering of a literal `do:` reaches MicroPython **only with a fused iterate step**; on today's
   ISA it stops at 1.9×. The fused step is one new opcode, against re-encoding `LE` and `AT`.
2. The pass needs three things the compiler does not have: the receiver-class guard (a class-test opcode or
   equivalent, #395's gap), the slot indices of the kernel collection it lowers for, and the `contents`
   re-read question (a body that grows the receiver; see `patch_ocdo.py`'s docstring). Each kernel collection
   with its own `do:` (Array, OrderedCollection, Interval, Dictionary) is a separate lowering.
3. Not measured, and cheap to measure the same way: the kernel's own `OrderedCollection>>do:` rewritten with
   `OP_ITER_AT`. That is no compiler work at all and helps every `do:`, including non-literal blocks and
   everything built on `do:` (`inject:into:`, `collect:`, `select:`); the block activation per element stays.

Not measured: a body that does work (the body here is one register load, as `for e in oc: e` is one load and a
pop); a non-literal block (unchanged by any lowering); the real guard.
