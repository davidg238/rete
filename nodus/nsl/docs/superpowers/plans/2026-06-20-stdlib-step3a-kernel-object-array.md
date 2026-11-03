# Std-lib Step 3a — Kernel Infrastructure + Object + Array Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the C `PRIM_Array` mock with a real Newspeak `Array` class (and a real root `Object` class) compiled into every image from `lib/kernel.ns` and reached through the `platform` capability — keeping the conformance suite green on both image paths.

**Architecture:** A fixed `lib/kernel.ns` (top-level sibling classes `Object` and `Array`) is concatenated after the app and compiled into the same image. The compiler makes `Object` the real root: the class named `Object` delegates to NULL, every other rootless/`= Object` class delegates to `Vtable_Object` (the universal-root change). App code reaches kernel classes only through the step-2 `platform` routing (`Array` → `platform Array`); since the kernel classes are top-level siblings, `resolve_class_ref` never resolves them directly. An `Array` instance **is** the indexable storage object (vtable = the kernel `Array` vtable, length tagged-SMI at slot 0); allocation stays a VM primitive (redirected from the mock), and `Array.ns` instance methods call `basicAt:`/`basicSize` on `self`. Object-protocol primitives fall through to vtable lookup for heap-object receivers so `Object.ns` serves them, while intrinsics (SMI/Float/Symbol) keep their primitive branches.

**Tech Stack:** Python compiler (`tools/ns_compile_oevm.py`), `solidify.py`; C VM in sibling repo `../oevm` (`src/dispatch.c`, `src/gc.c`, `src/oevm_main.c`, `src/oevm_image_main.c`); a new Newspeak source `lib/kernel.ns`; pytest + the `../ns-tests` conformance suite.

## Global Constraints

- **Three sibling repos:** `ns-oevm` (compiler/harness/`lib`; cwd `~/workspace/ns-oevm`), `../oevm` (the C VM), `../ns-tests` (conformance tests + new tests). Paths are relative to the `ns-oevm` cwd.
- **Run the conformance suite from the `ns-oevm` cwd:** `python3 tools/run_tests_oevm.py` (wrong cwd silently reports 0/47). Default gate runs BOTH Path A (solidified-C) and Path B (`.oimg`).
- **Regression bar:** 48 pass / 3 skip on both paths (the migration safety net — moving `Array`/`new:`/`at:`/`at:put:`/`size`/`do:` from C into `.ns` preserves observable behaviour). "Green" = same counts as `master`.
- **Capability-purity (preserve step 2):** app code reaches kernel classes only via the threaded `platform`; kernel classes are top-level siblings so they are NOT directly resolvable by app code. The kernel itself needs no `platform` (allocation is a VM primitive; `Object.ns` uses `==`/literals).
- **Kernel reach is via `platform`, never lexical:** keeps the future "kernel as outer module" migration source-compatible (spec §2).
- **Array representation (RATIFIED):** an Array instance IS the indexable object (vtable = kernel Array vtable, `slots[0] = TAG_SMI(length)`, elements at `slots[1..n]`). No wrapper. Allocation stays a VM primitive. No `platform` inside any `Array.ns` method. (Wrapper + `platform array:` allocation is recorded as recheck-later, spec §8 (C).)
- **`error:` stays a universal prim** (not gated, not in `Object.ns`) — RESOLVED, spec §8 (A).
- **Intrinsics are NOT reparented** to `Object.ns` in 3a (recheck-later, spec §8 (B)).
- **Opcode/PRIM facts:** class object = the bare `(oop)&Vtable_<vt_id>`; `vt_id(qname) = qname.replace(".", "_")`; class-side sends dispatch via `Meta_<vt>`; vtable C symbols are `extern const struct Vtable Vtable_<vt_id>`.
- **Commit after each task; work on a branch in each repo touched; do NOT push** (push only on explicit say-so).

---

## File Structure

- `lib/kernel.ns` (create) — the kernel source: top-level `class Object` and `class Array`.
- `tools/ns_compile_oevm.py` (modify):
  - `main()` parse block (~`:1684-1709`) — after the `is_script` wrap, parse `lib/kernel.ns` and append its modules to `ast["body"]`.
  - `compile_class` delegate logic (~`:1339-1347`) — the universal-root rule.
- `../oevm/src/dispatch.c` (modify) — `kernel_array_vtable` global; gate `PRIM_Array`; redirect `new:`; extend `basicAt:`/`basicAt:put:`/`basicSize` gate; fall-through for Object-protocol prims; (final) delete the mock.
- `../oevm/src/oevm.h` (modify) — declare `extern oop kernel_array_vtable;`.
- `../oevm/src/gc.c` (modify) — mark `kernel_array_vtable` as a root.
- `../oevm/src/oevm_main.c` (modify, Path A) and `../oevm/src/oevm_image_main.c` (modify, Path B) — set `kernel_array_vtable` after vtables init, before entry init.
- `../ns-tests/04_somns_basic/25_object_protocol.ns`, `26_array_kernel.ns` (create) — new protocol tests.

---

### Task 1: Kernel concatenation + `Object` as the real root

**Files:**
- Create: `lib/kernel.ns` (Object only, in this task)
- Modify: `tools/ns_compile_oevm.py` `main()` parse block (~`:1707` after is_script wrap); `compile_class` (~`:1339-1347`)
- Modify: `../oevm/src/dispatch.c` (Object-protocol prim fall-through for heap objects)
- Test: `../ns-tests/04_somns_basic/25_object_protocol.ns`

**Interfaces:**
- Consumes: parsed app AST `{"type":"program","body":[...]}`; `vt_id(qname)`; `compile_class`'s `super_name`/`delegate`/`solidify.add_vtable(vt, delegate_name=...)`.
- Produces: every image now contains a top-level `Object` class (`Vtable_Object`); rootless/`= Object` heap classes delegate to `Vtable_Object`; the class named `Object` delegates to NULL. `Object.ns` serves `printString`/`asString`/`isNil`/`notNil`/`ifNil:`/`ifNotNil:`/`=` for heap-object receivers.

- [ ] **Step 1: Write the failing e2e test**

Create `../ns-tests/04_somns_basic/25_object_protocol.ns`. A user heap object's `isNil`/`printString`/`=` must now come from `Object.ns` (not the old `"[Object %p]"`):

```
(*
expect:
false
self-equal
hello
*)
class ObjectProtocolTest packageUsing: manifest = () (
class Widget = () (
  public describe = ( ^ 'hello' )
) : ()
public main: platform args: args = (
    | w |
    w:: Widget new.
    w isNil ifTrue: [ 'is-nil' println ] ifFalse: [ 'false' println ].
    (w = w) ifTrue: [ 'self-equal' println ] ifFalse: [ 'not-equal' println ].
    w describe println.
)
) : (
)
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
python3 tools/ns_compile_oevm.py ../ns-tests/04_somns_basic/25_object_protocol.ns ../oevm/src/solidified_image.c
(cd ../oevm && gcc -Wall -Wextra -std=c99 -DOEVM_APPLIANCE src/vm.c src/gc.c src/dispatch.c src/solidified_image.c src/oevm_main.c -o oevm_runner -lm && ./oevm_runner)
```

Expected: FAIL. Today `Widget` has no real `Object`, `w = w` hits the `=` prim (works), but `w isNil` and `=` semantics depend on prim handling; the test is constructed so that without `Object.ns` + concat the program either mis-resolves or the kernel isn't present. (At minimum `Object.ns` does not yet exist, so the suite-level proof in Step 7 is the real gate; this single-file run confirms the kernel is compiled in once Steps 3-6 land.)

- [ ] **Step 3: Create `lib/kernel.ns` with `Object`**

```
(*
ns-oevm kernel library (std-lib step 3a). Top-level sibling classes, reached by
app code only through the `platform` capability. No platform is needed here:
`=` uses the universal `==` identity primitive; isNil/printString use literals.
*)
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
```

- [ ] **Step 4: Concatenate `lib/kernel.ns` after the app in `main()`**

In `tools/ns_compile_oevm.py`, immediately after the `is_script` wrapping block ends (just before `# Compile` / `compiler = OevmCompiler(ast)` at ~`:1711`), insert:

```python
    # std-lib step 3a: concatenate the kernel library AFTER the app so the app's
    # first class stays the entry (entry = next(iter(all_classes))). Kernel classes
    # are top-level siblings: not nested in the app, so resolve_class_ref never
    # resolves them in app scope — app code still reaches them via `platform`
    # (step-2 routing). Reached only through platform → source-compatible with the
    # future "kernel as outer module" form.
    kernel_path = os.path.join(os.path.dirname(__file__), "..", "lib", "kernel.ns")
    kproc = subprocess.run(["node", parse_script, kernel_path],
                           capture_output=True, text=True)
    if kproc.returncode != 0:
        print("Kernel parse failed:")
        print(kproc.stderr or kproc.stdout)
        sys.exit(1)
    kernel_ast = json.loads(kproc.stdout)
    ast["body"].extend(kernel_ast["body"])
```

- [ ] **Step 5: Universal-root delegate rule in `compile_class`**

In `tools/ns_compile_oevm.py`, `compile_class`, replace the current Object special-case (`:1339-1347`):

```python
        super_name = node.get("super")
        delegate = None
        if super_name and super_name != "Object":
            # `Object` is the builtin root, not a user class; inheriting from it
            # is equivalent to having no superclass clause — the delegate is the
            # VM root (NULL). Object's protocol (printString, ==, etc.) is served
            # by primitive interception in the VM, not a user vtable.
            sup = self.resolve_class_ref(super_name)
            delegate = self.vt_id(sup[0]) if sup else super_name
        elif node.get("superExpr"):
```

with the universal-root rule:

```python
        super_name = node.get("super")
        delegate = None
        # std-lib step 3a: `Object` is now a REAL kernel class (lib/kernel.ns).
        #   - the class named `Object` is the root: delegate NULL.
        #   - every other rootless or `= Object` heap class delegates to Vtable_Object
        #     (the universal root), so all heap objects get Object.ns protocol.
        #   - a named non-Object super resolves as before.
        if qname.rsplit(".", 1)[-1] == "Object":
            delegate = None
        elif super_name and super_name != "Object":
            sup = self.resolve_class_ref(super_name)
            delegate = self.vt_id(sup[0]) if sup else super_name
        elif super_name == "Object" or (super_name is None and not node.get("superExpr")):
            delegate = "Object"
        elif node.get("superExpr"):
```

(The trailing `elif node.get("superExpr"):` and its body remain unchanged. Note `qname` is already bound at the top of `compile_class`, `:1332`.)

- [ ] **Step 6: Make Object-protocol prims fall through to vtable lookup for heap objects**

In `../oevm/src/dispatch.c`, the Object-protocol prim cases currently end their non-SMI/Float/Symbol branch by emitting `"[Object %p]"` and returning. For each of `PRIM_printString` (`:1098-1099`), `PRIM_asString` (`:1122-1123`), and `PRIM_println` (`:1079-1080`), replace the heap-object `else { snprintf(..., "[Object %p]", ...); }` (+ its return) with a fall-through so a real heap object reaches its vtable method:

For `PRIM_printString` change:
```c
                } else {
                    snprintf(buf, sizeof(buf), "[Object %p]", (void *)receiver);
                }
                extern oop gc_allocate(oop vtable, size_t size);
                oop sym_obj = gc_allocate(SymbolVtable, sizeof(struct Symbol));
                ...
                return sym_obj;
```
to:
```c
                } else {
                    // std-lib step 3a: a real heap object's printString is served
                    // by Object.ns — fall through to vtable method lookup.
                    break;
                }
                extern oop gc_allocate(oop vtable, size_t size);
                oop sym_obj = gc_allocate(SymbolVtable, sizeof(struct Symbol));
                ...
                return sym_obj;
```
Apply the analogous change to `PRIM_asString` (`break` in the heap-object `else`) and `PRIM_println` (replace `printf("[Object %p]\n", ...)` + `return receiver` path with `break`). For `PRIM_isNil`/`PRIM_notNil`/`==`/`=`: leave `==` and identity universal; for `isNil`/`notNil`, if they currently return a value for any non-nil receiver, gate them to `break` when the receiver is a heap `ObjectInstance` (vtable is neither Integer/Float/Symbol nor NULL) so `Object.ns isNil`/`notNil` serve heap objects. (NULL/`nil` and intrinsics keep their prim answers.)

After the prim `switch`, control already continues to `lookup_method_cached` (`:1255-1261`); a `break` therefore dispatches to the receiver's vtable → `Object.ns`.

- [ ] **Step 7: Run the e2e test and full suite (both paths)**

```bash
python3 tools/ns_compile_oevm.py ../ns-tests/04_somns_basic/25_object_protocol.ns ../oevm/src/solidified_image.c
(cd ../oevm && gcc -Wall -Wextra -std=c99 -DOEVM_APPLIANCE src/vm.c src/gc.c src/dispatch.c src/solidified_image.c src/oevm_main.c -o oevm_runner -lm && ./oevm_runner)
python3 tools/run_tests_oevm.py
```

Expected: the single-file run prints `false`, `self-equal`, `hello`. The suite is 48 pass / 3 skip on both paths (Array still served by the mock — unchanged this task).

- [ ] **Step 8: Commit (two repos)**

```bash
git add lib/kernel.ns tools/ns_compile_oevm.py ../ns-tests/04_somns_basic/25_object_protocol.ns
git commit -m "feat(stdlib): concatenate lib/kernel.ns; Object as real universal root"
git -C ../oevm add src/dispatch.c
git -C ../oevm commit -m "feat(stdlib): Object-protocol prims fall through to vtable for heap objects"
```
(`../ns-tests` change: commit it in `../ns-tests` on a matching feature branch — `git -C ../ns-tests add ... && git -C ../ns-tests commit ...`.)

---

### Task 2: `Array.ns` reached through `platform`; boot wiring; GC root

**Files:**
- Modify: `lib/kernel.ns` (add `Array`)
- Modify: `../oevm/src/dispatch.c` (`kernel_array_vtable` global; gate `PRIM_Array`; redirect `new:`; extend `basicAt:`/`basicAt:put:`/`basicSize` gate)
- Modify: `../oevm/src/oevm.h` (declare `extern oop kernel_array_vtable;`)
- Modify: `../oevm/src/gc.c` (mark `kernel_array_vtable`)
- Modify: `../oevm/src/oevm_main.c` (Path A boot), `../oevm/src/oevm_image_main.c` (Path B boot)
- Test: `../ns-tests/04_somns_basic/26_array_kernel.ns`

**Interfaces:**
- Consumes: `Vtable_Object` (Task 1); the kernel `Array` class's symbol `Vtable_Array` (qname `Array` → `vt_id` `Array`); `indexable_new`-style allocation; the `PRIM_Array`/`PRIM_new`/`PRIM_ix_basicAt`/`PRIM_ix_basicAt_put`/`PRIM_ix_basicSize` cases.
- Produces: `extern oop kernel_array_vtable;` set at boot to `(oop)&Vtable_Array`; `send(platform, "Array")` returns it; `<Array class> new: n` allocates an Array-vtable indexable; `basicAt:`/`basicAt:put:`/`basicSize` accept Array-vtable receivers; `Array.ns` `at:`/`at:put:`/`size`/`do:` work.

- [ ] **Step 1: Write the failing test**

Create `../ns-tests/04_somns_basic/26_array_kernel.ns`:

```
(*
expect:
3
10
20
30
*)
class ArrayKernelTest packageUsing: manifest = () (
public main: platform args: args = (
    | a |
    a:: Array new: 3.
    a at: 1 put: 10.
    a at: 2 put: 20.
    a at: 3 put: 30.
    a size println.
    a do: [ :x | x println ].
)
) : (
)
```

- [ ] **Step 2: Run to verify current behaviour (mock) still passes, then it will route through Array.ns**

```bash
python3 tools/ns_compile_oevm.py ../ns-tests/04_somns_basic/26_array_kernel.ns ../oevm/src/solidified_image.c
(cd ../oevm && gcc -Wall -Wextra -std=c99 -DOEVM_APPLIANCE src/vm.c src/gc.c src/dispatch.c src/solidified_image.c src/oevm_main.c -o oevm_runner -lm && ./oevm_runner)
```
Expected now: PASS via the mock (3,10,20,30). This test guards that the migration preserves behaviour; after this task the same output must come from `Array.ns`. (To prove the route changes, Step 8 temporarily breaks the mock to confirm `Array.ns` is what serves it.)

- [ ] **Step 3: Declare the global and add `Array` to the kernel**

In `../oevm/src/oevm.h`, near the other capability declarations (after `oop platform_object(void);`, ~`:295`):

```c
/* std-lib step 3a: the kernel Array class vtable (lib/kernel.ns), set at boot to
   (oop)&Vtable_Array. `platform Array` returns it; `new:` on it allocates an
   Array-vtable indexable; basicAt:/basicSize accept Array-vtable receivers. */
extern oop kernel_array_vtable;
```

In `lib/kernel.ns`, add (rootless → delegates to Object via Task 1's rule):

```
class Array = () (
  public at: i = ( ^ self basicAt: i )
  public at: i put: v = ( self basicAt: i put: v. ^ v )
  public size = ( ^ self basicSize )
  public do: aBlock = ( 1 to: self size do: [ :i | aBlock value: (self basicAt: i) ]. ^ self )
) : (
)
```

- [ ] **Step 4: Define `kernel_array_vtable` and wire it through the platform path in `dispatch.c`**

In `../oevm/src/dispatch.c`, near the mock statics (`:389-391`), add the definition:

```c
/* std-lib step 3a: set at boot (Path A: oevm_main.c; Path B: oevm_image_main.c)
   to (oop)&Vtable_Array. NULL until set → PRIM_Array falls back to the mock. */
oop kernel_array_vtable = NULL;
```

Gate `PRIM_Array` (`:647`) to return the real class when registered:

```c
            if (pid == PRIM_Array) {
                if (kernel_array_vtable != NULL) return kernel_array_vtable;
                static int mocks_initialized = 0;
                ... (existing mock init + `return (oop)&ArrayClassMockObj;`)
            }
```

Redirect class-side `new:` to allocate an Array-vtable indexable when the receiver is the kernel Array class. Add this case alongside the existing mock `new:` (`:666`), BEFORE it:

```c
            if (receiver == kernel_array_vtable && pid == PRIM_new) {
                extern oop gc_allocate(oop vtable, size_t size);
                intptr_t n = IS_SMI(args[0]) ? UNTAG_SMI(args[0]) : 0;
                if (n < 0) n = 0;
                oop a = gc_allocate(kernel_array_vtable,
                                    sizeof(struct ObjectInstance) + ((size_t)n + 1) * sizeof(oop));
                struct ObjectInstance *inst = (struct ObjectInstance *)a;
                inst->slots[0] = TAG_SMI(n);                 // length header (indexable layout)
                for (intptr_t i = 1; i <= n; i++) inst->slots[i] = NULL;
                return a;
            }
```

Extend the storage-prim gate so Array-vtable receivers reach `basicAt:`/`basicAt:put:`/`basicSize`. At the indexable-prim guard (`:724`, currently `obj_vtable(receiver) == (oop)&IndexableVtable`), broaden to:

```c
            if (receiver != NULL && !IS_SMI(receiver) && indexable_initialized &&
                (obj_vtable(receiver) == (oop)&IndexableVtable ||
                 obj_vtable(receiver) == kernel_array_vtable)) {
```

- [ ] **Step 5: Mark the global as a GC root**

In `../oevm/src/gc.c`, in the root-scanning function alongside `debugger_hook` (`:626-629`), add:

```c
    extern oop kernel_array_vtable;
    mark(kernel_array_vtable);
```

(`mark()` early-rejects SMIs/non-heap pointers, so marking the static/image vtable is safe.)

- [ ] **Step 6: Boot wiring — Path A**

In `../oevm/src/oevm_main.c`, between `init_solidified_vtables()` (`:39`) and `run_solidified_entry_point()` (`:43`), add:

```c
    extern oop kernel_array_vtable;
    extern const struct Vtable Vtable_Array;   // from the generated solidified_image.c
    kernel_array_vtable = (oop)&Vtable_Array;
```

- [ ] **Step 7: Boot wiring — Path B**

In `../oevm/src/oevm_image_main.c`, after `kernel_boot()` and before `oimg_bootstrap(...)`, add the same wiring. The Path B `Array` vtable lives in the loaded image; resolve it by its image symbol the same way the entry vtable is resolved (`oimg_resolve` of the `Array` class ref), assigning the result to `kernel_array_vtable`. If the loader does not already expose a class-by-name lookup, add a minimal `oimg_resolve_class("Array")` helper mirroring `oimg_root()`/`img_root_ref` and use it here.

> Implementer note: Path B class resolution is the one part that may need a small loader helper rather than a direct symbol reference (the `.oimg` doesn't link C symbols). Confirm how the entry class vtable is obtained in `oimg_bootstrap`/`oevm_image_main.c` and mirror it for `Array`. If this proves larger than a few lines, report DONE_WITH_CONCERNS and we will split Path B into its own task.

- [ ] **Step 8: Verify Array.ns serves the operations (both paths)**

```bash
python3 tools/ns_compile_oevm.py ../ns-tests/04_somns_basic/26_array_kernel.ns ../oevm/src/solidified_image.c
(cd ../oevm && gcc -Wall -Wextra -std=c99 -DOEVM_APPLIANCE src/vm.c src/gc.c src/dispatch.c src/solidified_image.c src/oevm_main.c -o oevm_runner -lm && ./oevm_runner)
python3 tools/run_tests_oevm.py
```

Expected: `26_array_kernel.ns` prints `3,10,20,30`; suite 48 pass / 3 skip both paths. To confirm the route now goes through `Array.ns` (not the mock), temporarily edit the mock `at:` case to return a wrong value, rebuild, run `26_array_kernel.ns` — output must be UNCHANGED (mock dormant) — then revert the temporary edit.

- [ ] **Step 9: Commit (two/three repos)**

```bash
git add lib/kernel.ns ../ns-tests/04_somns_basic/26_array_kernel.ns
git commit -m "feat(stdlib): Array.ns (at:/at:put:/size/do:) reached via platform"
git -C ../oevm add src/dispatch.c src/oevm.h src/gc.c src/oevm_main.c src/oevm_image_main.c
git -C ../oevm commit -m "feat(stdlib): kernel_array_vtable wiring; new:/basicAt: accept Array; GC root"
git -C ../ns-tests add 04_somns_basic/26_array_kernel.ns 2>/dev/null; git -C ../ns-tests commit -m "test(stdlib): Array kernel-class round-trip" 2>/dev/null || true
```

---

### Task 3: Delete the `PRIM_Array` mock

**Files:**
- Modify: `../oevm/src/dispatch.c` (remove mock objects + `new:`/`at:`/`at:put:`/`size`/`do:` mock cases)

**Interfaces:**
- Consumes: `kernel_array_vtable` always non-NULL at runtime (kernel.ns is always concatenated, boot always wires it).
- Produces: a `dispatch.c` with no `ArrayClassMockObj`/`ArrayClassMockVtable`/`ArrayMockVtable` and no mock dispatch; `PRIM_Array` returns `kernel_array_vtable` unconditionally.

- [ ] **Step 1: Confirm the gate is always taken, then simplify `PRIM_Array`**

Replace the gated `PRIM_Array` case (Task 2 Step 4) with:

```c
            if (pid == PRIM_Array) {
                return kernel_array_vtable;   // real kernel Array class (lib/kernel.ns)
            }
```

- [ ] **Step 2: Delete the mock dispatch cases**

Remove from `../oevm/src/dispatch.c`:
- the mock `new:` case `receiver == (oop)&ArrayClassMockObj && pid == PRIM_new` (`:666-678`);
- the entire `obj_vtable(receiver) == (oop)&ArrayMockVtable` block handling `PRIM_at`/`PRIM_at_put`/`PRIM_size`/`PRIM_do` (`:682-718`);
- the static decls `ArrayClassMockObj`/`ArrayClassMockVtable`/`ArrayMockVtable` (`:389-391`) and the `mocks_initialized` flag.

Keep the Task-2 `receiver == kernel_array_vtable && pid == PRIM_new` allocation case and the indexable storage prims.

- [ ] **Step 3: Build and run the full suite (both paths)**

```bash
python3 tools/run_tests_oevm.py
```
Expected: 48 pass / 3 skip on both paths. Also rebuild the single-file Array test to confirm no regression:
```bash
python3 tools/ns_compile_oevm.py ../ns-tests/04_somns_basic/26_array_kernel.ns ../oevm/src/solidified_image.c
(cd ../oevm && gcc -Wall -Wextra -std=c99 -DOEVM_APPLIANCE src/vm.c src/gc.c src/dispatch.c src/solidified_image.c src/oevm_main.c -o oevm_runner -lm && ./oevm_runner)
```
Expected: `3,10,20,30`.

- [ ] **Step 4: Commit**

```bash
git -C ../oevm add src/dispatch.c
git -C ../oevm commit -m "feat(stdlib): delete PRIM_Array mock; Array is now pure .ns + platform"
```

---

### Task 4: Refresh the committed default image so a plain build links

**Files:**
- Modify: `../oevm/src/solidified_image.c` (regenerate so the committed default includes `Vtable_Array`)

**Interfaces:**
- Consumes: the kernel-concat compiler (Task 1) and boot wiring (Task 2), which now reference `Vtable_Array`.
- Produces: a committed `solidified_image.c` that defines `Vtable_Array`, so `oevm_main.c`'s `extern const struct Vtable Vtable_Array;` links in a fresh `make` (not just under the test harness, which regenerates per run).

- [ ] **Step 1: Verify the problem**

The committed `../oevm/src/solidified_image.c` (the default ScriptApp sample) predates kernel concat, so it lacks `Vtable_Array`; `oevm_main.c` now references it → a plain build would fail to link. Confirm:

```bash
grep -c "Vtable_Array" ../oevm/src/solidified_image.c   # expect 0 before regen
```

- [ ] **Step 2: Regenerate the default image with the kernel included**

Pick the canonical default sample the repo uses (the existing ScriptApp hello, e.g. `../ns-tests/01_expressions/` or whatever `solidified_image.c` currently encodes — inspect its module name to match). Regenerate it through the kernel-concat compiler:

```bash
# Use the same source the committed default represents; example:
python3 tools/ns_compile_oevm.py ../ns-tests/01_expressions/01_arithmetic.ns ../oevm/src/solidified_image.c
grep -c "Vtable_Array" ../oevm/src/solidified_image.c   # expect > 0 after regen
```

(If the project has a canonical "default image" source documented elsewhere, use that instead; the requirement is only that the committed `solidified_image.c` defines `Vtable_Array` and builds.)

- [ ] **Step 3: Confirm a clean build links**

```bash
(cd ../oevm && gcc -Wall -Wextra -std=c99 -DOEVM_APPLIANCE src/vm.c src/gc.c src/dispatch.c src/solidified_image.c src/oevm_main.c -o oevm_runner -lm && ./oevm_runner)
```
Expected: builds and runs without link error.

- [ ] **Step 4: Commit**

```bash
git -C ../oevm add src/solidified_image.c
git -C ../oevm commit -m "chore(stdlib): regenerate default solidified image with kernel (Vtable_Array)"
```

---

## Self-Review

**Spec coverage (against `docs/design/2026-06-20-ns-oevm-stdlib-step3a-kernel-object-array.md`):**
- §2 kernel build (concatenate, top-level siblings, reached via platform) → Task 1 Steps 3-4.
- §3 platform binding + mock retirement (gate then delete) → Task 2 Step 4 (gate) + Task 3 (delete); indexable storage prims kept (Task 2 Step 4 broadens, never removes them).
- §4 Object-as-root + prim gating (universal heap root; intrinsics primitive; `==`/`error:` universal) → Task 1 Step 5 (delegate rule) + Step 6 (heap fall-through, intrinsics untouched, `error:` not gated).
- §5 class protocols + Array-IS-indexable representation → Task 1 Step 3 (`Object.ns`) + Task 2 Step 3 (`Array.ns`) + Step 4 (allocation/repr).
- §6 boot/GC wiring (both paths, roots) → Task 2 Steps 5-7.
- §7 testing (suite green both paths; new Object + Array protocol tests) → tests in Tasks 1-2; suite gate in every task.
- §8 open questions → recorded in the spec; not implemented here (correct).

**Placeholder scan:** the one soft spot is Task 2 Step 7 (Path B class-by-name resolution), which depends on the loader's existing class-resolution shape; it carries an explicit implementer note and an escalation path rather than a fake one-liner. Everything else has exact code + commands + expected output.

**Type/name consistency:** `kernel_array_vtable` (declared `oevm.h`, defined `dispatch.c`, set in both boot files, marked in `gc.c`, read in `PRIM_Array`/`new:`/storage-gate) is spelled identically throughout. `Vtable_Array` matches `vt_id("Array")`. The delegate rule keys on `qname.rsplit(".",1)[-1] == "Object"` and emits delegate `"Object"` → `Vtable_Object`, consistent with the kernel's top-level `class Object`. Array representation (slot 0 = `TAG_SMI(n)`, elements 1..n) matches the storage-prim layout the kept `basicAt:` uses.

**Risk note (flagged for execution):** if the universal-root delegate change (Task 1 Step 5) regresses any conformance program (e.g. an existing rootless class whose dispatch now walks to `Object.ns`), the suite gate in Task 1 Step 7 catches it before Array work begins — the decomposition deliberately puts the root change first and alone.
