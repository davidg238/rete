# Allocation attribution — the host measurement

**Date:** 2026-08-12
**Design:** `docs/design/2026-08-12-allocation-attribution-design.md` (stage 1, D7)
**Plan:** `docs/superpowers/plans/2026-08-12-allocation-attribution-stage1.md` Task 4
**Gate:** `cd nodus/tuvm && bash tests/porta_sync_host.sh` → `PORTA SYNC OK`, `RC=0`

## Command

```bash
cd nodus/tuvm && bash tests/porta_sync_host.sh
python3 tools/mprof_resolve.py /tmp/porta_node.oimg.methods.json /tmp/porta_node.log --alloc
```

The dump lands in **`/tmp/porta_node.log`**, not on the script's stderr —
`porta_sync_host.sh:94` runs the node as
`/tmp/oevm_porta … >/tmp/porta_node.log 2>&1 &`. Reading the script's own stderr
returns zero rows and looks exactly like "the feature does not work". Naming the
sink is the whole of the fix (see `instrument-failure-reads-as-system-failure`).

Window: leg 8's `dbg/req` profile, closed by the node itself.
`mprof[window]: methods=190 activations=14136145 overflow=0 cap=256`.

## The number stage 2 needs

```
distinct allocating methods: 64
```

**64 of the 190 methods that ran allocated at all**, against 740 `CompiledMethod`s
in the image. That is the D8 sizing input, and it lands at the small end of the
range the design bracketed: the allocating population is roughly a third of the
*active* method set and under a tenth of the image.

`allocOverflow=0` — the table did not saturate, so no rows are missing.

## The no-frame bucket did not materialise

```
mprofa[window]: noframe_arena=0/0 noframe_cheap=0/0 allocOverflow=0
```

**Zero.** Design §8's third risk — that `current_frame` is absent or misleading at
allocation sites, and that a dominant no-frame bucket would mean the table is not
answering the question — does not bite on this path. Every allocation in the
window happened inside a frame.

Stated with its limit: this is one window on one host path. Boot-time allocation
happens before `mprof_start()`, so the window structurally cannot see it, and a
zero here is **not** evidence that boot allocates nothing. It is evidence that
nothing *inside a profiling window* allocates frameless.

## Totals

| heap | bytes | allocations |
|---|---|---|
| arena | 79 600 | 1 374 |
| C heap | 915 | 229 |

The arena carries **99% of the bytes**. The C heap is a rounding error by volume
— which is consistent with #190's finding that the C-heap leak mattered because
it *never came back*, not because it was large.

## Top of the table

| arenaB | arenaN | cheapB | cheapN | % | method |
|---|---|---|---|---|---|
| 14 496 | 302 | 0 | 0 | **18.0%** | `Block_Kernel_Cbor_block40>>value:` |
| 8 064 | 168 | 654 | 168 | **10.8%** | `Kernel>>valueCompare:with:` |
| 6 528 | 102 | 0 | 0 | 8.1% | `Kernel_Cbor>>appendColl:into:` |
| 4 160 | 40 | 0 | 0 | 5.2% | `Kernel_OrderedCollection>>init_new` |
| 2 928 | 61 | 261 | 61 | 4.0% | `Kernel_Cbor>>encodeSymbol:into:` |
| 3 136 | 56 | 0 | 0 | 3.9% | `Kernel_ByteBuilder>>init_new` |
| 2 856 | 51 | 0 | 0 | 3.5% | `Kernel_Cbor>>encodeEntry:value:keys:vals:depth:` |
| 2 592 | 54 | 0 | 0 | 3.2% | `Kernel_Cbor>>encode:into:depth:` |
| 2 448 | 51 | 0 | 0 | 3.0% | `Block_Kernel_Cbor_block39>>value::` |
| 2 304 | 36 | 0 | 0 | 2.9% | `Kernel_Cbor>>encodeMap:into:depth:` |
| 2 064 | 14 | 0 | 0 | 2.6% | `Kernel_ByteBuilder>>grow` |
| 1 648 | 10 | 0 | 0 | 2.0% | `Kernel_ByteBuilder>>frozenBytes` |

## Two findings

### 1. It corroborates #163 from the other side, and independently

#163 established by **activation counting** that a CBOR encode's cost is byte
plumbing rather than the recursive descent — one `acc add: b` costs four
activations, and `ByteBuilder`'s doubling growth plus `frozenBytes`' exact-size
copy are two thirds of a flat encode, while the CBOR walk itself is 0.5%.

This measures **bytes**, not activations, and the same cluster is on top: the
encoder's blocks and collection appends take the first three places, and
`ByteBuilder`'s `init_new` + `grow` + `frozenBytes` together account for **8.5%**
of all attributed bytes across 80 allocations. Two independent instruments, two
different units, the same answer. That is worth more than either alone — and it
means #163's proposed fixes (size the accumulator, delete the map re-copy, make a
byte-append cost one activation) are now backed by an allocation argument as well
as a call-count one.

### 2. NEW: the C heap is almost entirely comparison scratch

Only **two** methods allocate on the C heap at all:

| method | cheapB | cheapN | share of C heap |
|---|---|---|---|
| `Kernel>>valueCompare:with:` | 654 | 168 | **71%** |
| `Kernel_Cbor>>encodeSymbol:into:` | 261 | 61 | 29% |

Both are on the map-key path. The C heap on this node is therefore not a
general-purpose heap at all; it is scratch for handling map keys.

> **CORRECTED 2026-08-12 (see `2026-08-12-allocation-attribution-c6.md` §5.3).**
> This paragraph originally read "CBOR keys are sorted by their **encoded** bytes,
> so every key comparison encodes to compare", and that is wrong about
> `valueCompare:with:`. There are **two** orders: the encoder sorts by encoded
> bytes via `compareBytes:with:` (`kernel.nsl:854`), while `valueCompare:with:` is
> the **Map's own** order — its construction sort (`kernel.nsl:439`) and its
> binary-search key lookup (`kernel.nsl:393`), neither of which encodes anything.
> What actually allocates is narrower and more fixable: rank 6 compares two
> Symbols as `byteCompare: a printString with: b printString` (`kernel.nsl:3315`),
> because `dispatch.c`'s `PRIM_at`/`PRIM_size` fast paths cover String and
> RawBytes but not Symbol — **so every Symbol comparison materializes two Strings
> to read their bytes.** #204 was filed against the original sentence; the
> corrected reading is in its answer.

That is a sharper target than "the C heap leaks" ever was, and it is exactly what
#190 could not say: `sys/cheap` reported outstanding blocks climbing, and this
names the two methods that could possibly be responsible. **This is the capability
the design was built for, demonstrated on its first real run.**

## What this does NOT establish

- **Nothing about the target.** This is the host path — the union of everything
  `porta_sync_host.sh` exercises, with a posix backend. The C6 runs one app in
  steady state and its allocating population may be smaller (which would make
  stage 2's table cheaper) or larger (which would mean 64 was never the bound it
  looks like). Task 5 measures it.
- **Nothing about `gc_allocate`'s call frequency in wall-clock terms**, which is
  the number design D4 admits it does not have and which decides whether
  always-on is affordable. 1 374 arena allocations across a 14.1 M-activation
  window is a ratio of about **1 allocation per 10 300 activations**, consistent
  with D4's two-orders-of-magnitude projection — but that ratio is not a cost,
  and the cost was not measured here.
- **Nothing about outstanding bytes.** D2 is cumulative by construction: a method
  at the top of this table may free everything it allocates. Pairing it with
  `sys/cheap`'s aggregate is what makes a leak nameable.
