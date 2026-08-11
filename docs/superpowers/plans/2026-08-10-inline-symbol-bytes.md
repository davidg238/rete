# Inline Symbol bytes — implementation plan (rete#190 stage 2, design §4 / sub-project B)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended)
> or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`)
> syntax for tracking.

**Goal:** Move a `Symbol`'s name bytes inline into the heap object, exactly as `struct String`
already does, so the per-decoded-Symbol `malloc` that leaks ~10 C-heap blocks per check-in stops
existing.

**Architecture:** `struct Symbol` gains a flexible array member `char name[]` and loses its
`const char *name`. `symbol_name()` collapses from a blob/RAM branch to `return s->name;`, because
inline bytes work identically in an XIP image and in RAM — which is the whole argument, and the
reason `String` needed no blob accessor. The VM, the Path-A C emitter (`solidify.py`), the layout
dumper and the `.oimg` serializer must all land in **one commit**: an image built by one and loaded
by the other is silently wrong, not loudly broken. This plan closes that hole by bumping the image
`format_version` 8 → 9 so a stale pairing is rejected at load.

**Tech Stack:** C99 (host gcc + `riscv32` esp-zephyr gcc), Python 3 (`solidify.py`,
`ns_compile_oevm.py`, `oimg_serialize.py`), nsl, Zephyr/ESP32-C6.

## Global Constraints

- **Zero backward compatibility is required.** Both ends are reflashed together. No versioned
  field, no dual-read, no deprecation window (CLAUDE.md).
- **The serializer and the VM must land in the same commit** (design §4, "The image format changes
  with it").
- **`review/` is never swept, moved, tidied or reordered.**
- Gates, all three, must be green before the hardware task:
  - `cd nodus/nsl && python3 tools/run_tests_oevm.py` → `258 passed, 0 failed`, **and read the aux
    gate line** — it can be red while the count reads zero failures.
  - `cd nodus/tuvm && bash tools/run_c_tests.sh` → no `FAIL`
  - `cd nodus/tuvm && bash tests/porta_sync_host.sh` → `PORTA SYNC OK`
- **Never run two nsl suites concurrently** — they share `/tmp` paths. Run long builds in the
  controlling session, not in a subagent (subagents background a long build and wait forever).
- `footprint: SKIP (no arm-zephyr-eabi toolchain)` is pre-existing and expected.

---

## What the survey found that design §4 gets wrong

These four corrections were established by reading the tree on 2026-08-10 and must be folded back
into `docs/design/2026-08-10-node-memory-and-liveness-design.md` §4 as part of Task 6. They are
recorded here first because Tasks 1–4 depend on them.

1. **§4 says "Two static Symbols cannot hold a flexible array member." There are nine hand-written
   ones and about 265 generated ones.** Hand-written: `src/dispatch.c:278-279` (`true_sym_obj`,
   `false_sym_obj`) and `src/kernel_boot.c:51-61` (`KSym__eq_eq`, `KSym_sender`, `KSym_value`,
   `KSym_accessViolation_`, `KSym_debug_`, `KSym_true`, `KSym_false`). Generated: every
   `StaticSymbol_*` in `src/solidified_image.c`, all emitted by the **single line**
   `nodus/nsl/tools/solidify.py:132`. That file is the Path-A / Model-A image and is still linked —
   `tools/run_c_tests.sh` builds six tests against `ENG_A`, and `zephyr/CMakeLists.txt` exposes
   `-DTUVM_MODEL=A`. The generated wall is one line of Python, not 265 edits, but it is **not
   optional**.
2. **The `.oimg` `format_version` is hardcoded `8` in `src/oimg_loader.c:42/123`.** §4 accepts
   "silently wrong" as the failure mode of a mismatched serializer/VM pair. Bumping to `9` in the
   loader and the writer turns that into a one-line loud rejection. Take it.
3. **`tools/dump_layout.c:14` marks `Symbol.name` with `is_ptr = 1`.** It must become `0`, exactly
   like `String.bytes` at line 49. The rest of the layout follows `offsetof` automatically, so no
   other dumper edit is needed.
4. **A lifetime hazard §4 does not name.** Today `Symbol->name` is a leaked `malloc`, so a
   `const char *` from `symbol_name()` stays valid forever — *even after the Symbol itself is
   collected*. With inline bytes the text dies with the object. The GC is **mark-sweep and
   non-moving** (`gc.c` sweep coalesces free blocks; no per-object destructor, no relocation), so a
   live Symbol's address is stable — but a `symbol_name()` pointer held across a `gc_allocate` that
   triggers a collection is newly dangling. Task 2 is the audit that closes this before the flip.

Also worth knowing, and load-bearing for Task 4:

- `mark()` returns early for `is_in_blob(ptr)` and `is_read_only(ptr)`, so image and flash Symbols
  are never traced. **Only heap Symbols reach `trace_one`** — which is exactly the set the new leaf
  case must cover.
- `oimg_serialize.py` already contains the precise template, written for `String` at lines 505-508
  and 532-538: a variable-size record reserved individually rather than in the fixed-size
  `KIND_ORDER` loop, with the payload written at `structs["String"]["fields"]["bytes"]["offset"]`.
- **`CHeap` has no "outstanding blocks" field.** Its five fields are
  `outBytes, outHigh, allocs, frees, fails`. The block count quoted throughout §11.5 is
  **`allocs − frees`**, computed by the reader. Task 7 depends on this.

---

## File Structure

| File | Responsibility after this plan |
|---|---|
| `nodus/tuvm/src/oevm.h` | `struct Symbol` with `size_t length; char name[];` — field order changes, because a flexible array member must be last |
| `nodus/tuvm/src/oimg.h` | `symbol_name()` collapses to `return s->name;`; the blob branch disappears |
| `nodus/tuvm/src/gc.c` | `trace_one` gains a `SymbolVtable` leaf case beside `String`/`RawBytes` |
| `nodus/tuvm/src/dispatch.c` | 2 static Symbols → FAM form; 2 dynamic sites → `gc_allocate(… + len + 1)` + `memcpy` |
| `nodus/tuvm/src/vm.c` | 3 dynamic sites → same shape |
| `nodus/tuvm/src/platform.c` | 1 dynamic site (`asSymbol`) → the malloc and its NULL check disappear entirely |
| `nodus/tuvm/src/kernel_boot.c` | 7 static Symbols → FAM form |
| `nodus/tuvm/src/oimg_loader.c` | `format_version` 8 → 9 |
| `nodus/tuvm/tools/dump_layout.c` | `Symbol.name` `is_ptr` 1 → 0 |
| `nodus/nsl/tools/solidify.py` | line 132 emits the FAM initializer; regenerates all ~265 `StaticSymbol_*` |
| `nodus/tuvm/src/solidified_image.c` | regenerated fixture (tracked in git) |
| `nodus/nsl/tools/oimg_serialize.py` | `SYMBOL` leaves the fixed-size loop; `strbytes:` reservations disappear; writer version 8 → 9 |
| `nodus/tuvm/tests/test_symbol_inline.c` | **new** — the regression test that makes the fix falsifiable on the host |
| `nodus/tuvm/tools/run_c_tests.sh` | runs the new test |

---

## Task 1: Prove the static flexible-array-member extension on both toolchains

The riskiest assumption in design §4 is one sentence: *"GCC accepts static initialisation of a
flexible array member as a documented extension, and both toolchains here are GCC — take the
extension."* If that is false on the `riscv32` esp-zephyr compiler, the whole shape changes and 275
static Symbols have no home. Falsify it first, in ten minutes, before touching a line of the VM.

**Files:**
- Create: `/tmp/claude-1000/-home-david-workspace-rete/bc6eb6f1-cf2a-45e6-b898-186e0eff33b6/scratchpad/fam_spike.c` (throwaway — nothing is committed by this task)

**Interfaces:**
- Consumes: nothing.
- Produces: a yes/no on the FAM extension for both compilers, and the exact `-W` flags needed to
  keep it quiet. Task 3 assumes "yes".

- [ ] **Step 1: Write the spike**

```c
/* rete#190 stage 2, Task 1: does a static const struct with a flexible array
   member initialise, land in .rodata, and read back correctly — on BOTH
   toolchains? The VM needs ~275 of these (dispatch.c 2, kernel_boot.c 7,
   solidified_image.c ~265). */
#include <stdio.h>
#include <string.h>

struct Object { void *vtable; };
struct Symbol {
    struct Object base;
    size_t length;
    char name[];        /* inline, NUL-terminated: allocate length + 1 */
};

static const struct Symbol s_true  = { { (void *)0 }, 4, "true" };
static const struct Symbol s_false = { { (void *)0 }, 5, "false" };
static const struct Symbol s_colon = { { (void *)0 }, 16, "accessViolation:" };

int main(void) {
    /* the address is the identity — it must be stable and distinct */
    if (&s_true == &s_false) { puts("FAIL: addresses collide"); return 1; }
    if (strcmp(s_true.name, "true") != 0)   { puts("FAIL: true"); return 1; }
    if (strcmp(s_false.name, "false") != 0) { puts("FAIL: false"); return 1; }
    if (strcmp(s_colon.name, "accessViolation:") != 0) { puts("FAIL: colon"); return 1; }
    if (s_true.length != 4 || s_colon.length != 16) { puts("FAIL: length"); return 1; }
    /* the payload must be NUL-terminated: symbol_name() feeds strcmp at 20+ sites */
    if (s_colon.name[16] != '\0') { puts("FAIL: not NUL-terminated"); return 1; }
    printf("FAM static init OK (sizeof=%zu)\n", sizeof(struct Symbol));
    return 0;
}
```

- [ ] **Step 2: Run it on the host toolchain, with the project's warning posture**

```bash
SP=/tmp/claude-1000/-home-david-workspace-rete/bc6eb6f1-cf2a-45e6-b898-186e0eff33b6/scratchpad
gcc -std=c99 -Wall -Wextra -pedantic -o "$SP/fam_spike" "$SP/fam_spike.c" && "$SP/fam_spike"
```

Expected: prints `FAM static init OK (sizeof=…)`. `-pedantic` is likely to emit
`ISO C forbids empty initializer braces`-class warnings — **record the exact warning text**; it
decides whether Task 3 needs a `#pragma`/`-Wno-` or nothing at all. A warning is fine; a hard error
is the finding.

- [ ] **Step 3: Compile it with the C6's toolchain**

The C6 build lives in the ESP32 west workspace (`~/zephyr-esp32`), and the Zephyr SDK ships the
`riscv64-zephyr-elf` gcc that targets `rv32`. Find it and compile — no run, compilation is the
question:

```bash
SDK=$(ls -d ~/zephyr-sdk-* 2>/dev/null | head -1)
CC="$SDK/riscv64-zephyr-elf/bin/riscv64-zephyr-elf-gcc"
ls -l "$CC" || echo "TOOLCHAIN NOT AT THIS PATH — locate it before continuing"
SP=/tmp/claude-1000/-home-david-workspace-rete/bc6eb6f1-cf2a-45e6-b898-186e0eff33b6/scratchpad
"$CC" -std=c99 -Wall -Wextra -march=rv32imac_zicsr -mabi=ilp32 \
      -c -o "$SP/fam_spike.o" "$SP/fam_spike.c" && echo "C6 TOOLCHAIN: COMPILES"
```

Expected: `C6 TOOLCHAIN: COMPILES`.

- [ ] **Step 4: Prove the statics landed in read-only memory, not `.data`**

This is the part a compile check misses. The VM's `mark()` skips these objects via
`is_read_only()`; if the FAM form silently moved them into a writable section the GC's assumptions
change.

```bash
SP=/tmp/claude-1000/-home-david-workspace-rete/bc6eb6f1-cf2a-45e6-b898-186e0eff33b6/scratchpad
SDK=$(ls -d ~/zephyr-sdk-* | head -1)
"$SDK/riscv64-zephyr-elf/bin/riscv64-zephyr-elf-nm" -S "$SP/fam_spike.o" | grep -i "s_true\|s_false\|s_colon"
```

Expected: each symbol's type letter is `r`/`R` (read-only data), **not** `d`/`D`/`b`/`B`. Record
the sizes — they must be `sizeof(struct Symbol) + length + 1` rounded to alignment, which is the
first direct confirmation that the payload really is inline.

- [ ] **Step 5: Record the verdict, commit nothing**

Write the three answers (host compiles? C6 compiles? section is `.rodata`?) into the task's
report. **If any answer is "no", STOP and escalate** — §4's fallback is *"a layout-compatible
sized-storage struct behind a macro"*, which is a different plan and needs David's call before it
is built.

---

## Task 2: Audit every `symbol_name()` consumer for the new lifetime rule

Today the name outlives everything because it is leaked. After Task 3 it dies with its Symbol. This
task establishes, before the change, that no site holds a `symbol_name()` result across an
allocation — and leaves the rule written down so the next person does not reintroduce one.

**Files:**
- Modify: `nodus/tuvm/src/oimg.h` (comment only, above `symbol_name`)

**Interfaces:**
- Consumes: correction 4 above.
- Produces: a documented invariant and a confirmed-clean (or itemised-dirty) list of the 32 call
  sites. Task 3 fixes anything this task finds.

- [ ] **Step 1: Enumerate the sites — count first, then read**

A truncated search reads exactly like a complete answer, so count before reading:

```bash
cd /home/david/workspace/rete/nodus/tuvm
grep -rn "symbol_name(" src/ | wc -l          # expect 32 (31 call sites + the definition)
grep -rn "symbol_name(" src/
```

- [ ] **Step 2: Classify each site**

For each, decide which of three shapes it is:

- **(a) Immediate** — the result feeds `strcmp`/`strncmp`/`memcmp`/`platform_console_write`/
  `printf` and is dead before the next statement. No allocation intervenes. Safe, no change.
- **(b) Held across an allocation** — the pointer is still live when a `gc_allocate`, `indexable_new`,
  or any VM send happens. **Newly dangling. Must be fixed in Task 3** by copying the bytes into a
  local buffer first, the same discipline `asSymbol` already applies to its String receiver.
- **(c) Stored** — the pointer is written into a struct field, a global, or returned upward. Same
  verdict as (b).

The survey found all of `dispatch.c:1606`, `dispatch.c:1713`, `dispatch.c:1872`, `platform.c:685`,
`serial.c:29`, `udp.c:62`, `mirror.c:49` to be shape (a) on inspection — each writes to the console
or compares immediately. **Confirm that independently rather than inheriting it**, and check the
remaining sites, which were not individually read: `hanoi_native.c:62`, `dispatch.c:295/406/428/437/446/528/574/963/1040/1091/1137/1165/1174-1175/1201/1637/1639/1648`,
`vm.c:107/147/1027/1042`.

- [ ] **Step 3: Write the invariant where the next person will hit it**

In `src/oimg.h`, directly above `symbol_name`:

```c
/* rete#190 stage 2: the bytes are INLINE in the Symbol object, so the returned
   pointer lives exactly as long as the Symbol does. Before this change `name`
   was a leaked malloc and the pointer was immortal, which is why no call site
   ever had to think about it. The rule now: do not hold this pointer across an
   allocation, a VM send, or anything else that can trigger gc_collect — copy
   the bytes into a local first. The GC is non-moving, so a LIVE Symbol's bytes
   never relocate; the hazard is collection, not compaction. */
```

- [ ] **Step 4: Commit the audit**

```bash
cd /home/david/workspace/rete
git add nodus/tuvm/src/oimg.h
git commit -m "docs(#190): the inline-Symbol lifetime rule, written where symbol_name() is read

The leaked malloc was, perversely, providing lifetime safety: a symbol_name()
pointer was immortal because nothing ever freed it. Inline bytes end that, so
the rule goes above the accessor before the flip lands."
```

Report the classification table. **If any site is shape (b) or (c), list it explicitly** — Task 3
must fix it, and a plan that says "all clean" without the enumeration is the truncated-search
failure again.

---

## Task 3: The atomic flip — struct, accessor, GC, all 275 statics, all 6 dynamic sites, the serializer

One commit, because a half-landed change produces an `.oimg` that loads and misbehaves. Big, but
the boundary is forced by the format.

**Files:**
- Modify: `nodus/tuvm/src/oevm.h:77-81`, `nodus/tuvm/src/oimg.h:54-57`,
  `nodus/tuvm/src/gc.c` (`trace_one`, near line 600),
  `nodus/tuvm/src/dispatch.c:278-279,458-459,1682-1689`,
  `nodus/tuvm/src/vm.c:121-122,718-719,1619-1620`,
  `nodus/tuvm/src/platform.c:1224-1235`,
  `nodus/tuvm/src/kernel_boot.c:51-61`,
  `nodus/tuvm/src/oimg_loader.c:42,123-124`,
  `nodus/tuvm/tools/dump_layout.c:14`,
  `nodus/nsl/tools/solidify.py:132`,
  `nodus/nsl/tools/oimg_serialize.py` (~lines 500-540, and the writer version)
- Regenerate: `nodus/tuvm/src/solidified_image.c` (+ its `.stmap`)

**Interfaces:**
- Consumes: Task 1's verdict (the FAM extension works), Task 2's dirty-site list.
- Produces: `struct Symbol { struct Object base; size_t length; char name[]; }`;
  `symbol_name(struct Symbol *) -> const char *` unchanged in signature;
  `.oimg` `format_version == 9`. Task 4's test and Task 5's gates depend on all three.

- [ ] **Step 1: Change the struct**

`src/oevm.h`, replacing lines 77-81. Note the field order flips — a flexible array member must be
the last member, so `length` moves ahead of the payload:

```c
/* Symbol: an interned selector/name. std-lib 3c-1's String proved the shape:
   the bytes are INLINE in the heap object — one allocation, no ownership
   question, and identical in RAM and in an XIP image, which is why
   symbol_name() needs no blob branch (rete#190 stage 2). NUL-terminated
   because symbol_name() returns a const char * consumed by strcmp at more
   than twenty sites — allocate length + 1. GC leaf: gc.c trace_one must not
   scan `name` as oops. */
struct Symbol {
    struct Object base;
    size_t length;
    char name[];        /* inline, NUL-terminated: allocate length + 1 */
};
```

- [ ] **Step 2: Collapse the accessor**

`src/oimg.h`, replacing lines 54-57 (keep the Task 2 comment above it):

```c
static inline const char *symbol_name(struct Symbol *s) {
    return s->name;
}
```

- [ ] **Step 3: Add the GC leaf case — the landmine**

Without this, `trace_one`'s generic word-count fallback scans raw symbol text as pointers. In
`src/gc.c`'s `trace_one`, beside the existing `String`/`RawBytes` arms (around line 600), and add
`extern oop SymbolVtable;` to the block of externs at the top of the function:

```c
    } else if (SymbolVtable != NULL && ptr->vtable == SymbolVtable) {
        /* rete#190 stage 2: same leaf reasoning as StringVtable/RawBytesVtable
           above — the inline `name` bytes are NOT oops. Before the bytes moved
           inline, Symbol fell through to the generic word-count fallback, which
           scanned a single const char * harmlessly; with inline bytes that
           fallback would misinterpret raw symbol text as pointers. */
        mark(ptr->vtable);
```

- [ ] **Step 4: Convert the six dynamic assignment sites**

Each becomes one `gc_allocate` sized for the payload, then a `memcpy`. Three of them lose nothing
else; `platform.c` and `dispatch.c:asString` **lose their `oevm_malloc` and its NULL check
entirely**, which is the point of the whole exercise.

`src/vm.c:121-122`:

```c
    static const char textName[] = "text";
    oop textSym = gc_allocate(SymbolVtable, sizeof(struct Symbol) + sizeof textName);
    ((struct Symbol *)textSym)->length = sizeof textName - 1;
    memcpy(((struct Symbol *)textSym)->name, textName, sizeof textName);
```

`src/vm.c:718-719`:

```c
        static const char valueName[] = "value";
        Symbol_value = gc_allocate(SymbolVtable, sizeof(struct Symbol) + sizeof valueName);
        ((struct Symbol *)Symbol_value)->length = sizeof valueName - 1;
        memcpy(((struct Symbol *)Symbol_value)->name, valueName, sizeof valueName);
```

`src/vm.c:1619-1620`:

```c
        static const char debugName[] = "debug:";
        oop sym = gc_allocate(SymbolVtable, sizeof(struct Symbol) + sizeof debugName);
        ((struct Symbol *)sym)->length = sizeof debugName - 1;
        memcpy(((struct Symbol *)sym)->name, debugName, sizeof debugName);
```

`src/dispatch.c:458-459`:

```c
            static const char avName[] = "accessViolation:";
            oop sym_av = gc_allocate(SymbolVtable, sizeof(struct Symbol) + sizeof avName);
            ((struct Symbol *)sym_av)->length = sizeof avName - 1;
            memcpy(((struct Symbol *)sym_av)->name, avName, sizeof avName);
```

`src/dispatch.c:1682-1689` (`PRIM_asString`) — `buf` is a local `char[128]` already holding the
rendered text, so the malloc simply goes away:

```c
                extern oop gc_allocate(oop vtable, size_t size);
                size_t blen = strlen(buf);
                /* rete#190 stage 2: the bytes are inline, so there is no
                   separate allocation to fail and no NULL to answer. */
                oop sym_obj = gc_allocate(SymbolVtable, sizeof(struct Symbol) + blen + 1);
                ((struct Symbol *)sym_obj)->length = blen;
                memcpy(((struct Symbol *)sym_obj)->name, buf, blen + 1);
                return sym_obj;
```

`src/platform.c:1224-1235` (`PRIM_pf_asSymbol`) — **this is the leak**, one malloc per decoded wire
Symbol. Replace the body from `struct String *s` through the `length` store, and rewrite the stale
comment above it:

```c
        /* Rung 3 Task 6: `platform asSymbol: aString` — build a genuine
           (non-interned) Symbol from a String's bytes, for CBOR tag-39 decode
           (and transitively Record tags, which require `isSymbol`). Symbol
           equality is VALUE-based (printString byteCompare), so this
           non-interned Symbol `=` the image-literal #foo, giving round-trip.
           rete#190 stage 2: the name bytes are INLINE, so this is now a single
           allocation and there is nothing to leak — the malloc'd-and-never-freed
           `name` this comment used to describe as "a known-minor matching that
           existing pattern" was the C-heap leak that crashed three C6 boards.
           GC-safety: length is read into a local BEFORE gc_allocate, and the
           receiver's bytes are copied AFTER it, with no allocation in between —
           args[0] is never dereferenced across an allocation. */
        struct String *s = (struct String *)args[0];
        size_t len = s->length;
        oop sym = gc_allocate(SymbolVtable, sizeof(struct Symbol) + len + 1);
        if (sym == NULL) { *out = NULL; return 1; }
        struct Symbol *sy = (struct Symbol *)sym;
        sy->length = len;
        if (len) memcpy(sy->name, ((struct String *)args[0])->bytes, len);
        sy->name[len] = 0;
        *out = sym; return 1;
```

Note the re-read of `args[0]` after `gc_allocate`: the allocation can collect, and while the GC does
not move objects, re-reading the root is the cheaper habit and costs nothing.

- [ ] **Step 5: Convert the nine hand-written statics**

`src/dispatch.c:278-279`:

```c
const struct Symbol true_sym_obj  = { { (oop)&Vtable_SymbolVtable }, 4, "true" };
const struct Symbol false_sym_obj = { { (oop)&Vtable_SymbolVtable }, 5, "false" };
```

`src/kernel_boot.c:51-61` — same transposition, `length` then the literal:

```c
static const struct Symbol KSym__eq_eq          = { { (oop)&Vtable_SymbolVtable }, 2,  "==" };
static const struct Symbol KSym_sender          = { { (oop)&Vtable_SymbolVtable }, 6,  "sender" };
static const struct Symbol KSym_value           = { { (oop)&Vtable_SymbolVtable }, 5,  "value" };
static const struct Symbol KSym_accessViolation_= { { (oop)&Vtable_SymbolVtable }, 16, "accessViolation:" };
static const struct Symbol KSym_debug_          = { { (oop)&Vtable_SymbolVtable }, 6,  "debug:" };
static const struct Symbol KSym_true            = { { (oop)&Vtable_SymbolVtable }, 4,  "true" };
static const struct Symbol KSym_false           = { { (oop)&Vtable_SymbolVtable }, 5,  "false" };
```

- [ ] **Step 6: Convert the ~265 generated statics — one line of Python**

`nodus/nsl/tools/solidify.py:132`, swapping the two initialiser fields:

```python
            c_code.append(f'const struct Symbol {sym_var} = {{ {{ (oop)&Vtable_SymbolVtable }}, {len_sym}, "{sym_name}" }};')
```

- [ ] **Step 7: Fix the layout dumper**

`nodus/tuvm/tools/dump_layout.c:14` — `is_ptr` (the 4th macro argument) 1 → 0, mirroring
`String.bytes` at line 49:

```c
    F(Symbol, "name",        0, 0, offsetof(struct Symbol, name)) \
```

- [ ] **Step 8: Teach the serializer to write inline bytes**

`nodus/nsl/tools/oimg_serialize.py`. Three edits, all mirroring what `String` already does.

First, `SYMBOL` can no longer ride the fixed-size-per-kind loop, so drop it from `KIND_ORDER`
(line 189) — its entry in `KIND_STRUCT` stays, because `_put` still resolves field offsets through
it:

```python
KIND_ORDER = ["VTABLE", "METHOD", "CLOSURE", "FLOAT", "LARGEINT", "INSTANCE", "BOXEDBINDING"]
```

Second, replace the `strbytes:` reservation loop (line ~502) with a per-Symbol variable-size
reservation, exactly like the `str:` loop beneath it:

```python
        # rete#190 stage 2: Symbols are VARIABLE-size (header + inline
        # NUL-terminated bytes), so they cannot ride the fixed-size-per-kind
        # reserve loop above — same treatment String has had since std-lib
        # 3c-1. The separate `strbytes:` allocation is gone: the payload lives
        # in the record itself, after `length`.
        sym_hdr = self.structs["Symbol"]["size"]
        for name in self.app_syms:
            reserve(f"sym:{name}", sym_hdr + len(name.encode("utf-8")) + 1)
```

Third, replace the Symbol emit block (line ~519) — the `name` ref-word write disappears:

```python
        sym_bytes_off = self.structs["Symbol"]["fields"]["name"]["offset"]
        for name in self.app_syms:
            base = self.offset[f"sym:{name}"]
            raw = name.encode("utf-8") + b"\x00"
            self._put(buf, base, "Symbol", "base.vtable", self.w.word(RefWord.builtin(self.reg["SymbolVtable"])))
            self._put(buf, base, "Symbol", "length", self._scalar_bytes("length", len(name)))
            nbase = base + sym_bytes_off
            buf[nbase: nbase + len(raw)] = raw
```

Two things to check while in this file, because they are easy to miss: the `sym:` offsets were
previously assigned by the `KIND_ORDER` loop and are now assigned by `reserve`, so **grep for every
other `sym:` and `strbytes:` reference** (`_symbol_ref`, `_aref`, `method_name_map`) and confirm
none still expects the old two-object layout.

- [ ] **Step 9: Bump the image format version at both ends**

Serializer — `OimgWriter(self.oop, version=1, …)` at line 232 is a *writer* revision, not the
format version; find where `header_bytes` writes the u16 at offset 4 and change the constant it
emits from `8` to `9`.

Loader — `src/oimg_loader.c:123-124`:

```c
    if (format_version != 9) {
        fprintf(stderr, "oimg_load_blob: unsupported format_version %u (expected 9)\n", format_version);
```

and the comment block at lines 32/42 that documents `format_version 8`. Add the reason:

```c
 * format_version 9 (rete#190 stage 2): Symbol name bytes are INLINE in the
 * record (no separate strbytes allocation, no `name` ref-word). Bumped so a
 * v8 image meets a v9 VM as a loud rejection rather than a silent misread.
```

- [ ] **Step 10: Regenerate the Path-A fixture**

`src/solidified_image.c` is tracked in git as the `MethodCallTest` fixture and must be rebaked from
the same source `zephyr/run.sh` uses:

```bash
cd /home/david/workspace/rete/nodus/tuvm
python3 ../nsl/tools/ns_compile_oevm.py tests/MethodCallTest.nsl src/solidified_image.c
git diff --stat src/solidified_image.c
```

If `tests/MethodCallTest.nsl` is not the right source, find the one whose class names match the
`Method_MethodCallTest_*` symbols at the top of the current file — **do not hand-edit the generated
file**, and do not commit a fixture whose diff shows anything but the Symbol initialiser
transposition plus offsets.

- [ ] **Step 11: Build and run the C tests**

```bash
cd /home/david/workspace/rete/nodus/tuvm && bash tools/run_c_tests.sh 2>&1 | tail -40
```

Expected: no `FAIL`. This is the first real gate — it exercises both `ENG_A` (which links the
regenerated `solidified_image.c`) and `ENG_B` (which links `kernel_boot.c`), so it catches a
mistake in either static-Symbol wall.

- [ ] **Step 12: Run the nsl suite — and read the aux line**

```bash
cd /home/david/workspace/rete/nodus/nsl && python3 tools/run_tests_oevm.py 2>&1 | tail -30
```

Expected: `Passed: 258 / Failed: 0`, and the aux-gate line clean apart from the expected
`footprint: SKIP (no arm-zephyr-eabi toolchain)`. **The pass count alone is not the gate** — the aux
line can be red while the count reads zero failures. This run rebuilds `dump_layout`, regenerates
`/tmp/oevm_layout.json` from the new struct, and recompiles every test image through the new
serializer, so it is the real proof that the format change is coherent end to end.

- [ ] **Step 13: Run the porta wire gate**

```bash
cd /home/david/workspace/rete/nodus/tuvm && bash tests/porta_sync_host.sh 2>&1 | tail -20
```

Expected: `PORTA SYNC OK`. This is the one that exercises `asSymbol` on the decode path — the site
the leak lived at — against a real porta.

- [ ] **Step 14: Commit, in one commit**

```bash
cd /home/david/workspace/rete
git add nodus/tuvm/src/oevm.h nodus/tuvm/src/oimg.h nodus/tuvm/src/gc.c \
        nodus/tuvm/src/dispatch.c nodus/tuvm/src/vm.c nodus/tuvm/src/platform.c \
        nodus/tuvm/src/kernel_boot.c nodus/tuvm/src/oimg_loader.c \
        nodus/tuvm/tools/dump_layout.c nodus/tuvm/src/solidified_image.c \
        nodus/tuvm/src/solidified_image.stmap \
        nodus/nsl/tools/solidify.py nodus/nsl/tools/oimg_serialize.py
git commit -m "feat(#190): Symbol name bytes go inline, and the malloc-per-decoded-Symbol leak with them

struct String already solved this: inline bytes work identically in RAM and in
an XIP image, which is why String never needed a blob accessor. Symbol takes
the same shape, so symbol_name() collapses to a field read, the three-owner
ownership question disappears, and platform asSymbol: stops leaking one C-heap
block per decoded wire Symbol.

The VM and the serializer land together because an .oimg built by one and read
by the other is silently wrong; format_version 8 -> 9 makes that pairing loud
instead. gc.c gains a SymbolVtable leaf case — without it the generic
word-count fallback would scan raw symbol text as pointers.

Static Symbols: 2 in dispatch.c, 7 in kernel_boot.c, and ~265 regenerated from
one line of solidify.py. Design §4 said there were two."
```

---

## Task 4: The host regression test that makes the fix falsifiable

Three gates passing proves nothing broke. This proves the thing that was supposed to happen
happened — and guards the GC landmine, which no existing test would catch.

**Files:**
- Create: `nodus/tuvm/tests/test_symbol_inline.c`
- Modify: `nodus/tuvm/tools/run_c_tests.sh`

**Interfaces:**
- Consumes: `struct Symbol` (Task 3), `oevm_cheap_stats()` / `struct OevmCHeapStats` (stage 1,
  fields `out_bytes, out_high, allocs, frees, fails`), `gc_allocate`, `gc_collect`, `SymbolVtable`.
- Produces: `test_symbol_inline` in the C gate.

- [ ] **Step 1: Write the failing test**

Two properties. The first is the fix; the second is the landmine.

```c
/* rete#190 stage 2: Symbol name bytes are inline (src/oevm.h), which is what
   ends the malloc-per-decoded-Symbol C-heap leak that crashed three C6 boards.
   Two properties, because passing gates prove only that nothing broke.

   Build (Model A link set, same as test_cheap_stats.c):
     gcc -std=c99 -Isrc -DOEVM_HEAP_SIZE=65536 tests/test_symbol_inline.c \
         src/vm.c src/gc.c src/dispatch.c src/platform.c src/serial.c src/udp.c \
         src/mirror.c src/backend/posix/platform_posix.c \
         src/backend/posix/serial_posix.c src/backend/posix/udp_posix.c \
         src/oimg_loader.c src/solidified_image.c -lm -o /tmp/tsi && /tmp/tsi

   Verifies:
     1. Building Symbols costs the C heap NOTHING. Before this change each one
        cost an oevm_malloc that was never freed; the seam's own counters are
        the witness, and `allocs` not moving is a stronger claim than
        "out_bytes came back down".
     2. A Symbol whose name bytes spell a valid heap address survives a
        collection. Without the SymbolVtable leaf case in gc.c's trace_one, the
        generic word-count fallback scans that text as an oop — this test is
        the only thing standing between that landmine and a live node. */
#include "../src/oevm.h"
#include "../src/oimg.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

extern oop SymbolVtable;
extern oop gc_allocate(oop vtable, size_t size);
extern void gc_collect(void);

static oop make_symbol(const char *text, size_t len) {
    oop s = gc_allocate(SymbolVtable, sizeof(struct Symbol) + len + 1);
    assert(s != NULL);
    ((struct Symbol *)s)->length = len;
    memcpy(((struct Symbol *)s)->name, text, len);
    ((struct Symbol *)s)->name[len] = 0;
    return s;
}

int main(void) {
    /* the VM must be booted far enough that SymbolVtable exists */
    vm_boot();
    assert(SymbolVtable != NULL);

    /* ---- property 1: Symbols cost the C heap nothing ---- */
    struct OevmCHeapStats a = oevm_cheap_stats();
    for (int i = 0; i < 200; i++) {
        char nm[24];
        int n = snprintf(nm, sizeof nm, "leakCanary%d:", i);
        oop s = make_symbol(nm, (size_t)n);
        /* the bytes live INSIDE the object, not at the far end of a pointer */
        const char *got = symbol_name((struct Symbol *)s);
        assert(got == (const char *)s + offsetof(struct Symbol, name));
        assert(strcmp(got, nm) == 0);
        assert(((struct Symbol *)s)->length == (size_t)n);
    }
    struct OevmCHeapStats b = oevm_cheap_stats();
    assert(b.allocs == a.allocs);          /* not one malloc for 200 Symbols */
    assert(b.out_bytes == a.out_bytes);
    assert(b.fails == a.fails);
    printf("test_symbol_inline: property 1 (200 Symbols cost the C heap 0 bytes) OK\n");

    /* ---- property 2: the GC leaf case ---- */
    /* name bytes that spell a plausible heap pointer. If trace_one lacks its
       SymbolVtable arm, the generic fallback marks this text as an oop. */
    oop victim = make_symbol("xxxxxxxxxxxxxxxx", 16);
    char ptrtext[sizeof(void *) + 1];
    memcpy(ptrtext, &victim, sizeof(void *));
    ptrtext[sizeof(void *)] = 0;
    oop trap = make_symbol(ptrtext, sizeof(void *));

    gc_collect();

    /* both must still be intact and readable after the collection */
    assert(((struct Symbol *)victim)->length == 16);
    assert(strcmp(symbol_name((struct Symbol *)victim), "xxxxxxxxxxxxxxxx") == 0);
    assert(((struct Symbol *)trap)->length == sizeof(void *));
    assert(memcmp(symbol_name((struct Symbol *)trap), &victim, sizeof(void *)) == 0);
    printf("test_symbol_inline: property 2 (inline bytes are a GC leaf) OK\n");

    printf("test_symbol_inline: OK\n");
    return 0;
}
```

Two things the implementer must resolve against the real tree rather than trusting this snippet:

- **`vm_boot()` is a placeholder for whatever actually initialises `SymbolVtable`.** Read the top of
  `tests/test_heap_stats.c` and `tests/test_cheap_stats.c` and copy whatever boot call they use (or
  none, if the Model-A link set initialises statically via `init_solidified_vtables`). If nothing
  boots it, call `init_solidified_vtables()` directly.
- **`gc_collect()` must actually see `victim` and `trap` as reachable roots**, or property 2 tests
  nothing — a collected object proves no leaf case. Locals in `main` are found only if the GC scans
  the C stack. `gc.c` scans the *VM* stack (`scan_vm_stack`), not the C stack, so these two locals
  are probably **not** roots. If so, the assertion must change: instead of asserting survival,
  assert that the collection **completes without crashing or corrupting the heap** — call
  `gc_collect()` and then `oevm_heap_stats()` and check `live_bytes <= heap_size`, plus allocate and
  read back a fresh Symbol afterwards. Getting this right is the whole value of the test; a test
  that passes because its objects were unreachable is a false green.

- [ ] **Step 2: Run it against the pre-fix VM to watch it fail**

If Task 3 is already committed, check out the parent commit's VM sources into a scratch tree and
build the test there. Expected on the OLD code: property 1 fails on `b.allocs == a.allocs` — the
old `make_symbol` shape does not even compile, which is itself the demonstration. Simplest honest
version: build the test at `HEAD~1` and record the compile error as the "fails before" evidence,
then note that in the test's header comment.

- [ ] **Step 3: Run it against the fixed VM**

```bash
cd /home/david/workspace/rete/nodus/tuvm
gcc -std=c99 -Isrc -DOEVM_HEAP_SIZE=65536 tests/test_symbol_inline.c \
    src/vm.c src/gc.c src/dispatch.c src/platform.c src/serial.c src/udp.c \
    src/mirror.c src/backend/posix/platform_posix.c \
    src/backend/posix/serial_posix.c src/backend/posix/udp_posix.c \
    src/oimg_loader.c src/solidified_image.c -lm -o /tmp/tsi && /tmp/tsi
```

Expected: three `OK` lines. If the link set is short a file, copy `ENG_A` out of
`tools/run_c_tests.sh` verbatim rather than guessing.

- [ ] **Step 4: Wire it into the gate**

In `tools/run_c_tests.sh`, beside the stage-1 line:

```bash
# rete#190 stage 2: Symbol name bytes inline — no C-heap cost, and a GC leaf.
run test_symbol_inline "$ENG_A" -DOEVM_HEAP_SIZE=65536
```

- [ ] **Step 5: Run the whole C gate and commit**

```bash
cd /home/david/workspace/rete/nodus/tuvm && bash tools/run_c_tests.sh 2>&1 | tail -40
cd /home/david/workspace/rete
git add nodus/tuvm/tests/test_symbol_inline.c nodus/tuvm/tools/run_c_tests.sh
git commit -m "test(#190): 200 Symbols must cost the C heap zero bytes, and inline bytes must be a GC leaf

Three green gates prove nothing broke; neither proves the leak is gone. The
seam's own counters are the witness: allocs not moving across 200 Symbol
builds is a stronger claim than out_bytes coming back down.

Property 2 guards the landmine — a Symbol whose name text spells a heap
address, put through a collection. Without trace_one's SymbolVtable arm the
generic word-count fallback reads that text as an oop."
```

---

## Task 5: Measure the fix on the host before spending a board on it

The gauge is the point of stage 1. Use it on the host first: a cheap, repeatable reading that says
whether the per-check-in slope is actually gone, before committing 20 minutes of C6 time and then
ten hours of bell time.

**Files:**
- None modified. This task produces a measurement.

**Interfaces:**
- Consumes: `tests/porta_sync_host.sh`, `sys/cheap` (`allocs`, `frees`).
- Produces: a host-side blocks-per-check-in slope, before and after. Task 6 quotes it.

- [ ] **Step 1: Establish the pre-fix host slope**

Check out the branch point (`git stash` any work first), run the porta sync host gate, and read
`sys/cheap` off the resulting space across as many check-ins as the harness runs. Compute
**outstanding blocks = `allocs` − `frees`** at the first and last check-in, and divide by the tick
delta.

Expected shape, from the board: roughly **10 blocks per check-in**. The host number will differ in
magnitude — a host run decodes a different volume of wire Symbols — but it must be **clearly
positive and linear in check-ins**, or the host is not reproducing the leak and this task should
report that and stop rather than manufacture a number.

- [ ] **Step 2: Measure the post-fix host slope**

Same procedure on the branch tip.

Expected: the slope collapses to approximately **0 blocks per check-in**. Steady-state churn
(allocs and frees both rising, outstanding flat) is the pass; a residual positive slope means
something else also leaks and is a finding worth its own issue, not a reason to fail the task.

- [ ] **Step 3: Record both numbers**

Report them as `blocks/check-in, before → after`, with the tick range each was measured over. **Do
not report a two-point read without saying which two ticks it came from** — a two-point read of a
sawtoothing counter is exactly the mistake §11.6 records.

---

## Task 6: Fold the corrections back into the design, then gate on hardware

The four §4 corrections and the measured effect both belong in mainline documents before the rung
closes — a rung is not closed until its durable knowledge leaves `docs/superpowers/`.

**Files:**
- Modify: `docs/design/2026-08-10-node-memory-and-liveness-design.md` (§4, and a §11.7 addendum)
- Modify: `review/tests/CLAIMS.md` — **only if** the ladder already has a claim row for this leak;
  `review/` is never restructured, only appended to where it already invites it.

**Interfaces:**
- Consumes: Tasks 1-5.
- Produces: a design doc that agrees with the code.

- [ ] **Step 1: Correct §4 in place**

Replace the "Two static Symbols cannot hold a flexible array member the ordinary way" paragraph with
the real count and the real mechanism (nine hand-written, ~265 from `solidify.py:132`), and add the
`format_version` bump and the lifetime rule as consequences 3 and 4. The design is a decision
record, and a decision record that miscounts the work by two orders of magnitude is the kind of
half-claim a later session acts on.

- [ ] **Step 2: Add §11.7 — what stage 2 measured**

Record, in check-ins rather than minutes: the pre-fix slope (~10 blocks/check-in, cliff at tick
610-614), the host before/after from Task 5, and — once Step 4 lands — the board's outstanding-block
count at the same fixed ticks the leaky run was read at.

- [ ] **Step 3: Commit the documents**

```bash
cd /home/david/workspace/rete
git add docs/design/2026-08-10-node-memory-and-liveness-design.md
git commit -m "docs(#190): §4 said two static Symbols; there are nine hand-written and ~265 generated"
```

- [ ] **Step 4: The c6spare hardware gate — compare at a FIXED TICK**

Flash the fixed build to `c6spare` (resolve the board through the collector env files, never a
ttyUSB number — there are three C6 devkits on the bench and only the by-id hash tells them apart)
and run it at the **2 s cadence**, the same one the leaky run used.

The comparison is against these stage-1 readings, and it is made at a **fixed check-in count**, not
a fixed wall time — the failure is driven by check-in count, so a wall-clock comparison confounds it
with the cadence:

| reading | leaky build | fixed build must show |
|---|---|---|
| outstanding blocks (`allocs` − `frees`) at tick 589 | 5 878 (1 s cadence) | far below, and **not rising with tick** |
| outstanding blocks at tick 614 | 6 094 (2 s cadence) | approximately the same as its own tick-300 reading |
| alive at tick 614 | **no** — died 610/613/614, three runs | **yes** |
| `fails` | 0 throughout | 0 throughout |

**Pass = the node is still checking in past tick 614 with outstanding blocks flat.** The cliff was
never reached by a build that did not leak, so surviving it is the whole claim.

Two traps this gate has already sprung once, both recorded in §11:

- **A node that stops checking in is not a reading.** A flat line from a wedged node is
  indistinguishable from a flat line from a fixed one. Confirm the node is *alive and ticking* at
  every sample, not merely that the number stopped moving.
- **A green goes stale at the next edit, even a comment-only one.** Flash the exact commit you
  intend to report, and record its hash.

- [ ] **Step 5: The bell long run**

Only after Step 4 passes. `c6-bell` has a *measured* 10 h 41 m lifetime on the old build, which is
what makes its first post-fix run a direct comparison rather than an absence of evidence. Restore
it to service on the fixed build and let it run past 10 h 41 m with `outBytes` flat and `fails: 0`.

`port=down` is healthy for the bell — do not read it as a failure.

- [ ] **Step 6: Report, and close the rung**

Quote the effect **in check-ins**. Promote the result to the ladder
(`~/workspace/rete/review/program-sequence.md`) and note on rete#190 which mainline document
absorbed it. rete#192 (`CHeapStats fromList:` hangs a C6) stays open — this plan does not touch it,
and the class it keeps as a reproducer must survive.

---

## Self-review

**Spec coverage** — design §4 has five claims; each maps to a task:

| §4 claim | Task |
|---|---|
| `struct Symbol` gains inline NUL-terminated bytes | 3 (step 1) |
| `symbol_name()` collapses; the `oimg.h:54` blob branch disappears | 3 (step 2) |
| `gc.c` needs a `SymbolVtable` leaf case in `trace_one` | 3 (step 3), guarded by 4 (property 2) |
| Static Symbols need the GCC FAM extension | 1 (proves it), 3 (steps 5-6) |
| Four dynamic sites → `gc_allocate` + `memcpy`; two that malloc | 3 (step 4) — **six sites, not four; the survey found `vm.c:122/719/1620`, `dispatch.c:459/1688`, `platform.c:1233`, of which two malloc** |
| `oimg_serialize.py` must write inline bytes; serializer and VM in one commit | 3 (steps 8-9, 14) |
| §7 step 2's prediction (`outBytes` flat, `fails` 0, past 10 h 41 m) | 6 (steps 4-5) |

Gaps deliberately left: §4's fallback shape (*"a layout-compatible sized-storage struct behind a
macro"*) is **not** built — §4 says to record it and not build it, and Task 1 escalates to David if
the extension fails rather than silently building the fallback.

**Type consistency** — `struct Symbol { struct Object base; size_t length; char name[]; }` is used
identically in Tasks 3 and 4. `oevm_cheap_stats()` returns `struct OevmCHeapStats` with fields
`out_bytes / out_high / allocs / frees / fails` (C side), surfaced on the wire as `CHeap`'s
`outBytes / outHigh / allocs / frees / fails` — the two spellings are deliberate and both are used
in the right place. "Outstanding blocks" is `allocs − frees` everywhere, and is never referred to as
a field.

**Known soft spots, flagged rather than papered over** — plan-embedded code is unrun code, and both
#165 and #190 stage 1 carried roughly one defect per task in the plan's own text:

- Task 4's `vm_boot()` is a placeholder; the step says so and tells the implementer where to find
  the real one.
- Task 4's property 2 may test nothing if `gc_collect()` does not treat `main`'s locals as roots;
  the step names that failure mode and gives the alternative assertion.
- Task 3 step 9 does not give an exact line for the serializer's `format_version` write, because the
  writer's `version=1` argument is a different number; the step says to find `header_bytes`.
- Task 5's host slope may not reproduce the leak at all; the step says to report that and stop
  rather than manufacture a number.
