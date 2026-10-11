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
  A separate destination on `LE`/`AT` (as `ADD` already has) would make it ~6 bytecodes, ~250 cycles,
  ~1.7 s (~1.1×). A fused iterate opcode (`k <= count`, `e := contents[k]`, `k++`, branch, in one dispatch,
  Python's `FOR_ITER`) would make it ~3, ~1.0 s, below MicroPython. Both are ISA changes; the second is one
  more mechanism.
- **Placement moved again.** Adding the method shifted the flash layout: richards 8588 → 8138, blockValue 6301 →
  6355 against #412's table, with neither row touching the new code. The comparison above is within one image,
  so it is not affected; a cross-image comparison would have been.

Not measured: a body that does work (the body here is one register load, as `for e in oc: e` is one load and a
pop); a non-literal block (unchanged by any lowering); the real guard.
