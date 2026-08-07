# Attributing profile cost to code, not to callees

**Date:** 2026-08-07 · **Follows:** `docs/design/2026-08-06-method-profile-over-porta-design.md`
(D1–D10) and its hardware gate `docs/results/2026-08-06-mprof-window-hardware.md`

## 1. The question this answers

The first real profile off a field node
(`docs/results/2026-08-06-mprof-window-hardware.md`) said four per-byte accessors are 61.2%
of everything the node does, and the whole byte/CBOR cluster is 76.8%. Every one of those
rows is `Kernel_*`. A reader — including the platform's own author — cannot tell from that
table whether the cost is something their code provoked or something the platform does on
its own.

**The counter is keyed by callee.** `mprof_bump(m)` (`nodus/tuvm/src/vm.c:628`) takes the
method being *entered*, so the table can only ever report what ran, never on whose behalf.
That is not missing data; it is the choice of key. No view or grouping recovers it: rolling
the 177 rows up by declaring class answers "99.98% is `Kernel_*`", which is true and useless.

What a reader needs is **inclusive** cost — charged to a method and everything it called —
beside the **self** cost already collected.

## 2. What already exists, and what does not

Verified 2026-08-06/07 rather than assumed:

- **`push_frame` (`vm.c:622`) is the single choke point** for every activation, and
  `mprof_bump` is already called from inside it. At that moment it holds the callee `m`, the
  caller frame `prev_ctx`, and the full dynamic chain via `ctx->prev_frame`.
- **`prev_ctx->saved_pc` is the exact send site.** The interpreter writes `ctx->saved_pc = pc`
  on the caller's frame immediately before dispatching a send (`vm.c:1025, 1061, 1240, 1329,
  1390`). Call-site attribution therefore needs no new data collection.
- **A source map already exists.** `nodus/nsl/tools/stmap.py` emits a `.stmap` beside every
  image — `{"functions": {"<Vtable>>selector>": [[bytecode_offset, line], ...]}}` — for both
  the oimg and solidified paths (`ns_compile_oevm.py:2224`). `PortaBridgeNodeC6.stmap` holds
  601 functions and was written by the build now on the board. Both ends resolve:
  `Kernel_Cbor>>encode:into:depth:` → `kernel.nsl:709`, `PortaBridgeNodeC6>>tick` →
  `PortaBridgeNodeC6.nsl:149`.

  **So annotated source needs no compiler change.** An earlier draft of this analysis claimed
  it did; that was wrong, and the correction is what makes rung B small.
- **What does not exist:** any notification to the profiler when the stack *shrinks*, and any
  record in the `.stmap` of which source *file* each qualified name came from (it carries one
  `source` field but holds methods from several files; the file is inferable from the class
  prefix, not recorded).

## 3. Decisions

### D1 — Every frame gets both numbers; no user/platform boundary is drawn

Inclusive and self are reported for all methods, kernel included. The reader draws the line
between "mine" and "the platform" by looking.

Three boundaries were considered — the innermost open `Spans` bracket (declared by the
programmer), the nearest non-`kernel.nsl` frame (inferred by the VM), and none. **None wins,
for now, because the platform's author and its user are the same person and both questions
are live at once:** "am I holding it wrong" wants call sites ranked, "is the platform slow"
wants kernel internals ranked. A boundary chosen now would be chosen without data.

This also avoids a genuinely hard classification: `kernel.nsl` holds both runtime primitives
a user cannot reason about (`ByteArray>>u8At:`) and library code a user calls deliberately
and could call differently (`PortaBridge>>checkIn`, `Cbor>>encode:`). Calling both "platform"
would hide the half a user can act on.

**What would earn a boundary:** a second app, written by someone who is not the platform
author, whose profile is unreadable without one.

### D2 — Inclusive is accumulated by counter differencing, not by charging every frame

Charging every frame on the stack for every activation is O(depth) on the hot path. Instead:

- one true global `g_mprof_acts`, incremented on every push;
- each open frame stamps `entry = g_mprof_acts` at push;
- at pop, `incl[slot] += g_mprof_acts - entry`.

**Exact, not estimated**, and O(1) at both ends. The per-activation cost stays the
hash-probe-increment it is today. This is what makes D1 affordable — the cost objection
against "charge every frame" was against the naive implementation, not against the number.

Rejected: deriving inclusive off-node from call-edge counts (the gprof approach). Edge-share
apportionment is an estimate that is wrong whenever a callee behaves differently per caller,
and cycles break it. Exactness is this profiler's main virtue and is not worth trading.

### D3 — Per-frame state lives in a parallel stack, not in `MethodContext`

`struct MethodContext` (`nodus/tuvm/src/oevm.h:135`) has a deliberately pinned layout: the
comments record keeping `regs` at offset 6 and holding the layout identical on 32- and
64-bit hosts. `regs` is the trailing array, so a new field can only go *before* it, which
shifts register indexing. **Do not grow it.**

The profiler keeps its own stack of `{entry, slot}` pairs, pushed and popped in lockstep with
`push_frame`. Its capacity is derived at `mprof_start` from the VM stack capacity divided by
the minimum frame size — **not an invented constant**, per the standing rule against tunables
without data.

### D4 — The profiler is told whenever the stack shrinks

Lockstep requires it, and today nothing provides it. `vm_protected_fire` and `vm_on_do`
(`vm.c:223`, `vm.c:294`) unwind by restoring `vm_stack_ptr` and `current_frame` wholesale —
N frames vanish in a single assignment, visiting none of them. There are roughly four such
sites including `OP_RET` and the `vm_stack_ptr = 0` reset.

Every one calls a single helper that closes frames down to the target. This is the most
invasive part of the design and the likeliest source of a subtle bug.

The alternative — letting unwound frames leak their depth counters — was rejected because it
degrades **silently and permanently within a window**: once a method's depth never returns to
zero, that method stops accumulating inclusive for the rest of the capture, and nothing in
the output says so.

### D5 — The window's edges are closed by walking the live stack

`mprof_start` zeroes the table while frames are already open; `mprof_stop` fires with frames
still open (the stop runs inside `profileWindowEnd`, so every frame below it is live).

Without handling this, **`main:args:` reports 0% inclusive when the truth is ~100%** — a
number that looks like data and is exactly backwards. So `mprof_start` walks the live stack
from the bottom and opens the frames already on it (entry = 0, depths set in stack order),
and `mprof_stop` walks it and closes those still open. O(depth), twice per window.

### D6 — Recursion is guarded by a per-slot active-depth counter

`Cbor>>encode:into:depth:` is recursive. Unguarded, an inner activation's range is contained
in the outer's and both accumulate it, so the inner window is counted twice.

A per-slot active-depth counter, accumulating only on the outermost `1 → 0` transition, makes
`incl[M]` count each activation that happened while `M` was on the stack **exactly once**,
however deep the recursion went. This is exact — the same definition gprof and perf use — not
an approximation. Cost: 512 B at 256 slots, and two branches on paths that already exist.

What it gives up is per-descent breakdown ("the third level of recursion cost this much").
Nothing wants that, and rung B would not recover it either, because cycles collapse there too.

### D7 — Self is a partition; inclusive is not. This is a presentation rule.

Inclusive costs never sum to the total, and this has nothing to do with recursion.
`main:args:` reads ~100%, `tick` 92%, `PortaBridge>>checkIn` 88% — all true at once, because
each contains the next. Added, they exceed 100% by a wide margin.

Therefore:

- **Self numbers may be drawn as shares of a whole** — stacked bars, partitions, "four
  accessors are 61.2%". Everything on the 2026-08-06 readout is a self number and stays valid.
- **Inclusive numbers may only be drawn as independent bars against the total.** Never
  stacked, never summed, never a pie.

This is recorded as a decision rather than a note because it is the part of the output a
future reader is most likely to get wrong, and getting it wrong produces a chart that looks
authoritative and is nonsense.

### D8 — The header reports true activations, and the resolver's compensation is deleted

`activations=` is currently `sum(g_mprof_n)` (`vm.c:491`), which is precisely why it excludes
overflowed activations and why the resolver has to add `overflow` back to get an honest
denominator (#165 Task 2, commit 4315fb0).

`g_mprof_acts` from D2 is the true count. The header reports it, and **the resolver's
`+ overflow` compensation is removed rather than left beside it** — a correction, not an
accretion. The `INCOMPLETE` banner stays: it reports missing *methods*, which is a different
fact from a wrong denominator.

### D9 — Call-site keying and annotated source are rung B, and rung A's data decides the table

Rung A does not change the table's key, so cardinality does not move and the capacity question
(the prior design's D9) stays deferred.

Rung B keys on `(caller method, caller saved_pc)` to attribute cost to a **send site**, joins
through `.oimg.methods.json` and `.stmap` to a source line, and renders the `.nsl` file with
an inclusive/self margin. All the data exists (§2). What it costs is cardinality: 177 methods
is plausibly 400–700 distinct call sites, against a table already at 0.69 load.

**Rung B therefore forces the capacity decision that has been deferred twice.** Running rung A
first replaces the estimate above with a measurement, so the choice between a 512-slot table
and Space-Saving displacement is made against a real call-site count.

## 4. Output

The dump row gains a field:

```
mprof[window]: methods=N activations=A overflow=M cap=C
mprof: <offset> <self> <incl>
```

`mprof_resolve.resolve()` returns self and inclusive with their percentages. The view sorts by
either: inclusive answers "where does the time go", self answers "what is hot". No
compatibility shim — both ends are rebuilt together.

## 5. Testing

The **C unit test owns the arithmetic**, because it is the only place the exact numbers are
assertable (nsl cannot read the table — the prior design's D5 keeps it write-only). It must
cover:

1. A known call shape with known inclusive relationships.
2. A recursive method that must not double-count (D6).
3. A frame open across the whole window reading ~100% (D5).
4. An unwind that discards several frames, after which the discarded methods still accumulate
   (D4) — the failure this guards is silent and permanent.

The nsl test asserts reachability only, the same split as #165 Tasks 4 and 5. The host suite
can run a real window and grep stderr, since the profiler is a build flag rather than a
platform feature — but note the host suite remains structurally blind to the ISR seam, so
nothing here says anything about behaviour under pulse-mailbox timing.

## 6. Deliberately not in this rung

- Any user/platform boundary (D1).
- Call-site keying, source annotation, and any change to table cardinality (D9).
- The counter's cost at realistic load (#166). It is unmeasured today and this design does not
  measure it — but rung A is the right place to measure it, because the table's shape is
  unchanged and the ON/OFF arms stay comparable.
- Recording the source *file* per qualified name in the `.stmap` (§2). Needed by rung B, not
  by rung A.
