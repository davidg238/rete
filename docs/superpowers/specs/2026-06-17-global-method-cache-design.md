# Global Method Lookup Cache — Design

**Date:** 2026-06-17
**Status:** Approved (design)
**Topic:** Perf lever #1 of 2 (inline caching). Sibling lever (unboxed floats) is a separate spec.

## Problem

Under Model B (the default build since the OIMG work merged), the VM no longer benefits
from gcc optimizing a static image, so the steady-state ~1.9× geomean gap to PrimordialSoup
is fully felt. Message dispatch is one of the two named perf levers.

Every non-primitive send resolves its method via `lookup_defining_vtable` (`src/dispatch.c:62`):
a linear scan of each vtable's bindings, walking the delegate chain — `O(chain depth × vtable size)`
per send. There is no method cache today.

### Why the existing `OP_SEND_IC` machinery is not the answer

The VM already defines `struct InlineCache` (`oevm.h:139`), `send_ic` (`dispatch.c:771`), and an
`OP_SEND_IC` handler (`vm.c:256`). That design is a **per-call-site inline cache embedded in the
instruction stream**, and `send_ic` *writes* the resolved vtable/method back into that slot
(`dispatch.c:814–815`).

Under Model B the instruction stream lives in a `PROT_READ` mmap'd blob (`oimg_loader.c:76`).
Emitting `OP_SEND_IC` would make `send_ic` write into read-only memory → SIGSEGV. So the dormant
`OP_SEND_IC` path cannot be wired up as-is for the default build. (The compiler also never emits it.)

## Decision

Implement a **global method lookup cache**: a RAM-resident, direct-mapped hash table keyed by
`(receiver_vtable, selector) → (method, defining_vtable)`, consulted by `send()` before falling
back to `lookup_defining_vtable`. This is the classic Smalltalk-80 global method cache.

It lives entirely in RAM, independent of the read-only image, so it is safe under both Model A and
Model B with **no bytecode, compiler, ABI, or image-format changes**.

Per-call-site inline caching (an `OP_SEND_IC` RAM side-table) is explicitly **out of scope** here —
"A now, B later." It is to be revisited only if profiling after this change shows call-site
monomorphism leaving measurable wins on the table.

## Design

### Data structure (file-local static in `dispatch.c`)

```c
#define METHOD_CACHE_SIZE 4096   /* power of two */
struct MethodCacheEntry {
    oop vtable;       /* receiver vtable used as key (NULL = empty slot) */
    oop selector;     /* interned Symbol, pointer-identity key */
    oop method;       /* resolved method closure */
    oop defining_vt;  /* vtable that defined `method` (for access check) */
};
static struct MethodCacheEntry method_cache[METHOD_CACHE_SIZE];
```

- Direct-mapped: one slot per hash, overwrite on collision. Simplest correct design; no probing.
- Zero-initialized at startup (BSS). A slot with `vtable == NULL` is empty.
- Key is pointer identity on `(receiver_vtable, selector)`. Selectors are interned Symbols —
  `lookup_defining_vtable` already compares them with `==` (`dispatch.c:68`), so identity is valid.

### Lookup wrapper

New function `lookup_method_cached(oop receiver_vtable, oop selector, oop *out_method, oop *out_defining_vt)`:

1. `hash = (((uintptr_t)receiver_vtable >> 4) ^ ((uintptr_t)selector >> 3)) & (METHOD_CACHE_SIZE - 1)`
2. If `entry.vtable == receiver_vtable && entry.selector == selector`:
   return cached `method` + `defining_vt` (hit — no chain walk).
3. Else (miss): call `lookup_defining_vtable(receiver_vtable, selector, &method)` to get
   `defining_vt` + `method`. **Only if both are non-NULL**, fill the slot
   (`vtable`, `selector`, `method`, `defining_vt`). Return the result either way.

Misses / does-not-understand results are **never cached**, so installing a new selector later simply
produces a fresh miss → correct without negative-entry invalidation.

### Integration point

In `send()` (`src/dispatch.c`), replace the direct call at line 747:

```c
oop method = NULL;
oop defining_vt = lookup_defining_vtable(receiver_vtable, selector, &method);
```

with `lookup_method_cached(receiver_vtable, selector, &method, &defining_vt)`.

The per-send access-control check (`dispatch.c:755–757`) stays unchanged — it is receiver-dependent
and cheap, and the cached `defining_vt` feeds it directly.

The dormant `send_ic` path (`dispatch.c:771`, unemitted) is left untouched.

### Invalidation

`primitive_vtable_bind` (`vm.c:925`) is the **only** runtime site that mutates vtable bindings — it
either replaces an existing method (`vm.c:946`) or appends a new `(selector, method)` binding
(`vm.c:955–957`). All Newspeak runtime class/method installation flows through it.

Add `method_cache_flush()` (a `memset` of the table to zero), called at the top of
`primitive_vtable_bind`. This covers the only stale case — a *replaced* method whose old pointer was
cached. `bind:to:` is rare relative to sends, so flush cost is negligible.

A generation-counter flush (bump a global; entries carry the generation they were filled at) is a
possible later refinement if flush frequency ever matters. Full memset is the starting implementation
because it is obviously correct.

### Why correct under Model B

- No writes to the blob — the cache is separate RAM.
- No bytecode, compiler, ABI, or image-format changes.
- Therefore both test paths (A and B) and 32-bit builds are unaffected by construction; the cache is
  pure dispatch-internal acceleration with identical observable semantics.

### Instrumentation (measurement)

Counters maintained in `dispatch.c`, gated behind a compile flag (e.g. `-DOEVM_CACHE_STATS`,
mirroring the existing `OEVM_TRACE` pattern) so they cost nothing in production builds:

- `hits` — entry matched on lookup.
- `misses` — fell through to `lookup_defining_vtable`.
- `fills` — slot written after a successful miss.
- `collisions` — a fill that overwrote a *different, non-empty* `(vtable, selector)` slot.
  This is the direct signal for the single-table concern below: a high collision rate means hot
  vtables are being evicted by colder/bigger ones.
- `flushes` — `bind:to:` invalidations (count, so we can see how often the whole table is cleared).

Printed at VM shutdown (or `atexit`). Used to answer two questions empirically: (1) how often does
the table actually flush in real workloads, and (2) does a single shared table cause harmful
interference (collision rate) — before deciding whether set-associativity / per-vtable tables are
worth it.

## Testing & success criteria

- **Conformance must hold 36/5/1** on both paths: `cd ns-oevm && python3 tools/run_tests_oevm.py`
  with `OEVM_PATH=AB` (default), and the individual `A` / `B` modes.
- **Image e2e = 42**: `bash oevm/tests/test_load_run.sh`.
- **32-bit = 42**: `bash oevm/tests/test_portability_32.sh`.
- **C unit test** for the cache: hit, miss-then-fill, collision-overwrite, and flush-clears-all.
- **Invalidation correctness test**: send a selector (populating the cache), replace its method via
  `bind:to:`, re-send, and assert the *new* method runs. Guards the flush.
- **Perf**: measure `richards`, `deltablue` (if it runs), and `fib(32)` via `oevm/bench.sh` and
  record before/after in `ns-tests/00_perf/RESULTS.md`. Expectation: a win on poly-send-heavy code
  (richards/deltablue); `fib` roughly neutral (already dispatch-light after block inlining).

## Out of scope / future work

- Per-call-site inline caching via an `OP_SEND_IC` RAM side-table (lever "B").
- Unboxed floats (separate perf lever / separate spec).
- Polymorphic (N-way set-associative) variants of this cache, unless profiling justifies them.

### Open concern to validate empirically (not block on)

A single shared direct-mapped table means a hot vtable's entries can be evicted by collisions from
colder/larger vtables — "hot vtables paying the price of slower, bigger vtables." We do **not**
pre-optimize for this. Instead the `collisions` counter measures it directly; if it proves harmful on
real workloads (richards/deltablue), the mitigation menu is: larger table, N-way set-associative
slots, or per-vtable inline tables. Get the simple version working and measured first.
