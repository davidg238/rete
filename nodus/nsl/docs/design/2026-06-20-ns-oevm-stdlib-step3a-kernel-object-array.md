# ns-oevm std-lib step 3a — kernel infrastructure + Object + Array

**Date:** 2026-06-20
**Status:** design — ratified in the 2026-06-20 brainstorm; ready for writing-plans.
**Repo note:** this repo (`ns-oevm`) holds compiler/grammar/docs/`lib`; the VM is a
**sibling repo at `../oevm`** (`../oevm/src/{dispatch,vm,gc,kernel_boot}.c`, `oevm.h`).
The conformance suite is a **third sibling repo `../ns-tests`**. All `../oevm/...`
paths are relative to this repo's cwd.
**Depends on:** std-lib steps 1 (real indexable repr + `platform_object()`) and 2
(boot seams hand real platform; compiler routes capitalised globals through the
threaded `platform`) — both merged to master across all three repos.
**Parent spec:** `docs/design/2026-06-20-ns-oevm-stdlib-design.md` (§8 step 3).

---

## 1. Decomposition of step 3

Spec §8 lists step 3 as one step, but it bundles a new kernel-compilation
mechanism, a root-hierarchy change, and the library classes — too much for one
plan. Step 3 splits into three independently-shippable sub-steps, each keeping the
conformance suite green:

- **3a (this spec):** kernel infrastructure (compile `lib/kernel.ns` into every
  image; wire kernel classes into `platform`) + `Object` as a real root class +
  `Array.ns` replacing the C `PRIM_Array` mock.
- **3b (later):** `Collection` (abstract) + `OrderedCollection`; `Array` gains the
  derived enumeration protocol (`collect:`/`select:`/…). `String` lands here or 3c.
- **3c (later):** `Dictionary` + `Set` + the boolean `hash`/`=`/`printString`
  primitives (§4 of the parent spec) — the key-capable step.

`String` is **deferred out of 3a** (parent-spec §9 soft item).

---

## 2. Kernel build — concatenate now, outer-module later

The compiler currently compiles a single `.ns` file into one image
(`ns_compile_oevm.py:1681`, `src_ns = positional[0]`). 3a adds a fixed kernel
source `lib/kernel.ns` that the compiler compiles **together with the app**, as
additional sibling top-level module(s), into the same image. Multiple top-level
modules already work structurally (`map_classes` iterates `program.body`).

**Reachability is via `platform`, never lexical nesting.** App code reaches kernel
classes only through the step-2 routing (`Array` compiles to `platform Array`), so
the app source is **byte-identical** whether the kernel is a sibling module or an
enclosing one. This makes the future migration to the Newspeak-idiomatic "kernel
as the outer module that wraps the app" (`Kernel usingPlatform: platform` with the
app nested/injected) an internal change to the kernel build — it never touches app
sources or the capability model. 3a does the simple concatenation; the outer-module
form is the recorded later direction, not 3a work.

Per-target library-variant selection (parent-spec §3.1) becomes "pick which
`kernel.ns`" at build time.

---

## 3. Platform binding + mock retirement

In step 2, `send(platform, "Array")` returns the mock via `PRIM_Array`
interception (fires by selector before vtable lookup, `dispatch.c:640-663`,
regardless of receiver). In 3a it must return the **real** kernel `Array` class
object.

**Binding.** Boot wires the compiled kernel class objects into the `platform`
object's vtable (`Array` → the compiled Array class object). `platform Array` then
resolves by ordinary vtable lookup. The wiring happens at boot on both paths
(generated entry C for Path A; `oimg_loader.c` for Path B), after the kernel
vtables exist.

**Mock retirement — two moves, both inside 3a:**
1. **Gate:** make `PRIM_Array` (and the mock's `new:`/`at:`/`at:put:`/`size`)
   fire only when no real platform binding exists — so the suite stays green while
   `Array.ns` is introduced.
2. **Delete:** remove the `PRIM_Array`/`new:`/`at:`/`at:put:`/`size` mock
   interception once `Array.ns` covers it.

**Keep** the indexable *storage* primitives `array:` / `basicAt:` /
`basicAt:put:` / `basicSize` — these are the genuine VM primitives that `Array.ns`
calls; they are not the mock.

The namespace-Dictionary and direct-class-ref alternatives were rejected
(Dictionary is 3c → bootstrap chicken-and-egg; direct refs break capability-purity
and per-target variants).

---

## 4. Object as a real root + primitive gating

Today `Object` is a fake "builtin root": `= Object` means *no superclass*
(delegate NULL) and Object-protocol selectors are served by VM primitive
interception (`ns_compile_oevm.py:1341-1345`). The Object-protocol prims
(`printString`/`println`/`asString`/`==`/`=`/`isNil`/`notNil`/`error:`/…,
`dispatch.c:580-598`) switch on receiver type and fall to `"[Object %p]"` for heap
objects (`dispatch.c:1086-1108`).

**3a makes `Object` a real `.ns` class:**

- **`Object.ns` is the real root**, delegate NULL.
- **Compiler:** rootless heap classes (`= Object` or no super clause) delegate to
  Object's vtable instead of NULL. (`superExpr`/navigating-super classes unchanged.)
- **VM gating (universal root for heap objects):** the Object-protocol prims that
  `Object.ns` implements via `platform` — `printString`, `asString`, `isNil`,
  `notNil`, `ifNil:`, `ifNotNil:`, `hash`, `=` — are **receiver-gated to fall
  through to vtable lookup for heap-object receivers** (the `break`→method-dispatch
  path at `dispatch.c:1251-1261`), while intrinsic receivers (SMI/Float/Symbol)
  keep their primitive branches. No conformance test asserts the current
  `"[Object %p]"` heap output, so the change is safe.
- **Stay universal prims (NOT gated):** `==` (identity), arithmetic, comparisons,
  and **`error:`** (RESOLVED 2026-06-20 — `error:` aborts at the VM level;
  `Object.ns` cannot abort without a new primitive, so `error:` keeps its universal
  prim; no `platform error:` is added in 3a).
- **Intrinsics are NOT reparented** in 3a (Integer/Float/Symbol/Block/Boolean keep
  primitive Object-protocol). Booleans gain `hash`/`=`/`printString` in 3c.

---

## 5. Class protocols (minimal — retire the mock with green tests)

- **`Object.ns`** (root, delegate NULL): `printString` (default — class/identity
  string), `asString`, `isNil` (→ false), `notNil` (→ true), `ifNil:` /
  `ifNotNil:`, `=` (→ `platform object: self identicalTo: arg`), `hash`
  (→ `platform objHashcode: self`).
- **`Array.ns`** (`= Object`): instance `at:`, `at:put:`, `size`, `do:` — pure
  `.ns`, calling `basicAt:` / `basicAt:put:` / `basicSize` on **`self`**. The
  enumeration protocol (`collect:`/`select:`/…) arrives with `Collection` in 3b.

**Array representation (RATIFIED 2026-06-20): an Array instance IS the indexable
object.** Allocation (`Array new: n`, `{…}`, and `platform array: n`) produces an
object whose **vtable is the kernel Array vtable** with the length tagged-SMI at
slot 0 and elements at slots 1..n — no separate wrapper/`storage` slot. Therefore:

- The `basicAt:` / `basicAt:put:` / `basicSize` storage prims are gated to accept a
  receiver whose vtable is **either `IndexableVtable` or the kernel Array vtable**
  (today they fire only on `IndexableVtable`). Array.ns instance methods send these
  to `self`.
- **Allocation stays a VM primitive** (the mock's class-side `new:` is redirected,
  not deleted: when the receiver is the kernel Array class vtable, allocate an
  Array-vtable indexable instead of the mock). Raw allocation is legitimately a
  VM/platform primitive (parent-spec §4b), so this is the principled line, not a
  shortcut. **Consequence: NO `platform` is needed inside any `Array.ns` method** —
  this is precisely what lets 3a avoid threading platform into the kernel.

Protocol is intentionally the minimum needed to (a) make every conformance use of
`Array`/`{…}`/`do:`/`new:` resolve to `.ns` code and (b) give heap objects a real
Object protocol. Anything beyond that is 3b/3c.

---

## 6. Boot / GC wiring

- Kernel class **vtables** and the `platform`→class **bindings** are registered as
  GC roots, mirroring the `debugger_hook` root-marking pattern (parent-spec §3.2,
  `gc.c:626-629`).
- Boot constructs the kernel class objects and installs the platform bindings
  **before** the app's `init`/`main:args:` run, on both Path A (generated entry C)
  and Path B (`oimg_loader.c`).

---

## 7. Testing

- **Regression net:** the existing conformance suite (48 pass / 3 skip on both
  Path A and Path B) must stay green throughout — `Array`/`new:`/`at:`/`at:put:`/
  `size`/`do:` move from C into `.ns` with observable behaviour preserved. This is
  the migration's safety net.
- **New protocol tests** (in `../ns-tests`): `Array.ns` over real indexable storage
  (allocate, at:put:/at: round-trip, size, do: iteration, `{…}` literal); a
  heap-object test proving `Object.ns` now serves `printString`/`=`/`hash`/`isNil`
  for a user class (not the old `"[Object %p]"`).
- **Bar per milestone:** green on `oevm_runner`, both paths — run the suite from the
  `ns-oevm` cwd (`python3 tools/run_tests_oevm.py`).

---

## 8. Open questions (recorded — RECHECK LATER, not 3a work)

- **(A) `error:` / abort as a capability — RECHECK LATER.** `self error:` (used by
  sieve's `assert:equals:`) is a VM prim that aborts; `Object.ns` cannot abort
  without a primitive. RESOLVED for 3a: keep `error:` a **universal prim** (don't
  gate it; no `platform error:` added). Revisit when error-handling / full
  capability-purity is taken up — whether to add a `platform error:` (or a
  reflective/`mirror` signalling capability) so `Object.ns` owns `error:` like the
  rest of its protocol, rather than leaving an un-gated VM prim.
- **(B) Boolean / intrinsic protocol via Object — RECHECK LATER.** In 3a,
  intrinsics (booleans/Integer/Float/Symbol/Block) are NOT reparented to
  `Object.ns`, so they don't get `ifNil:` etc. from it. This is the same axis as
  "full root including intrinsics" below — confirmed deferred. Booleans get
  `hash`/`=`/`printString` in 3c; revisit full intrinsic reparenting then.
- **Full root including intrinsics?** Whether Integer/Float/Symbol/Block/Boolean
  should eventually delegate to `Object.ns` (removing the per-type primitive
  Object-protocol branches) for a uniform end state. Deferred; likely revisited in
  3c (booleans) or a dedicated step. The 3a "universal root" covers heap objects only.
- **Kernel as outer module.** Migrating from sibling-concatenation (§2) to the
  Newspeak-idiomatic enclosing-module form. Binding-compatible by construction
  (reachability is via `platform`), so deferrable without app-source churn.
- **(C) Array as a wrapper + fully capability-pure allocation — RECHECK LATER.**
  3a makes an Array instance BE the indexable object and keeps allocation as a VM
  primitive gated on the Array-class receiver. Revisit moving to a wrapper instance
  that holds an indexable in a `storage` slot, with allocation done via `platform
  array:` from kernel code — which needs `platform` threaded into the kernel (the
  "kernel as outer module" form, §2). Pairs naturally with that migration.
- **`String` representation.** Indexable-backed `.ns` vs primitive core — settle
  when `String` lands (3b/3c).

## References

- `tools/ns_compile_oevm.py:1341-1345` — current fake `Object` builtin-root handling.
- `tools/ns_compile_oevm.py:1681` — single-file compile entry; `:1058-1069` `map_classes`.
- `../oevm/src/dispatch.c:580-598` — PRIM_NAMES (Object-protocol selectors).
- `../oevm/src/dispatch.c:640-663` — `PRIM_Array` mock interception (pre-vtable, any receiver).
- `../oevm/src/dispatch.c:1086-1108` — `printString` prim's per-receiver branches (`"[Object %p]"` heap fallback).
- `../oevm/src/dispatch.c:1251-1261` — send()'s fall-through to vtable method lookup.
- `../oevm/src/dispatch.c:439` (`platform_object`), `../oevm/src/gc.c:626-629` (root-marking pattern).
- Parent spec §3 (capability spine), §3.1 (per-target variants), §3.2 (GC), §5 (class scope), §8 (sequencing).
