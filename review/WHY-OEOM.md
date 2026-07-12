# Why OEOM — the kernel that survived the graveyard

*Companion to `WHY.md`. Written 2026-07-11, prompted by the question "where do regular
objects keep their vtable?" — whose answer turned out to be the whole story of why this
VM is small. Code anchors are to tuvm at the time of writing.*

---

## The claim

Piumarta & Warth's *Open, Extensible Object Models* (VPRI, 2008 — the "OEOM" paper)
claims a complete object system needs only **three structures and one operation**:

- an **object**: a pointer whose word 0 names a vtable;
- a **vtable**: itself an object, holding selector→method bindings plus a `delegate`
  pointer for lookup fallback;
- a **binding**: one selector→method pair;
- and **send** = look the selector up through the vtable/delegate chain, then apply.

Everything else — classes, inheritance, metaclasses, even the lookup rule itself — is
built *inside* the system out of those parts. The tower is closed by one fixed point:
the vtable-of-vtables is its own vtable. That exact line is in the code:

```c
/* kernel_boot.c:86 — the tower stops here */
const struct Vtable Vtable_VtableVtable = { { (oop)&Vtable_VtableVtable }, ... };
```

This is Smalltalk-80's metaclass tower boiled down to its minimum: a few hundred lines
of C, and not a toy.

## What a "regular" object costs

The question that prompted this note. A user-class instance is (`oevm.h:229`):

```c
struct ObjectInstance { struct Object base; oop slots[]; };
```

— **one word** (the vtable pointer) plus its slots. That word is the *entire* per-object
cost of the object model, and it is a word every design would spend anyway, because it
does triple duty:

1. **dispatch** — `send` starts its lookup there;
2. **GC layout** — the precise collector reads it to know slots-vs-raw-bytes-vs-float
   (`mark()` cannot walk the heap without it);
3. **access control** — `check_access_allowed` keys off the defining vtable.

The class's vtable is **shared and flash-resident**. The solidifier emits every nsl
class as a `const struct Vtable` in the image — `Vtable_Kernel_OrderedCollection`,
`Vtable_Kernel_Map`, one per class plus a `Vtable_Meta_*` twin for class-side methods
(~200 in the current image, vs the ~14 hand-written builtin vtables in
`kernel_boot.c`). Path A: `.rodata` in the binary; Path B: inside the `.oimg` blob.
Either way: XIP, `is_in_blob`, never marked, never copied, **zero RAM**. A thousand
instances of a class share one flash vtable; each pays one word.

So "the VM uses lots of vtables" decomposes as: a ROM line item (~200 const structs),
plus one RAM word per object that the GC needed regardless. The builtin count (~14) is
already at the floor — roughly one per distinct physical layout the collector must
distinguish. There is no meaningful "fewer vtables" design point below this one.

*(The one runtime exception: Newspeak nested classes bound to an enclosing instance —
`obj Inner new` — can't exist in a static image. `dispatch.c`'s BoundClass is a tiny
heap wrapper pairing (nested class's flash vtable, enclosing instance) that funnels
into the same OP_NEW path. The vtable itself is still the image one.)*

## What OEOM buys this project

**One choke point.** Because literally all behavior flows through `send`, anything
bolted onto dispatch applies uniformly to the whole system. Each of these was
implemented *once*, at lookup, instead of per-type:

- the Newspeak encapsulation check (`check_access_allowed`, on every send — verified:
  the method cache stores only the resolved method, never a cached access verdict);
- the DNU → crash-record path the supervisor consumes;
- the mirror protocol / in-image debugger;
- capability confinement's enforcement surface.

The alternative for a VM this size is Berry's shape: tagged unions + C `switch`
dispatch. Legitimate — but then encapsulation, mirrors, and confinement each get
re-implemented per type, per call path. For a project where **confinement is the
product**, the uniformity is load-bearing, not overhead.

**It survived the graveyard.** Five attempts churned through languages, parsers, VMs,
and control-flow semantics (`WHY.md`). The complexity balloon that killed ns-oevm was
NLR + fibers + reify-on-escape — *control flow*. The OEOM kernel crossed from ns-oevm
into tuvm essentially intact and never needed rework. A kernel that stays stable while
everything around it is torn up four times is earning its keep.

**It is minimal in exactly the dimension a 256 KB part cares about.** Three structs;
one word per object that doubles as the GC descriptor; class metadata compiled to
const flash.

## The honest caveat: we use the M, not the O

The *O* in OEOM — "open" — is the part tuvm deliberately does not use. Piumarta's
pitch was a **mutable** object model: change the lookup rule at runtime, give single
objects their own dispatch semantics, grow prototypes into classes. That openness is
what the STEPS project wanted (self-describing systems in ~20K lines).

tuvm takes the opposite stance and **freezes** the model: vtables solidified to const
XIP structures; builtin delegates hardcoded to `NULL` (decoupled-XIP decision, MB
Task 2); dispatch semantics pinned. What we inherited is the minimalism and
uniformity, not the runtime malleability — and that is the right theft. On a device
that must run unattended for years, "the object model cannot change out from under
you" is a feature. The escape hatch (mirror primitives) is kept for the debugger, as
granted capability rather than ambient power.

The deeper compliment to the design: it stayed powerful *after its headline feature
was removed*. Most object models are load-bearing in ways you discover only when you
try to freeze or shrink them. This one shrank to a few flash-resident structs and one
C function — and still carries a metaclass tower, a capability model, and an in-image
debugger on its back.

## Reference

Ian Piumarta and Alessandro Warth, *Open, Extensible Object Models*, VPRI Technical
Report TR-2006-003-a; also in *Self-Sustaining Systems* (S3) 2008. The `id` object
model / COLA lineage. Piumarta's minimal-kernel work is the substrate oevm was built
on; nsl-tuvm is that kernel, kept small on purpose.
