# A literal `do:` block inlined at the call site, behind a class guard (rete#37)

> Status: **DRAFT, 2026-10-11, for David.** Follows `docs/design/2026-10-11-37-iter-at-design.md`
> (`OP_ITER_AT`, kernel-only, merged) and the queued question on #37 (2026-10-11 comment): what does it
> take to inline the `do:` block at the call site? The spike that prices it is §Spike below; its code is
> on branch `37-do-inline-spike`.

## Why

After `OP_ITER_AT`, ocDo is 5594 ms on the WeAct (3.75× MicroPython). About 4.4 of the 5.6 µs per
element is the block call: the kernel's `do:` activates the caller's block once per element. The
spike `docs/spikes/2026-10-11-37-ocdo-lowered/README.md` hand-patched the block into the caller and
measured **1219 ms (0.82×)**, with a stand-in for a class guard that the ISA does not have. That stand-in is
the gap: at `oc do: [:e | …]` the compiler does not know `oc` is an OrderedCollection.

## Proposed decisions

- **P1 — A guard, not static knowledge.** The call site tests the receiver's class at run time and
  falls back to today's send:

  ```
  r := receiver
  IS_VT t, <OrderedCollection vtable>, r ; JMP_IF_FALSE t, try_next
     count, contents := r's slots ; ITER_AT loop, the block's body inlined, e bound to the element
     JMP done
  try_next: … (the next guarded receiver, or)
  send:  r do: <the closure>          — today's code, unchanged
  done:  result = r
  ```

  `IS_VT a, k, c` is one new opcode: R(a) := (R(c) is a heap object whose vtable is constant k). The
  vtable reaches the method's constant pool the way `super` sends already put one there
  (`VTABLE:<id>`). The test is exact: an app subclass of OrderedCollection, which may override
  `do:`, takes the send. Static knowledge (a local assigned from `OrderedCollection new`, a typed
  slot) would drop the guard and the fallback. It is left out until the data says the guard costs
  enough to matter (§Spike, row `guard`). Nothing here forecloses it.
- **P2 — The kernel owns the layout.** The lowering reads `count` and `contents` from the receiver,
  so it binds every app to the kernel's representation of OrderedCollection. That is allowed: both
  are rebuilt together, and there is no fleet. It still needs one owner. The compiler resolves the slot
  indices by name from the kernel class (`class_slots`), never as numbers. A kernel change that renames or
  drops the slots fails the compile, as D5 of the iter-at design does for the kernel's own shape.
  (Stronger and later: inline the kernel's `do:` method itself, with `aBlock` bound to the literal block
  and `self` to the receiver register. Then the kernel's source is the single description, and the
  `ITER_AT` recogniser applies to it unchanged. That means general method inlining with a
  guard. The spike does not need it.)
- **P3 — Receivers: OrderedCollection and Array.** These are the two that `ITER_AT` already serves,
  and they share a loop (limit and indexable from two slots, or slot 0 and the receiver itself for an
  Array). Interval and Dictionary have their own `do:` shapes and stay sends until a profile names
  them. A miss costs one `IS_VT` + `JMP_IF_FALSE` per guard before the send (§Spike, row `miss`).
- **P4 — The body follows the rules already in force for inlined `to:do:`.** `^` in the block is
  the enclosing method's return. Block temps get registers and are reset each iteration. A nested
  closure that captures `e` captures the element register, as it captures `i` today. The debugger
  sees `e` through `inlined_vars`. The `do:` frame and the block frame no longer exist, so a breakpoint
  in the kernel's `do:` no longer fires for an inlined site.
- **P5 — Code size is a gate, measured on the bell.** Each literal `do:` body is emitted twice per
  guarded receiver plus the fallback: three copies for OC + Array. §Spike, step 3.

## Spike

Branch `37-do-inline-spike` (from master after `OP_ITER_AT`). The spike puts the real `IS_VT` in the
VM, and the compiler emits P1 for OrderedCollection and Array when `NSL_DO_INLINE` names the method
(`all` for every app method; kernel source excluded). Rows on the WeAct, one image, three boots:

| row | what | ms | vs MicroPython (1491) |
|---|---|---:|---:|
| ocDo | control: today's send + kernel `do:` (ITER_AT) | | |
| ocDoInline | P1, OC hit | | |
| arDo / arDoInline | the same over a 10-element Array | | |
| miss | P1 compiled, receiver an Interval: both guards miss, then the send | | |

Correctness: the full nsl suite with `NSL_DO_INLINE=all`.
Code size: the bell image with and without `NSL_DO_INLINE=all`.

_(results to follow)_

## Questions for David

1. **OC and Array both, or OC only?** Each guarded receiver adds one copy of every literal `do:` body
   and one `IS_VT` to every miss.
2. **Slot indices by name (P2) now, kernel-method inlining later?** Or go straight to inlining the
   kernel's method, which is the more general mechanism and the larger build.
3. **Off in debug builds?** The hot-patch precedent (#313) makes debug builds the faithful ones. An
   inlined `do:` cannot be stepped into, and a patched kernel `do:` would no longer affect inlined
   sites. My lean: on in both, because the debugger shows the inlined `e` and patching the kernel's
   `do:` is not a thing anyone does. But it is your call.
4. **`collect:` / `select:` / `inject:into:`.** They stay sends (Q3 of the iter-at design) unless
   the spike's numbers say otherwise.
