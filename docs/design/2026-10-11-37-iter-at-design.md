# `OP_ITER_AT`: the kernel's `do:` in one dispatch per element (rete#37)

> Status: **DRAFT for David**, 2026-10-11. Follows the spike record
> `docs/spikes/2026-10-11-37-ocdo-lowered/README.md` (branch `37-ocdo-spike`, not for merging)
> and David's direction of the same day: the kernel's own `OrderedCollection>>do:` on
> `OP_ITER_AT`, gated by the full nsl suite, **before** any lowering at call sites.

## Why

The spike hand-patched one row to use a fused iterate opcode and measured it on the WeAct
RP2350B (three boots, identical to the ms): `ocDo` 7668 ms (5.1× MicroPython), today's ISA
lowered by hand 2838 ms (1.9×), **fused 1219 ms (0.82×)**. A fused step is what reaches
MicroPython. The spike left one question open: how does the compiler come to emit the opcode
from kernel source, when no source syntax names it?

## Decisions (proposed)

- **D1 — The compiler recognises a shape the kernel already has.** No new syntax, no new
  selector. Both kernel `do:` methods are already in that shape:

  ```
  Array>>do:             1 to: self size do: [ :i | aBlock value: (self basicAt: i) ]
  OrderedCollection>>do: 1 to: count     do: [ :i | aBlock value: (contents basicAt: i) ]
  ```

  `compile_to_do_inline` (`nodus/nsl/tools/ns_compile_oevm.py`) emits `OP_ITER_AT` in place
  of its compare/branch/add skeleton when **all** of these hold, and otherwise emits today's
  loop unchanged:
  1. `to:do:` with no `by:` (step 1), and the block is a literal;
  2. every use of `i` in the body is the argument of `X basicAt: i`, for one `X`;
  3. `X` is `self`, a slot or a method local (not an expression), and the body does not
     assign it;
  4. the method is in the kernel (see D3).

  Each `X basicAt: i` in the body then reads the element register; it emits no `OP_AT`.

- **D2 — The opcode moves from the spike into the ISA unchanged**, at 0x1F, with the
  spike's register layout: `ITER_AT b, off`, R(b) next index, R(b+1) limit, R(b+2) `X`,
  R(b+3) ← element. Past the limit it branches by `off`; otherwise it loads the element
  and increments the index. Per element: `ITER_AT`, body, `JMP`.
  `X` and the limit are evaluated once, at loop entry. For the limit this is already
  true of every inlined `to:do:`. For `X` it is new, and **dialect row 33 makes it legal**.

- **D3 — Kernel-only, so a shape miss stays a compiler bug.** `OP_AT` is emitted for every
  `basicAt:` send and falls back to a real send on a non-indexable receiver, so a user
  class may define its own `basicAt:`. `OP_ITER_AT` has no send fallback: it aborts by name
  on a shape it does not expect. The kernel's discipline (`basicAt:` only on kernel Arrays,
  the same discipline `OP_AT`'s gate cites in `vm.c`) is what makes the abort a lowering
  bug, never a user error. Opening the rule to user code means giving the opcode a
  `basicAt:` send fallback. That belongs to the later call-site pass, with #395's receiver
  guard.

- **D4 — Modifying the collection during its traversal stays undefined, and it is
  memory-safe** (dialect row 33). The fused loop holds the array it started with. An array
  is replaced when it grows, never shrunk, and the limit never exceeds its size at entry
  (`count ≤ contents size` for an OrderedCollection, `self size` for an Array). So a body
  that adds, removes or stores reads stale or `nil` elements. It never reads out of bounds,
  and it never aborts. See Q1 for the alternative of failing fast, as Java does.

- **D5 — The fast path must not be lost silently.** A kernel edit that breaks the shape
  would fall back to the slow loop without a word. The gate is a compile test asserting that
  `Array>>do:` and `OrderedCollection>>do:` each contain one `OP_ITER_AT` and no `OP_AT`.

## What it reaches, and what it does not

`do:` and everything the kernel builds on it: `inject:into:`, `collect:`, `select:`,
`reject:`, `detect:ifNone:`, `includes:`, `size` on a generic Collection. The block is still
invoked once per element (`aBlock value: e`, a non-literal block inside the kernel method),
so the per-element cost is `ITER_AT` + a block round trip + `JMP`. The spike's 1219 ms row
also inlined the body at the call site, so the kernel-only result will sit between 1219 and
7668 ms. **The WeAct rerun measures how much of the 2.7× the block round trip keeps.**
Call-site lowering (inlining the block too) stays the next lever after that.

## Plan

1. `OP_ITER_AT` into the ISA: `oevm.h`, `vm.c` (from the spike, with the comment rewritten
   for the ISA), the compiler's opcode table, the liveness table (`_capture_regs` /
   the def-use map at `ns_compile_oevm.py:321`), the disassembler and the image map if they
   enumerate opcodes.
2. The D1 recogniser in `compile_to_do_inline`, plus the D5 test.
3. The full nsl suite (aux line green), Go gates, `porta_sync_host.sh`.
4. The WeAct rerun (build on msi, UF2 here), `ocDo` and the #296 rows against
   `optimizations.md` Opt #6.
5. Close-out: the opcode and the rule into `nodus/oevm/docs/optimizations.md` (Opt #7)
   and the ISA reference; the recogniser into `nodus/nsl/docs/`.

## Questions for David

1. **Fail fast, as Java does?** My recommendation is no. Java's `ConcurrentModificationException`
   needs a `modCount` slot on the collection, an increment in every `add:`/`remove…`, and a
   check after every body. The check cannot live in `ITER_AT`, because it sees the Array
   and not the OrderedCollection. So it adds about two dispatches per element: roughly the
   saving `ITER_AT` buys over today's ISA lowering, given back. Even Java makes it
   best-effort, not a guarantee. D4 already makes the undefined case memory-safe. If a
   check is wanted, it fits the debug-build-only precedent (#313, hot patch), and a real
   bug caught in the field is what would earn it.
2. **Kernel-only (D3) for now?** The alternative is a send fallback inside the opcode, so
   the rule can apply to user code as well. That means a second exit path in the
   instruction, for loops nobody writes yet.
3. **Is `inject:into:` worth a direct shape?** It is `self do: [...]` today: two block
   round trips per element (the `do:` block, then `b`). Leave it until the rerun says
   where the time goes.
