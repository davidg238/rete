# ns-oevm std-lib step 3b — Kernel module foundation + Collection + OrderedCollection

> **Status:** design ratified 2026-06-20 (brainstorm with David). Next: writing-plans → SDD.
> **Predecessor:** [step 3a](2026-06-20-ns-oevm-stdlib-step3a-kernel-object-array.md) (Object root + Array.ns, magic `PRIM_Array` prim). **Parent design:** [stdlib capability design](2026-06-20-ns-oevm-stdlib-design.md).

## 1. Goal & scope

Promote the flat 3a kernel (top-level sibling classes reached via a one-off magic `Array` primitive) into a real **`Kernel` module** — a class parameterized by its native capabilities (`Kernel usingPlatform: platform`) that nests the library classes — and add the first two collection classes on top:

- **In scope:** the `Kernel`-module foundation (restructure `lib/kernel.ns`, boot `Kernel` as the app's `platform`, retire 3a's `Array` scaffolding); `Collection` (abstract, functional); `OrderedCollection` (growable, Array-backed); `Array` joins `Collection`.
- **Deferred to 3c:** `String`, `Dictionary`, `Set`, boolean `hash`/`=`/`printString`, Streams.

The bar is unchanged: **green on `oevm_runner`, both image paths (Path A solidified-C, Path B `.oimg`)** — not "parses."

## 2. ARCHITECTURAL HIGHLIGHT — static-bound classes, not virtual (a foundational embedded tradeoff)

**This is the load-bearing design decision of 3b; read it first.**

> **ns-oevm resolves kernel-internal class references at compile time to a constant vtable + `OP_NEW`. So our kernel-internal class refs are statically bound, not virtual. We adopt the module structure and capability injection (the practical 80%) without late-bound class virtuality.**

In *full* Newspeak, classes are **messages** — a nested class is a late-bound *virtual slot* on its enclosing object, overridable per-instance/per-module. ns-oevm does **not** implement that: a nested-class reference (`OrderedCollection new`, `^ Array`) is resolved **lexically at compile time** by `resolve_class_ref` and emitted as a direct `OP_NEW` against a constant `VTABLE:<vt_id>` (construction) or a slot read (value). The receiver's runtime identity never participates; there is no "send a class name to an object → get its nested class" path.

What we therefore adopt, and what we consciously omit:

| Adopted (authentic Newspeak) | Omitted (deliberate subset) |
|---|---|
| Modules as classes parameterized by capabilities (`usingPlatform:`) | Classes as late-bound virtual slots (class overriding) |
| No global namespace — every name reached through a handed-in object | Per-instance / per-module class virtuality |
| Class nesting as the structuring mechanism | Runtime class-name dispatch (`anObject SomeClass`) |

**Why this is the right call for this target** (three reasons — not "instantiation is a hotspot"; class references happen at object-creation points, not inner loops):

1. **Our actual roadmap need is served without virtuality.** The one concrete thing virtual classes would buy us is **per-target library-variant selection** — a growable `OrderedCollection` on Linux vs a bounded one on Zephyr (parent design §3.1). A fixed embedded target picks its variant **once at boot** (hand the app a different `Kernel` module / bindings), so **boot-time module choice** gives the same outcome as runtime class override with none of the machinery. Runtime overriding buys nothing extra *for a fixed-target device*.

2. **It's reversible — not a one-way door.** The structure here *is* Newspeak's ("top-level classes are module definitions, their instances are modules"); only the **binding** differs (static `OP_NEW` vs late-bound send). Adding virtuality later is a compiler emit change (class ref → send) + a VM change (class slots become overridable) that **builds on this same substrate** and doesn't discard 3b; the library source is largely forward-compatible (the only revisit is hardcoded species, §6.1, → `self species`). So we can start static and add virtuality *if a real need appears*.

3. **The full model is interpreter-unfriendly here.** *Full* Newspeak treats **all** name references — slots, classes, methods — as late-bound message invocations. That is pervasive overhead that production Newspeak erases with a **Truffle/Graal JIT**. ns-oevm is a **JIT-less bytecode interpreter**, so the full model would be genuinely costly *on this VM*. (Narrow virtual-*classes*-only would be modest — ~one cacheable send per class reference — but see reasons 1–2: we don't need even that yet.)

**What we're deferring is real, not fluff.** The canonical payoff of virtual classes is *mixin-style class override* — e.g. Gilad Bracha's *Executable Grammars in Newspeak* (2007), where one grammar **class/mixin** is **subclassed** to retarget it (parser, syntax colorizer, AST builder), relying on class-hierarchy inheritance. That is directly adjacent to this project (which uses **Ohm** grammars, `grammar/ns.ohm`): if we ever wanted Newspeak-native *subclassable* executable grammars, that is exactly where static binding would bite — and the trigger to revisit per reason 2. References: Bracha et al., *Modules as Objects in Newspeak* (bracha.org/newspeak-modules.pdf); Bracha, *Executable Grammars in Newspeak* (bracha.org/executableGrammars.pdf).

Note one asymmetry: the **app→kernel boundary stays late-bound** (`platform OrderedCollection` is a genuine runtime send answered by an accessor) — so the *outward-facing* surface is the more Newspeak-flavored half, while the *internal* kernel graph is statically bound. This is also why variant selection has a natural seam (a different `Kernel`'s accessor returns a different impl) without internal virtuality.

## 3. The module stack & boot wiring

Today boot hands a **C `platform_object()`** (native primitives: `array:`/`basicNew:indexable:`, `print:`/`printNewline:`, `object:identicalTo:`, `objHashcode:`) straight to the app's `main: platform args:`. Under 3b a `Kernel` module is inserted between them:

```
   native platform (C platform_object)     ← primitives: basicNew:indexable:, print:, identity, hash
        │  injected via usingPlatform:
        ▼
   Kernel module instance                  ← nests Object, Array, Collection, OrderedCollection
        │  handed to the app AS its `platform`
        ▼
   App (main: platform args:)              ← reaches library classes via `platform <Name>`
```

**Boot becomes two-stage (both paths):**
1. Build the native C `platform_object()` (unchanged).
2. Construct `Kernel usingPlatform: <native>` → a `Kernel` instance. The `Kernel` class is found at boot by symbol/name exactly as the entry class already is (Path A: C symbol `Vtable_Kernel`; Path B: loader by-name). Constructing a top-level class that takes `usingPlatform:` *and* nests classes is already exercised by the app entry — proven machinery, one layer earlier.
3. Hand the **Kernel instance** to the app as `platform`, then run `main:args:` (replacing today's "hand the C object directly").

**Three reachability paths — each uses a mechanism confirmed to exist:**

1. **Kernel → primitives:** lexically through the injected `platform` param (`platform basicNew:indexable:`, `platform print:`). In scope for every nested class via the outer chain (the step-2 outer-walk fix enables this).
2. **Kernel → Kernel** (`Collection>>select:` building an `OrderedCollection`; `Array new:` referencing `Array`): compile-time lexical resolution → `OP_NEW` / slot read. No platform, no registry.
3. **App → Kernel:** `platform OrderedCollection` is a runtime send answered by an accessor method on `Kernel`, which reads the nested-class slot and returns the class object.

## 4. The `Kernel` module structure

`lib/kernel.ns` becomes one module wrapping the library classes as nested siblings:

```
class Kernel usingPlatform: platform = () (
  class Object = () ( ... ) : ()
  class Collection = () ( ... ) : ()                "rootless → delegates to Kernel.Object"
  class Array = Collection (
    public at: i      = ( ^ self basicAt: i )
    public at: i put: v = ( self basicAt: i put: v. ^ v )
    public size       = ( ^ self basicSize )
    public do: aBlock = ( 1 to: self size do: [:i | aBlock value: (self basicAt: i)] )
  ) : (
    public new: n = ( ^ platform basicNew: self indexable: n )   "capability-pure alloc"
  )
  class OrderedCollection = Collection ( ... ) : ()

  "app-facing accessors: the app reaches classes via `platform <Name>`"
  public Object            = ( ^ Object )
  public Array             = ( ^ Array )
  public Collection        = ( ^ Collection )
  public OrderedCollection = ( ^ OrderedCollection )
) : ()
```

**Array allocation is capability-pure.** `Array class>>new:` calls the native **`basicNew: <class> indexable: n`** primitive. The class object literally **is** `(oop)&Vtable_Kernel_Array` (3a fact: "class object = the bare `(oop)&Vtable_<vt_id>`"), so the primitive stamps that vtable and the SMI length header (`slots[0] = TAG_SMI(n)`, elements `1..n`). This replaces 3a's magic `kernel_array_vtable new:` special case.

**Accessors are hand-written** (one one-liner per public class), not compiler-generated: explicit, contained, zero compiler change, zero blast radius on existing nested-class semantics. Auto-generating accessors for a module's public nested classes (closer to real Newspeak class-slots) is a clean 3c+ refinement, not worth the compiler change or broader blast radius now. A method named `Array` whose body reads the `Array` nested-class slot does not collide: in the body, `Array` resolves as a slot (nested classes are slots), independent of the method name.

**Array is NOT a `Collection` (ratified 2026-06-21, Option C).** `Array` subclasses `Object` only (delegates to `Kernel.Object`) and stays the thin primitive indexable (`at:`/`at:put:`/`size`/`do:`). The functional protocol lives on `Collection`/`OrderedCollection`. An `Array` instance IS the raw indexable (slot0 = length, no `outer`→Kernel link), so inherited `Collection` methods (which allocate siblings and reach `platform` via `self`'s outer chain) cannot run on an `Array` receiver without a repr change — and the suite shows the functional protocol on Array is used by nobody (11 files use `Array` directly; 0 use `collect:`/`select:`). See `docs/NEWSPEAK_DIALECT.md` #3 for the full rationale and the 3c revisit path.

## 5. `Collection` (abstract, functional)

All derived methods fold over a single `do:`. **NLR-free except `detect:`** (see §5.1).

```
class Collection = () (
  do: aBlock        = ( ^ self error: 'subclassResponsibility' )
  public size       = ( ^ self inject: 0     into: [:n :e | n + 1] )
  public isEmpty    = ( ^ self size = 0 )
  public inject: a into: b = ( | acc | acc:: a. self do: [:e | acc:: b value: acc value: e]. ^ acc )
  public collect: b = ( | r | r:: OrderedCollection new. self do: [:e | r add: (b value: e)]. ^ r )
  public select: b  = ( | r | r:: OrderedCollection new. self do: [:e | (b value: e) ifTrue: [r add: e]]. ^ r )
  public reject: b  = ( | r | r:: OrderedCollection new. self do: [:e | (b value: e) ifFalse: [r add: e]]. ^ r )
  public includes: o = ( ^ self inject: false into: [:acc :e | acc or: [e = o]] )
  public asArray    = ( | arr i | arr:: Array new: self size. i:: 1. self do: [:e | arr at: i put: e. i:: i + 1]. ^ arr )
  public detect: b ifNone: n = ( self do: [:e | (b value: e) ifTrue: [^ e]]. ^ n value )
  public detect: b  = ( ^ self detect: b ifNone: [ self error: 'not found' ] )
) : ()
```

Derived methods reference `OrderedCollection` and `Array` as **lexical siblings** — the whole point of the module.

### 5.1 `detect:` uses NLR (deliberate)

`detect:`/`detect:ifNone:` are the one early-exit pair; they use a non-local return (`^`) out of the `do:` block for **true short-circuit** (stops iterating at the first match). Everything else is a fold.

- **NLR is verified working** in ns-oevm (focused test: `^` inside a block, propagated through an intermediate method, returns non-locally from the home method → confirmed; deltablue/richards/queens use the pattern and pass).
- **Rationale:** `detect:` *is* an early-exit operation — a `do:`/`inject:` fold cannot break (the collection drives the loop), so a fold `detect:` would scan the whole collection and need a unique `NONE` sentinel distinct from every element. NLR gives exact "find-first-and-stop" semantics, pays one unwind on success (cheaper than scanning the tail), and avoids the sentinel. "Core library is NLR-free" is a nice-to-have, not a correctness property; `detect:` is the justified exception. NLR remains available to app code regardless.

## 6. `OrderedCollection` (a `Collection` subclass)

Growable, **`Array`-backed**:

- **Representation:** two fixed slots — `contents` (backing `Array`) + `count` (SMI). `new` initializes `count:: 0`, `contents:: Array new: 8`.
- **`do: b`** = `1 to: count do: [:i | b value: (contents at: i)]`; **`size`** = `^ count` (overrides the generic fold).
- **Growth:** when `count = contents size`, allocate `Array new: (2 × capacity)`, copy, swap in `contents`.
- **Protocol:** `add:`/`addLast:` (grow-then-store), `removeLast` (return + decrement), `removeFirst` (return first, shift down), `first`/`last`, `at:`/`at:put:` (bounds-checked against `count`), `isEmpty`.

**Deliberate subset choices:**
1. **Species is hardcoded:** `collect:`/`select:`/`reject:` always return `OrderedCollection`, not "the receiver's species" (no `species` protocol — matches embedded scope).
2. **`removeFirst` is an O(n) shift** (no head-index ring buffer). Simple and fine at embedded sizes; a ring buffer is a later optimization.

## 7. Migration, magic-prim retirement & staying green

The suite (**51 pass / 3 skip, both paths**) is the migration safety net — every existing program's *observable* behavior must be preserved.

**Consolidation — 3b retires 3a's Array-specific scaffolding into the general mechanism.** Because `Array` allocation now flows through `platform basicNew: <class> indexable: n`, the following all retire:
- the receiver-independent `PRIM_Array` intercept;
- the `kernel_array_vtable` C global + its Path A/B boot wiring + GC root;
- the `format_version 2` `kernel_array_ref` header word.

They collapse into one general mechanism: boot constructs the `Kernel` instance (which holds `Array` as a nested-class slot), and the allocator takes the class as an argument. This reworks 3a's Path B format work — toward a simpler, general mechanism (3a's Array hacks graduating into the real plan), not a second special case.

**Three compatibility touch-points:**

1. **`Object` moves into `Kernel`** → its vtable id changes `Object` → `Kernel.Object` (`vt_id` `Kernel_Object`). The 3a universal-root rule (rootless/`=Object` heap classes delegate to the root) **retargets** from `Vtable_Object` to `Vtable_Kernel_Object`. Simple-name keying (`qname.rsplit(".",1)[-1] == "Object"`) still identifies the root; only the emitted delegate *target* changes. App heap classes chain to `Kernel.Object` as before; the 3a heap-object prim fall-through still reaches it.
2. **Boot becomes two-stage, both paths** (build native → construct `Kernel` → hand to app). Reuses the entry-class instantiation machinery one layer earlier.
3. **App-side compiler routing is unchanged:** `Array`/`OrderedCollection` still compile to `platform <Name>` sends (step 2); they now land on Kernel accessors instead of the magic prim. The only compiler change is the universal-root delegate *target* (#1).

**Risk posture / sequencing:** the root-delegate retarget (#1) and the two-stage boot (#2) are where a regression would surface — so the plan sequences the `Kernel`-module foundation + boot + root change **first and alone** (mirroring 3a putting its root change first), verified green on both paths before any collection class lands on top.

## 8. Testing

**Regression net:** suite stays 51/3 both paths; deltablue/richards/queens (full object protocol + NLR) prove the restructure preserved semantics. Foundation verified green before collections land.

**New protocol tests (both Path A and Path B):**
- **Module foundation:** the app reaches a kernel class through the accessor (`platform OrderedCollection` returns a usable class); `Array new:` still round-trips after magic-prim retirement.
- **`Collection` protocol:** `collect:`/`select:`/`reject:`/`inject:into:`/`includes:`/`isEmpty`/`size`/`asArray` over a known collection, asserting exact results.
- **`Array` stays a primitive (Option C):** an `Array` answers `at:`/`at:put:`/`size`/`do:` (3a protocol) and `Object` protocol (`isNil`/`printString`/`=`); it does NOT answer `collect:`/`select:`/`asArray` (those are `doesNotUnderstand` on an Array — see `docs/NEWSPEAK_DIALECT.md` #3).
- **`OrderedCollection` growth & order:** `add:` past the initial capacity (forces reallocation), then `do:`/`asArray` confirm element order and count survive the copy; `removeFirst`/`removeLast`/`first`/`last`.
- **`detect:` short-circuit:** a `detect:` whose predicate increments a counter — assert it stops at the first match (counter == match index), pinning the NLR early-exit choice.

**Bar per step:** green on `oevm_runner`, both paths.

## 9. Open items / deferred

- **3c:** `String` (the Symbol-vs-String representation question), `Dictionary` (open-addressing over indexable, keyed by `hash`/`=`), `Set` (Dictionary with value=key), boolean `hash`/`=`/`printString`, Streams + `Serial`.
- **Auto-generated class accessors** for a module's public nested classes (a step further toward Newspeak class-slots) — deferred refinement (§4).
- **Virtual classes (late-bound class refs / mixin override)** — deferred per §2. Reversible on the same module substrate (compiler emit + VM class-slot override; source forward-compatible modulo §6.1 species). **Revisit trigger:** a real need for runtime class override — e.g. Newspeak-style *subclassable* executable grammars over the Ohm front-end (Bracha, *Executable Grammars in Newspeak*).
- **`OrderedCollection` ring buffer** for O(1) `removeFirst` — deferred optimization (§6).
- **Carried-forward step-1 minors** (still deferred): `fflush(stdout)` in platform `print:`/`printNewline:`; `indexable_new` prototype in `oevm.h`.

## References
- Predecessor: `docs/design/2026-06-20-ns-oevm-stdlib-step3a-kernel-object-array.md`
- Parent: `docs/design/2026-06-20-ns-oevm-stdlib-design.md` (§3.1 variant selection, §4 primitive line, §5 class scope)
- Compiler: `tools/ns_compile_oevm.py` (`resolve_name` :346, `resolve_class_ref` :1191, nested-class read :501 / construct :827, universal-root rule :1349)
- VM (sibling `../oevm`): `src/dispatch.c`, `src/oevm_main.c` (Path A boot), `src/oevm_image_main.c` + `src/oimg_loader.c` (Path B boot)
- SOMns reuse: `SOMns/core-lib/Kernel.ns`, `Collections.ns` (protocol, not research surface)
