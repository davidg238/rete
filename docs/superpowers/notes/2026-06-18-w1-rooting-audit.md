# W1 Rooting Audit — Runtime `gc_allocate` / `float_alloc` Sites

**Date:** 2026-06-18
**Branch:** w1-gc-trigger
**Auditor:** task-4 (automated, human-verified)

## Methodology

`gc_allocate` can trigger `gc_collect` on arena exhaustion (W1 trigger is now wired).
`float_alloc` uses a private bump-arena allocated via `malloc`; it never triggers `gc_collect`.
Therefore only `gc_allocate` sites require rooting analysis.

For each `gc_allocate` site the question is: between the call and the moment the
result and any sibling oops in flight become reachable from a GC root, is any live
oop held ONLY in a location invisible to the conservative C-stack scan?

Invisible locations are: malloc'd C structs (off-stack), static/global C variables
not already in `gc_collect`'s precise root set, and interior pointers whose base
is not itself a GC root.

The conservative `scan_c_stack` (Task 1) covers:
- every aligned word on the live C call stack (function args, locals, spilled registers)
- the `jmp_buf` from `setjmp` (callee-saved / register-file)

`scan_vm_stack` covers all live VM frame registers on the current fiber's stack.

The precise global root list in `gc_collect` covers:
`IntegerVtable`, `FloatVtable`, `MethodContextVtable`, `FiberVtable`, `SymbolVtable`,
`VtableVtable`, `PrimitiveClosureVtable`, `BytecodeClosureVtable`, `RunnableFiberVtable`,
`SuspendedFiberVtable`, `DeadFiberVtable`, `Symbol_unwindTo_value`, `Symbol_value`,
`current_fiber`.

---

## Allocation Site Table

### `float_alloc` sites (NO GC during `float_alloc`)

`float_alloc` calls `malloc` for new slabs; it does NOT call `gc_collect`.
Therefore no site using only `float_alloc` needs rooting analysis.

| file:line | what is allocated | live oops in C locals across it | covered by | action |
|-----------|------------------|---------------------------------|------------|--------|
| src/vm.c:392 | `struct Float` | `op1`, `op2`, `ctx` (frame ptr) | stack-scan (C locals) + scan_vm_stack (`ctx` regs) | none |
| src/vm.c:424 | `struct Float` | `op1`, `op2`, `ctx` | stack-scan + scan_vm_stack | none |
| src/vm.c:444 | `struct Float` | `op1`, `op2`, `ctx` | stack-scan + scan_vm_stack | none |
| src/dispatch.c:795 | `struct Float` (asFloat) | `receiver` (SMI or Float — no heap oop to protect) | N/A — no GC fires | none |
| src/dispatch.c:806 | `struct Float` (sqrt) | same as asFloat | N/A — no GC fires | none |

---

### `gc_allocate` sites

#### Worked example (from brief): `make_message` — dispatch.c:276

```c
static oop make_message(oop selector) {
    oop msg = gc_allocate((oop)&MessageMockVtable,
                          sizeof(struct ObjectInstance) + sizeof(oop));
    ((struct ObjectInstance *)msg)->slots[0] = selector;
    return msg;
}
```

`selector` is a C-local argument, live on the C call stack across `gc_allocate`.
Covered by `scan_c_stack`. **No action.**

---

#### dispatch.c:33 — `largeint_alloc`

```c
struct LargeInteger *li = gc_allocate(LargeIntegerVtable, sizeof(struct LargeInteger));
```

Called from `largeint_alloc(int sign, uint64_t mag)`.  Live oops in scope: none —
`sign` and `mag` are primitive scalars, not oops.  The vtable `LargeIntegerVtable`
is a global that is NOT in the precise root list, but it does not need to be: it
points at `Vtable_LargeIntegerVtable`, a `const struct Vtable` in the data segment
(`kernel_boot.c:98`), NOT an arena allocation — `mark()` no-ops on it because
`is_valid_heap_object` fails the arena range check, and a static const never needs
collecting. (This is unlike `debugger_hook`, which holds a heap-allocated closure
and therefore DID need rooting — see Systemic Notes.) There is no data-segment
scan; the only scans are the conservative C-stack scan and the precise roots.
Callers of `largeint_alloc` hold source oops (`a`, `b`) as C locals.

| dispatch.c:33 | `struct LargeInteger` | no heap-oop args to protect | stack-scan (callers hold source oops as C locals) | none |

---

#### dispatch.c:276 — `make_message` (the worked example)

| dispatch.c:276 | `struct ObjectInstance` (Message wrapper) | `selector` (C-local arg) | stack-scan | none |

---

#### dispatch.c:348 — `trigger_access_violation` lazy Symbol

```c
if (Symbol_accessViolation == NULL) {
    oop sym_av = gc_allocate(SymbolVtable, sizeof(struct Symbol));
    Symbol_accessViolation = sym_av;
}
```

This is a one-shot lazy initializer. Live oops at the time of the call:
`receiver`, `selector`, `argc`, `args` are all C-local function arguments (on the C stack).
`target_selector` is NULL here (we're in the `if (target_selector == NULL)` branch).
`sym_av` is stored into `Symbol_accessViolation` (a global) immediately; no second
allocation occurs before it is safe. Callee-stack coverage is complete.

| dispatch.c:348 | `struct Symbol` (accessViolation:) | `receiver`, `selector` C locals | stack-scan | none |

---

#### dispatch.c:467 — `boundclass_construct`

```c
oop nested_vtable = bci->slots[0];   // read before alloc
oop outer         = bci->slots[1];   // read before alloc
oop init_method   = ...;             // found by vtable walk before alloc
oop inst = gc_allocate(nested_vtable,
                       sizeof(struct ObjectInstance) + slot_count * sizeof(oop));
```

Live oops across `gc_allocate`:
- `nested_vtable` — C local, on C stack
- `outer` — C local, on C stack
- `init_method` — C local, on C stack (NULL or a method closure)
- `bc`, `selector`, `argc`, `args` — function arguments, on C stack

All covered by `scan_c_stack`.

Note: `init_method` is resolved by scanning `nvt->bindings` (a `realloc`'d array
inside the GC-managed `nested_vtable`). Because this is a non-moving GC, `bs`
(interior pointer) remains valid as long as `nvt` / `nested_vtable` is reachable —
which it is via the C stack.

| dispatch.c:467 | `struct ObjectInstance` (nested instance) | `nested_vtable`, `outer`, `init_method`, `bc`, `selector` | stack-scan | none |

---

#### dispatch.c:574 — `PRIM_Array / new:` (Array allocation)

```c
oop arr = gc_allocate((oop)&ArrayMockVtable,
                      sizeof(struct ObjectInstance) + (size + 1) * sizeof(oop));
```

`ArrayMockVtable` is a static C struct, not a GC heap object — no rooting needed.
`args[0]` (the size SMI) is a C local. No heap oops held across this call.

| dispatch.c:574 | `struct ObjectInstance` (Array) | `args[0]` is SMI (not heap oop) | stack-scan | none |

---

#### dispatch.c:705 — `PRIM_createClosure`

```c
oop cl = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
((struct Closure *)cl)->home_ctx  = (oop)current_frame;
((struct Closure *)cl)->entry_point = (void *)args[0];
```

Live oops across the allocation: `args[0]` (the entry-point SMI — it IS a tagged
SMI, so not a heap oop), `current_frame` is a pointer to the current VM stack frame
(not a heap allocation — it is on the `vm_stack` array scanned by `scan_vm_stack`).
`BytecodeClosureVtable` is a global root.

| dispatch.c:705 | `struct Closure` (bytecode closure) | `current_frame` (VM stack, not heap) — no heap oops at risk | scan_vm_stack | none |

---

#### dispatch.c:817 — `PRIM_boundOuter` (BoundClass wrapper)

```c
oop bcvt = ensure_boundclass_vtable();   // returns static C struct address
oop bc = gc_allocate(bcvt, sizeof(struct ObjectInstance) + 3 * sizeof(oop));
```

`ensure_boundclass_vtable()` returns a pointer to a file-static `struct Vtable`
(not a GC heap object). Live oops across `gc_allocate`:
- `receiver` — C local (function arg), the nested vtable oop
- `args[0]`, `args[1]` — C locals (outer instance, slot-count SMI)

All on the C stack → stack-scan covers them.

| dispatch.c:817 | `struct ObjectInstance` (BoundClass) | `receiver`, `args[0]` heap oops | stack-scan | none |

---

#### dispatch.c:953 — `PRIM_printString` (Symbol from string)

```c
oop sym_obj = gc_allocate(SymbolVtable, sizeof(struct Symbol));
char *new_str = malloc(strlen(buf) + 1);
strcpy(new_str, buf);
((struct Symbol *)sym_obj)->name = new_str;
```

`buf` is a C local `char[128]` (not an oop). `receiver` is the only live heap oop
and it is a C local function argument. The `malloc` for `new_str` occurs AFTER
`gc_allocate` — the order is deliberate; if it were reversed, `new_str` would
leak on GC, but that leak would be harmless (it's a `char *`, not an oop that GC
tracks). `sym_obj` is immediately used before any further allocation.

| dispatch.c:953 | `struct Symbol` (printString) | `receiver` (C local arg) | stack-scan | none |

---

#### dispatch.c:977 — `PRIM_asString` (Symbol from string)

Identical pattern to `printString` above.

| dispatch.c:977 | `struct Symbol` (asString) | `receiver` (C local arg) | stack-scan | none |

---

#### dispatch.c:1017 — `PRIM_comma` (string concat)

```c
struct Symbol *a = (struct Symbol *)receiver;
struct Symbol *b = (struct Symbol *)args[0];
size_t newlen = a->length + b->length;
char *new_str = malloc(newlen + 1);
memcpy(...);
oop sym_obj = gc_allocate(SymbolVtable, sizeof(struct Symbol));
((struct Symbol *)sym_obj)->name = new_str;
```

`new_str` is a `char *` (not an oop). `a` and `b` are raw pointers to `receiver`
and `args[0]` — both C locals on the C stack. The `malloc` for `new_str` runs
BEFORE `gc_allocate`, so if GC fires during `gc_allocate`, `new_str` is a C
local on the C stack (not an oop — it won't be freed by GC; it could leak if
the Symbol allocation fails catastrophically, but that is OOM territory).

| dispatch.c:1017 | `struct Symbol` (comma concat) | `receiver`, `args[0]` C locals | stack-scan | none |

---

#### vm.c:79 — `run_interpreter` lazy Fiber creation

```c
current_fiber = gc_allocate(RunnableFiberVtable ? RunnableFiberVtable : FiberVtable,
                            sizeof(struct Fiber));
```

This is a one-shot lazy initializer for the main fiber. No existing GC-managed
oops are live at this point (it is called at the very first interpreter entry).
`RunnableFiberVtable` / `FiberVtable` are global roots. The result is immediately
stored in `current_fiber` (a global root).

| vm.c:79 | `struct Fiber` (main fiber) | no prior heap oops in scope | global root (`current_fiber`) + precise roots (vtables) | none |

---

#### vm.c:88 — `run_interpreter` lazy `Symbol_unwindTo_value`

```c
Symbol_unwindTo_value = gc_allocate(SymbolVtable, sizeof(struct Symbol));
```

One-shot lazy initializer. Result immediately stored in the global root
`Symbol_unwindTo_value`. No heap oops held in C locals at this moment
(function arguments `closure`, `receiver` are C locals but are not yet live
in the GC sense before the first alloc).

Actually: `closure` and `receiver` ARE live C locals at this point (they are
function arguments of `run_interpreter`). They are on the C call stack.

| vm.c:88 | `struct Symbol` (unwindTo:value:) | `closure`, `receiver` C locals | stack-scan | none |

---

#### vm.c:93 — `run_interpreter` lazy `Symbol_value`

Same pattern as vm.c:88.

| vm.c:93 | `struct Symbol` (value) | `closure`, `receiver`, `Symbol_unwindTo_value` C locals / global | stack-scan + global | none |

---

#### vm.c:499 — `OP_NEW` bytecode

```c
oop vt = ctx->regs[src1];
struct ObjectInstance *new_obj = gc_allocate(vt, ...);
for (int i = 0; i < slot_count; i++) new_obj->slots[i] = NULL;
ctx->regs[dest] = (oop)new_obj;
```

Live oops: `vt` is a C local on the C stack; `ctx->regs` are covered by `scan_vm_stack`.

| vm.c:499 | `struct ObjectInstance` (OP_NEW) | `vt` C local; all regs via `ctx` | stack-scan + scan_vm_stack | none |

---

#### vm.c:733 — `primitive_vtable_bindings` (outer array)

```c
struct ObjectInstance *arr = gc_allocate(NULL, sizeof(struct ObjectInstance) + vt->size * sizeof(oop));
```

No oops in scope yet that need protecting (only `receiver`/`vt` as C locals).
`receiver` is a function arg on the C stack.

| vm.c:733 | `struct ObjectInstance` (bindings array) | `receiver`/`vt` C locals | stack-scan | none |

---

#### vm.c:736 — `primitive_vtable_bindings` (BoxedBinding loop)

```c
for (size_t i = 0; i < vt->size; i++) {
    struct BoxedBinding *bb = gc_allocate(BoxedBindingVtable, sizeof(struct BoxedBinding));
    bb->symbol = binding_symbol(&bs[i]);
    bb->method = binding_method(&bs[i]);
    arr->slots[i] = (oop)bb;
}
```

At each iteration, when `gc_allocate` fires:

**Live oops in C locals:**
- `arr` — the outer array allocated at vm.c:733. It is a C local (`struct ObjectInstance *`) on the C stack → **covered by `scan_c_stack`**.
- `vt` / `receiver` — C locals on the C stack → covered.
- `bs` — raw interior pointer into `vt->bindings` (a `realloc`'d `malloc` array). This is NOT a heap oop; it is a raw pointer into a non-GC-managed allocation. The base `vt` is reachable; this is a non-moving GC, so `bs` remains valid.

**`arr->slots[i..n-1]` are uninitialized (no zero-clearing before the loop):**

`arena_try_alloc` does NOT zero-initialize the payload. Slots `i..n-1` of `arr`
contain stale bytes (zeros from fresh bump or recycled content from a prior freed
object). When GC traces `arr` (vtable = NULL, so `trace_object` is used), it will
scan all `header->size / sizeof(oop)` words, including uninitialized slots.

Uninitialized words that look like valid heap pointers would be spuriously retained
(conservative false-retention). This is expected and harmless for a conservative GC.

Uninitialized words that were stale oops from a prior object would also be traced,
potentially retaining those objects one extra cycle. Again: conservative and safe.

**Crucially:** `arr` itself is covered by `scan_c_stack`, so it will not be freed
during any of the inner `gc_allocate` calls. Previously stored `arr->slots[0..i-1]`
BoxedBinding oops are reachable via `arr`, so they will not be freed either.

**No gap here.** The uninitialized-slot issue is an artifact of the conservative
scan, not a use-after-free.

| vm.c:736 | `struct BoxedBinding` (loop) | `arr`, `vt`, `receiver` C locals; `bs` interior ptr (non-moving GC) | stack-scan | none |

---

#### vm.c:786 — `primitive_fiber_create`

```c
oop closure = args[0];
...
oop new_fib_oop = gc_allocate(SuspendedFiberVtable ? SuspendedFiberVtable : FiberVtable,
                              sizeof(struct Fiber));
```

Live oops: `closure`, `args[0]` — C local function arguments on the C stack.
`cl` and `method` are raw pointer aliases of `closure` (not independent heap oops).
After allocation, `new_fib_oop` is a C local.

| vm.c:786 | `struct Fiber` (new fiber) | `closure` C local arg | stack-scan | none |

---

#### vm.c:931 — `primitive_register_debugger_hook` lazy Symbol

```c
oop sym = gc_allocate(SymbolVtable, sizeof(struct Symbol));
((struct Symbol *)sym)->name = "debug:";
Symbol_debug_ = sym;
```

One-shot lazy initializer. `debugger_hook` = `args[0]` has just been stored as
a global (`debugger_hook` is a global in dispatch.c — but NOT in the gc_collect
precise root list!).

**Potential issue:** `debugger_hook` is a file-scoped global in dispatch.c that
holds an oop (a closure/object), but it is NOT listed in `gc_collect`'s precise
roots. If GC fires during the `gc_allocate` at vm.c:931, `debugger_hook` is a
global oop not covered by precise roots.

**But:** `debugger_hook` is also reachable through the VM stack: it was just passed
as `args[0]` to `primitive_register_debugger_hook`, so `args[0]` is on the C stack
(via the `primitive_fiber_switch` or `send` call chain). And `debugger_hook` = `args[0]`
was stored at line 929, just before the `gc_allocate` at line 931.

At the moment GC fires at vm.c:931, the call stack includes the frame for
`primitive_register_debugger_hook` where `args[0]` is still live (the global
assignment does not remove it from the C stack). So `args[0]` (the hook value)
is covered by `scan_c_stack`.

However, after `primitive_register_debugger_hook` returns and the C stack unwinds,
`debugger_hook` is held ONLY in the global, which is NOT in the precise root list.
Subsequent GC cycles would not trace `debugger_hook`. **This is a systemic gap
for `debugger_hook` across all GC cycles, not just at this allocation site.**

Recording here as a **CONCERN** (not blocking for this allocation site, but noted
for the global-root list).

| vm.c:931 | `struct Symbol` (debug:) | `debugger_hook`=`args[0]` C local at call time | stack-scan (at this call) | FIXED — `debugger_hook` + `Symbol_debug_` added to `gc_collect` precise roots (see Systemic Notes) |

---

## Systemic Notes (not per-site allocation issues)

### `method_cache` (static array, dispatch.c:139) — FIXED in this task

`method_cache[METHOD_CACHE_SIZE]` is a static C array holding oop fields
(`vtable`, `selector`, `method`, `defining_vt`). It is NOT a GC root and NOT
flushed by `gc_collect`. If a vtable in the cache becomes dead (unreachable via
other roots) and is collected, then the cache entry holds a stale pointer.

On next lookup, the stale `vtable` pointer comparison will fail (the memory has
been reclaimed and either re-zeroed or reused for a different object), resulting
in a cache miss — safe. BUT if the arena reuses that exact address for a new
object whose pointer identity collides, a spurious cache hit could dispatch the
wrong method. This is an existing correctness concern.

**Fix applied:** `method_cache_flush()` is now called at the top of `gc_collect`
(after `heap_init()` guard, before the mark-reset walk) — see `src/gc.c`. This
clears all stale oop entries before any memory is reclaimed or reused.

### `debugger_hook` (global, dispatch.c:23) — FIXED in this task

The global `debugger_hook` holds an oop (a hook closure) that is registered via
`primitive_register_debugger_hook`. It is NOT in the precise root list in
`gc_collect`. If GC runs while a debugger hook is active, the hook closure could
be collected.

**Fix applied:** `mark(debugger_hook)` and `mark(Symbol_debug_)` are now added
to `gc_collect`'s precise root section in `src/gc.c`. Verified by a new TDD test
in `tests/test_gc_trigger.c` (`debugger_hook_global_survives`) that disables the
conservative C-stack scan and confirms the object survives via the precise root.

---

## Conclusion

**No roots required at any `gc_allocate` call site in `src/dispatch.c` or `src/vm.c`.**

The conservative C-stack scan (`scan_c_stack`, Task 1) covers all oops held in C
locals across every runtime allocation site. The precise global root list covers
vtables and sentinel symbols. `scan_vm_stack` covers VM frame registers.

`float_alloc` sites need no analysis (they cannot trigger `gc_collect`).

Two pre-existing systemic concerns are noted but are NOT within scope of per-site
rooting:
1. `method_cache` — stale entries after GC; fix: flush cache in `gc_collect`.
2. `debugger_hook` / `Symbol_debug_` — globals not in precise root list; fix: add
   to `gc_collect`'s root scan.

Neither of these creates a use-after-free at the analyzed `gc_allocate` sites —
they are survivability concerns for objects that outlive individual allocation
sites.

---

## Final-Review Addendum (whole-branch review, 2026-06-19)

The whole-branch code review found two additional file-scope `oop` globals missing
from `gc_collect`'s precise root list. Both are now fixed in `src/gc.c`.

### `Symbol_accessViolation` (CRITICAL — UAF under Model A)

`src/dispatch.c:23` defines `oop Symbol_accessViolation = NULL;`. Under
`-DOEVM_APPLIANCE` (Model A), this global is lazily heap-allocated via
`gc_allocate(SymbolVtable, ...)` on the first `doesNotUnderstand:` dispatch and
stored only in this global — never on the VM stack. Without rooting, the next
GC cycle would sweep the object → use-after-free when subsequently used as
`target_selector`.

(Under Model B, `kernel_boot.c:154` points it at a data-segment const, masking the
bug. Model A is a shipped, tested config.)

**Fix:** `mark(Symbol_accessViolation)` added to `gc_collect`'s precise root section,
next to the existing `debugger_hook`/`Symbol_debug_` block.

**TDD verification:** `accessviolation_global_survives()` added to
`tests/test_gc_trigger.c` — mirrors `debugger_hook_global_survives`, disables the
conservative C-stack scan, confirms fail-before-fix / pass-after-fix.

### `unwind_return_value` (defensive)

`src/vm.c:25` defines `oop unwind_return_value = NULL;`. During a non-local return
it holds a live arena oop while the interpreter unwinds frames. Currently safe only
because no allocation occurs between set and consume — a fragile invariant now that
W1 makes GC fire from arbitrary `gc_allocate` calls. The value is NULL outside an
active unwind, so marking is a no-op in steady state.

**Fix:** `mark(unwind_return_value)` added immediately after `Symbol_accessViolation`.

Note: `unwind_target_frame` (also vm.c) is a VM-stack frame pointer, not an arena
allocation; `mark()` correctly ignores it. Not added.

### Comprehensive sweep of all remaining file-scope `oop` globals

A full audit of all file-scope `oop` globals confirmed the remaining unrooted ones
are either:
- **Data-segment constants** (vtables in `solidified_image.c` / `kernel_boot.c`):
  `is_valid_heap_object()` returns 0; `mark()` is a no-op. No rooting needed.
- **VM-stack frame pointers** (`unwind_target_frame`, `current_frame`): point into
  the `vm_stack` array, not the arena; `mark()` correctly ignores them.

No further rooting gaps exist.
