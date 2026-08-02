# std-lib step 3c-1 — String + universal value protocol (hash/=/printString) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Introduce a real `String` type (immutable, packed UTF-8 bytes) replacing the "string literal == interned Symbol" status quo, and complete a universal value protocol — `printString` returns a real `String` everywhere, a new `hash` message across all types, and `String`'s content-based `=`/`hash` — closing the dialect #5 boolean gap.

**Architecture:** `String` is a VM intrinsic mirroring `Symbol` — a standalone `StringVtable` (NULL delegate), Object protocol via universal prims, String protocol via prims keyed on the receiver vtable. It is a *bare, immutable, non-interned* UTF-8 byte object (`struct String { Object base; size_t length; uint8_t bytes[]; }`) with **inline** bytes. The load-bearing VM change is a GC leaf case so packed bytes are not misread as oops. String literals compile to static `String` constants on Path A (solidified-C) and serialized `String` constants on Path B (`.oimg`).

**Tech Stack:** C (sibling VM repo `../oevm`: `dispatch.c`, `gc.c`, `kernel_boot.c`, `oimg_loader.c`, `oevm.h`), Python 3 (this repo `ns-oevm`: `tools/ns_compile_oevm.py`, `tools/solidify.py`, `tools/oimg_serialize.py`, `tools/builtins_registry.py`), Newspeak (`lib/kernel.ns`), `.ns` conformance tests (sibling repo `../ns-tests`).

## Global Constraints

- **The bar is "green on `oevm_runner`, both image paths," not "parses."** Run conformance from the **`ns-oevm` cwd**: `python3 tools/run_tests_oevm.py` (set `OEVM_PATH=A`, `OEVM_PATH=B`, or `OEVM_PATH=AB`). Wrong cwd → false 0 passes.
- **Baseline suite: 54 pass / 0 fail / 3 skip, both paths.** No task may regress it; 3c-1 raises it with new tests.
- **THREE sibling repos:** `ns-oevm` (compiler/lib/docs/tests-runner), `../oevm` (C VM), `../ns-tests` (conformance `.ns` files). Commit per-repo.
- **Locate edit sites by MATCHING QUOTED CODE, not line numbers** (line numbers drift). Each step quotes an anchor.
- **Commit trailers required.** End every commit message with: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. **Do NOT push** (David pushes on explicit say-so).
- **Spec:** `docs/design/2026-06-21-ns-oevm-stdlib-step3c1-string-booleans.md`. **Issue:** [#1](https://github.com/davidg238/ns-oevm/issues/1) (deferred rune protocol).
- **Build the VM** before running the suite: the runner rebuilds, but to compile-check use the VM's normal build (e.g. `cd ../oevm && make` or the project's build script).
- **String byte-granularity is permanent** (dialect #10): `size`/`at:` deal in bytes; `at:` returns a SMI `0..255`, never nil. No rune protocol in 3c-1.

## File structure

| File (repo) | Responsibility | Tasks |
|---|---|---|
| `../oevm/src/oevm.h` | `struct String`; `extern oop StringVtable;` | 1 |
| `../oevm/src/kernel_boot.c` | `Vtable_StringVtable` definition + global assign + `builtin_registry` slot | 1 |
| `../oevm/src/dispatch.c` | `oop StringVtable`; `make_string` helper; new prims `hash`/`identityHash`; String `=`/`size`/`at:`/`isEmpty`; `printString`→String (+`#`/booleans); `,`→String | 1,2,3 |
| `../oevm/src/gc.c` | `StringVtable` root + `trace_one` leaf case | 1 |
| `lib/kernel.ns` | `Object>>hash = ( ^ self identityHash )` | 2 |
| `tools/ns_compile_oevm.py` | split string-literal kind → String constant; Path A `StaticString_` emission + `UPDATE_LIMITS` | 3 |
| `tools/solidify.py` | emit `Vtable_StringVtable` extern + `StaticString_` struct helper | 3 |
| `tools/builtins_registry.py`, `tools/gen_builtins.py` | register `StringVtable` builtin + `String` struct layout | 1,4 |
| `tools/oimg_serialize.py` | emit `String` constant records (vtable ref + length + inline bytes) | 4 |
| `../oevm/src/oimg_loader.c` | accept String constants (format bump if needed) | 4 |
| `docs/NEWSPEAK_DIALECT.md` | add #10, amend #3 & #5 | 3 |
| `../ns-tests/04_somns_basic/3*_*.ns` | new protocol tests | 3,4 |

---

### Task 1: VM foundation — `struct String`, `StringVtable`, GC leaf (inert)

Adds the type and its GC/boot wiring with **no behavior change** (nothing creates a String yet). Deliverable: VM builds, suite still 54/0/3 both paths (an unused vtable changes nothing).

**Files:**
- Modify: `../oevm/src/oevm.h` (beside `struct Symbol`; with the other `extern oop *Vtable;` decls)
- Modify: `../oevm/src/kernel_boot.c` (vtable decls block, definitions block, `kernel_boot`, `builtin_registry`)
- Modify: `../oevm/src/dispatch.c` (global decl beside `oop SymbolVtable`)
- Modify: `../oevm/src/gc.c` (`trace_one`, roots)
- Modify: `tools/builtins_registry.py` (register `StringVtable`)

**Interfaces:**
- Produces: `struct String { struct Object base; size_t length; uint8_t bytes[]; }`; `extern oop StringVtable;` (a builtin vtable, NULL delegate, no bindings); `StringVtable` registered in `builtin_registry` at a new index.

- [ ] **Step 1: Add `struct String` to `oevm.h`.** Locate the anchor:
```c
struct Symbol {
    struct Object base;
    const char *name;
    size_t length;
};
```
Immediately after it, add:
```c
/*
 * String: an immutable, NON-interned, packed UTF-8 byte sequence (std-lib 3c-1).
 * Unlike Symbol (separate malloc'd `name`), the bytes are INLINE in the heap
 * object — one allocation, and a GC leaf (gc.c trace_one must not scan `bytes`
 * as oops). Byte-granularity protocol only; runes are deferred (issue #1).
 */
struct String {
    struct Object base;     // vtable == StringVtable
    size_t length;          // byte length
    uint8_t bytes[];        // inline UTF-8 payload, `length` bytes (no NUL required)
};
```

- [ ] **Step 2: Declare the global in `oevm.h`.** Find the anchor `extern oop SymbolVtable;` and add on the next line:
```c
extern oop StringVtable;
```

- [ ] **Step 3: Define the global in `dispatch.c`.** Find the anchor `oop SymbolVtable = NULL;` and add on the next line:
```c
oop StringVtable = NULL;
```

- [ ] **Step 4: Declare + define `Vtable_StringVtable` in `kernel_boot.c`.** Find the anchor `const struct Vtable Vtable_SymbolVtable;` (forward-declared storage block) and add after it:
```c
const struct Vtable Vtable_StringVtable;
```
Then find the anchor (definitions block):
```c
const struct Vtable Vtable_SymbolVtable          = { { (oop)&Vtable_VtableVtable }, NULL, NULL, 0, 0, 0 };
```
and add after it:
```c
const struct Vtable Vtable_StringVtable          = { { (oop)&Vtable_VtableVtable }, NULL, NULL, 0, 0, 0 };
```

- [ ] **Step 5: Wire the global + registry in `kernel_boot`.** Find `SymbolVtable           = (oop)&Vtable_SymbolVtable;` and add after it:
```c
    StringVtable           = (oop)&Vtable_StringVtable;
```
Add the extern at the top of `kernel_boot` near the other `extern oop` lines (or rely on `oevm.h`). Then find the last `builtin_registry[...] = ...` assignment in `kernel_boot` (the highest current index — search `builtin_registry[` and take the max N) and append at index `N+1`:
```c
    builtin_registry[/* next free index, e.g. 24 */] = StringVtable;
```
Use the actual next free index; keep it consistent with Step 6.

- [ ] **Step 6: Register `StringVtable` as a builtin in `tools/builtins_registry.py`.** Find the anchor `'SymbolVtable': 7,` and add a `StringVtable` entry with the **same numeric index used in Step 5** (the next free builtin index). Verify the index is unused elsewhere in the dict.

- [ ] **Step 7: Add `StringVtable` as a GC root in `gc.c`.** Find the anchor:
```c
    extern oop SymbolVtable;
    extern oop VtableVtable;
```
add `extern oop StringVtable;` to that extern block, then find `mark(SymbolVtable);` and add after it:
```c
    mark(StringVtable);
```

- [ ] **Step 8: Add the `trace_one` leaf case in `gc.c`.** Find the anchor (the Float leaf case):
```c
    } else if (FloatVtable != NULL && ptr->vtable == FloatVtable) {
        mark(ptr->vtable);
```
Insert a String leaf case immediately before it (add `extern oop StringVtable;` to the `trace_one` extern block at the top of the function, beside `extern oop FloatVtable;`):
```c
    } else if (StringVtable != NULL && ptr->vtable == StringVtable) {
        mark(ptr->vtable);          /* leaf: inline bytes are NOT oops — do not scan */
```

- [ ] **Step 9: Build the VM.** Run: `cd ../oevm && make` (or the project build). Expected: clean compile.

- [ ] **Step 10: Run the full suite both paths.** Run: `OEVM_PATH=AB python3 tools/run_tests_oevm.py` (from `ns-oevm` cwd). Expected: **54 pass / 0 fail / 3 skip** on both paths (foundation is inert).

- [ ] **Step 11: Commit (both repos).**
```bash
cd ../oevm && git add src/oevm.h src/dispatch.c src/kernel_boot.c src/gc.c && git commit -m "feat(stdlib): String VM foundation — struct String, StringVtable, GC leaf (3c-1 task 1)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
cd ../ns-oevm && git add tools/builtins_registry.py && git commit -m "feat(stdlib): register StringVtable builtin (3c-1 task 1)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: `hash` / `identityHash` prims + String protocol prims (inert for String)

Adds the new `hash` and `identityHash` messages and String's `=`/`size`/`at:`/`isEmpty` prims. These are **additive**: `hash`/`identityHash` are brand-new selectors (nothing sent them before), and the String branches never fire yet (no String objects exist until Task 3). Suite stays green. Deliverable: `2 hash`, `nil hash`, `(Object new) hash` work; suite 54/0/3.

**Files:**
- Modify: `../oevm/src/dispatch.c` (PRIM enum, `PRIM_NAMES`, switch cases; `make_string` helper; `<string.h>` include if absent)
- Modify: `lib/kernel.ns` (`Object>>hash`)
- Test: `../ns-tests/04_somns_basic/31_hash_protocol.ns`

**Interfaces:**
- Consumes: `StringVtable`, `struct String` (Task 1).
- Produces: prims `hash` and `identityHash` on every receiver; String `=` (content), `size` (bytes), `at:` (byte→SMI), `isEmpty`; `make_string(const char*, size_t)` helper in `dispatch.c`; `Kernel.Object>>hash`.

- [ ] **Step 1: Write the failing test** `../ns-tests/04_somns_basic/31_hash_protocol.ns`. Model it on an existing `04_somns_basic` test (e.g. open `30_collections.ns` for the class/`main:args:`/assertion idiom). It must assert: `2 hash = 2`; `nil hash` is a stable SMI; `5 hash = 5`; `(3 = 3) ifTrue:` path (sanity); and two distinct heap objects have distinct `identityHash`. Use the suite's existing assertion/print convention (print `'PASS'`/`'FAIL'` or the harness's expected-output mechanism — match a neighboring test exactly).

- [ ] **Step 2: Run it to verify it fails.** Run: `OEVM_PATH=A python3 tools/run_tests_oevm.py 31_hash_protocol` (or the runner's single-test form; check `run_tests_oevm.py --help` / a neighboring invocation). Expected: FAIL — `hash`/`identityHash` are `doesNotUnderstand`.

- [ ] **Step 3: Add the prim ids.** In `dispatch.c` find the anchor in the enum:
```c
    PRIM_boundOuter, PRIM_comma, PRIM_timesRepeat, PRIM_millis,
```
add a new line after it:
```c
    PRIM_hash, PRIM_identityHash,
```
Then find the parallel `PRIM_NAMES` anchor:
```c
    "__boundOuter:slotCount:", ",", "timesRepeat:", "millis",
```
add after it (keeping enum/name order identical):
```c
    "hash", "identityHash",
```

- [ ] **Step 4: Add the `make_string` helper.** Ensure `#include <string.h>` is present at the top of `dispatch.c` (it uses `strcmp`/`memcpy` already — verify). Just above `oop send(` (anchor: `/* Perform open dynamic message dispatch */`), add:
```c
/* Allocate an immutable String with inline UTF-8 bytes (std-lib 3c-1). */
static oop make_string(const char *bytes, size_t len) {
    extern oop gc_allocate(oop vtable, size_t size);
    oop s = gc_allocate(StringVtable, sizeof(struct String) + len);
    ((struct String *)s)->length = len;
    if (len > 0) memcpy(((struct String *)s)->bytes, bytes, len);
    return s;
}
```

- [ ] **Step 5: Add the `hash` and `identityHash` cases.** In the universal `switch (pid)` (anchor: `case PRIM_boundOuter: {`), add after that case's closing `break;`:
```c
            case PRIM_identityHash: {
                if (IS_SMI(receiver)) return receiver;
                if (receiver == NULL) return TAG_SMI(0);
                return TAG_SMI((intptr_t)(((uintptr_t)receiver) >> 4));
            }
            break;
            case PRIM_hash: {
                if (IS_SMI(receiver)) return receiver;            /* SMI hashes to itself */
                if (receiver == NULL) return TAG_SMI(1);          /* nil: fixed constant */
                if (StringVtable != NULL && obj_vtable(receiver) == StringVtable) {
                    struct String *s = (struct String *)receiver; /* content hash (FNV-1a) */
                    uint32_t h = 2166136261u;
                    for (size_t i = 0; i < s->length; i++) { h ^= s->bytes[i]; h *= 16777619u; }
                    return TAG_SMI((intptr_t)(h & 0x3FFFFFFF));
                }
                if (SymbolVtable != NULL && obj_vtable(receiver) == SymbolVtable)
                    return TAG_SMI((intptr_t)(((uintptr_t)receiver) >> 4)); /* interned: identity */
                if (FloatVtable != NULL && obj_vtable(receiver) == FloatVtable)
                    return TAG_SMI((intptr_t)((long)((struct Float *)receiver)->value) & 0x3FFFFFFF);
                if (LargeIntegerVtable != NULL && obj_vtable(receiver) == LargeIntegerVtable)
                    return TAG_SMI((intptr_t)(((struct LargeInteger *)receiver)->mag) & 0x3FFFFFFF);
                break;   /* heap object: fall through to Kernel.Object>>hash (overridable) */
            }
            break;
```
(Booleans are Symbol singletons → identity-hash via the Symbol branch; `true`/`false` are distinct objects, so they hash distinctly.)

- [ ] **Step 6: Add `Object>>hash` to `lib/kernel.ns`.** Find the anchor in `class Object`:
```
    public asString = ( ^ self printString )
```
add after it:
```
    public hash = ( ^ self identityHash )
```

- [ ] **Step 7: Add String `=` (content).** Find `case PRIM_eq:` in `dispatch.c`. Read its current body; insert this branch as the **first** statements of the case:
```c
                if (receiver != NULL && !IS_SMI(receiver) && StringVtable != NULL &&
                    obj_vtable(receiver) == StringVtable) {
                    oop o = args[0];
                    if (o == NULL || IS_SMI(o) || obj_vtable(o) != StringVtable) return false_symbol;
                    struct String *a = (struct String *)receiver, *b = (struct String *)o;
                    if (a->length != b->length) return false_symbol;
                    return (memcmp(a->bytes, b->bytes, a->length) == 0) ? true_symbol : false_symbol;
                }
```

- [ ] **Step 8: Add String `size`/`at:`/`isEmpty`.** Find `case PRIM_size:` and insert as its first statements:
```c
                if (receiver != NULL && !IS_SMI(receiver) && StringVtable != NULL &&
                    obj_vtable(receiver) == StringVtable)
                    return TAG_SMI((intptr_t)((struct String *)receiver)->length);
```
Find `case PRIM_at:` and insert as its first statements:
```c
                if (receiver != NULL && !IS_SMI(receiver) && StringVtable != NULL &&
                    obj_vtable(receiver) == StringVtable) {
                    struct String *s = (struct String *)receiver;
                    intptr_t idx = IS_SMI(args[0]) ? UNTAG_SMI(args[0]) : 0;
                    if (idx < 1 || idx > (intptr_t)s->length) { fprintf(stderr, "VM Error: String at: %ld out of bounds (size %zu)\n", (long)idx, s->length); abort(); }
                    return TAG_SMI((intptr_t)s->bytes[idx - 1]);   /* 1-based; returns 0..255 */
                }
```
`isEmpty` is `size = 0`; rather than a new prim, leave `isEmpty` to callers (`aString size = 0`) for 3c-1 — **no `isEmpty` prim** (YAGNI; the spec's `isEmpty` was a convenience). If a test needs it, use `size = 0`.

- [ ] **Step 9: Build the VM.** Run: `cd ../oevm && make`. Expected: clean compile.

- [ ] **Step 10: Run the new test + full suite.** Run: `OEVM_PATH=A python3 tools/run_tests_oevm.py 31_hash_protocol` → PASS. Then `OEVM_PATH=AB python3 tools/run_tests_oevm.py` → **55 pass / 0 fail / 3 skip** both paths (baseline + the new test; nothing regressed).

- [ ] **Step 11: Commit (both repos + tests).**
```bash
cd ../oevm && git add src/dispatch.c && git commit -m "feat(stdlib): hash/identityHash prims + String =/size/at: prims (3c-1 task 2)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
cd ../ns-oevm && git add lib/kernel.ns && git commit -m "feat(stdlib): Object>>hash via identityHash (3c-1 task 2)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
cd ../ns-tests && git add 04_somns_basic/31_hash_protocol.ns && git commit -m "test(stdlib): hash/identityHash protocol (3c-1 task 2)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Flip — string literals are `String`; `printString`→String; `,`→String (Path A)

The irreducible behavior flip: string literals become `String` constants (Path A), `printString` returns Strings (with `#` for Symbols, special-cased booleans), and `,` produces Strings. These are coupled (changing one without the others breaks `perfstex`/`stringCompare`), so they land together. **Path B is left red by design** (gate `OEVM_PATH=A`); Task 4 restores it. Deliverable on Path A: suite green + new String tests.

**Files:**
- Modify: `tools/ns_compile_oevm.py` (literal kind split; `StaticString_` emission + `UPDATE_LIMITS`)
- Modify: `tools/solidify.py` (`Vtable_StringVtable` extern; `StaticString_` struct emission)
- Modify: `../oevm/src/dispatch.c` (`PRIM_printString`, `PRIM_comma`)
- Modify: `docs/NEWSPEAK_DIALECT.md` (#10 add; #3, #5 amend)
- Test: `../ns-tests/04_somns_basic/32_string.ns`

**Interfaces:**
- Consumes: `make_string` (Task 2), `struct String`/`StringVtable` (Task 1).
- Produces: `'abc'` literals are `String` instances (Path A); `anyValue printString` is a `String`; `aString , aString` is a `String`.

- [ ] **Step 1: Write the failing test** `../ns-tests/04_somns_basic/32_string.ns` (model on a neighbor). Assert: two distinct literals of equal content are `=` (`'abc' = 'abc'` → true); `('ab' , 'c') = 'abc'` → true and `('ab','c') hash = 'abc' hash`; `'abc' size = 3`; `('abc' at: 1) = 97`; `'abc' = #abc` → false; `42 printString = '42'`; `(true printString) = 'true'`; `(false printString) = 'false'`; `(#foo printString) = '#foo'`; `nil printString = 'nil'`. (Pin dialect #10: also assert a 2-byte UTF-8 literal's `size` is 2 — e.g. a literal containing `é` — if the source toolchain preserves UTF-8 source bytes; otherwise note and skip that line.)

- [ ] **Step 2: Run it to verify it fails.** Run: `OEVM_PATH=A python3 tools/run_tests_oevm.py 32_string`. Expected: FAIL (`'abc' = 'abc'` is currently interned-Symbol identity = true, but `printString`/`,` still produce Symbols, and `'abc' = #abc` is currently true since both are symbols).

- [ ] **Step 3: Split the literal kind in the compiler.** In `tools/ns_compile_oevm.py` find the anchor:
```python
            elif kind == "string" or kind == "symbol":
                cidx = self.add_constant(str(val))
                self.emit_ABx(OP_LD_CONST, target_reg, cidx)
```
Replace with a split that routes string literals to a String constant and leaves symbol literals as-is:
```python
            elif kind == "symbol":
                cidx = self.add_constant(str(val))
                self.emit_ABx(OP_LD_CONST, target_reg, cidx)
            elif kind == "string":
                cidx = self.add_string_constant(str(val))   # String constant, not interned Symbol
                self.emit_ABx(OP_LD_CONST, target_reg, cidx)
```
Add an `add_string_constant` method beside `add_constant` (find `def add_constant`): it must register a distinct constant kind so the constant materializes as a `String`. Mirror `add_constant`'s structure but tag the entry (e.g. a `("STRING", value)` marker the constant emitter recognizes) so Path A emits a `StaticString_` and the in-memory/Path-B emitters route to String. Inspect how `add_constant` stores entries and how the constant pool is later emitted (search `add_constant`, `LARGEINT:`, `SMI:` markers — strings should follow the same tagging discipline, e.g. a `STRING:`-prefixed marker or a typed tuple).

- [ ] **Step 4: Emit `StaticString_` constants on Path A.** The string-literal path currently rode the `StaticSymbol_` machinery (`tools/ns_compile_oevm.py`, anchor `self.solidify.symbols[name] = f"StaticSymbol_{sanitized}"`). Add a parallel `StaticString_` path. In `tools/solidify.py`, find the symbol struct emission anchor:
```python
            c_code.append(f'const struct Symbol {sym_var} = {{ {{ (oop)&Vtable_SymbolVtable }}, "{sym_name}", {len_sym} }};')
```
and add a sibling helper that, for each string constant, emits a fixed-layout struct matching `struct String` (base, length, inline bytes) — bytes as a brace-init so non-printable/UTF-8 bytes are safe:
```python
            # std-lib 3c-1: String constants (inline bytes, StringVtable).
            raw = s_value.encode("utf-8")
            byte_init = ", ".join(str(b) for b in raw) if raw else "0"
            c_code.append(
                f'static const struct {{ struct Object base; size_t length; uint8_t bytes[{max(len(raw),1)}]; }} '
                f'{str_var} = {{ {{ (oop)&Vtable_StringVtable }}, {len(raw)}, {{ {byte_init} }} }};'
            )
```
Also emit the extern: find `c_code.append("extern const struct Vtable Vtable_SymbolVtable;")` and add after it `c_code.append("extern const struct Vtable Vtable_StringVtable;")`. Reference each String constant as `(oop)&{str_var}` wherever the constant pool needs its address (mirror how `(oop)&{sym_var}` / `StaticSymbol_` refs are emitted).

- [ ] **Step 5: `UPDATE_LIMITS` for String constants (Path A).** Find the anchor `c_code += f"    UPDATE_LIMITS(&{sym}, sizeof(struct Symbol));\n"` and add a parallel loop over the String constants:
```python
        for sstr in <string-constants-collection>:
            c_code += f"    UPDATE_LIMITS(&{sstr}, sizeof(struct Object) + sizeof(size_t) + {sstr}_len);\n"
```
Use the actual byte length per constant (or `sizeof(struct String) + len`). This marks them read-only so GC's `is_read_only` skips them (no leaf tracing needed for data-segment constants).

- [ ] **Step 6: Change `PRIM_printString` to produce Strings (+`#`/booleans).** In `dispatch.c` replace the body of `case PRIM_printString:` (anchor the existing `char buf[128];` … `return sym_obj;` block) with:
```c
            case PRIM_printString: {
                char buf[128];
                if (IS_SMI(receiver)) {
                    snprintf(buf, sizeof(buf), "%ld", (long)UNTAG_SMI(receiver));
                } else if (receiver != NULL && obj_vtable(receiver) == FloatVtable) {
                    snprintf(buf, sizeof(buf), "%g", ((struct Float *)receiver)->value);
                } else if (receiver != NULL && obj_vtable(receiver) == LargeIntegerVtable) {
                    struct LargeInteger *li = (struct LargeInteger *)receiver;
                    snprintf(buf, sizeof(buf), "%s%llu", li->sign < 0 ? "-" : "", (unsigned long long)li->mag);
                } else if (receiver != NULL && StringVtable != NULL && obj_vtable(receiver) == StringVtable) {
                    return receiver;                          /* a String prints as itself */
                } else if (receiver != NULL && obj_vtable(receiver) == SymbolVtable) {
                    if (receiver == true_symbol)  return make_string("true", 4);   /* boolean: no '#' */
                    if (receiver == false_symbol) return make_string("false", 5);
                    struct Symbol *sy = (struct Symbol *)receiver;                 /* genuine Symbol: '#' prefix */
                    size_t n = sy->length;
                    char *tmp = (char *)malloc(n + 1);
                    tmp[0] = '#'; memcpy(tmp + 1, symbol_name(sy), n);
                    oop r = make_string(tmp, n + 1); free(tmp); return r;
                } else if (receiver == NULL) {
                    snprintf(buf, sizeof(buf), "nil");
                } else {
                    break;   /* real heap object: Object.ns printString */
                }
                return make_string(buf, strlen(buf));
            }
            break;
```

- [ ] **Step 7: Change `PRIM_comma` to produce a String.** Replace the `PRIM_comma` body (anchor the `// String concatenation` block that builds a Symbol) with a String⊕String concat:
```c
            case PRIM_comma: {
                /* String concatenation -> new String (std-lib 3c-1). Only
                   String-String is defined; anything else falls through to DNU. */
                if (receiver != NULL && !IS_SMI(receiver) && StringVtable != NULL &&
                    obj_vtable(receiver) == StringVtable &&
                    args[0] != NULL && !IS_SMI(args[0]) && obj_vtable(args[0]) == StringVtable) {
                    struct String *a = (struct String *)receiver;
                    struct String *b = (struct String *)args[0];
                    size_t n = a->length + b->length;
                    oop s = gc_allocate(StringVtable, sizeof(struct String) + n);
                    ((struct String *)s)->length = n;
                    memcpy(((struct String *)s)->bytes, a->bytes, a->length);
                    memcpy(((struct String *)s)->bytes + a->length, b->bytes, b->length);
                    return s;
                }
            }
            break;
```
(`extern oop gc_allocate(...)` is already declared in the function; if not in scope here, add it.)

- [ ] **Step 8: Build the VM + regenerate the Path A image.** Run: `cd ../oevm && make`. Then rebuild Path A as the runner does (the runner regenerates the solidified C from `.ns`; confirm no manual step is needed). Expected: clean compile.

- [ ] **Step 9: Run the new test + full suite on Path A.** Run: `OEVM_PATH=A python3 tools/run_tests_oevm.py 32_string` → PASS. Then `OEVM_PATH=A python3 tools/run_tests_oevm.py` → **56 pass / 0 fail / 3 skip** (baseline 54 + tests 31,32). **`stringCompare`/`perfstex` must still pass** (content `=`, `String⊕String` `,`). Path B is expected RED at this point — do not run B yet.

- [ ] **Step 10: Update `docs/NEWSPEAK_DIALECT.md`.** Add row **#10** (String — UTF-8 byte sequence, byte-granularity, no `Character`, non-interned, runes deferred → issue #1) using the spec §9 text. Amend **#3** title/body to "`Array` **and** `String` are bare indexables, not `Collection`s." Amend **#5** to note booleans now answer the full `hash`/`=`/`printString` protocol (deviation persists: no `True`/`False` classes). Match the existing table's column format exactly.

- [ ] **Step 11: Commit (compiler + VM + docs + test).**
```bash
cd ../oevm && git add src/dispatch.c && git commit -m "feat(stdlib): printString/comma produce String; # prefix + boolean printString (3c-1 task 3)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
cd ../ns-oevm && git add tools/ns_compile_oevm.py tools/solidify.py docs/NEWSPEAK_DIALECT.md && git commit -m "feat(stdlib): string literals -> String constants (Path A) + dialect #10 (3c-1 task 3)

Path B left red by design; restored in task 4.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
cd ../ns-tests && git add 04_somns_basic/32_string.ns && git commit -m "test(stdlib): String identity/content/byte protocol + printString (3c-1 task 3)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: Path B (`.oimg`) String constants — restore both-paths-green

Serialize `String` constants into the `.oimg` and load them, so Path B matches Path A. Deliverable: suite **56/0/3 on BOTH paths**.

**Files:**
- Modify: `tools/gen_builtins.py` (+ `tools/builtins_registry.py` if struct layouts live there) — add `String` struct layout metadata
- Modify: `tools/oimg_serialize.py` (emit `String` constant records)
- Modify: `../oevm/src/oimg_loader.c` (accept `String` constants; bump `format_version` if the descriptor/constant table changes)

**Interfaces:**
- Consumes: `StringVtable` builtin index (Task 1), the String constant collection (Task 3).
- Produces: `.oimg` images containing materialized `String` constants.

- [ ] **Step 1: Confirm the failure.** Run: `OEVM_PATH=B python3 tools/run_tests_oevm.py 32_string`. Expected: FAIL (string constants not serialized/loaded). This is the Task-4 target.

- [ ] **Step 2: Add the `String` struct layout to the metadata.** `tools/oimg_serialize.py` uses `self.structs["Symbol"]` (field offsets). Find where `self.structs` is populated (search `self.structs` / `gen_builtins`) and add a `String` entry describing `{ base.vtable, length, bytes }` (bytes = inline payload after `length`). Mirror the `Symbol` layout source — likely generated by `tools/gen_builtins.py` from the C structs; if so, add `struct String` to its input list and regenerate.

- [ ] **Step 3: Emit `String` constant records in `oimg_serialize.py`.** Mirror the Symbol emission (anchor):
```python
        for name in self.app_syms:
            base = self.offset[f"sym:{name}"]
            self._put(buf, base, "Symbol", "base.vtable", self.w.word(RefWord.builtin(self.reg["SymbolVtable"])))
            self._put(buf, base, "Symbol", "name", self.w.word(self._aref(f"strbytes:{name}")))
            self._put(buf, base, "Symbol", "length", self._scalar_bytes("length", len(name)))
            sbase = self.offset[f"strbytes:{name}"]
            raw = name.encode("utf-8") + b"\x00"
            buf[sbase: sbase + len(raw)] = raw
```
Add an analogous reserve+emit for the app's String constants, but with **inline** bytes (no separate `strbytes:` pointer — the bytes live in the record after `length`):
```python
        for sval in self.app_strings:                       # the String-constant collection
            base = self.offset[f"str:{sval_key}"]
            raw = sval.encode("utf-8")
            self._put(buf, base, "String", "base.vtable", self.w.word(RefWord.builtin(self.reg["StringVtable"])))
            self._put(buf, base, "String", "length", self._scalar_bytes("length", len(raw)))
            ibase = base + self.structs["String"]["bytes"]   # offset of inline payload
            buf[ibase: ibase + len(raw)] = raw
```
and a matching `reserve(f"str:{sval_key}", struct_String_header + len(raw))` in the reservation pass (mirror `reserve(f"strbytes:{name}", ...)`). Thread the String constants from the compiler's constant pool into `self.app_strings` (mirror how `self.app_syms` is collected). Ensure `self.reg["StringVtable"]` resolves (Task 1 / `builtins_registry.py`).

- [ ] **Step 4: Accept String constants in the loader.** Inspect `../oevm/src/oimg_loader.c`: app objects are laid out in the image buffer and relocated generically, so an inline-bytes `String` record needs no special decode **if** its bytes are inline and its vtable ref resolves via `builtin_registry`. Verify the generic object-load path handles a record whose vtable is a builtin and whose payload is raw bytes (no internal oop fields to relocate). If the loader validates known constant kinds or per-object sizes, add `String`. If the on-disk header/constant-table layout changes, bump `format_version` 3→4: update the guard (anchor):
```c
    if (format_version != 3) {
        fprintf(stderr, "oimg_load: unsupported format_version %u (expected 3)\n", format_version);
```
to accept 4, and update the writer's `format_version` in `oimg_serialize.py` to match. If no layout change is needed, leave the version at 3 and note that in the commit.

- [ ] **Step 5: Build + regenerate `.oimg` + run Path B.** Run: `cd ../oevm && make`, then `OEVM_PATH=B python3 tools/run_tests_oevm.py 32_string` → PASS, and `OEVM_PATH=B python3 tools/run_tests_oevm.py 31_hash_protocol` → PASS.

- [ ] **Step 6: Run the full suite BOTH paths.** Run: `OEVM_PATH=AB python3 tools/run_tests_oevm.py`. Expected: **56 pass / 0 fail / 3 skip on both paths.**

- [ ] **Step 7: Commit (compiler tooling + VM).**
```bash
cd ../ns-oevm && git add tools/oimg_serialize.py tools/gen_builtins.py tools/builtins_registry.py && git commit -m "feat(stdlib): serialize String constants into .oimg (Path B) (3c-1 task 4)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
cd ../oevm && git add src/oimg_loader.c && git commit -m "feat(stdlib): load String constants from .oimg (3c-1 task 4)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: "Not a Collection" pin + final verification + default image refresh

Locks the Option-C behavior for String, refreshes the committed default solidified image (3b precedent: keep it regenerated from the latest test), and does the whole-suite final check.

**Files:**
- Test: `../ns-tests/04_somns_basic/32_string.ns` (extend) or a new `33_string_not_collection.ns`
- Modify: the committed default solidified image (whatever 3b committed — find via `git log` for "default solidified image"/"solidified_image")

**Interfaces:**
- Consumes: everything from Tasks 1–4.

- [ ] **Step 1: Write the failing "not a Collection" assertion.** Add to `../ns-tests/04_somns_basic/32_string.ns` (or new `33_string_not_collection.ns`, modeled on `30_collections.ns`'s DNU idiom): assert `'abc' collect: [:c | c]` is a `doesNotUnderstand` (mirror how `03_does_not_understand.ns` / the Array Option-C test catches/asserts DNU). Use the suite's DNU-assertion convention exactly.

- [ ] **Step 2: Run to verify it fails (if the assertion isn't yet satisfied) or passes.** Run: `OEVM_PATH=A python3 tools/run_tests_oevm.py 32_string` (or `33_...`). A `String` (intrinsic, no Collection methods) should already DNU `collect:` — if the test harness needs a specific DNU-capture form, adjust the test until it correctly asserts the DNU. Expected once correct: PASS.

- [ ] **Step 3: Refresh the committed default solidified image.** Find the 3b precedent: `git -C ../ns-oevm log --oneline | grep -i "solidified image"` and the file it commits (e.g. a checked-in `solidified_image.c` or a snapshot under `tools/`). Regenerate it from the current kernel + a representative test (3b used `30_collections.ns`; for 3c-1 use `32_string.ns`) with the project's snapshot mechanism (`tools/snapshot_images.py` — inspect its usage). Only do this if 3b committed such an artifact; otherwise skip and note it.

- [ ] **Step 4: Final full-suite run, both paths.** Run: `OEVM_PATH=AB python3 tools/run_tests_oevm.py`. Expected: **≥56 pass / 0 fail / 3 skip on both paths** (54 baseline + 31 + 32 [+33]). Record exact counts.

- [ ] **Step 5: Commit.**
```bash
cd ../ns-tests && git add 04_somns_basic/ && git commit -m "test(stdlib): String is not a Collection (Option C DNU) (3c-1 task 5)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
cd ../ns-oevm && git add -A && git commit -m "chore(stdlib): refresh default solidified image for 3c-1 String

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>" || echo "no image artifact to refresh"
```

---

## Self-Review

**1. Spec coverage:**
- §2 bare/immutable/non-interned String → Task 1 (type), Task 3 (literals); not-a-Collection → Task 5; `,`-as-prim → Task 3; content `=`/`hash` → Task 2. ✓
- §3 struct + GC leaf + literal emission both paths → Task 1 (struct/GC), Task 3 (Path A), Task 4 (Path B). ✓
- §4 String protocol (size/at:/=/hash/`,`/printString) → Tasks 2–3. (`do:`/`isEmpty` deferred — noted Task 2 Step 8; spec listed them as conveniences, callers use `size`/`1 to:do:`. **Deviation from spec §4 — acceptable YAGNI, recorded here.**) ✓
- §5 printString→String everywhere, `#`/boolean → Task 3 Step 6; `hash` prim + contract → Task 2; `=` → Task 2. ✓
- §6 booleans close #5 gap → Task 2 (hash) + Task 3 (printString) + dialect amend Task 3 Step 10. ✓
- §7 migration (printString→String, literals→String, `=` identity→content; `stringCompare` pin) → Task 3 Steps 6–9. ✓
- §8 testing → Tasks 2–5 tests. §9 dialect → Task 3 Step 10. ✓

**2. Placeholder scan:** The Path-A/Path-B serialization steps (Task 3 Steps 3–5, Task 4 Steps 2–3) reference collections/offsets to be located in the compiler/serializer (`add_string_constant`, `self.app_strings`, `self.structs["String"]`) rather than fully-quoted current code, because that machinery wasn't read line-for-line. Each gives the **Symbol-pattern anchor to mirror** and concrete code; the executing subagent must read the surrounding `add_constant`/`app_syms` code and follow the pattern. Flagged, not hidden.

**3. Type consistency:** `make_string(const char*, size_t)` defined Task 2 Step 4, used Task 3 Steps 6–7. `struct String {base; length; bytes[]}` consistent across oevm.h, dispatch prims, solidify static-struct layout, oimg record. `StringVtable` builtin index consistent across kernel_boot.c (Step 5) and builtins_registry.py (Step 6) — **executor must use the same number in both.** `hash`/`identityHash` selectors consistent (enum/names parallel; `Object>>hash` calls `identityHash`).

**Known coupling risk (called out for the executor):** Task 3 is a coupled flip and intentionally leaves **Path B red between Task 3 and Task 4** — run only `OEVM_PATH=A` in Task 3, `OEVM_PATH=AB` from Task 4 on.
