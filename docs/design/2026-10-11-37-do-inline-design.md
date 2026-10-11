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
  closure that captures `e` would capture the element register, which the loop reuses (§Spike: the probe
  read garbage). So **a block whose nested closures name its params or temps keeps the send**. The arms
  of always-inlined selectors are not closures and do not count. The debugger
  sees `e` through `inlined_vars`. The `do:` frame and the block frame no longer exist, so a breakpoint
  in the kernel's `do:` no longer fires for an inlined site.
- **P5 — Code size is a gate.** Each literal `do:` body is emitted once per guarded receiver, plus the
  closure for the fallback: three copies for OC + Array. Measured (§Spike): the bell +0 B (it has no
  site), DeltaBlue +0.7 %.

## Spike

Branch `37-do-inline-spike` (from master after `OP_ITER_AT`). The spike puts the real `IS_VT` in the
VM, and the compiler emits P1 for OrderedCollection and Array when `NSL_DO_INLINE` names the method
(`all` for every app method; kernel source excluded). Rows on the WeAct, one image, three boots:

| row | what | ms (×3 boots) | vs MicroPython (1491) |
|---|---|---:|---:|
| ocDo | control: today's send + kernel `do:` (ITER_AT) | 5681 | 3.8× |
| **ocDoInline** | P1, OC hit | **1087** | **0.73×** |
| arDo | the same over a 10-element Array | 6393 | 4.3× |
| **arDoInline** | P1, Array hit (one OC guard missed first) | **1155** | **0.78×** |
| ivDo / ivDoInline | Interval: both guards miss, then the send | 7340 / 7506 | +2.3 % |

The record, with the arithmetic, is `docs/spikes/2026-10-11-37-do-inline/README.md`. **The guard costs
~0.55 µs per call on a hit (~5 % of ocDoInline) and ~1.7 µs per call on a miss.** Static knowledge would
buy at most that 5 %. On these numbers P1's guard is the answer, and the deferred alternative stays deferred.

Correctness: the full nsl suite with `NSL_DO_INLINE=all`, 293 passed / 0 failed (opti7040; the aux gates
that need 32-bit headers, pytest or cmake did not run there).

**Code size (step 3), C6 layout, `NSL_DO_INLINE=all`.**

- **The bell: +0 B.** `ShipsBellNodeC6.nsl` and `lib/Bells.nsl` contain no collection `do:` with a literal
  block. Their `do:` lines are `on:do:` handlers and `whenever:do:`. The images are byte-identical. None of
  the board nodes under `nodus/oevm/zephyr/` has a site either. Today's node code iterates with
  `to:do:` or through kernel helpers, so on the nodes P1 buys nothing and costs nothing.
- **DeltaBlue** (`tests/05_somns_benchmarks/07_deltablue.nsl`, the densest real program): 7 sites
  lowered, 178,736 → 180,064 B (**+1,328 B, +0.7 %, ~190 B a site** for two guarded copies, with the
  fallback closure kept). perfstex: 1 site, +352 B (a ten-statement body).
- No block disappears from the image: the fallback still needs the closure. Only a static-knowledge
  site (P1's deferred alternative) would drop it.

**Found while building the spike.**

- **A closure inside the inlined body that captures `e` reads a register the loop reuses.** The first
  probe printed `6.37e-322` where the send printed `6`. The spike refuses those blocks and keeps the
  send (`do_inline_wanted`). This is the limitation at `finalize_contexts` ("inlined-loop captured
  locals remain NOT upward-safe"), and **plain `to:do:` has it today**: `1 to: 3 do: [:e | blocks add:
  [e]]` and summing the closures answers 12, not 6. That bug is separate from this design: **#417**
  (P3, found-during #37).
- **The fallback's closure still forces the method's captured temps into a heap Context.** `sum:: sum + e`
  in an inlined body reads and writes `sum` through `LD_CTX`/`ST_CTX` because the fallback copy captures
  it. This is no regression (the send captures it too), but the inlined path does not get registers.
  Only static knowledge would fix it.

## Questions for David

1. **OC and Array both, or OC only?** Both, I lean. The Array arm adds ~95 B a site and costs an OC hit
   nothing (its guard is never reached). It costs a miss one more `IS_VT` (~0.3 µs). The Array row gains 5.5×.
2. **Slot indices by name (P2) now, kernel-method inlining later?** By name now, I lean. The spike's
   lookup is four lines, and it already fails the compile if the kernel renames a slot. Inlining the kernel's
   method is the more general mechanism. It is worth building when a second kernel method wants the same
   treatment (`collect:` and friends, Q4), not before.
3. **Off in debug builds?** The hot-patch precedent (#313) makes debug builds the faithful ones. An
   inlined `do:` cannot be stepped into, and a patched kernel `do:` would no longer affect inlined
   sites. My lean: on in both, because the debugger shows the inlined `e` and patching the kernel's
   `do:` is not a thing anyone does. But it is your call.
4. **`collect:` / `select:` / `inject:into:`.** They stay sends (Q3 of the iter-at design) unless
   profiles say otherwise. No board node has even a literal `do:` site today (P5), so the case for
   any of this on the nodes is the benchmarks' and the tutorials', not the bell's.
5. **New: is this worth landing now, given that no node uses it?** It is ~70 lines of compiler and one
   opcode, the suite is green with it on everywhere, and it closes ocDo to 0.73× MicroPython. Against that,
   every literal `do:` in app code changes shape, and the fix for #417 (closures capturing an inlined
   local) belongs to the same machinery. Options: (a) land P1 as it stands, keeping the send for the
   capturing blocks; (b) fix #417 first, then land P1 without the capture exception; (c) park it on the
   branch until an app has a hot `do:`.
