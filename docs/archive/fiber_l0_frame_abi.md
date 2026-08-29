> **HISTORICAL — archived 2026-08-29.** Fibers, NLR and growable stacks were DELETED in Milestone A (2026-07-04, `docs/MILESTONE_A.md` §1); the VM is run-to-completion. What this document built and which SURVIVED is the flat non-recursive interpreter and the heap `Context` layer (`MILESTONE_A.md` §2). Read it for how the interpreter got flat, not for what the VM does. Current architecture: `docs/vm.md`.

# L0 — Frame / bytecode ABI contract (the de-recursify foundation)

**Status:** **L0 FROZEN, 2026-06-30.** Contract complete; fork §6 decided (Option B, full
upward closures); encoding implemented in `src/frame_abi.h` (accessors + reserved `Context`);
acceptance proof `tests/test_frame_abi.c` green (plain + ASan+UBSan). First deliverable of the
Option-C rewrite (`fiber_implementation.md` §7). **Next: L1** (Stack object + `Context`/boxing
+ precise roots), the first layer to execute against this contract. Remaining L0 acceptance
item §7d (both image paths agree on the encoding) lands with L4, which is the first consumer to
change either path.
**Branch:** `derecursify-fibers` (oevm). **Date:** 2026-06-30.
**Gate cleared:** the §10 spike is green (non-recursive CALL/RET, base-relative links
survive 10 reallocs, 2-fiber `ABABAB`) — clean under ASan+UBSan.
`docs/spikes/fiber_spike.c`.

> Reconciliation note up front: oevm is a **register VM** (per-frame `regs[]` window,
> register-operand bytecode, result delivered into a caller *register*), not a stack
> machine like Toit or the spike. The spike validated the *mechanics* (relative links,
> flat dispatch, stack-swap); this contract maps those mechanics onto oevm's register
> frame. Staying register-based is what the design doc's §5 already assumes ("generalise
> the hijack's `src2` decode" for RET) — this is not a reversal of that decision.

---

## 1. What L0 fixes (and what it does not)

L0 is the **contract** every other layer depends on: the on-stack frame layout, how
cross-frame references are stored, how a return delivers its result, the unwind-descriptor
shape, and the bytecode delta. L0 is *specification + the struct/encoding changes that
both the compiler and the VM compile against* — it does **not** include the non-recursive
dispatch loop (L2), the unwinder (L3), or combinator lowering (L4). Those consume L0.

The single irreversible decision L0 bakes in: **cross-frame links are base-relative,
not absolute pointers** (`fiber_implementation.md` §6a). Everything below follows from it.

---

## 2. Current frame (what we are changing)

`struct MethodContext` (`src/oevm.h:113`), bump-allocated on the flat `oop vm_stack[]`
(`src/vm.c:34`, default `VM_STACK_MAX_WORDS=16384`); `vm_stack_ptr` is the bump cursor;
`current_frame` is the live frame. Frame = fixed header + `regs[num_registers]`:

| Off | Field | Today | Kind |
|----|-------|-------|------|
| 0 | `vtable` | NULL fast-path / `MethodContextVtable` when reified | sentinel |
| 1 | `prev_frame` | **absolute `oop`** → caller `MethodContext` | **dynamic link** |
| 2 | `saved_pc` | `const uint32_t*` into the method's immutable instructions | return addr |
| 3 | `home_ctx` | **absolute `oop`** → defining context (self for a method) | **lexical link** |
| 4 | `closure` | `oop` → the `BytecodeClosure` being run | heap ref |
| 5 | `gen_tag` | 32-bit lifecycle generation (union, 32/64-safe) | scalar |
| 6 | `cleanup_cl` | `oop` → optional `ensure:` block | heap ref |
| 7..| `regs[]` | R0=receiver, R1..=args, then temps | register window |

Result delivery today: caller's `OP_SEND`/`OP_SEND_IMPLICIT` carries the **destination
register in the `src2` byte** (bits 24–31). The Zephyr hijack already decodes it and does
`ctx->regs[suspended_src2] = result` (`src/vm.c:392-398`), accounting for the trailing
`InlineCache` slot on `OP_SEND`. **That decode IS the de-recursified RET** — L0 promotes it
from a hijack special-case to the universal return protocol.

What breaks under growable/relocatable stacks: **`prev_frame` and `home_ctx` are absolute
pointers into `vm_stack`.** If the stack `realloc`s (the whole point — cheap growable
per-fiber stacks, §6a), every such pointer dangles. `saved_pc` is fine (points into the
immutable program, which never moves — this is exactly Toit's frame-marker invariant).

---

## 3. L0 contract — the new frame

Header unchanged in *shape* (offsets stay 0..7 for 32/64 parity and to keep the
"frame is an Object payload" reification trick). Two fields change *meaning*:

### 3.1 `prev_frame` → base-relative index (SMI)
Stored as a tagged SMI = the caller frame's **word offset from the stack base**
(`caller - vm_stack`). `NULL`/0-equivalent stays the bottom sentinel. Walking the dynamic
chain (GC frame walk `gc.c:508`, NLR target search, the new L2 pop-until-target loop)
becomes `vm_stack + SMI_VAL(prev_frame)`. A `realloc` that moves `vm_stack` needs **zero**
fixups — only the base pointer changes, every stored offset stays valid. (Spike demo 2
proves exactly this for the analogous `saved_fb`.)

### 3.2 `saved_pc` — unchanged (absolute, into immutable code)
Stays a raw `const uint32_t*`. The program is immutable and never relocates; saved PCs are
the one legitimate absolute reference on a frame (Toit's frame marker is the same idea).
GC/stack-walk recognises it by position, not by tagging.

### 3.3 Result delivery — register, addressed via the caller's saved instruction
RET (L2) reads the caller frame's `saved_pc`, backs up to the SEND that suspended it,
decodes `src2`, and writes the result into **the caller frame's** `regs[src2]`. The caller
frame is found via the (now relative) `prev_frame`. No C value is returned; no C frame
unwinds. This is the `src/vm.c:392-398` logic, generalised and made target-independent.

### 3.4 `home_ctx` — **THE OPEN FORK (needs your call, §6)**
`home_ctx` is the lexical link an `OP_LD_LEXICAL depth,idx` walk follows. It points either
(a) to an **ancestor frame on the same fiber stack**, or (b) to a **reified heap context**
once a closure escapes its home frame's lifetime (oevm reifies via the `vtable` patch +
`gen_tag` dangling-return guard). (a) must relativise like `prev_frame`; (b) is a normal
heap `oop`. The two cases need a discriminator. See §6 for the proposed resolution and the
decision I need.

### 3.5 Block (closure) representation
A `BytecodeClosure` captures `home_ctx` + `home_generation` (`struct Closure`,
`src/oevm.h:148`). Under §3.4 the captured link uses the same relative/heap discriminator.
Toit makes blocks pure base-relative Smis; oevm closures are heap objects (they carry an
entry point), so oevm's block is "heap closure object whose `home_ctx` obeys §3.4" — not a
bare Smi. No change to closure *allocation*; only how the home link is stored/followed.

---

## 4. Unwind-descriptor shape (consumed by L3, fixed here)

One descriptor format for NLR / `ensure:` / (future) exceptions, mirroring Toit's
`UNWIND_IMPLEMENTATION` reason encoding (`interpreter_run.cc:1281`):

- **target**: a base-relative frame index (same currency as `prev_frame`) — the frame to
  land on. Falls out of §3.1 for free (the reason §6a.3 cites for adopting relative now).
- **reason/payload**: small tagged code — `RET` value, NLR (+ result), branch (+ bytecode
  index), exception (+ value). L0 reserves the encoding; L3 implements the walk.
- **cleanup**: `cleanup_cl` (existing) is replayed as ordinary bytecode by L3, not via a C
  call (Toit's "run the `finally` as bytecode then re-enter the unwinder").

L0 only *reserves and documents* this; no unwinder code lands until L3.

---

## 5. Bytecode delta (reserved here, emitted in L4)

No opcode renumbering of the live set (`enum Opcode`, `src/oevm.h:241`). L0 reserves:

- **`OP_BLOCK_INVOKE`** — invoke a block/`value*` as a frame push (already core to L2;
  `value*` combinators in `dispatch.c` collapse into it).
- **Inline branch family** for L4 lowering of `ifTrue:`/`ifFalse:`/`and:`/`or:` — reuse
  existing `OP_JMP`/`OP_JMP_IF_FALSE`; likely no new opcode, just compiler emission.
- **Loop back-edge** for `whileTrue:`/`whileFalse:`/`to:do:`/`timesRepeat:` — also
  `OP_JMP`/`OP_JMP_IF_FALSE` + a counter register; no new opcode expected.
- **`OP_UNWIND`** — re-enter the L3 unwinder after a replayed cleanup block (Toit's
  `UNWIND` bytecode). New opcode.
- **Removed:** `OP_SEND_IC` / `send_ic` — confirmed dead (`fiber_implementation.md` §5;
  compiler never emits it, would SIGSEGV the read-only Model B blob). Retire in L6, but L0
  records it as not part of the new ABI.

`OP_LD_LEXICAL`/`OP_ST_LEXICAL`/`OP_LD_SENDER` keep their opcodes; their *semantics* shift
to the §3.4 relative/heap link discipline.

---

## 6. The one decision I need: how `home_ctx` (the lexical link) is represented

Every other field has a forced answer. `home_ctx` does not — and a second spike
(`docs/spikes/home_link_spike.c`, green) reframed it from an *encoding* question into a
**closure-semantics** question. Two findings retired my first draft:

- **A bare relative offset is UNSAFE cross-fiber.** A closure's home can live on a
  *different* fiber's stack — and that is the fiber main path, not a corner (the entry block
  is created on the spawner's stack and run on the new fiber's stack). The spike's closure,
  invoked on fiber B, read B's slot (`999`) instead of its home value (`42`). A relative
  offset only means something against a *known* stack base.
- **In-place reification + `gen_tag` means there is no "heap context" today.** oevm patches
  a frame's `vtable` in place (`vm.c:254/519/851/944`); the frame stays on the stack and dies
  with it; an escape past the home's pop is *guarded* by `gen_tag`, not kept alive on a heap.
  So my draft's "SMI-vs-heap, reuse the existing reification" was a mirage — that heap path is
  new machinery, not a reuse. Also: an SMI in `home_ctx` is *already* a stop-sentinel in the
  live lexical walks (`vm.c:507`, `vm.c:1156`), so a relative-SMI home would break them.

Spike results (each row a candidate representation):

| representation | cross-fiber | escape-past-pop | cost |
|---|---|---|---|
| bare relative offset (old Opt 1) | ❌ reads wrong stack | ❌ | cheapest — **ruled out** |
| **offset + stack-id** | ✅ `42` | ❌ stale slot (`777`) | a stack-id word per link + a generation/liveness check |
| **reify-on-escape (Toit)** | ✅ `42` | ✅ `42` | a heap copy of live outer vars at escape |

**The growable-stacks win is NOT at stake here.** It comes entirely from `prev_frame` being
relative (§3.1) — always same-stack, unambiguous, already proven by the §10 spike. Both
surviving home-link options keep growable per-fiber stacks. So this fork does not cost us
growth; it only decides closure *reach*.

The decision was a **language-semantics** choice:

- **Option A — keep oevm's current downward-only closures.** A block may read outer vars while
  its home frame is live; using it after the home returns is an *error* (what `gen_tag` already
  catches). Representation: `home_ctx` = {stack-id, relative offset} + generation check.
  Cross-fiber safe; escape-past-pop stays the error it is today; no heap reification.
- **Option B — full upward (Toit-class) closures.** A block may outlive its home frame; outer
  state survives. Representation: **reify-on-escape** (box live outer vars to the heap at
  escape; `home_ctx` follows the heap object). Strictly more capable; costs a heap copy at
  escape and new reification machinery; turns the `gen_tag` dangling-return *error* class into
  valid behaviour.

### 6.1 DECIDED — Option B (full upward, Toit-class closures). 2026-06-30, David.
Rationale: closures are **foundational** — pay the hard work up front; the runtime capability
compounds as the language grows (full Smalltalk/Toit block semantics, continuations,
callbacks held across time). This is the strategic-framing "foundational axis paid up front"
call, applied deliberately even though B (unlike no-globals/capabilities) *could* be retrofitted.

**Encoding under B — the SMI-vs-heap discriminator (now real).** `home_ctx` holds either:
- a **relative offset (SMI)** — an on-stack home on the *current* fiber, fast path, valid only
  while same-fiber and the home is live; or
- a **reified `Context` heap object (oop)** — produced by reify-on-escape; cross-fiber- and
  pop-safe.

The lexical walk / GC discriminate by `IS_SMI`. This is exactly Toit's model (on-stack blocks
base-relative; escaping blocks reified). It does **not** need the {stack-id} word of Option A:
the invariant "a relative (non-reified) home is only ever followed on its own fiber" holds
*because escape forces reification first* — so a bare relative offset always resolves against
the current fiber stack, correctly. The home-link spike's cross-fiber hazard is closed not by
carrying a stack-id but by reifying before the cross-fiber handoff.

**What B obliges the rewrite to build (downstream of L0, listed so L0 reserves for it):**
- An **escape trigger**: the points where a closure may outlive its home or cross fibers —
  primarily `fiberCreate:`/storing a block into a `Fiber`, and storing a block into any heap
  slot/returning it upward. At those points, **reify** the home: box the live outer variables
  into a heap `Context` and repoint the closure's `home_ctx` (and any nested relative links) at
  it. (L1 builds the `Context` object + boxing; L5 wires the fiber escape sites.)
- The live lexical walks that currently treat **SMI `home_ctx` as a stop-sentinel**
  (`vm.c:507` OP_NLR, `vm.c:1156` `get_top_level_method_closure`) must be reworked: under B an
  SMI means "relative link — follow it (resolve to a frame on the current fiber stack)", and a
  *reified* `Context` (heap oop) is the new chain element. Termination is "no home" (NULL) or
  the method frame, not "is an SMI".
- GC must trace reified `Context` objects as ordinary heap roots; on-stack relative homes are
  covered by the existing frame walk (made precise in L1).
- `OP_NLR` target resolution must handle a reified-context target (the `^` home may be a heap
  `Context`, not a live frame) — this is where "deletes the dangling-return error class" lands.

> Note: B is the strictly-larger commit. The layering still holds — L0 freezes this encoding;
> L1 builds the `Context`/boxing; L2 the flat dispatch; L3 the unwinder over reified homes; L5
> the escape sites. We are NOT trying to ship reification in L0; L0 only fixes the encoding and
> the contracts above so L1+ can build against them.

---

## 7. Acceptance for L0 (when is the contract "done")

L0 is done when: (a) §6 is decided (✅ Option B); (b) `struct MethodContext` +
`struct Closure` carry the new link encoding with helper accessors
(`frame_link_get/set` for the relative dynamic link; `home_link_get/set` discriminating
relative-SMI vs reified-`Context` per §6.1) that centralise the relative↔address
conversion in one place; (c) the `Context` heap-object shape is *reserved* (the reify-on-escape
target — L1 builds it, but L0 fixes its identity in the encoding so the discriminator is
total); (d) the serializer / both image paths (Path A solidified C, Path B `.oimg`) agree on
the encoding (no live consumer changed yet — L4 does that); (e) a focused unit test asserts a
relative link survives a simulated `vm_stack` move (the §10-spike property, re-asserted against
the *real* `struct MethodContext`). No behavioural change to the running engine yet — L1 is the
first layer that executes against the new contract.

---

## 8. References
- Design: `docs/fiber_implementation.md` (§5 audit, §6a relative-link rationale, §7 layers).
- Spike: `docs/spikes/fiber_spike.c` (the green L0 mechanics gate).
- Code: `src/oevm.h:113` (`MethodContext`), `:148` (`Closure`), `:241` (`Opcode`);
  `src/vm.c:200-227` (frame alloc), `:392-398` (the result-register decode = future RET),
  `:876-883` (`exit_frame` = the C return L2 removes); `src/gc.c:508` (frame walk).
