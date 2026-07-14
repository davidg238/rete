# Std-lib Step 3b — Kernel Module + Collection + OrderedCollection (Option C) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Promote the flat 3a kernel (top-level `Object` + `Array` reached via a magic `Array` primitive) into a real **`Kernel usingPlatform: platform` module** that nests the library classes, boot it two-stage as the app's `platform`, retire 3a's `Array` scaffolding, and add **`Collection`** (abstract, functional) + **`OrderedCollection`** (growable, Array-backed) — staying green on both image paths.

**Architecture:** `lib/kernel.ns` becomes one module class `Kernel usingPlatform: platform` nesting `Object`, `Array`, `Collection`, `OrderedCollection` as siblings. Boot is two-stage (both paths): build the native C `platform_object()`, construct `Kernel usingPlatform: <native>`, then hand the **Kernel instance** to the app as its `platform`. The app reaches library classes through hand-written accessor methods on `Kernel` (`platform Array`). Allocation is capability-pure: `platform basicNew: <class> indexable: n` (a native prim), replacing 3a's magic `kernel_array_vtable`. Per the ratified **Option C** decision, **`Array` subclasses `Object`, NOT `Collection`** — it stays the thin primitive indexable; the functional protocol lives on `Collection`/`OrderedCollection`.

**Tech Stack:** Python compiler (`tools/ns_compile_oevm.py`, `tools/oimg_serialize.py`, `tools/solidify.py`); Ohm grammar/parser (`grammar/ns-parse.js`); C VM in sibling repo `../oevm` (`src/dispatch.c`, `src/gc.c`, `src/oevm_main.c`, `oevm_image_main.c`, `src/oimg_loader.c`, `src/oevm.h`); Newspeak source `lib/kernel.ns`; pytest + the `../ns-tests` conformance suite.

## Global Constraints

- **Three sibling repos:** `ns-oevm` (compiler/harness/`lib`/`docs`; cwd `/home/david/workspace/ns-oevm`), `../oevm` (the C VM), `../ns-tests` (conformance tests). All paths are relative to the `ns-oevm` cwd.
- **Run the conformance suite from the `ns-oevm` cwd:** `python3 tools/run_tests_oevm.py`. Path selection via the `OEVM_PATH` env var: `OEVM_PATH=A` (solidified-C only), `OEVM_PATH=B` (.oimg only), default `AB` (both). Wrong cwd silently reports 0 passes.
- **Regression bar:** the suite is **51 pass / 3 skip** on `master`. "Green" = same counts. After Task 3 the bar is `OEVM_PATH=A` 51/3 (Path B is temporarily red until Task 4 finishes the loader); after Task 4 onward the bar is `OEVM_PATH=AB` 51/3.
- **Capability-purity (preserve):** the app reaches library classes ONLY through the handed `platform` (now the Kernel instance). Kernel reaches native primitives ONLY through its injected `platform` param. No VM globals for classes.
- **Kernel-internal Array allocation NEVER uses `Array new:`** — inside `lib/kernel.ns`, write `platform basicNew: Array indexable: n`. Reason: inside the module `Array` is a lexical sibling, so `Array new: n` compiles to a compile-time `OP_NEW` that allocates a fixed-slot object (NOT an indexable). Passing `Array` as an *argument* (`platform basicNew: Array indexable: n`) compiles to a slot read (the class BoundClass), which the native allocator turns into an indexable. App code may use `Array new: n` (it routes through `platform Array` → the class-side `new:` accessor at runtime).
- **Array repr (Option C, RATIFIED):** an `Array` instance IS the indexable object — `slots[0] = TAG_SMI(length)`, elements at `slots[1..n]`, NO `outer` slot. `Array` subclasses `Object` (delegates to `Kernel.Object` via the universal-root rule); it is NOT a `Collection`. `anArray collect:` is a runtime `doesNotUnderstand`, not a compile error (standard dynamic dispatch).
- **Opcode/PRIM facts:** class object = the bare `(oop)&Vtable_<vt_id>`; `vt_id(qname) = qname.replace(".", "_")` (so `Kernel.Object` → `Kernel_Object` → C symbol `Vtable_Kernel_Object`); a class's nested classes are stored in its slots as **BoundClass** values (vtable@slot0, outer@slot1, slotcount@slot2); class-side methods dispatch via `Meta_<vt>` and run with `self` = the BoundClass; the `delegate` field of a vtable is emitted verbatim as `(oop)&Vtable_<delegate>` (`solidify.py:190`).
- **Commit after each task; work on a feature branch in each repo touched; do NOT push** (push only on explicit say-so — see memory `push-only-when-asked`).

---

## File Structure

- `docs/NEWSPEAK_DIALECT.md` (create) — standing reference enumerating ns-oevm's deviations from full Newspeak + whys (Task 1).
- `docs/design/2026-06-20-ns-oevm-stdlib-step3b-kernel-module-collections.md` (modify) — amend §4/§8 to record Option C (Task 1).
- `lib/kernel.ns` (rewrite) — the `Kernel` module: nests `Object`, `Array` (Tasks 3), `Collection`, `OrderedCollection` (Task 5) + accessors.
- `tools/ns_compile_oevm.py` (modify):
  - slot READ outer-walk (`:485-500`) and slot ASSIGN outer-walk (`:550-563`) — class-side first-hop reads slot 1 (Task 2).
  - universal-root delegate target (`:1352`) — `"Object"` → `"Kernel_Object"` (Task 3).
  - Path A boot-gen (`:1883-1909`) — two-stage construction (Task 3).
  - Path B call site (`:1799-1806`) — pass the Kernel descriptor instead of `kernel_array_vtable_name` (Task 4).
- `tools/oimg_serialize.py` (modify) — `header_bytes`/`generate` carry the Kernel descriptor; retire `kernel_array_ref`; bump format_version to 3 (Task 4).
- `../oevm/src/dispatch.c` (modify) — add `basicNew:indexable:` prim; broaden the indexable gate; delete the `PRIM_Array` intercept, the `kernel_array_vtable new:` intercept, and the `kernel_array_vtable` global (Task 3).
- `../oevm/src/oevm.h` (modify) — remove `extern oop kernel_array_vtable;` (Task 3).
- `../oevm/src/gc.c` (modify) — remove the `kernel_array_vtable` root mark (Task 3).
- `../oevm/src/oevm_main.c` (modify, Path A) — remove the `kernel_array_vtable` wiring block (Task 3).
- `../oevm/src/solidified_image.c` (regenerate) — committed default image with the Kernel module + two-stage boot (Task 3).
- `../oevm/src/oimg_loader.c` (modify) — read the Kernel descriptor; two-stage `oimg_bootstrap`; retire `kernel_array_ref`/`oimg_kernel_array_vtable` (Task 4).
- `../oevm/oevm_image_main.c` (modify) — remove the `kernel_array_vtable` wiring block (Task 4).
- `../ns-tests/04_somns_basic/28_class_side_outer.ns` (create) — class-side outer-walk regression (Task 2).
- `../ns-tests/04_somns_basic/29_kernel_module.ns` (create) — module accessor + Array round-trip (Task 3).
- `../ns-tests/04_somns_basic/30_collections.ns` (create) — Collection/OrderedCollection protocol + growth + detect: (Task 5).

---

### Task 1: Documentation — Newspeak dialect reference + spec amendment

**Files:**
- Create: `docs/NEWSPEAK_DIALECT.md`
- Modify: `docs/design/2026-06-20-ns-oevm-stdlib-step3b-kernel-module-collections.md` (§4 line 98, §8 line 168)

**Interfaces:**
- Consumes: nothing (documentation only).
- Produces: a canonical, append-only list of language-level deviations the later tasks (and future steps) reference; the 3b spec no longer claims Array-as-Collection.

- [ ] **Step 1: Create `docs/NEWSPEAK_DIALECT.md`**

Create the file with exactly this content:

```markdown
# ns-oevm — a Newspeak dialect (intentional differences from full Newspeak)

> **Purpose.** ns-oevm is a *dialect* of Newspeak — a deliberate subset targeted at
> embedded scripting on a JIT-less bytecode interpreter (st-zephyr scope), not a
> Newspeak research vehicle. This document is the single, append-only record of
> every place ns-oevm intentionally diverges from full Newspeak semantics, **why**,
> and the **trigger** that would make us revisit. Add an entry whenever a step
> introduces a new divergence; never silently break the language.

| # | Deviation | Full Newspeak | ns-oevm | Why | Revisit trigger |
|---|-----------|---------------|---------|-----|-----------------|
| 1 | **Static-bound classes (not virtual)** | A nested class is a late-bound *virtual slot* on its enclosing object, overridable per-instance/per-module; `anObject SomeClass` is a real message. | A nested-class reference is resolved **lexically at compile time** to a constant vtable + `OP_NEW` (construction) or a slot read (value). The receiver's runtime identity never selects the class. | The one roadmap payoff (per-target library-variant selection) is served by **boot-time module choice**; full late binding is pervasive overhead a JIT-less interpreter can't absorb (production Newspeak uses Truffle/Graal). | A real need for runtime class override — e.g. Newspeak-style *subclassable executable grammars* (Bracha) over the Ohm front-end. Reversible: compiler emit (class-ref → send) + VM class-slot override on the same substrate. |
| 2 | **No mixins / mixin inheritance** | The class hierarchy is built on mixin application; every superclass clause applies a mixin, so behavior composes freely across hierarchies. | Static single inheritance via a fixed `delegate` pointer per vtable. **No mixin facility** — shared behavior is factored only into **abstract superclasses** (e.g. `Collection` holds the functional folds; `OrderedCollection` inherits them). Moving code into a superclass does not change *which* receiver shapes can run it (that's why `Array` can't borrow `Collection`'s methods — see #3). | Mixins are the virtual-class machinery of #1; abstract-superclass factoring covers the embedded subset's needs. | Same as #1 (mixins are how virtual classes compose). |
| 3 | **`Array` is not a `Collection`** (std-lib step 3b, Option C) | `Array` (an ArrayedCollection) is a `Collection`, inheriting `collect:`/`select:`/`asArray`. | `Array` subclasses `Object` only; it is the thin primitive indexable (`at:`/`at:put:`/`size`/`do:`). The functional protocol lives on `Collection`/`OrderedCollection`. `anArray collect:` is a runtime `doesNotUnderstand`. | An `Array` instance IS the raw indexable (slot0 = length, no `outer`→Kernel link), so inherited `Collection` methods that allocate siblings or reach `platform` could not work without changing the indexable repr. Evidence: 11 suite files use `Array` directly via the indexable protocol; **0** use `collect:`/`select:` anywhere — the functional protocol on Array is used by nobody (YAGNI). | A real `anArray collect:`/`asArray` need. Reversible in 3c by giving `Array` an `outer` header **or** the deferred Array-as-wrapper repr (step-3a spec §8(C)), then `class Array = Collection`. |
| 4 | **No type annotations / `<: Value`** | Slots, params, and returns may carry type annotations and `<: Value` immutability clauses. | The grammar subsets these away; programs are dynamically typed only. | Not needed for the embedded scripting subset; smaller grammar/compiler. | A static-checking or doc-tooling need. |
| 5 | **Booleans are intrinsic singletons, not `True`/`False` classes** | `true`/`false` are instances of `True`/`False`, full library classes. | `true`/`false` are VM intrinsic singletons; only `hash`/`=`/`printString` are added as prims (3c). No `True`/`False` classes. | VM-level dispatch on intrinsics is cheaper and sufficient at embedded scope. | A need to subclass/extend boolean behavior in source. |
| 6 | **Module nested classes reached via hand-written accessors** | A module's public nested classes are auto-exposed as late-bound class-slots on the module object. | Each public library class has a hand-written one-line accessor on `Kernel` (`public Array = ( ^ Array )`); the app reaches it via `platform Array`. | Auto-generating accessors is a compiler change with broader blast radius; hand-written is explicit and contained. | Adopting auto class-slot accessors (a step toward real Newspeak class-slots) in 3c+. |
| 7 | **`error:` is a universal primitive** | Errors/aborts are message sends, capability-mediated. | `error:` is a universal VM prim available on every receiver, not a handed capability. | Pragmatic for the current subset; abort is a VM concern. | Reworking error handling as a capability (step-3a spec §8(A)). |

## How to use this file
- One row per deviation. Keep the **Why** and **Revisit trigger** concrete.
- When a future step *removes* a deviation, strike the row and note the step that closed it (don't delete the history).
- Cross-reference the spec/section that ratified each decision.
```

- [ ] **Step 2: Amend spec §4 (line 98) to record Option C**

In `docs/design/2026-06-20-ns-oevm-stdlib-step3b-kernel-module-collections.md`, replace the line:

```
`Array` becomes a `Collection` subclass (`class Array = Collection ...` via the universal-root chain), inheriting `collect:`/`select:`/`inject:`/`includes:`/`asArray` on top of its indexable `do:`.
```

with:

```
**Array is NOT a `Collection` (ratified 2026-06-21, Option C).** `Array` subclasses `Object` only (delegates to `Kernel.Object`) and stays the thin primitive indexable (`at:`/`at:put:`/`size`/`do:`). The functional protocol lives on `Collection`/`OrderedCollection`. An `Array` instance IS the raw indexable (slot0 = length, no `outer`→Kernel link), so inherited `Collection` methods (which allocate siblings and reach `platform` via `self`'s outer chain) cannot run on an `Array` receiver without a repr change — and the suite shows the functional protocol on Array is used by nobody (11 files use `Array` directly; 0 use `collect:`/`select:`). See `docs/NEWSPEAK_DIALECT.md` #3 for the full rationale and the 3c revisit path.
```

- [ ] **Step 3: Amend spec §8 (line 168) to drop the Array-as-Collection test**

Replace the line:

```
- **`Array` as a `Collection`:** an `Array` answering inherited `collect:`/`select:`/`asArray` (proves `Array < Collection` wired through `Kernel.Object`).
```

with:

```
- **`Array` stays a primitive (Option C):** an `Array` answers `at:`/`at:put:`/`size`/`do:` (3a protocol) and `Object` protocol (`isNil`/`printString`/`=`); it does NOT answer `collect:`/`select:`/`asArray` (those are `doesNotUnderstand` on an Array — see `docs/NEWSPEAK_DIALECT.md` #3).
```

- [ ] **Step 4: Commit (ns-oevm)**

```bash
git -C /home/david/workspace/ns-oevm checkout -b feat/stdlib-step3b
git -C /home/david/workspace/ns-oevm add docs/NEWSPEAK_DIALECT.md docs/design/2026-06-20-ns-oevm-stdlib-step3b-kernel-module-collections.md
git -C /home/david/workspace/ns-oevm commit -m "docs(stdlib): Newspeak dialect reference (NEWSPEAK_DIALECT.md); step-3b Option C (Array not a Collection)"
```

---

### Task 2: Compiler fix — class-side slot-read outer-walk + regression test

**Files:**
- Modify: `tools/ns_compile_oevm.py` (slot READ `:485-500`, slot ASSIGN `:550-563`)
- Test: `../ns-tests/04_somns_basic/28_class_side_outer.ns`

**Interfaces:**
- Consumes: `self.class_side_qname` (set when compiling a class-side/metaclass method); the existing `outer C` precedent (`:654-660`) where the first hop out of a class-side `self` reads slot 1.
- Produces: a plain identifier resolving to an enclosing-class slot at `depth > 0` from a class-side method now walks slot 1 on the first hop (BoundClass `outer`), slot 0 thereafter — so `Array class>>new:` can reach the Kernel's `platform` slot (Task 3).

- [ ] **Step 1: Write the failing test**

Create `../ns-tests/04_somns_basic/28_class_side_outer.ns`:

```
(*
expect:
42
*)
class ClassSideOuter packageUsing: manifest = () (
  | secret ::= 42. |
  class Probe = () (
  ) : (
    public reveal = ( ^ secret )
  )
  public main: platform args: args = (
    (Probe reveal) println.
  )
) : (
)
```

(`Probe reveal` is a class-side send to the `Probe` BoundClass; `reveal` reads `secret`, a slot of the enclosing `ClassSideOuter` instance at `depth == 1`.)

- [ ] **Step 2: Run the test to verify it fails**

```bash
cd /home/david/workspace/ns-oevm
python3 tools/ns_compile_oevm.py ../ns-tests/04_somns_basic/28_class_side_outer.ns ../oevm/src/solidified_image.c
(cd ../oevm && gcc -Wall -Wextra -std=c99 -DOEVM_APPLIANCE src/vm.c src/gc.c src/dispatch.c src/solidified_image.c src/oevm_main.c -o oevm_runner -lm && ./oevm_runner)
```

Expected: NOT `42` (garbage or crash). Before the fix the first outer hop reads `Probe` BoundClass slot 0 = the vtable (not `outer`), then dereferences a vtable word as `secret`.

- [ ] **Step 3: Fix the slot READ outer-walk**

In `tools/ns_compile_oevm.py`, the slot read path currently is (`:485-500`):

```python
            elif res[0] == "slot":
                slot_idx, depth = res[1], res[2]
                base_reg = self.get_self_reg()
                if depth == 0:
                    self.emit_ABC(OP_LD_SLOT, target_reg, slot_idx, base_reg)
                else:
                    # Walk outer chain
                    prev_reg = base_reg
                    for _ in range(depth):
                        next_reg = self.alloc_reg()
                        self.emit_ABC(OP_LD_SLOT, next_reg, 0, prev_reg) # outer is always slot 0
                        prev_reg = next_reg
                    self.emit_ABC(OP_LD_SLOT, target_reg, slot_idx, prev_reg)
                    # Reclaim outer walk registers
                    self.next_free_reg -= depth
                self.free_self_reg(base_reg)
```

Replace the `else` branch's loop so the first hop is class-side aware (mirrors the `outer C` path at `:654-660`):

```python
            elif res[0] == "slot":
                slot_idx, depth = res[1], res[2]
                base_reg = self.get_self_reg()
                if depth == 0:
                    self.emit_ABC(OP_LD_SLOT, target_reg, slot_idx, base_reg)
                else:
                    # Walk outer chain. A class-side `self` is a BoundClass whose
                    # enclosing instance is at slot 1, not slot 0 — so the FIRST hop
                    # out of a class-side self reads slot 1; every later hop (and all
                    # instance-side hops) read slot 0. Mirrors the `outer C` path.
                    prev_reg = base_reg
                    for w in range(depth):
                        next_reg = self.alloc_reg()
                        outer_slot = 1 if (w == 0 and self.class_side_qname is not None) else 0
                        self.emit_ABC(OP_LD_SLOT, next_reg, outer_slot, prev_reg)
                        prev_reg = next_reg
                    self.emit_ABC(OP_LD_SLOT, target_reg, slot_idx, prev_reg)
                    # Reclaim outer walk registers
                    self.next_free_reg -= depth
                self.free_self_reg(base_reg)
```

- [ ] **Step 4: Fix the slot ASSIGN outer-walk symmetrically**

In `tools/ns_compile_oevm.py`, the slot assign path currently is (`:550-563`):

```python
            elif res[0] == "slot":
                slot_idx, depth = res[1], res[2]
                base_reg = self.get_self_reg()
                if depth == 0:
                    self.emit_ABC(OP_ST_SLOT, target_reg, slot_idx, base_reg)
                else:
                    prev_reg = base_reg
                    for _ in range(depth):
                        next_reg = self.alloc_reg()
                        self.emit_ABC(OP_LD_SLOT, next_reg, 0, prev_reg)
                        prev_reg = next_reg
                    self.emit_ABC(OP_ST_SLOT, target_reg, slot_idx, prev_reg)
                    self.next_free_reg -= depth
                self.free_self_reg(base_reg)
```

Replace the `else` branch's loop the same way:

```python
            elif res[0] == "slot":
                slot_idx, depth = res[1], res[2]
                base_reg = self.get_self_reg()
                if depth == 0:
                    self.emit_ABC(OP_ST_SLOT, target_reg, slot_idx, base_reg)
                else:
                    prev_reg = base_reg
                    for w in range(depth):
                        next_reg = self.alloc_reg()
                        # class-side self: first hop reads slot 1 (see slot-read above)
                        outer_slot = 1 if (w == 0 and self.class_side_qname is not None) else 0
                        self.emit_ABC(OP_LD_SLOT, next_reg, outer_slot, prev_reg)
                        prev_reg = next_reg
                    self.emit_ABC(OP_ST_SLOT, target_reg, slot_idx, prev_reg)
                    self.next_free_reg -= depth
                self.free_self_reg(base_reg)
```

- [ ] **Step 5: Run the test to verify it passes**

```bash
cd /home/david/workspace/ns-oevm
python3 tools/ns_compile_oevm.py ../ns-tests/04_somns_basic/28_class_side_outer.ns ../oevm/src/solidified_image.c
(cd ../oevm && gcc -Wall -Wextra -std=c99 -DOEVM_APPLIANCE src/vm.c src/gc.c src/dispatch.c src/solidified_image.c src/oevm_main.c -o oevm_runner -lm && ./oevm_runner)
```

Expected: `42`.

- [ ] **Step 6: Run the full suite (both paths) — confirm no regression**

```bash
cd /home/david/workspace/ns-oevm
python3 tools/run_tests_oevm.py
```

Expected: 52 pass / 3 skip both paths (51 prior + the new `28_class_side_outer.ns`). If any *existing* test regresses, STOP — it means a class-side method already relied on the old slot-0 first hop; report it (this would contradict the bug analysis and needs investigation, not a workaround).

- [ ] **Step 7: Commit (ns-oevm + ns-tests)**

```bash
git -C /home/david/workspace/ns-oevm add tools/ns_compile_oevm.py
git -C /home/david/workspace/ns-oevm commit -m "fix(compiler): class-side slot-read/assign first hop reads BoundClass outer (slot 1)"
git -C /home/david/workspace/ns-tests checkout -b feat/stdlib-step3b
git -C /home/david/workspace/ns-tests add 04_somns_basic/28_class_side_outer.ns
git -C /home/david/workspace/ns-tests commit -m "test(stdlib): class-side method reaching enclosing-instance slot"
```

---

### Task 3: Kernel module foundation — Path A (solidified-C)

**Files:**
- Rewrite: `lib/kernel.ns` (Kernel module nesting Object + Array, + accessors)
- Modify: `tools/ns_compile_oevm.py` (universal-root target `:1352`; Path A boot-gen `:1883-1909`)
- Modify: `../oevm/src/dispatch.c` (add `basicNew:indexable:` prim; broaden indexable gate; delete `PRIM_Array` intercept, `kernel_array_vtable new:` intercept, and the `kernel_array_vtable` global)
- Modify: `../oevm/src/oevm.h` (remove `extern oop kernel_array_vtable;`)
- Modify: `../oevm/src/gc.c` (remove the `kernel_array_vtable` root mark)
- Modify: `../oevm/src/oevm_main.c` (remove the `kernel_array_vtable` wiring block)
- Regenerate: `../oevm/src/solidified_image.c`
- Test: `../ns-tests/04_somns_basic/29_kernel_module.ns`

**Interfaces:**
- Consumes: the class-side slot-read fix (Task 2); `vt_id("Kernel.Object") == "Kernel_Object"`; the `__boundOuter:slotCount:` BoundClass mechanism for nested-class slots; `platform_object()`; `gc_allocate`; `boundclass_construct`'s class-side dispatch.
- Produces: `lib/kernel.ns` is the `Kernel` module; the app's `platform` is the **Kernel instance**; `platform Array` returns the Array class (BoundClass) and `Array new: n` allocates an indexable via `platform basicNew: self indexable: n`; every rootless/`= Object` heap class delegates to `Vtable_Kernel_Object`; the native prim `basicNew:indexable:` exists on the platform object; the `kernel_array_vtable` global / `PRIM_Array` intercept are gone. **Gate: `OEVM_PATH=A` green; Path B is brought back in Task 4.**

- [ ] **Step 1: Write the failing test**

Create `../ns-tests/04_somns_basic/29_kernel_module.ns`:

```
(*
expect:
3
10
20
30
an Object
*)
class KernelModuleTest packageUsing: manifest = () (
class Widget = () (
) : ()
public main: platform args: args = (
    | a w |
    a:: Array new: 3.
    a at: 1 put: 10.
    a at: 2 put: 20.
    a at: 3 put: 30.
    a size println.
    a do: [ :x | x println ].
    w:: Widget new.
    w printString println.
)
) : (
)
```

(`Array new: 3` exercises the module accessor + class-side `new:` + native allocator; `w printString` exercises the retargeted `Object` root via `Kernel.Object`.)

- [ ] **Step 2: Rewrite `lib/kernel.ns` as the `Kernel` module (Object + Array)**

Replace the entire contents of `lib/kernel.ns` with:

```
(*
ns-oevm kernel library (std-lib step 3b). One module — `Kernel usingPlatform:
platform` — nesting the library classes as siblings. The native C
platform_object() is injected as `platform`; the APP receives the Kernel
INSTANCE as its own `platform` and reaches library classes through the
hand-written accessors at the bottom.

Reachability (spec §3):
  Kernel -> native primitives : lexically via the injected `platform` param.
  Kernel -> Kernel            : compile-time lexical (sibling slot read / OP_NEW).
  App    -> Kernel            : runtime send to an accessor (`platform Array`).

NB: inside this module NEVER write `Array new: n` (it compiles to a fixed-slot
OP_NEW, not an indexable). Allocate indexables via `platform basicNew: Array
indexable: n` (Array as an argument is a slot read of the class object).
*)
class Kernel usingPlatform: platform = () (
  class Object = () (
    public isNil = ( ^ false )
    public notNil = ( ^ true )
    public ifNil: aBlock = ( ^ self )
    public ifNotNil: aBlock = ( ^ aBlock value: self )
    public = other = ( ^ self == other )
    public printString = ( ^ 'an Object' )
    public asString = ( ^ self printString )
  ) : (
  )

  class Array = () (
    public at: i = ( ^ self basicAt: i )
    public at: i put: v = ( self basicAt: i put: v. ^ v )
    public size = ( ^ self basicSize )
    public do: aBlock = ( 1 to: self size do: [ :i | aBlock value: (self basicAt: i) ]. ^ self )
  ) : (
    public new: n = ( ^ platform basicNew: self indexable: n )
  )

  public Object = ( ^ Object )
  public Array  = ( ^ Array )
) : (
)
```

- [ ] **Step 3: Retarget the universal-root delegate to `Kernel.Object`**

In `tools/ns_compile_oevm.py`, the universal-root rule currently ends (`:1346-1352`):

```python
        if qname.rsplit(".", 1)[-1] == "Object":
            delegate = None
        elif super_name and super_name != "Object":
            sup = self.resolve_class_ref(super_name)
            delegate = self.vt_id(sup[0]) if sup else super_name
        elif super_name == "Object" or (super_name is None and not node.get("superExpr")):
            delegate = "Object"
```

Change the final assignment so rootless/`= Object` heap classes delegate to the nested `Kernel.Object` (emitted as `Vtable_Kernel_Object`):

```python
        if qname.rsplit(".", 1)[-1] == "Object":
            delegate = None
        elif super_name and super_name != "Object":
            sup = self.resolve_class_ref(super_name)
            delegate = self.vt_id(sup[0]) if sup else super_name
        elif super_name == "Object" or (super_name is None and not node.get("superExpr")):
            # std-lib step 3b: `Object` now lives inside the Kernel module
            # (lib/kernel.ns), so its vtable is Vtable_Kernel_Object. The simple-name
            # check above still identifies the root (delegate NULL); every other
            # rootless / `= Object` heap class delegates to the nested root.
            delegate = "Kernel_Object"
```

(The `delegate` string is emitted verbatim as `(oop)&Vtable_{delegate}` — `solidify.py:190`. `qname.rsplit(".",1)[-1] == "Object"` still matches `Kernel.Object` → `delegate = None`, so the nested `Object` remains the NULL-delegate root.)

- [ ] **Step 4: Two-stage boot in the Path A generator**

In `tools/ns_compile_oevm.py`, the Path A entry generator currently is (`:1883-1909`):

```python
        # Allocate entry instance
        c_code += f"    // Instantiate entry class {entry_class}\n"
        c_code += "    extern oop gc_allocate(oop vtable, size_t size);\n"
        c_code += f"    oop entry_inst = gc_allocate((oop)&Vtable_{entry_class}, sizeof(struct ObjectInstance) + {len(compiler.class_slots[entry_class])} * sizeof(oop));\n"

        # Call initializer on entry instance
        params = compiler.all_classes[entry_class].get("params", [])
        init_sel = "".join(f"{p}:" for p in params)
        if not init_sel:
            init_sel = "new"
        init_selector = f"init_{init_sel}"

        # std-lib step 2: hand the real `platform` capability (step-1 singleton)
        # to the entry class's initializer and to main:args:. args is still empty.
        c_code += f"    oop platform = platform_object();\n"
        c_code += f"    oop fake_args = TAG_SMI(0);\n"
        c_code += f"    oop init_args[2] = {{ platform, fake_args }};\n"

        init_sym_var = compiler.solidify.symbols[init_selector]
        c_code += f"    send(entry_inst, (oop)&{init_sym_var}, {len(params)}, init_args);\n\n"

        # Call main:args:
        c_code += f"    oop main_args[2] = {{ platform, fake_args }};\n"
        main_sym_var = compiler.solidify.symbols["main:args:"]
        c_code += f"    oop res = send(entry_inst, (oop)&{main_sym_var}, 2, main_args);\n"
        c_code += f"    return res;\n"
        c_code += "}\n"
```

Replace that whole block with the two-stage version:

```python
        # std-lib step 3b: two-stage boot. (1) native C platform; (2) construct the
        # Kernel module `Kernel usingPlatform: <native>`; (3) hand the Kernel
        # INSTANCE to the app as its `platform`.
        c_code += "    extern oop gc_allocate(oop vtable, size_t size);\n"
        c_code += f"    oop native_platform = platform_object();\n"
        c_code += f"    oop fake_args = TAG_SMI(0);\n"

        # (2) construct Kernel usingPlatform: native
        kparams = compiler.all_classes["Kernel"].get("params", [])
        kinit_sel = "".join(f"{p}:" for p in kparams) or "new"
        kinit_selector = f"init_{kinit_sel}"
        kinit_sym = compiler.solidify.symbols[kinit_selector]
        c_code += f"    oop kernel_inst = gc_allocate((oop)&Vtable_Kernel, sizeof(struct ObjectInstance) + {len(compiler.class_slots['Kernel'])} * sizeof(oop));\n"
        c_code += f"    oop kernel_init_args[1] = {{ native_platform }};\n"
        c_code += f"    send(kernel_inst, (oop)&{kinit_sym}, {len(kparams)}, kernel_init_args);\n\n"

        # (3) instantiate the entry class, handing it the Kernel instance as `platform`
        c_code += f"    // Instantiate entry class {entry_class}\n"
        c_code += f"    oop entry_inst = gc_allocate((oop)&Vtable_{entry_class}, sizeof(struct ObjectInstance) + {len(compiler.class_slots[entry_class])} * sizeof(oop));\n"

        params = compiler.all_classes[entry_class].get("params", [])
        init_sel = "".join(f"{p}:" for p in params) or "new"
        init_selector = f"init_{init_sel}"
        c_code += f"    oop init_args[2] = {{ kernel_inst, fake_args }};\n"
        init_sym_var = compiler.solidify.symbols[init_selector]
        c_code += f"    send(entry_inst, (oop)&{init_sym_var}, {len(params)}, init_args);\n\n"

        c_code += f"    oop main_args[2] = {{ kernel_inst, fake_args }};\n"
        main_sym_var = compiler.solidify.symbols["main:args:"]
        c_code += f"    oop res = send(entry_inst, (oop)&{main_sym_var}, 2, main_args);\n"
        c_code += f"    return res;\n"
        c_code += "}\n"
```

- [ ] **Step 5: Add the native `basicNew:indexable:` prim (dispatch.c)**

In `../oevm/src/dispatch.c`, the prim enum (`:583-585`) currently ends:

```c
    /* --- std-lib step 1: real indexable + platform capability --- */
    PRIM_ix_array, PRIM_ix_basicAt, PRIM_ix_basicAt_put, PRIM_ix_basicSize,
    PRIM_pf_identicalTo, PRIM_pf_hashcode, PRIM_pf_print, PRIM_pf_printNewline,
```

Add the new id at the end:

```c
    /* --- std-lib step 1: real indexable + platform capability --- */
    PRIM_ix_array, PRIM_ix_basicAt, PRIM_ix_basicAt_put, PRIM_ix_basicSize,
    PRIM_pf_identicalTo, PRIM_pf_hashcode, PRIM_pf_print, PRIM_pf_printNewline,
    /* --- std-lib step 3b: capability-pure indexable allocation --- */
    PRIM_pf_basicNewIndexable,
```

The parallel `PRIM_NAMES` table (`:599-600`) currently ends:

```c
    "array:", "basicAt:", "basicAt:put:", "basicSize",
    "object:identicalTo:", "objHashcode:", "print:", "printNewline:",
```

Add the matching name at the same position:

```c
    "array:", "basicAt:", "basicAt:put:", "basicSize",
    "object:identicalTo:", "objHashcode:", "print:", "printNewline:",
    "basicNew:indexable:",
```

In the platform-receiver switch (`:701-727`), add a case before `default:`:

```c
                case PRIM_pf_basicNewIndexable: {
                    /* `platform basicNew: <class> indexable: n` — capability-pure
                       indexable allocation. args[0] is the class object: a BoundClass
                       wrapper (slot0 = its nested vtable) or a bare (oop)&Vtable. The
                       new instance is stamped with that vtable, slot0 = TAG_SMI(n)
                       length header, elements 1..n nil. Replaces 3a's magic
                       kernel_array_vtable `new:`. */
                    extern oop gc_allocate(oop vtable, size_t size);
                    oop cls = args[0];
                    oop vt = (cls != NULL && !IS_SMI(cls) && boundclass_vtable_initialized &&
                              obj_vtable(cls) == (oop)&BoundClassVtable)
                             ? ((struct ObjectInstance *)cls)->slots[0]
                             : cls;
                    intptr_t n = IS_SMI(args[1]) ? UNTAG_SMI(args[1]) : 0;
                    if (n < 0) n = 0;
                    oop a = gc_allocate(vt, sizeof(struct ObjectInstance) + ((size_t)n + 1) * sizeof(oop));
                    struct ObjectInstance *inst = (struct ObjectInstance *)a;
                    inst->slots[0] = TAG_SMI(n);
                    for (intptr_t i = 1; i <= n; i++) inst->slots[i] = NULL;
                    return a;
                }
```

- [ ] **Step 6: Broaden the indexable gate; delete the `kernel_array_vtable` intercepts and global (dispatch.c)**

In `../oevm/src/dispatch.c`:

(a) Delete the `PRIM_Array` intercept (`:645-648`):

```c
            // Intercept "Array" — return the real kernel Array class (lib/kernel.ns)
            if (pid == PRIM_Array) {
                return kernel_array_vtable;
            }
```

(Remove these lines entirely. `"Array"` now falls through the prim block to normal method dispatch — the app's `platform Array` lands on the `Kernel>>Array` accessor.)

(b) Delete the `kernel_array_vtable new:` intercept (`:650-663`):

```c
            // Intercept "new:" on the kernel Array class — allocate Array-vtable indexable.
            // Guard against an unwired image (kernel_array_vtable == NULL): without it a
            // `nil new:` would match NULL == NULL and allocate a bogus array.
            if (kernel_array_vtable != NULL && receiver == kernel_array_vtable && pid == PRIM_new) {
                extern oop gc_allocate(oop vtable, size_t size);
                intptr_t n = IS_SMI(args[0]) ? UNTAG_SMI(args[0]) : 0;
                if (n < 0) n = 0;
                oop a = gc_allocate(kernel_array_vtable,
                                    sizeof(struct ObjectInstance) + ((size_t)n + 1) * sizeof(oop));
                struct ObjectInstance *inst = (struct ObjectInstance *)a;
                inst->slots[0] = TAG_SMI(n);                 /* length header (indexable layout) */
                for (intptr_t i = 1; i <= n; i++) inst->slots[i] = NULL;
                return a;
            }
```

(Remove entirely. `Array new: n` now allocates via the class-side `new:` method → `platform basicNew:indexable:`.)

(c) Broaden the indexable storage gate (`:669-671`) from a vtable comparison to a shape check, removing the `kernel_array_vtable` reference. Current:

```c
            if (receiver != NULL && !IS_SMI(receiver) && indexable_initialized &&
                (obj_vtable(receiver) == (oop)&IndexableVtable ||
                 obj_vtable(receiver) == kernel_array_vtable)) {
```

Replace with:

```c
            // std-lib step 3b: indexables are vtable-agnostic now (no kernel_array_vtable
            // global). basicAt:/basicAt:put:/basicSize are only emitted by kernel Array
            // methods on self, and every indexable has slot0 = TAG_SMI(length). Gate on
            // the pid + that shape.
            if (receiver != NULL && !IS_SMI(receiver) &&
                (pid == PRIM_ix_basicAt || pid == PRIM_ix_basicAt_put || pid == PRIM_ix_basicSize) &&
                IS_SMI(instance_slot((struct ObjectInstance *)receiver, 0))) {
```

(d) Delete the `kernel_array_vtable` global definition (`:389-391`):

```c
/* std-lib step 3a: set at boot (Path A: oevm_main.c; Path B: oevm_image_main.c)
   to the kernel Array vtable (lib/kernel.ns).  Always non-NULL at runtime. */
oop kernel_array_vtable = NULL;
```

(Remove entirely.)

- [ ] **Step 7: Remove the `kernel_array_vtable` declaration / root / wiring (oevm.h, gc.c, oevm_main.c)**

In `../oevm/src/oevm.h`, delete (`:297-300`):

```c
/* std-lib step 3a: the kernel Array class vtable (lib/kernel.ns), set at boot to
   (oop)&Vtable_Array. `platform Array` returns it; `new:` on it allocates an
   Array-vtable indexable; basicAt:/basicSize accept Array-vtable receivers. */
extern oop kernel_array_vtable;
```

In `../oevm/src/gc.c`, delete the root mark (`:631-635`):

```c
    /* std-lib step 3a: kernel Array class vtable (dispatch.c). The vtable lives
       in static C storage (Path A) or image blob (Path B) — mark() early-rejects
       non-heap pointers, so this is always safe. */
    extern oop kernel_array_vtable;
    mark(kernel_array_vtable);
```

(The Kernel instance stays reachable through the running frames' `platform` arg; its nested-class vtables live in static C / the image blob, never on the GC heap, so no new root is needed.)

In `../oevm/src/oevm_main.c`, delete the wiring block (`:42-49`):

```c
    /* std-lib step 3a: wire kernel Array class vtable so `platform Array` returns
       the real kernel class compiled from lib/kernel.ns (Vtable_Array lives in
       the generated solidified_image.c). Must be after init_solidified_vtables(). */
    {
        extern oop kernel_array_vtable;
        extern const struct Vtable Vtable_Array;   /* from generated solidified_image.c */
        kernel_array_vtable = (oop)&Vtable_Array;
    }
```

- [ ] **Step 8: Build + run the module test (Path A)**

```bash
cd /home/david/workspace/ns-oevm
python3 tools/ns_compile_oevm.py ../ns-tests/04_somns_basic/29_kernel_module.ns ../oevm/src/solidified_image.c
(cd ../oevm && gcc -Wall -Wextra -std=c99 -DOEVM_APPLIANCE src/vm.c src/gc.c src/dispatch.c src/solidified_image.c src/oevm_main.c -o oevm_runner -lm && ./oevm_runner)
```

Expected: `3`, `10`, `20`, `30`, `an Object`. (Clean compile with no `kernel_array_vtable` references remaining.)

- [ ] **Step 9: Regenerate the committed default solidified image**

The committed `../oevm/src/solidified_image.c` predates the Kernel module + two-stage boot, so a plain `make` would not define `Vtable_Kernel`/`init_platform:` and `oevm_main.c` would mis-link. Regenerate it through the new compiler from the canonical default source. First confirm the current default and that it lacks the Kernel:

```bash
cd /home/david/workspace/ns-oevm
grep -m1 "Instantiate entry class" ../oevm/src/solidified_image.c   # shows the current default's entry class name
grep -c "Vtable_Kernel" ../oevm/src/solidified_image.c              # expect 0 before regen
```

Regenerate from the same minimal ScriptApp the default represents (use the module test if the repo has no separately-tracked default source):

```bash
python3 tools/ns_compile_oevm.py ../ns-tests/04_somns_basic/29_kernel_module.ns ../oevm/src/solidified_image.c
grep -c "Vtable_Kernel" ../oevm/src/solidified_image.c             # expect > 0 after regen
(cd ../oevm && gcc -Wall -Wextra -std=c99 -DOEVM_APPLIANCE src/vm.c src/gc.c src/dispatch.c src/solidified_image.c src/oevm_main.c -o oevm_runner -lm && ./oevm_runner)
```

Expected: builds and runs (prints the `29_kernel_module.ns` expected output).

- [ ] **Step 10: Run the suite, Path A only**

```bash
cd /home/david/workspace/ns-oevm
OEVM_PATH=A python3 tools/run_tests_oevm.py
```

Expected: 53 pass / 3 skip on Path A (52 from Task 2 + the new `29_kernel_module.ns`). Path B is expected to be red until Task 4 — do NOT run `AB` as the gate for this task.

- [ ] **Step 11: Commit (three repos)**

```bash
git -C /home/david/workspace/ns-oevm add lib/kernel.ns tools/ns_compile_oevm.py
git -C /home/david/workspace/ns-oevm commit -m "feat(stdlib): Kernel module foundation + two-stage boot (Path A); universal root -> Kernel.Object"
git -C /home/david/workspace/oevm checkout -b feat/stdlib-step3b
git -C /home/david/workspace/oevm add src/dispatch.c src/oevm.h src/gc.c src/oevm_main.c src/solidified_image.c
git -C /home/david/workspace/oevm commit -m "feat(stdlib): basicNew:indexable: prim; retire kernel_array_vtable/PRIM_Array; two-stage boot (Path A)"
git -C /home/david/workspace/ns-tests add 04_somns_basic/29_kernel_module.ns
git -C /home/david/workspace/ns-tests commit -m "test(stdlib): Kernel module accessor + Array round-trip"
```

---

### Task 4: Kernel module foundation — Path B (.oimg loader)

**Files:**
- Modify: `tools/oimg_serialize.py` (`header_bytes` `:62-109`; `generate` `:245-247` + kernel-ref computation `:412`)
- Modify: `tools/ns_compile_oevm.py` (Path B call site `:1799-1806`)
- Modify: `../oevm/src/oimg_loader.c` (header contract `:23-44`; ref decl `:53-55`; load `:87-114`; resolve `:127-131`; `oimg_bootstrap` `:133-146`)
- Modify: `../oevm/oevm_image_main.c` (remove the `kernel_array_vtable` wiring block `:29-35`)

**Interfaces:**
- Consumes: the Kernel module (Task 3); `oimg_resolve`; `platform_object()`; `gc_allocate`; the image's `Vtable_Kernel`, `init_platform:` symbol, and Kernel slot count.
- Produces: the `.oimg` header carries a **Kernel descriptor** (`kernel_vtable_ref`, `kernel_init_sel_ref`, `kernel_nslots`) at format_version 3, replacing 3a's single `kernel_array_ref`; `oimg_bootstrap` constructs `Kernel usingPlatform: <native>` and hands the Kernel instance to the app. **Gate: `OEVM_PATH=AB` green.**

- [ ] **Step 1: Serializer — replace `kernel_array_ref` with the Kernel descriptor in `header_bytes`**

In `tools/oimg_serialize.py`, `header_bytes` currently is (`:62-109`):

```python
    def header_bytes(self, blob_size, cluster_count, root_ref,
                     init_sel_ref=0, main_sel_ref=0, init_argc=0, entry_nslots=0,
                     kernel_array_ref=0):
```

with a trailing block (`:104-109`):

```python
        # std-lib step 3a: kernel Array class vtable (format_version 2)
        if kernel_array_ref:
            h += self.word(kernel_array_ref)
            # pad the whole header to ALIGN so the object region stays 8-aligned
            if len(h) % ALIGN:
                h += b"\x00" * (ALIGN - (len(h) % ALIGN))
        return h
```

Change the signature and the trailing block to carry the three Kernel-descriptor fields (format_version 3). New signature:

```python
    def header_bytes(self, blob_size, cluster_count, root_ref,
                     init_sel_ref=0, main_sel_ref=0, init_argc=0, entry_nslots=0,
                     kernel_vtable_ref=0, kernel_init_sel_ref=0, kernel_nslots=0):
```

Update the version line near the top of the method — currently:

```python
        version = 2 if kernel_array_ref else self.version
```

to:

```python
        version = 3 if kernel_vtable_ref else self.version
```

And replace the trailing `kernel_array_ref` block with the descriptor (keep the doc-comment layout in sync):

```python
        # std-lib step 3b: Kernel module descriptor (format_version 3). Layout from
        # the ref-word block start A (= 24 on W==8): kernel_vtable_ref @A+4W (56),
        # kernel_init_sel_ref @A+5W (64), kernel_nslots (u16) @A+6W (72). Header is
        # padded to ALIGN (80 on W==8). Replaces 3a's single kernel_array_ref @A+4W.
        if kernel_vtable_ref:
            h += self.word(kernel_vtable_ref)
            h += self.word(kernel_init_sel_ref)
            h += self.u16(kernel_nslots)
            if len(h) % ALIGN:
                h += b"\x00" * (ALIGN - (len(h) % ALIGN))
        return h
```

Also update the byte-layout doc comment in `header_bytes` (`:64-87`) so the CONTRACT block describes the format-3 descriptor (`kernel_vtable_ref @A+4W`, `kernel_init_sel_ref @A+5W`, `kernel_nslots @A+6W`, header 80 bytes on W==8) instead of `kernel_array_ref @A+4W` / 64 bytes. This comment is the mirror of `oimg_loader.c`'s contract — keep them identical.

- [ ] **Step 2: Serializer — compute the descriptor in `generate`**

In `tools/oimg_serialize.py`, `generate` currently takes (`:245-247`):

```python
    def generate(self, graph, entry_vtable_name,
                 init_sel_name=None, init_argc=0, main_sel_name="main:args:",
                 entry_nslots=0, kernel_array_vtable_name=None):
```

Change the kernel parameter to the descriptor triple:

```python
    def generate(self, graph, entry_vtable_name,
                 init_sel_name=None, init_argc=0, main_sel_name="main:args:",
                 entry_nslots=0, kernel_vtable_name=None, kernel_init_sel_name=None,
                 kernel_nslots=0):
```

The kernel-ref computation currently is (`:412`):

```python
        # std-lib step 3a: kernel Array class vtable (format_version 2).
        kernel_array_ref = self._vtable_ref(kernel_array_vtable_name) if kernel_array_vtable_name else 0
```

Replace with the descriptor refs (vtable ref via `_vtable_ref`, init-selector ref via the symbol-ref helper used for `init_sel_ref`/`main_sel_ref`):

```python
        # std-lib step 3b: Kernel module descriptor (format_version 3).
        kernel_vtable_ref = self._vtable_ref(kernel_vtable_name) if kernel_vtable_name else 0
        kernel_init_sel_ref = self._symbol_ref(kernel_init_sel_name) if kernel_init_sel_name else 0
```

> Implementer note: use the **same** ref-builder that `init_sel_ref`/`main_sel_ref` use for selector symbols (find where `init_sel_ref`/`main_sel_ref` are computed in `generate` and reuse that exact helper — it is `_symbol_ref` or an inline `_aref("sym:...")`; mirror it precisely so `kernel_init_sel_ref` is built identically). Then thread the three values into the `header_bytes(...)` call (replace the `kernel_array_ref=...` keyword arg with `kernel_vtable_ref=kernel_vtable_ref, kernel_init_sel_ref=kernel_init_sel_ref, kernel_nslots=kernel_nslots`).

- [ ] **Step 3: Compiler Path B call site — pass the Kernel descriptor**

In `tools/ns_compile_oevm.py`, the Path B serialization call currently is (`:1799-1810`):

```python
    # std-lib step 3a: pass kernel Array vtable name so the serializer
    # stores it as kernel_array_ref in the header (format_version 2).
    _kernel_array = "Array" if "Array" in compiler.all_classes else None
    blob = OimgSerializer(layout, REGISTRY, registry_version=VERSION).generate(
        _Graph, entry_class,
        init_sel_name=_init_selector, init_argc=_init_argc,
        main_sel_name="main:args:", entry_nslots=_entry_nslots,
        kernel_array_vtable_name=_kernel_array)
```

Replace with the Kernel descriptor:

```python
    # std-lib step 3b: pass the Kernel module descriptor (vtable name, init selector,
    # slot count) so the serializer stores it in the header (format_version 3). The
    # loader constructs `Kernel usingPlatform: <native>` and hands it to the app.
    _kparams = compiler.all_classes["Kernel"].get("params", [])
    _kinit_selector = f"init_{''.join(f'{p}:' for p in _kparams) or 'new'}"
    _kernel_nslots = len(compiler.class_slots["Kernel"])
    blob = OimgSerializer(layout, REGISTRY, registry_version=VERSION).generate(
        _Graph, entry_class,
        init_sel_name=_init_selector, init_argc=_init_argc,
        main_sel_name="main:args:", entry_nslots=_entry_nslots,
        kernel_vtable_name="Kernel", kernel_init_sel_name=_kinit_selector,
        kernel_nslots=_kernel_nslots)
```

- [ ] **Step 4: Loader — read the Kernel descriptor (oimg_loader.c)**

In `../oevm/src/oimg_loader.c`:

(a) Update the header contract comment (`:23-44`) to describe format 3 (`kernel_vtable_ref @A+4W`, `kernel_init_sel_ref @A+5W`, `kernel_nslots @A+6W`), mirroring the serializer doc edited in Step 1.

(b) Replace the `kernel_array_ref` static (`:53-55`):

```c
/* std-lib step 3a: kernel Array class vtable ref-word (format_version 2+).
   Stored at a + 4*W in the header (immediately after the entry_nslots pair). */
static uintptr_t img_kernel_array_ref  = 0;
```

with the descriptor statics:

```c
/* std-lib step 3b: Kernel module descriptor ref-words (format_version 3+).
   At a+4W (vtable), a+5W (init selector); kernel_nslots (u16) at a+6W. */
static uintptr_t img_kernel_vtable_ref   = 0;
static uintptr_t img_kernel_init_sel_ref = 0;
static uint16_t  img_kernel_nslots       = 0;
```

(c) In the load routine, the format check + descriptor read currently is (`:90-114`):

```c
    if (format_version != 1 && format_version != 2) {
        fprintf(stderr, "oimg_load: unsupported format_version %u\n", format_version);
        munmap(map, sz); return 1;
    }
    ...
    img_entry_nslots      = read_u16(base + a + 3 * W + 2);
    /* std-lib step 3a: kernel Array class vtable (format_version 2+).
       At a + 4*W (= 56 on 64-bit, 36 on 32-bit). 0 / NULL when absent. */
    img_kernel_array_ref  = (format_version >= 2) ? read_word(base + a + 4 * W) : 0;
```

Change the accepted version and the descriptor read:

```c
    if (format_version != 1 && format_version != 3) {
        fprintf(stderr, "oimg_load: unsupported format_version %u\n", format_version);
        munmap(map, sz); return 1;
    }
    ...
    img_entry_nslots      = read_u16(base + a + 3 * W + 2);
    /* std-lib step 3b: Kernel module descriptor (format_version 3+). 0 when absent. */
    img_kernel_vtable_ref   = (format_version >= 3) ? read_word(base + a + 4 * W) : 0;
    img_kernel_init_sel_ref = (format_version >= 3) ? read_word(base + a + 5 * W) : 0;
    img_kernel_nslots       = (format_version >= 3) ? read_u16 (base + a + 6 * W) : 0;
```

(d) Replace the resolve helper (`:127-131`):

```c
/* std-lib step 3a: resolve the kernel Array class vtable from the loaded image.
   Mirrors oimg_root(); returns NULL when the image has no Array vtable (format < 2). */
oop oimg_kernel_array_vtable(void) {
    return oimg_resolve(img_kernel_array_ref);
}
```

with the Kernel-descriptor accessor (used by `oimg_bootstrap` below):

```c
/* std-lib step 3b: resolve the Kernel module vtable from the loaded image.
   Returns NULL when the image has no Kernel descriptor (format < 3). */
oop oimg_kernel_vtable(void) {
    return oimg_resolve(img_kernel_vtable_ref);
}
```

- [ ] **Step 5: Loader — two-stage `oimg_bootstrap` (oimg_loader.c)**

In `../oevm/src/oimg_loader.c`, `oimg_bootstrap` currently is (`:133-146`):

```c
oop oimg_bootstrap(oop entry_vtable) {
    extern oop send(oop receiver, oop selector, int argc, oop *args);
    extern oop gc_allocate(oop vtable, size_t size);

    oop inst = gc_allocate(entry_vtable,
                           sizeof(struct ObjectInstance) + img_entry_nslots * sizeof(oop));

    oop init_args[2] = { platform_object(), TAG_SMI(0) };
    send(inst, oimg_resolve(img_init_sel_ref), (int)img_init_argc, init_args);

    oop main_args[2] = { platform_object(), TAG_SMI(0) };
    oop res = send(inst, oimg_resolve(img_main_sel_ref), 2, main_args);
    return res;
}
```

Replace with the two-stage version (construct Kernel from the native platform, then hand the Kernel instance to the app):

```c
oop oimg_bootstrap(oop entry_vtable) {
    extern oop send(oop receiver, oop selector, int argc, oop *args);
    extern oop gc_allocate(oop vtable, size_t size);

    /* std-lib step 3b: (1) native platform; (2) construct Kernel usingPlatform:
       <native>; (3) hand the Kernel INSTANCE to the app as its `platform`. The
       Kernel signature is fixed by the design (usingPlatform: -> 1 cap param). */
    oop native_platform = platform_object();
    oop kernel_inst = gc_allocate(oimg_kernel_vtable(),
                                  sizeof(struct ObjectInstance) + img_kernel_nslots * sizeof(oop));
    oop kernel_init_args[1] = { native_platform };
    send(kernel_inst, oimg_resolve(img_kernel_init_sel_ref), 1, kernel_init_args);

    oop inst = gc_allocate(entry_vtable,
                           sizeof(struct ObjectInstance) + img_entry_nslots * sizeof(oop));

    oop init_args[2] = { kernel_inst, TAG_SMI(0) };
    send(inst, oimg_resolve(img_init_sel_ref), (int)img_init_argc, init_args);

    oop main_args[2] = { kernel_inst, TAG_SMI(0) };
    oop res = send(inst, oimg_resolve(img_main_sel_ref), 2, main_args);
    return res;
}
```

- [ ] **Step 6: Remove the `kernel_array_vtable` wiring in the image main (oevm_image_main.c)**

In `../oevm/oevm_image_main.c`, delete the wiring block (`:29-35`):

```c
    /* std-lib step 3a: wire kernel Array class vtable from the loaded image
       (format_version 2+). Returns NULL for format_version 1 images → PRIM_Array
       returns NULL (no mock fallback; mock was deleted in step 3a). */
    {
        extern oop kernel_array_vtable;
        kernel_array_vtable = oimg_kernel_array_vtable();
    }
```

(The Kernel is now constructed inside `oimg_bootstrap`; no external wiring remains. If `oimg_kernel_array_vtable` is declared in a header (`oimg.h`/`oimg_loader.h`), remove that prototype too.)

- [ ] **Step 7: Build the image VM, then run the suite both paths**

```bash
cd /home/david/workspace/ns-oevm
python3 tools/run_tests_oevm.py
```

Expected: 53 pass / 3 skip on **both** Path A and Path B (the harness rebuilds `/tmp/oevm_img` from the image-VM sources). The `29_kernel_module.ns` test now also passes on Path B.

> Implementer note (Path B risk): the header byte-layout is the one place a mismatch is silent-then-fatal. If Path B fails to load (`unsupported format_version` / bad ref / wrong slot count), re-derive the offsets from the serializer doc comment (Step 1) and the loader contract (Step 4) side-by-side — they must be byte-identical. `oimg_serialize.py` already asserts emitted `header_len == precomputed` (commit `d50f7c9`); make sure that assert still holds after the layout change. If the descriptor work balloons beyond this task, report DONE_WITH_CONCERNS rather than hacking offsets.

- [ ] **Step 8: Commit (two repos)**

```bash
git -C /home/david/workspace/ns-oevm add tools/oimg_serialize.py tools/ns_compile_oevm.py
git -C /home/david/workspace/ns-oevm commit -m "feat(stdlib): .oimg Kernel descriptor (format_version 3); retire kernel_array_ref"
git -C /home/david/workspace/oevm add src/oimg_loader.c oevm_image_main.c
git -C /home/david/workspace/oevm commit -m "feat(stdlib): two-stage oimg_bootstrap (construct Kernel, hand to app); retire kernel_array_vtable"
```

---

### Task 5: `Collection` (abstract) + `OrderedCollection` (growable) + tests

**Files:**
- Modify: `lib/kernel.ns` (add `Collection`, `OrderedCollection` nested in `Kernel`, + accessors)
- Test: `../ns-tests/04_somns_basic/30_collections.ns`

**Interfaces:**
- Consumes: the Kernel module + accessor pattern + `platform basicNew: Array indexable: n` allocator (Tasks 3-4); NLR (`^` out of a block) for `detect:`.
- Produces: `platform Collection` / `platform OrderedCollection` return usable classes; `OrderedCollection` answers `add:`/`addLast:`/`do:`/`size`/`isEmpty`/`at:`/`at:put:`/`first`/`last`/`removeFirst`/`removeLast` and the inherited functional protocol `inject:into:`/`collect:`/`select:`/`reject:`/`includes:`/`asArray`/`detect:`/`detect:ifNone:`. `Array` is unchanged (Option C — not a `Collection`).

- [ ] **Step 1: Write the failing test**

Create `../ns-tests/04_somns_basic/30_collections.ns`:

```
(*
expect:
0
true
1
2
3
4
5
5
false
2
4
6
8
10
15
3
9
1
5
1
*)
class CollectionsTest packageUsing: manifest = () (
public main: platform args: args = (
    | oc summed evens doubled found counter |
    oc:: OrderedCollection new.
    oc size println.            "0"
    oc isEmpty println.         "true"
    "add: past the initial capacity (8) to force a grow-by-copy"
    1 to: 5 do: [ :i | oc add: i ].
    oc do: [ :x | x println ].  "1 2 3 4 5"
    oc size println.            "5"
    oc isEmpty println.         "false"
    doubled:: oc collect: [ :x | x * 2 ].
    doubled do: [ :x | x println ].   "2 4 6 8 10"
    summed:: oc inject: 0 into: [ :a :b | a + b ].
    summed println.             "15"
    evens:: oc select: [ :x | (x \\ 2) = 0 ].
    evens size println.         "2 (i.e. 2 and 4)"
    (evens at: 2) println.      "4 (the second even)"
    "detect: must short-circuit — counter counts predicate evaluations"
    counter:: OrderedCollection new.
    found:: oc detect: [ :x | counter add: x. x = 1 ].
    found println.              "1"
    counter size println.       "1 (stopped at the first element)"
)
) : (
)
```

- [ ] **Step 2: Run to verify it fails**

```bash
cd /home/david/workspace/ns-oevm
python3 tools/ns_compile_oevm.py ../ns-tests/04_somns_basic/30_collections.ns ../oevm/src/solidified_image.c
(cd ../oevm && gcc -Wall -Wextra -std=c99 -DOEVM_APPLIANCE src/vm.c src/gc.c src/dispatch.c src/solidified_image.c src/oevm_main.c -o oevm_runner -lm && ./oevm_runner)
```

Expected: FAIL — `OrderedCollection` is not yet a Kernel accessor (`platform OrderedCollection` → `doesNotUnderstand`).

- [ ] **Step 3: Add `Collection` and `OrderedCollection` to `lib/kernel.ns`**

In `lib/kernel.ns`, insert the two classes **after** the `Array` class and **before** the accessors (order matters — `Collection` must precede `OrderedCollection` for the sibling slot-layout merge; `collect_slots` requires the parent declared textually first):

```
  class Collection = () (
    do: aBlock = ( ^ self error: 'subclassResponsibility' )
    public size = ( ^ self inject: 0 into: [ :n :e | n + 1 ] )
    public isEmpty = ( ^ self size = 0 )
    public inject: a into: b = ( | acc | acc:: a. self do: [ :e | acc:: b value: acc value: e ]. ^ acc )
    public collect: b = ( | r | r:: OrderedCollection new. self do: [ :e | r add: (b value: e) ]. ^ r )
    public select: b = ( | r | r:: OrderedCollection new. self do: [ :e | (b value: e) ifTrue: [ r add: e ] ]. ^ r )
    public reject: b = ( | r | r:: OrderedCollection new. self do: [ :e | (b value: e) ifFalse: [ r add: e ] ]. ^ r )
    public includes: o = ( ^ self inject: false into: [ :acc :e | acc or: [ e = o ] ] )
    public asArray = ( | arr i | arr:: platform basicNew: Array indexable: self size. i:: 1. self do: [ :e | arr at: i put: e. i:: i + 1 ]. ^ arr )
    public detect: b ifNone: n = ( self do: [ :e | (b value: e) ifTrue: [ ^ e ] ]. ^ n value )
    public detect: b = ( ^ self detect: b ifNone: [ self error: 'not found' ] )
  ) : (
  )

  class OrderedCollection = Collection (
    | contents ::= platform basicNew: Array indexable: 8. count ::= 0. |
    public do: aBlock = ( 1 to: count do: [ :i | aBlock value: (contents at: i) ]. ^ self )
    public size = ( ^ count )
    public isEmpty = ( ^ count = 0 )
    public add: anObject = ( self ensureCapacity. count:: count + 1. contents at: count put: anObject. ^ anObject )
    public addLast: anObject = ( ^ self add: anObject )
    public at: i = ( (i < 1 or: [ i > count ]) ifTrue: [ ^ self error: 'index out of bounds' ]. ^ contents at: i )
    public at: i put: v = ( (i < 1 or: [ i > count ]) ifTrue: [ ^ self error: 'index out of bounds' ]. ^ contents at: i put: v )
    public first = ( ^ self at: 1 )
    public last = ( ^ self at: count )
    public removeLast = ( | v | v:: contents at: count. count:: count - 1. ^ v )
    public removeFirst = ( | v | v:: contents at: 1. 2 to: count do: [ :i | contents at: i - 1 put: (contents at: i) ]. count:: count - 1. ^ v )
    ensureCapacity = ( count = contents size ifTrue: [ self grow ]. ^ self )
    grow = ( | bigger | bigger:: platform basicNew: Array indexable: 2 * contents size. 1 to: count do: [ :i | bigger at: i put: (contents at: i) ]. contents:: bigger. ^ self )
  ) : (
  )
```

Then add the two accessors alongside the existing `Object`/`Array` accessors:

```
  public Object            = ( ^ Object )
  public Array             = ( ^ Array )
  public Collection        = ( ^ Collection )
  public OrderedCollection = ( ^ OrderedCollection )
```

(`Array` stays `class Array = ()` — Option C, NOT `= Collection`. `OrderedCollection` allocates its backing via `platform basicNew: Array indexable: ...` — never `Array new:` — per the Global Constraints. `contents`/`count` are initialized as slot initializers, which run with `self.outer == Kernel` already set, so `platform`/`Array` resolve via the instance outer-walk.)

- [ ] **Step 4: Build + run the test (Path A)**

```bash
cd /home/david/workspace/ns-oevm
python3 tools/ns_compile_oevm.py ../ns-tests/04_somns_basic/30_collections.ns ../oevm/src/solidified_image.c
(cd ../oevm && gcc -Wall -Wextra -std=c99 -DOEVM_APPLIANCE src/vm.c src/gc.c src/dispatch.c src/solidified_image.c src/oevm_main.c -o oevm_runner -lm && ./oevm_runner)
```

Expected: the exact `expect:` block (`0`, `true`, `1 2 3 4 5`, `5`, `false`, `2 4 6 8 10`, `15`, `2`, `4`, `1`, `1`).

- [ ] **Step 5: Run the full suite both paths**

```bash
cd /home/david/workspace/ns-oevm
python3 tools/run_tests_oevm.py
```

Expected: 54 pass / 3 skip on both paths (53 from Task 4 + `30_collections.ns`).

- [ ] **Step 6: Commit (two repos)**

```bash
git -C /home/david/workspace/ns-oevm add lib/kernel.ns
git -C /home/david/workspace/ns-oevm commit -m "feat(stdlib): Collection (abstract) + OrderedCollection (Array-backed, growable)"
git -C /home/david/workspace/ns-tests add 04_somns_basic/30_collections.ns
git -C /home/david/workspace/ns-tests commit -m "test(stdlib): Collection/OrderedCollection protocol, growth, detect: short-circuit"
```

---

## Self-Review

**Spec coverage (against `docs/design/2026-06-20-ns-oevm-stdlib-step3b-kernel-module-collections.md`, as amended in Task 1):**
- §2 static-bound-classes decision → recorded in `docs/NEWSPEAK_DIALECT.md` (#1, #2) (Task 1).
- §3 module stack + two-stage boot (both paths) + three reachability paths → Task 3 (Path A boot-gen, kernel.ns), Task 4 (Path B loader). Kernel→primitives via injected `platform`; Kernel→Kernel via compile-time sibling refs; App→Kernel via accessors.
- §4 Kernel module structure + capability-pure `basicNew: <class> indexable: n` + hand-written accessors → Task 3 (kernel.ns + prim). **Array-as-Collection deliberately dropped (Option C)** → Task 1 amends §4; deviation #3.
- §5 Collection (abstract, functional folds; detect: via NLR) → Task 5.
- §6 OrderedCollection (contents+count, grow-by-copy, full protocol) → Task 5.
- §7 migration / magic-prim retirement (PRIM_Array, kernel_array_vtable global+wiring+GC root, format_version 2 → general Kernel construction) → Task 3 (dispatch/boot/gc/oevm.h) + Task 4 (serializer/loader). Root-delegate retarget + two-stage boot sequenced first (Tasks 2-4) before collections (Task 5), per the risk posture.
- §8 testing (suite green both paths; module/Collection/OrderedCollection/detect: tests) → Tasks 2,3,5; suite gate every task. §8 "Array as a Collection" test replaced with the Option-C primitive test (Task 1).
- §9 deferred items (String/Dictionary/Set/booleans → 3c; auto-accessors; virtual classes; OC ring buffer; step-1 minors) → unchanged, not implemented here.

**Placeholder scan:** the two soft spots both carry explicit implementer notes + escalation paths rather than fake one-liners: Task 4 Step 2 (reuse the exact selector-ref helper that builds `init_sel_ref`/`main_sel_ref` — named precisely, to be confirmed against the file) and Task 4 Step 7 (Path B header byte-layout, with the keep-in-sync instruction and the existing `header_len` assert as a guard). Everything else has exact current code, the replacement, exact commands, and expected output.

**Type/name consistency:** `basicNew:indexable:` (selector), `PRIM_pf_basicNewIndexable` (enum, parallel to `PRIM_NAMES`), and its platform-switch case agree. `Kernel.Object` → `vt_id` `Kernel_Object` → `Vtable_Kernel_Object` matches the retargeted `delegate = "Kernel_Object"` and the simple-name root check. The Kernel init selector is derived identically in Path A (`kinit_selector`) and Path B (`_kinit_selector`) from `compiler.all_classes["Kernel"]["params"]` → `init_platform:`. Kernel slot count is `len(compiler.class_slots["Kernel"])` in both paths. The `.oimg` descriptor offsets (`@A+4W/+5W/+6W`, format_version 3, 80-byte header on W==8) are stated identically in the serializer doc-comment and the loader contract (the keep-in-sync instruction).

**Risk notes (flagged for execution):**
- Task 2's compiler fix is additive; if any *existing* test regresses it signals a pre-existing class-side dependency on the old behavior — investigate, don't paper over (Step 6).
- Task 3 leaves Path B red by design; the gate is `OEVM_PATH=A`. Both paths are green again only after Task 4. Do not merge between Tasks 3 and 4.
- Inside `lib/kernel.ns`, `Array new:` is forbidden (compiles to a non-indexable `OP_NEW`); only `platform basicNew: Array indexable: n` allocates indexables. This is the single most likely implementation slip — called out in the Global Constraints and at each use site.
