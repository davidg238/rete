# Std-lib Step 1: Real Indexable Representation + `platform` Capability Object — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land a real, GC-correct indexable object representation and a single `platform` capability object (allocation / identity / I-O primitives) in the VM, additively, without touching the compiler or breaking the 47/47 conformance suite.

**Architecture:** This is **step 1 of 6** in the capability-pure std-lib spec (`docs/design/2026-06-20-ns-oevm-stdlib-design.md` §8). It is **purely additive VM infrastructure in the sibling repo `../oevm`**: a first-class `IndexableVtable` (length stored as a tagged SMI so the GC word-scan skips it) with raw `basicAt:`/`basicAt:put:`/`basicSize` accessors, plus a lazily-constructed singleton `platform` object whose messages are the genuinely-VM-level primitives. Both are reached via the existing `send()` receiver-identity interception idiom (the same mechanism the current `PRIM_Array` mock uses), so they are capability-pure in effect: a primitive fires only when the receiver *is* the object you were handed. The legacy `PRIM_Array` mock is left **untouched** — the conformance suite still rides it. Retiring the mock and wiring `.ns` source happen in later steps (2–5).

**Tech Stack:** C99, GCC. VM in `../oevm/src/{vm,gc,dispatch,kernel_boot}.c` + `oevm.h`/`oimg.h`. Python compiler/harness in this repo (`ns-oevm/tools/`). C unit tests are standalone `main()` programs compiled ad hoc with `gcc` (see `../oevm/tests/test_arena.c` header for the convention). Conformance harness: `python3 tools/run_tests_oevm.py`.

## Global Constraints

- **Sibling-repo paths:** the VM is at `../oevm` relative to this repo (`ns-oevm`). All `src/...` and `tests/...` paths below are **inside `../oevm`** unless prefixed with `tools/` (which is in `ns-oevm`).
- **C standard:** `-std=c99`. Code must compile clean under `-Wall -Wextra` (the conformance build uses both) and under `-DOEVM_APPLIANCE` (Model A) **and** without it (Model B). Use no C11/`__int128`/`max_align_t`.
- **Conformance is the safety net:** the existing suite must stay **47/47 green on both image paths** (Path A solidified-C, Path B `.oimg`) after every task. Run `python3 tools/run_tests_oevm.py` from `ns-oevm`; default `OEVM_PATH=AB` runs both.
- **Indexable length is a tagged SMI** (`slots[0] = TAG_SMI(n)`, elements 1-based at `slots[1..n]`). This is load-bearing for GC correctness (§3.2 of the spec): the mark phase early-rejects `IS_SMI`, so a raw count word is never mis-traced.
- **Do not modify** `PRIM_Array`, `ArrayClassMockObj`, `ArrayMockVtable`, or any existing `PRIM_NAMES` entry. Append only.
- **Capability names** follow the SOMns-style already chosen in the spec §4: `array:`, `basicAt:`, `basicAt:put:`, `basicSize`, `object:identicalTo:`, `objHashcode:`, `print:`, `printNewline:`.
- **Commit freely; do NOT push** (`git push`) without explicit say-so.

---

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `../oevm/src/dispatch.c` | Modify | New `IndexableVtable`/`PlatformObj`/`PlatformVtable` statics, `indexable_new()`, `platform_object()`, appended `PRIM_*` enum ids + `PRIM_NAMES` entries, and two new receiver-interception blocks in `send()`. |
| `../oevm/src/oevm.h` | Modify | One declaration: `oop platform_object(void);` (so tests and, later, the compiler can obtain the capability). |
| `../oevm/tests/test_platform.c` | Create | Standalone C unit test driving the indexable accessors, the platform primitives, and GC survival through a collection. |

**Setup precondition (run once before Task 1, only if needed):** the C unit-test link set below does **not** include `src/solidified_image.c`, so no generated image is required. But if a clean checkout has a stale/missing `src/solidified_image.c` that breaks an unrelated build, regenerate it with the harness's Path A command: `python3 tools/ns_compile_oevm.py ../ns-tests/04_somns_basic/01_method_call.ns ../oevm/src/solidified_image.c` (run from `ns-oevm`).

---

### Task 1: Real indexable representation + raw accessors

Add a first-class indexable object (its own GC-rooted-safe vtable + allocator) and the raw `basicAt:`/`basicAt:put:`/`basicSize` accessors, reached by receiver-identity interception. The `platform` object and its allocation prim come in Task 2; Task 1 exercises the indexable by allocating one directly from the test via a temporary `extern` to `indexable_new`.

**Files:**
- Modify: `../oevm/src/dispatch.c` (statics near line 389; enum at 510–523; `PRIM_NAMES` at 526–536; new interception block in `send()` after the `ArrayMockVtable` instance block, ~line 653)
- Create: `../oevm/tests/test_platform.c`

**Interfaces:**
- Produces:
  - `static oop indexable_new(intptr_t n)` — allocate an indexable of `n` nil-filled element slots (`slots[0]=TAG_SMI(n)`, `slots[1..n]=NULL`); returns the new `oop`. Exposed to the Task-1 test via a local `extern` declaration (it becomes non-`extern`-facing again once Task 2's `platform array:` is the public allocator — but leaving the `extern` is harmless).
  - `static struct Vtable IndexableVtable` — the indexable's class vtable; `obj_vtable(anIndexable) == (oop)&IndexableVtable`.
  - Selectors on an indexable receiver: `basicSize` (→ tagged SMI count), `basicAt:` (1-based read, aborts out of bounds), `basicAt:put:` (1-based write, returns the stored value, aborts out of bounds).

- [ ] **Step 1: Write the failing test** — create `../oevm/tests/test_platform.c`

```c
/* Unit test for the real indexable representation + platform capability object
   (src/dispatch.c, std-lib step 1). Build:
     gcc -std=c99 -Isrc tests/test_platform.c \
         src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/kernel_boot.c \
         -lm -o /tmp/tp && /tmp/tp
   (link set = image-VM set minus oevm_image_main.c; this file supplies main). */
#include "../src/oevm.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

extern oop  send(oop receiver, oop selector, int argc, oop *args);
extern void kernel_boot(void);
extern void heap_init(void);
extern int  is_truthy(oop);
extern oop  SymbolVtable;

/* Task 1 reaches the allocator directly; Task 2 replaces this with `array:`. */
extern oop  indexable_new(intptr_t n);

/* Build a throwaway selector Symbol with the given name. send() classifies
   selectors by symbol_name() string, so a local Symbol works as long as its
   vtable is SymbolVtable. */
static oop sel(const char *name) {
    static struct Symbol pool[32];
    static int n = 0;
    struct Symbol *s = &pool[n++];
    s->base.vtable = SymbolVtable;
    s->name = name;
    s->length = strlen(name);
    return (oop)s;
}

int main(void) {
    heap_init();
    kernel_boot();

    /* Allocate a 3-element indexable directly. */
    oop arr = indexable_new(3);

    /* basicSize == 3 */
    oop sz = send(arr, sel("basicSize"), 0, NULL);
    assert(IS_SMI(sz) && UNTAG_SMI(sz) == 3);

    /* basicAt:put: then basicAt: round-trips (1-based). */
    oop putargs[2] = { TAG_SMI(1), TAG_SMI(42) };
    oop stored = send(arr, sel("basicAt:put:"), 2, putargs);
    assert(IS_SMI(stored) && UNTAG_SMI(stored) == 42);

    oop idx1 = TAG_SMI(1);
    oop got = send(arr, sel("basicAt:"), 1, &idx1);
    assert(IS_SMI(got) && UNTAG_SMI(got) == 42);

    /* fresh slots are nil (NULL) */
    oop idx2 = TAG_SMI(2);
    oop got2 = send(arr, sel("basicAt:"), 1, &idx2);
    assert(got2 == NULL);

    printf("TASK1 OK\n");
    return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails to build** (the symbols don't exist yet)

Run (from `../oevm`):
```bash
gcc -std=c99 -Isrc tests/test_platform.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/kernel_boot.c \
    -lm -o /tmp/tp
```
Expected: **link/compile error** — `undefined reference to 'indexable_new'` (and the new selectors do nothing yet).

- [ ] **Step 3: Append the new primitive ids to the enum** — `../oevm/src/dispatch.c`, inside the `enum { PRIM_NONE = 0, … }` block, after the last entry `PRIM_millis,` (line ~522):

```c
    /* --- std-lib step 1: real indexable + platform capability --- */
    PRIM_ix_array, PRIM_ix_basicAt, PRIM_ix_basicAt_put, PRIM_ix_basicSize,
    PRIM_pf_identicalTo, PRIM_pf_hashcode, PRIM_pf_print, PRIM_pf_printNewline,
```

- [ ] **Step 4: Append the parallel selector names** — `../oevm/src/dispatch.c`, inside `PRIM_NAMES[]`, after the last entry `"millis",` (line ~535). **Order and count must match Step 3 exactly** (the classifier maps index `i` → id `i+1`):

```c
    "array:", "basicAt:", "basicAt:put:", "basicSize",
    "object:identicalTo:", "objHashcode:", "print:", "printNewline:",
```

- [ ] **Step 5: Add the indexable statics + allocator** — `../oevm/src/dispatch.c`, just after the existing mock declarations (`ArrayMockVtable` at line ~391):

```c
/* --- std-lib step 1: real indexable representation ---------------------------
   Layout mirrors struct ObjectInstance: slots[0] holds the element count as a
   tagged SMI (so the GC word-scan early-rejects it via IS_SMI), slots[1..n]
   hold elements (1-based). This is the capability-pure backing store a future
   .ns Array will wrap; it is distinct from, and does not touch, the legacy
   PRIM_Array mock above. */
static struct Vtable IndexableVtable;

/* Allocate a fresh indexable of `n` nil-filled element slots. */
oop indexable_new(intptr_t n) {
    extern oop gc_allocate(oop vtable, size_t size);
    if (n < 0) n = 0;
    oop ix = gc_allocate((oop)&IndexableVtable,
                         sizeof(struct ObjectInstance) + ((size_t)n + 1) * sizeof(oop));
    struct ObjectInstance *inst = (struct ObjectInstance *)ix;
    inst->slots[0] = TAG_SMI(n);
    for (intptr_t i = 1; i <= n; i++) inst->slots[i] = NULL;
    return ix;
}

/* One-time init of the indexable vtable. Lazy (works on both boot paths). */
static int indexable_initialized = 0;
static void indexable_init(void) {
    if (indexable_initialized) return;
    IndexableVtable.base.vtable = VtableVtable;
    IndexableVtable.delegate = NULL;
    IndexableVtable.bindings = NULL;
    IndexableVtable.capacity = 0;
    IndexableVtable.size = 0;
    IndexableVtable.is_nested = 0;
    indexable_initialized = 1;
}
```

Note: `indexable_new` is intentionally **not** `static` so the Task-1 test can call it; `indexable_init` is static. `indexable_new` must call `indexable_init()` first — amend the allocator's first line:

```c
oop indexable_new(intptr_t n) {
    extern oop gc_allocate(oop vtable, size_t size);
    indexable_init();
    if (n < 0) n = 0;
    /* …unchanged… */
}
```

- [ ] **Step 6: Add the raw-accessor interception block in `send()`** — `../oevm/src/dispatch.c`, immediately **after** the closing brace of the `obj_vtable(receiver) == (oop)&ArrayMockVtable` block (the mock array-instance block ending at line ~653), and **before** the `// Intercept selector on a reified doesNotUnderstand:` block:

```c
            // --- std-lib step 1: real indexable raw accessors. Distinct
            //     selectors (basic*) from the mock's at:/size, so the
            //     conformance mock is untouched. Fires only for the real
            //     IndexableVtable receiver. ---
            if (receiver != NULL && !IS_SMI(receiver) &&
                indexable_initialized && obj_vtable(receiver) == (oop)&IndexableVtable) {
                struct ObjectInstance *ix = (struct ObjectInstance *)receiver;
                intptr_t n = UNTAG_SMI(instance_slot(ix, 0));
                if (pid == PRIM_ix_basicSize) {
                    return instance_slot(ix, 0);
                }
                if (pid == PRIM_ix_basicAt) {
                    intptr_t idx = IS_SMI(args[0]) ? UNTAG_SMI(args[0]) : 0;
                    if (idx < 1 || idx > n) {
                        fprintf(stderr, "VM Error: basicAt: index %ld out of bounds (size %ld)\n",
                                (long)idx, (long)n);
                        abort();
                    }
                    return instance_slot(ix, (size_t)idx);
                }
                if (pid == PRIM_ix_basicAt_put) {
                    intptr_t idx = IS_SMI(args[0]) ? UNTAG_SMI(args[0]) : 0;
                    if (idx < 1 || idx > n) {
                        fprintf(stderr, "VM Error: basicAt:put: index %ld out of bounds (size %ld)\n",
                                (long)idx, (long)n);
                        abort();
                    }
                    ix->slots[idx] = args[1];
                    return args[1];
                }
            }
```

- [ ] **Step 7: Build and run the test to verify it passes**

Run (from `../oevm`):
```bash
gcc -std=c99 -Isrc tests/test_platform.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/kernel_boot.c \
    -lm -o /tmp/tp && /tmp/tp
```
Expected: builds clean, prints `TASK1 OK`, exit 0.

- [ ] **Step 8: Verify conformance is still 47/47 on both paths**

Run (from `ns-oevm`):
```bash
python3 tools/run_tests_oevm.py
```
Expected: all tests PASS on Path A and Path B (47/47); no regressions. The new selectors/receivers are unreachable from existing `.ns` tests, so behaviour is unchanged.

- [ ] **Step 9: Commit**

```bash
cd ../oevm && git add src/dispatch.c tests/test_platform.c && \
git commit -m "feat(stdlib): real indexable repr + raw basicAt:/basicAt:put:/basicSize

Step 1a of the capability-pure std-lib. Adds a first-class IndexableVtable
(length as tagged SMI) and raw 1-based accessors via send() receiver
interception, alongside (not replacing) the PRIM_Array mock. Conformance
unchanged: 47/47 both paths.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: The `platform` capability object + allocation / identity / I-O prims

Add the singleton `platform` object and its primitives: `array:` (the public indexable allocator), `object:identicalTo:`, `objHashcode:`, `print:`, `printNewline:`. The object is lazily built so it works on both boot paths, and exposed via `platform_object()` for tests (and, in step 2, the compiler entry seam).

**Files:**
- Modify: `../oevm/src/dispatch.c` (platform statics by the indexable statics; platform interception block in `send()` right after the Task-1 indexable block)
- Modify: `../oevm/src/oevm.h` (declare `oop platform_object(void);`)
- Modify: `../oevm/tests/test_platform.c` (extend `main`)

**Interfaces:**
- Consumes: `indexable_new(intptr_t)`, `IndexableVtable` (Task 1).
- Produces:
  - `oop platform_object(void)` — lazily construct and return the singleton platform capability object (`PlatformObj`). Idempotent.
  - Messages on the platform receiver: `array:` (n → fresh indexable), `object:identicalTo:` (a, b → `true`/`false` symbol by pointer identity), `objHashcode:` (x → tagged-SMI hash; SMIs hash to themselves), `print:` (x → console, no newline; handles SMI / Symbol / nil), `printNewline:` (→ newline). Each returns the platform receiver except `array:` (returns the indexable), `object:identicalTo:` (returns a boolean symbol), and `objHashcode:` (returns an SMI).

- [ ] **Step 1: Write the failing test** — extend `../oevm/tests/test_platform.c` `main` (replace the `printf("TASK1 OK\n"); return 0;` tail with the following, keeping the Task-1 body above it). Also remove the now-redundant `extern oop indexable_new(...)` reliance by allocating via the platform:

```c
    /* --- Task 2: platform capability object --- */
    oop pf = platform_object();
    assert(pf != NULL);

    /* array: allocates an indexable equivalent to indexable_new */
    oop three = TAG_SMI(3);
    oop arr2 = send(pf, sel("array:"), 1, &three);
    oop sz2 = send(arr2, sel("basicSize"), 0, NULL);
    assert(IS_SMI(sz2) && UNTAG_SMI(sz2) == 3);

    /* object:identicalTo: — true for same oop, false for different */
    oop sameargs[2] = { arr2, arr2 };
    oop t = send(pf, sel("object:identicalTo:"), 2, sameargs);
    assert(is_truthy(t));
    oop diffargs[2] = { arr2, three };
    oop f = send(pf, sel("object:identicalTo:"), 2, diffargs);
    assert(!is_truthy(f));

    /* objHashcode: — an SMI hashes to itself; identical oops hash equal */
    oop h1 = send(pf, sel("objHashcode:"), 1, &three);
    assert(IS_SMI(h1) && UNTAG_SMI(h1) == 3);
    oop ha[1] = { arr2 };
    oop hb[1] = { arr2 };
    oop ah = send(pf, sel("objHashcode:"), 1, ha);
    oop bh = send(pf, sel("objHashcode:"), 1, hb);
    assert(ah == bh);

    /* print: / printNewline: — exercise the path (output asserted by harness) */
    oop msg = TAG_SMI(7);
    send(pf, sel("print:"), 1, &msg);
    send(pf, sel("printNewline:"), 0, NULL);

    printf("TASK2 OK\n");
    return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails** (platform_object undefined)

Run (from `../oevm`):
```bash
gcc -std=c99 -Isrc tests/test_platform.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/kernel_boot.c \
    -lm -o /tmp/tp
```
Expected: **link error** — `undefined reference to 'platform_object'`.

- [ ] **Step 3: Declare the accessor in the header** — `../oevm/src/oevm.h`, in the reflection/introspection block (after `oop primitive_register_debugger_hook(...)`, ~line 290):

```c
/* std-lib step 1: the singleton platform capability object (src/dispatch.c).
   Lazily constructed; the only handle to the VM-level allocation / identity /
   I-O primitives. The compiler entry seam will pass this where it currently
   passes fake_platform (ns_compile_oevm.py:1842), in step 2. */
oop platform_object(void);
```

- [ ] **Step 4: Add the platform statics + accessor** — `../oevm/src/dispatch.c`, right after the `indexable_init` function from Task 1:

```c
/* --- std-lib step 1: the platform capability object -------------------------
   A single object whose messages ARE the genuinely-VM-level primitives
   (allocation / identity / I-O). Reached only by receiver identity, so it is
   capability-pure: code that was not handed this object cannot send to it.
   Lazily built so it is available on both boot paths (Model A solidified and
   Model B image), neither of which is guaranteed to call kernel_boot. The
   object + its vtable are static (BSS), not arena-allocated, so they need no
   GC-root registration; revisit only if platform is ever heap-allocated. */
static struct Object PlatformObj;
static struct Vtable PlatformVtable;
static int platform_initialized = 0;

oop platform_object(void) {
    if (!platform_initialized) {
        indexable_init();   /* platform array: depends on the indexable vtable */
        PlatformVtable.base.vtable = VtableVtable;
        PlatformVtable.delegate = NULL;
        PlatformVtable.bindings = NULL;
        PlatformVtable.capacity = 0;
        PlatformVtable.size = 0;
        PlatformVtable.is_nested = 0;
        PlatformObj.vtable = (oop)&PlatformVtable;
        platform_initialized = 1;
    }
    return (oop)&PlatformObj;
}
```

- [ ] **Step 5: Add the platform interception block in `send()`** — `../oevm/src/dispatch.c`, immediately **after** the Task-1 indexable accessor block, still inside the `if (SymbolVtable != NULL && … pid != PRIM_NONE)` region:

```c
            // --- std-lib step 1: platform capability object. Allocation /
            //     identity / I-O. Fires only when the receiver IS the platform
            //     singleton (capability-pure). Unknown pids fall through to the
            //     normal switch / dispatch below. ---
            if (receiver == (oop)&PlatformObj && platform_initialized) {
                switch (pid) {
                case PRIM_ix_array:
                    return indexable_new(IS_SMI(args[0]) ? UNTAG_SMI(args[0]) : 0);
                case PRIM_pf_identicalTo:
                    return (args[0] == args[1]) ? true_symbol : false_symbol;
                case PRIM_pf_hashcode:
                    if (IS_SMI(args[0])) return args[0];
                    if (args[0] == NULL) return TAG_SMI(0);
                    return TAG_SMI((intptr_t)(((uintptr_t)args[0]) >> 4));
                case PRIM_pf_print:
                    if (args[0] == NULL) {
                        printf("nil");
                    } else if (IS_SMI(args[0])) {
                        printf("%ld", (long)UNTAG_SMI(args[0]));
                    } else if (SymbolVtable != NULL &&
                               obj_vtable(args[0]) == SymbolVtable) {
                        printf("%s", symbol_name((struct Symbol *)args[0]));
                    }
                    return receiver;
                case PRIM_pf_printNewline:
                    printf("\n");
                    return receiver;
                default:
                    break;
                }
            }
```

- [ ] **Step 6: Build and run the test to verify it passes**

Run (from `../oevm`):
```bash
gcc -std=c99 -Isrc tests/test_platform.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/kernel_boot.c \
    -lm -o /tmp/tp && /tmp/tp
```
Expected: builds clean, prints `7` then a blank line then `TASK1 OK`? — note: `TASK1 OK` printed before the Task-2 block, so output is:
```
TASK1 OK
7
TASK2 OK
```
(The Task-1 `printf("TASK1 OK\n")` was replaced by the Task-2 tail, so only `7`, then `TASK2 OK` print after Task-1 asserts. Confirm the final line is `TASK2 OK` and exit code 0.)

- [ ] **Step 7: Verify conformance still 47/47 both paths**

Run (from `ns-oevm`):
```bash
python3 tools/run_tests_oevm.py
```
Expected: 47/47 PASS both paths. The platform object is still unreachable from `.ns` tests (compiler unchanged), so no behavioural change.

- [ ] **Step 8: Commit**

```bash
cd ../oevm && git add src/dispatch.c src/oevm.h tests/test_platform.c && \
git commit -m "feat(stdlib): platform capability object (array:/identity/hash/IO)

Step 1b. Lazily-built singleton platform object reached by receiver identity;
exposes array:, object:identicalTo:, objHashcode:, print:, printNewline: as
VM-level primitives. platform_object() declared in oevm.h for the step-2 entry
seam. Conformance unchanged: 47/47 both paths.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: GC correctness — indexable survives collection, elements traced

Prove the new representation is GC-correct: an indexable holding heap references keeps them alive across a forced `gc_collect()` (the generic word-scan traces its element slots), the tagged-SMI length is never mis-traced, and the indexable itself survives when rooted. This is the §3.2 guarantee under test.

**Files:**
- Modify: `../oevm/tests/test_platform.c` (add a GC test, gated behind a root hook like `tests/test_arena.c`)

**Interfaces:**
- Consumes: `platform_object()`, `array:`, `basicAt:put:`, `basicSize`, `basicAt:` (Tasks 1–2); `gc_collect`, `gc_is_live`, `mark_object`, `gc_additional_roots_hook` (existing GC API, see `tests/test_arena.c`).
- Produces: nothing for later tasks — pure regression guard.

- [ ] **Step 1: Write the failing test** — extend `../oevm/tests/test_platform.c`. Add the externs and a file-scope root hook near the top (after the existing `extern` block):

```c
extern void gc_collect(void);
extern int  gc_is_live(oop ptr);
extern void mark_object(oop ptr);
extern void (*gc_additional_roots_hook)(void);

/* Root the outer indexable across a collection (mirrors tests/test_arena.c). */
static oop g_outer = NULL;
static void platform_roots(void) { mark_object(g_outer); }
```

Then insert this block into `main` **before** the final `printf("TASK2 OK\n"); return 0;`:

```c
    /* --- Task 3: GC correctness --- */
    gc_additional_roots_hook = platform_roots;

    /* Outer indexable holds an inner indexable in slot 1. Only the outer is
       rooted; the inner must survive because the outer's slot is traced. */
    oop two = TAG_SMI(2);
    g_outer = send(pf, sel("array:"), 1, &two);
    oop one2 = TAG_SMI(1);
    oop inner = send(pf, sel("array:"), 1, &one2);
    oop put_inner[2] = { TAG_SMI(1), inner };
    send(g_outer, sel("basicAt:put:"), 2, put_inner);

    gc_collect();

    /* inner survived (reached only through the outer's element slot) */
    assert(gc_is_live(inner));
    /* outer survived and its length (a tagged SMI) is intact */
    oop outer_sz = send(g_outer, sel("basicSize"), 0, NULL);
    assert(IS_SMI(outer_sz) && UNTAG_SMI(outer_sz) == 2);
    /* the stored reference is still the same inner object */
    oop idx1b = TAG_SMI(1);
    oop fetched = send(g_outer, sel("basicAt:"), 1, &idx1b);
    assert(fetched == inner);

    gc_additional_roots_hook = NULL;
```

- [ ] **Step 2: Run the test to verify it fails**

Run (from `../oevm`):
```bash
gcc -std=c99 -Isrc tests/test_platform.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/kernel_boot.c \
    -lm -o /tmp/tp && /tmp/tp
```
Expected at this point: the test should actually **PASS** if Tasks 1–2 are correct (length-as-SMI + generic tracing make it work with no new production code). If it **fails** — e.g. `gc_is_live(inner)` is false — that is a real GC-correctness bug in the representation: confirm `slots[0]` is `TAG_SMI(n)` (not a raw `size_t`), since a raw count word can alias and mis-trace. Fix the representation, not the test. Document the outcome either way.

- [ ] **Step 3: If a fix was needed, apply it; otherwise no production change**

If Step 2 passed, this step is a no-op (the test is a pure regression guard — that is a valid, intended outcome). If it failed, the fix is in `indexable_new` / the `slots[0]` convention in `../oevm/src/dispatch.c` — ensure the count is stored as `TAG_SMI(n)` exactly as written in Task 1 Step 5.

- [ ] **Step 4: Run the test to verify it passes**

Run (from `../oevm`):
```bash
gcc -std=c99 -Isrc tests/test_platform.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/kernel_boot.c \
    -lm -o /tmp/tp && /tmp/tp
```
Expected: prints `TASK2 OK` as the final line, exit 0 (the GC asserts pass silently before it).

- [ ] **Step 5: Verify conformance still 47/47 both paths**

Run (from `ns-oevm`):
```bash
python3 tools/run_tests_oevm.py
```
Expected: 47/47 PASS both paths.

- [ ] **Step 6: Commit**

```bash
cd ../oevm && git add tests/test_platform.c src/dispatch.c && \
git commit -m "test(stdlib): GC traces indexable element slots through collection

Step 1c. Forces a collection with an inner indexable reachable only through a
rooted outer's element slot; asserts survival, intact tagged-SMI length, and
reference identity. Locks in the length-as-SMI GC contract (spec 3.2).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review

**1. Spec coverage (step 1 = spec §8 item 1, with §3.2 GC + §4 prim line):**
- "real indexable object representation" → Task 1 (`IndexableVtable` + `indexable_new`, length as tagged SMI).
- "`platform` capability object with allocation/indexed/identity/I-O primitives" → Task 1 (raw indexed `basicAt:`/`basicAt:put:`/`basicSize`) + Task 2 (`array:` allocation, `object:identicalTo:` identity, `objHashcode:`, `print:`/`printNewline:` I-O).
- "register `platform` (and library vtables) as GC roots (§3.2)" → addressed by making `PlatformObj`/`PlatformVtable`/`IndexableVtable` **static BSS** (not arena), which needs no marking; Task 3 verifies arena-allocated *instances* trace correctly. Documented in Task 2 Step 4 comment.
- "retire the `PRIM_Array` mock" → **deliberately deferred.** Removing the mock breaks the conformance suite until the compiler resolves names through the namespace (step 2) and `.ns Array` exists (step 3). Step 1 lands the replacement *alongside* the mock; mock removal is spec §8 step 5. This scoping is stated in the plan's Architecture section and Global Constraints.

**Deferred from spec §4, with rationale (stated so a reviewer isn't surprised):**
- `basicNew:` (generic fixed-slot instance) — `.ns` classes already instantiate via the existing class machinery (`OP_NEW`/BoundClass), so a generic `basicNew:` isn't needed to unblock `Array`. Add when a `.ns` class needs raw reflective instantiation.
- `objClassName:` — returns a String, and String is spec §3/step 3. Deferred until a String representation exists.

**2. Placeholder scan:** no TBD/TODO/"add error handling"/"similar to Task N". Every code step shows complete code; every run step shows the exact command and expected output. Bounds checks are spelled out, not hand-waved.

**3. Type consistency:** `indexable_new(intptr_t)` returns `oop`, used consistently in Tasks 1–3. `platform_object(void)` returns `oop`, declared in `oevm.h` (Task 2 Step 3) and used in Tasks 2–3. Selector names match `PRIM_NAMES` (Task 1 Step 4) exactly everywhere they appear in tests: `array:`, `basicAt:`, `basicAt:put:`, `basicSize`, `object:identicalTo:`, `objHashcode:`, `print:`, `printNewline:`. Enum ids (`PRIM_ix_array`, `PRIM_ix_basicAt`, `PRIM_ix_basicAt_put`, `PRIM_ix_basicSize`, `PRIM_pf_identicalTo`, `PRIM_pf_hashcode`, `PRIM_pf_print`, `PRIM_pf_printNewline`) are in the same order in Step 3 (enum) and Step 4 (`PRIM_NAMES`) — the parallel-array invariant the classifier depends on. `true_symbol`/`false_symbol` are the existing macros (`dispatch.c:205-206`); `instance_slot`/`obj_vtable` are the existing inlines (`oimg.h:81,44`).

---

## Execution Handoff

Two execution options:

1. **Subagent-Driven (recommended)** — dispatch a fresh subagent per task, review between tasks, fast iteration.
2. **Inline Execution** — execute tasks in this session with checkpoints for review.
