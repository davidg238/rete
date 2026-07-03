# Global Method Lookup Cache Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a RAM-resident global method lookup cache so non-primitive sends skip the `lookup_defining_vtable` chain walk on cache hits.

**Architecture:** A direct-mapped, fixed-size, file-local table in `src/dispatch.c` keyed by `(receiver_vtable, selector) → (method, defining_vtable)`. `send()` consults it via a `lookup_method_cached` wrapper before falling back to `lookup_defining_vtable`. `primitive_vtable_bind` flushes the whole table on any binding mutation. The cache is pure RAM (never the read-only image blob), so it is safe under both Model A and Model B with no bytecode/compiler/ABI changes.

**Tech Stack:** C99, gcc. Conformance harness `ns-oevm/tools/run_tests_oevm.py`; C unit tests are standalone `main()` programs linked against the VM sources; perf via `oevm/bench.sh`.

## Global Constraints

- **Conformance must hold 36/5/1** on both paths: `cd ../ns-oevm && python3 tools/run_tests_oevm.py` (default `OEVM_PATH=AB`).
- **Image e2e must print 42:** `bash tests/test_load_run.sh`.
- **32-bit must print 42:** `bash tests/test_portability_32.sh`.
- **No writes to the image blob.** The cache lives only in process RAM. No changes to bytecode format, the compiler (`ns_compile_oevm.py`), the ABI, or the `.oimg` format.
- **Selectors are interned Symbols**; `lookup_defining_vtable` already keys on pointer identity (`==`). The cache uses the same pointer-identity key.
- **C unit test link pattern** (post-OIMG; `oimg_loader.c` defines the blob globals that `oimg.h` inlines reference):
  `gcc -std=c99 -Isrc -DOEVM_CACHE_STATS <test>.c src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c -lm -o /tmp/<bin>`
- All commits use the repo's footer: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`
- Work happens on branch `feat/global-method-cache` (already created off master `67f7ae3`).

---

### Task 1: Cache data structure, lookup wrapper, flush, and stats

The cache and its API, fully unit-tested but **dormant** — nothing calls `lookup_method_cached` yet except the test, and `method_cache_flush` is called only by the test. Conformance is therefore unaffected by construction.

**Files:**
- Modify: `src/oevm.h` (add struct + declarations after line 260, the `lookup_defining_vtable` decl)
- Modify: `src/dispatch.c` (add cache near the top of the file, after the includes / `IntegerVtable` definition)
- Test: `tests/test_method_cache.c` (create)

**Interfaces:**
- Produces:
  - `struct MethodCacheStats { unsigned long hits, misses, fills, collisions, flushes; };`
  - `oop lookup_method_cached(oop receiver_vtable, oop selector, oop *out_method, oop *out_defining_vt);` — returns `defining_vt` (also via `*out_defining_vt`) and `*out_method`; returns `NULL` (and leaves `*out_method == NULL`) when the selector is not understood. Caller must pass non-NULL `receiver_vtable`.
  - `void method_cache_flush(void);` — clears every slot.
  - `struct MethodCacheStats method_cache_stats(void);` — defined only under `-DOEVM_CACHE_STATS`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_method_cache.c`:

```c
/* Unit test for the global method lookup cache (src/dispatch.c).
   Build (must define OEVM_CACHE_STATS so the stats accessor exists):
     gcc -std=c99 -Isrc -DOEVM_CACHE_STATS tests/test_method_cache.c \
         src/vm.c src/gc.c src/dispatch.c src/solidified_image.c -lm -o /tmp/tmc && /tmp/tmc
   Selectors/methods are matched by pointer identity, so we use distinct
   heap pointers as fake symbols/methods — no Symbol layout needed. */
#include "../src/oevm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

extern oop VtableVtable;

/* Build a RAM vtable (not in the image blob) with the given bindings. */
static struct Vtable *mk_vtable(struct Binding *bindings, size_t n) {
    struct Vtable *vt = calloc(1, sizeof(struct Vtable));
    vt->base.vtable = VtableVtable;
    vt->delegate = NULL;
    vt->bindings = bindings;
    vt->capacity = n;
    vt->size = n;
    vt->is_nested = 0;
    return vt;
}

int main(void) {
    /* Two distinct "selectors" and "methods" as opaque pointers. */
    int selA, selB, methA, methB;
    struct Binding bindings[2] = {
        { .symbol = (oop)&selA, .method = (oop)&methA },
        { .symbol = (oop)&selB, .method = (oop)&methB },
    };
    struct Vtable *vt = mk_vtable(bindings, 2);

    method_cache_flush();
    struct MethodCacheStats s0 = method_cache_stats();

    /* First lookup of selA: MISS then FILL. */
    oop m = NULL, dv = NULL;
    oop r = lookup_method_cached((oop)vt, (oop)&selA, &m, &dv);
    assert(r == (oop)vt);
    assert(m == (oop)&methA);
    assert(dv == (oop)vt);
    struct MethodCacheStats s1 = method_cache_stats();
    assert(s1.misses == s0.misses + 1);
    assert(s1.fills  == s0.fills + 1);

    /* Second lookup of selA: HIT. */
    m = NULL; dv = NULL;
    r = lookup_method_cached((oop)vt, (oop)&selA, &m, &dv);
    assert(r == (oop)vt && m == (oop)&methA);
    struct MethodCacheStats s2 = method_cache_stats();
    assert(s2.hits == s1.hits + 1);

    /* Unknown selector: MISS, returns NULL, NOT cached (no fill). */
    int selUnknown;
    m = (oop)1; dv = (oop)1;
    r = lookup_method_cached((oop)vt, (oop)&selUnknown, &m, &dv);
    assert(r == NULL && m == NULL);
    struct MethodCacheStats s3 = method_cache_stats();
    assert(s3.fills == s2.fills);   /* misses are never cached */

    /* Flush clears: a subsequent lookup of selA must MISS again. */
    method_cache_flush();
    struct MethodCacheStats s4 = method_cache_stats();
    assert(s4.flushes == s3.flushes + 1);
    m = NULL; dv = NULL;
    r = lookup_method_cached((oop)vt, (oop)&selA, &m, &dv);
    assert(r == (oop)vt && m == (oop)&methA);
    struct MethodCacheStats s5 = method_cache_stats();
    assert(s5.misses == s4.misses + 1);   /* was flushed, so miss not hit */

    printf("test_method_cache: OK\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -std=c99 -Isrc -DOEVM_CACHE_STATS tests/test_method_cache.c src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c -lm -o /tmp/tmc`
Expected: FAIL to link — `undefined reference to 'lookup_method_cached'`, `'method_cache_flush'`, `'method_cache_stats'`.

- [ ] **Step 3: Add declarations to `src/oevm.h`**

After line 260 (`oop lookup_defining_vtable(...)`), add:

```c
/* Global method lookup cache (src/dispatch.c). RAM-resident, direct-mapped. */
struct MethodCacheStats { unsigned long hits, misses, fills, collisions, flushes; };
oop lookup_method_cached(oop receiver_vtable, oop selector, oop *out_method, oop *out_defining_vt);
void method_cache_flush(void);
#ifdef OEVM_CACHE_STATS
struct MethodCacheStats method_cache_stats(void);
#endif
```

- [ ] **Step 4: Implement the cache in `src/dispatch.c`**

Add immediately after the `#include` block / `IntegerVtable` definition near the top of `src/dispatch.c` (before `lookup_defining_vtable` at line 62 so the wrapper can call it — place the table + helpers here, and the wrapper after `lookup_defining_vtable`):

```c
/* ---- Global method lookup cache -------------------------------------------
   Direct-mapped, RAM-resident. Key: (receiver_vtable, selector) pointer
   identity. An empty slot has vtable == NULL. Never touches the image blob. */
#define METHOD_CACHE_SIZE 4096   /* power of two */
struct MethodCacheEntry { oop vtable; oop selector; oop method; oop defining_vt; };
static struct MethodCacheEntry method_cache[METHOD_CACHE_SIZE];

#ifdef OEVM_CACHE_STATS
static struct MethodCacheStats method_cache_stats_counters;
struct MethodCacheStats method_cache_stats(void) { return method_cache_stats_counters; }
#define CACHE_STAT_INC(field) (method_cache_stats_counters.field++)
#else
#define CACHE_STAT_INC(field) ((void)0)
#endif

static inline size_t method_cache_hash(oop vtable, oop selector) {
    return (size_t)((((uintptr_t)vtable >> 4) ^ ((uintptr_t)selector >> 3))
                    & (METHOD_CACHE_SIZE - 1));
}

void method_cache_flush(void) {
    memset(method_cache, 0, sizeof(method_cache));
    CACHE_STAT_INC(flushes);
}
```

Then, immediately **after** the closing brace of `lookup_defining_vtable` (after line 78), add the wrapper:

```c
/* Cached front-end to lookup_defining_vtable. Caller guarantees
   receiver_vtable != NULL. Misses (does-not-understand) are never cached. */
oop lookup_method_cached(oop receiver_vtable, oop selector,
                         oop *out_method, oop *out_defining_vt) {
    size_t h = method_cache_hash(receiver_vtable, selector);
    struct MethodCacheEntry *e = &method_cache[h];
    if (e->vtable == receiver_vtable && e->selector == selector) {
        CACHE_STAT_INC(hits);
        *out_method = e->method;
        *out_defining_vt = e->defining_vt;
        return e->defining_vt;
    }
    CACHE_STAT_INC(misses);
    oop method = NULL;
    oop defining_vt = lookup_defining_vtable(receiver_vtable, selector, &method);
    if (defining_vt != NULL && method != NULL) {
#ifdef OEVM_CACHE_STATS
        if (e->vtable != NULL &&
            (e->vtable != receiver_vtable || e->selector != selector)) {
            CACHE_STAT_INC(collisions);
        }
#endif
        e->vtable = receiver_vtable;
        e->selector = selector;
        e->method = method;
        e->defining_vt = defining_vt;
        CACHE_STAT_INC(fills);
    }
    *out_method = method;
    *out_defining_vt = defining_vt;
    return defining_vt;
}
```

Ensure `<string.h>` (for `memset`) and `<stdint.h>` (for `uintptr_t`) are included in `dispatch.c`; add them to the include block if absent.

- [ ] **Step 5: Run test to verify it passes**

Run: `gcc -std=c99 -Isrc -DOEVM_CACHE_STATS tests/test_method_cache.c src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c -lm -o /tmp/tmc && /tmp/tmc`
Expected: `test_method_cache: OK`

- [ ] **Step 6: Verify the dormant cache did not disturb conformance**

Run: `cd ../ns-oevm && python3 tools/run_tests_oevm.py`
Expected: `36` pass / `5` fail / `1` skip (unchanged — nothing calls the wrapper yet).

- [ ] **Step 7: Commit**

```bash
git add src/oevm.h src/dispatch.c tests/test_method_cache.c
git commit -m "feat(perf): global method cache structure + lookup wrapper (dormant)

Direct-mapped RAM (vtable,selector)->method cache with flush and gated
stats counters; unit-tested. Not yet consulted by send().

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Wire flush into `bind:to:` (invalidation)

Make `primitive_vtable_bind` flush the cache so a replaced method can never be served stale. Still dormant w.r.t. `send()`, so this lands the invalidation **before** the cache goes live in Task 3.

**Files:**
- Modify: `src/vm.c:925` (`primitive_vtable_bind`, add flush at top of body)
- Test: `tests/test_method_cache.c` (extend with an invalidation case)

**Interfaces:**
- Consumes: `method_cache_flush()`, `lookup_method_cached(...)` from Task 1.

- [ ] **Step 1: Add the failing invalidation test**

Append to `main()` in `tests/test_method_cache.c`, just before the final `printf`:

```c
    /* bind:to: must flush so a replaced method is never served stale. */
    extern oop primitive_vtable_bind(oop receiver, int argc, oop *args);
    method_cache_flush();
    /* Fill the cache for selA -> methA. */
    m = NULL; dv = NULL;
    lookup_method_cached((oop)vt, (oop)&selA, &m, &dv);
    assert(m == (oop)&methA);
    struct MethodCacheStats sb0 = method_cache_stats();
    /* Replace selA's method via the real bind path. */
    int methA2;
    oop bind_args[2] = { (oop)&selA, (oop)&methA2 };
    primitive_vtable_bind((oop)vt, 2, bind_args);
    struct MethodCacheStats sb1 = method_cache_stats();
    assert(sb1.flushes == sb0.flushes + 1);   /* bind flushed the cache */
    /* Next lookup must return the NEW method, not the cached old one. */
    m = NULL; dv = NULL;
    lookup_method_cached((oop)vt, (oop)&selA, &m, &dv);
    assert(m == (oop)&methA2);
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -std=c99 -Isrc -DOEVM_CACHE_STATS tests/test_method_cache.c src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c -lm -o /tmp/tmc && /tmp/tmc`
Expected: FAIL — assertion `sb1.flushes == sb0.flushes + 1` (bind does not yet flush), or stale method returned.

- [ ] **Step 3: Wire the flush into `primitive_vtable_bind`**

In `src/vm.c`, inside `primitive_vtable_bind` (line 925), after the argument/receiver validation and the `is_in_blob` guard, immediately before reading `vtable_bindings`, add:

```c
    /* Any binding mutation can stale a cached (vtable,selector)->method entry. */
    method_cache_flush();
```

(Place it after the `is_in_blob` abort check at line 941 and before line 943's `struct Binding *bs = vtable_bindings(...)`.)

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -std=c99 -Isrc -DOEVM_CACHE_STATS tests/test_method_cache.c src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c -lm -o /tmp/tmc && /tmp/tmc`
Expected: `test_method_cache: OK`

- [ ] **Step 5: Commit**

```bash
git add src/vm.c tests/test_method_cache.c
git commit -m "feat(perf): flush method cache on bind:to: + invalidation test

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Make the cache live — consult it from `send()`

Route the real dispatch path through the cache and prove every gate still holds.

**Files:**
- Modify: `src/dispatch.c:746-747` (inside `send()`)

**Interfaces:**
- Consumes: `lookup_method_cached(...)` from Task 1.

- [ ] **Step 1: Replace the lookup call in `send()`**

In `src/dispatch.c`, `send()` currently has at lines 746-747:

```c
    oop method = NULL;
    oop defining_vt = lookup_defining_vtable(receiver_vtable, selector, &method);
```

Replace those two lines with:

```c
    oop method = NULL;
    oop defining_vt = NULL;
    lookup_method_cached(receiver_vtable, selector, &method, &defining_vt);
```

Leave the immediately following `if (defining_vt == NULL || method == NULL) { return does_not_understand(...); }` and the access-control block (lines 748-758) exactly as they are — they consume `method` and `defining_vt` identically. Do **not** touch the dormant `send_ic` at line 800.

- [ ] **Step 2: Build and run the full conformance suite (both paths)**

Run: `cd ../ns-oevm && python3 tools/run_tests_oevm.py`
Expected: `36` pass / `5` fail / `1` skip — identical to baseline. PASS requires Path A and Path B to agree on every test.

- [ ] **Step 3: Run the image e2e and 32-bit gates**

Run: `bash tests/test_load_run.sh`
Expected: prints `42`.

Run: `bash tests/test_portability_32.sh`
Expected: prints `42` and `OK` (oop_size==4).

- [ ] **Step 4: Re-run the cache unit test (regression)**

Run: `gcc -std=c99 -Isrc -DOEVM_CACHE_STATS tests/test_method_cache.c src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c -lm -o /tmp/tmc && /tmp/tmc`
Expected: `test_method_cache: OK`

- [ ] **Step 5: Commit**

```bash
git add src/dispatch.c
git commit -m "feat(perf): consult global method cache in send()

send() now resolves via lookup_method_cached (cache hit skips the
vtable chain walk); access check + DNU path unchanged. Conformance
36/5/1 on both paths, image e2e 42, 32-bit 42.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: Stats reporting + perf/flush measurement

Deliver the numbers the design calls for: a shutdown stats dump (gated) and before/after perf plus flush-frequency and collision-rate measurement on real workloads.

**Files:**
- Modify: `src/dispatch.c` (add a gated `method_cache_report()` and register it with `atexit`) — **oevm repo**
- Modify: `../ns-tests/00_perf/RESULTS.md` (record measurements) — **separate `ns-tests` sibling repo; commit there independently**

**Interfaces:**
- Consumes: `method_cache_stats()` (under `OEVM_CACHE_STATS`).

- [ ] **Step 1: Add a gated shutdown report in `src/dispatch.c`**

Add, under the existing `#ifdef OEVM_CACHE_STATS` region near the cache definition:

```c
#ifdef OEVM_CACHE_STATS
static void method_cache_report(void) {
    struct MethodCacheStats s = method_cache_stats_counters;
    unsigned long total = s.hits + s.misses;
    fprintf(stderr,
        "[method-cache] hits=%lu misses=%lu fills=%lu collisions=%lu flushes=%lu "
        "hit_rate=%.1f%%\n",
        s.hits, s.misses, s.fills, s.collisions, s.flushes,
        total ? (100.0 * (double)s.hits / (double)total) : 0.0);
}
/* Register once on first flush (called at startup before main work). */
static void method_cache_register_report(void) {
    static int registered = 0;
    if (!registered) { registered = 1; atexit(method_cache_report); }
}
#endif
```

In `method_cache_flush()`, under `OEVM_CACHE_STATS`, call `method_cache_register_report();` so the report is guaranteed registered (flush runs at least once during boot/bind). `<stdlib.h>` (for `atexit`) must be included in `dispatch.c`; add it if absent.

- [ ] **Step 2: Verify report appears and is harmless**

Run: `gcc -std=c99 -Isrc -DOEVM_CACHE_STATS tests/test_method_cache.c src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c -lm -o /tmp/tmc && /tmp/tmc`
Expected: `test_method_cache: OK` followed by a `[method-cache] hits=... flushes=...` line on stderr.

Confirm the production build (no flag) still builds clean:
Run: `gcc -Wall -std=c99 src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/kernel_boot.c oevm_image_main.c -o /tmp/oevm_img -lm`
Expected: builds with no warnings; no `[method-cache]` output at runtime.

- [ ] **Step 3: Measure on real workloads**

Build a stats-enabled image VM and run the send-heavy benchmarks, capturing the stats line:

Run: `cd ../ns-oevm && python3 tools/run_tests_oevm.py` once to confirm 36/5/1 still holds, then for each of `richards`, `deltablue` (if it runs), and `fib`, run via `oevm/bench.sh <test.ns>` to get timings, and run a `-DOEVM_CACHE_STATS` build of the same program to capture `hits/misses/collisions/flushes`.

Expected: record `hit_rate`, `flushes` (how often the table clears — the user's question), and `collisions` (single-table eviction interference — the open concern). High `collisions` would justify revisiting table size / associativity.

- [ ] **Step 4: Record results in `ns-tests/00_perf/RESULTS.md`**

Append a dated section with: before/after wall-clock for `fib`, `richards`, `deltablue`; the cache `hit_rate`, `flushes`, and `collisions` per workload; and a one-line verdict on whether the single-table interference concern is real (per `collisions`).

- [ ] **Step 5: Commit (two repos — they are separate git repositories)**

In the **oevm** repo (the report code):

```bash
git add src/dispatch.c
git commit -m "perf(measure): gated method-cache stats report at shutdown

hits/misses/fills/collisions/flushes + hit_rate via -DOEVM_CACHE_STATS.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

In the **ns-tests** repo (the recorded numbers):

```bash
cd ../ns-tests && git add 00_perf/RESULTS.md
git commit -m "perf(measure): record method-cache before/after + flush/collision rates

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Notes for the implementer

- The cache wrapper sits **after** the primitive/control-flow fast paths in `send()` (the primitive gauntlet and `ifTrue:`/`whileTrue:` special cases at lines ~240-738), so primitives never reach it — they keep their own `selector_prim_id` cache. Only `lookup_defining_vtable` is cached.
- An empty slot is `vtable == NULL`; real entries always have a non-NULL vtable, so the sentinel never aliases a live entry.
- SMI receivers resolve to `IntegerVtable` (a stable pointer) before the cache is consulted; `NULL` receiver_vtable is handled by the `does_not_understand` branch *before* the cache call, so `lookup_method_cached` is only ever called with a non-NULL vtable.
- Do not modify `ns_compile_oevm.py`, the `.oimg` format, or `send_ic` — this lever is entirely VM-internal.
