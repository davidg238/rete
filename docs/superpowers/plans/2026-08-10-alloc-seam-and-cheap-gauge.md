# Allocation seam and the `sys/cheap` gauge — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Route every allocation the VM makes through one wrapper that counts bytes
outstanding and checks for NULL, and publish those counters as a new `sys/cheap` cell — so
that the C-heap leak in rete#190 becomes visible on an instrument before anyone tries to fix
it.

**Architecture:** Six `malloc`/`realloc` call sites and one `free` become `oevm_malloc` /
`oevm_realloc` / `oevm_free` in `gc.c` (already the VM's memory home, so no new `.c` file and
none of the nine enumerated source lists are touched). The wrapper carries an 8-byte size
header so `outBytes` is exact. A new platform prim surfaces the counters as an Array; an nsl
`CHeapStats` class names them; the kernel composes a `CHeap` record published on `sys/cheap`
every check-in, exactly as `sys/metrics` already is.

**Tech Stack:** C99 (`-std=c99 -Isrc`), the nsl dialect, Python for the image serializer,
Zephyr for the C6 build.

**Spec:** `~/workspace/rete/docs/design/2026-08-10-node-memory-and-liveness-design.md` — this
plan implements **sub-project A only**, which is **stage 1** of that design's four. Stages 2–4
(inline Symbol bytes; `gw/**` liveness sweep; panic note and reboot) get their own plans, and
stage 2's plan is deliberately not written until this one's hardware gate has produced a
reading. §7 of the design explains why.

---

## Global Constraints

- **`.nsl` is the source extension.** A bare `.ns` means a frozen oracle, an archive, or
  generated output — never a file you edit.
- **Prim ids are the 1-based position in `PRIM_NAMES` and are append-only.** They are arrived
  at by **COUNTING the table, never by arithmetic on the previous id.** `PRIM_NAMES` currently
  holds **151** entries (`bootId` is 151), so the new prim is **152** — and Task 3 re-counts
  rather than trusting this sentence.
- **Never run two nsl suites concurrently.** They share `/tmp` paths. Nothing enforces this;
  an orphaned subagent suite produces a false RED.
- **Read the aux-gate line**, not just the pass count. The suite prints `Passed: N / Failed: 0`
  *and* a line of aux gates that can be red while the count reads zero failures.
  `footprint: SKIP (no arm-zephyr-eabi toolchain)` is pre-existing and expected.
- **MEASURE the nsl suite baseline before Task 1; do not anchor on a number from a
  document.** `CLAUDE.md` says 258, commit `8fbf4c0` says 257, and an older memory says 255 —
  three sources, three answers, so all three are untrustworthy. Run
  `cd ~/workspace/rete/nodus/nsl && python3 tools/run_tests_oevm.py`, record what it actually
  prints, and write it here. This plan adds exactly **two** tests (Task 3 and Task 4), so the
  final count is baseline + 2. The suite takes over ten minutes; budget for it.
- Copyright header `// Copyright (c) 2026 Ekorau LLC` on every new Go file. No Go files here.
- Cite absolute paths in conversation (`~/workspace/rete/...`), never a bare relative path.

### A standing caution about this document

**Plan-embedded code is unrun code.** Every snippet below was written into this file and has
never executed. rete#165's post-implementation defects were nearly all the plan's own text.
Treat each code block as a sketch of intent with the right names and shapes — compile it, run
its test, and fix it where it is wrong. A snippet that does not compile is a defect in this
plan, not a puzzle to work around.

---

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `nodus/tuvm/src/gc.c` | modify | The seam itself: `oevm_malloc`/`oevm_realloc`/`oevm_free`, the counters, `oevm_cheap_stats()`. Also one of the six converted call sites (`:342`) |
| `nodus/tuvm/src/oevm.h` | modify | `struct OevmCHeapStats` + the three wrapper declarations, beside `struct OevmHeapStats` at `:470` |
| `nodus/tuvm/src/dispatch.c` | modify | Two converted sites (`:1633`, `:1669`), the one `free` (`:1635`), and `PRIM_NAMES` + the prim enum |
| `nodus/tuvm/src/vm.c` | modify | Two converted sites (`:1751` realloc, `:1794`) |
| `nodus/tuvm/src/platform.c` | modify | One converted site (`:1226` — the leak) and the new prim arm |
| `nodus/tuvm/src/prim_ids.h` | modify | The `PRIM_pf_cheapStats` mirror |
| `nodus/tuvm/tests/test_cheap_stats.c` | **create** | C unit test for the counters |
| `nodus/tuvm/tests/test_cheap_oom.c` | **create** | C unit test for the NULL path, via `-Wl,--wrap=malloc` |
| `nodus/tuvm/tools/run_c_tests.sh` | modify | Register both new C tests |
| `nodus/nsl/lib/kernel.nsl` | modify | `CHeapStats` class, `platform cheapStats` accessor, `cheap` record builder, `publishCheap` |
| `nodus/tests/15_heap_stats/04_cheap_invariants.nsl` | **create** | Prim-to-class invariants, both-path gate |
| `nodus/tests/22_spacesync/14_cheap_on_checkin.nsl` | **create** | The `sys/cheap` cell end-to-end |
| `nodus/tests/22_spacesync/09_batch_size.nsl` | modify | Add `cheap` to the measured-encoded-size assertions |
| `protocol/PROTOCOL-NSL-RECORDS.md` | modify | §4.4 row for `sys/cheap`, with a **measured** encoded size |

---

## Task 1: The allocation seam and its counters

Everything the VM allocates goes through one wrapper. This task changes **no behaviour** — the
NULL path still returns NULL to callers that don't check it, exactly as today. That is
deliberate: it keeps this task's diff mechanical and reviewable, and Task 2 changes behaviour
with its own test.

**Files:**
- Modify: `nodus/tuvm/src/oevm.h` (near `:470`, beside `struct OevmHeapStats`)
- Modify: `nodus/tuvm/src/gc.c` (add the seam; convert `:342`)
- Modify: `nodus/tuvm/src/dispatch.c:1633`, `:1635`, `:1669`
- Modify: `nodus/tuvm/src/vm.c:1751`, `:1794`
- Modify: `nodus/tuvm/src/platform.c:1226`
- Create: `nodus/tuvm/tests/test_cheap_stats.c`
- Modify: `nodus/tuvm/tools/run_c_tests.sh`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `void *oevm_malloc(size_t n)` — NULL on failure, having counted the failure
  - `void *oevm_realloc(void *p, size_t n)` — `p == NULL` behaves as `oevm_malloc(n)`
  - `void oevm_free(void *p)` — `p == NULL` is a no-op
  - `struct OevmCHeapStats { size_t out_bytes, out_high, allocs, frees, fails; }`
  - `struct OevmCHeapStats oevm_cheap_stats(void)`

- [ ] **Step 1: Write the failing test**

Create `nodus/tuvm/tests/test_cheap_stats.c`. Model it on the existing `tests/test_heap_stats.c`
— read that file first for the assertion idiom and `main` shape this suite uses, and match it
rather than the sketch below where they differ.

```c
/* rete#190 sub-project A: the C-allocation seam's counters.
   Link set: Model A (see tools/run_c_tests.sh ENG_A). */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "oevm.h"

int main(void) {
    struct OevmCHeapStats a = oevm_cheap_stats();

    void *p = oevm_malloc(100);
    assert(p != NULL);
    struct OevmCHeapStats b = oevm_cheap_stats();
    assert(b.allocs == a.allocs + 1);
    assert(b.out_bytes == a.out_bytes + 100);
    assert(b.out_high >= b.out_bytes);

    /* the payload must be 8-aligned: a 4-byte header on a 32-bit target would
       silently misalign every double the VM ever stores through this pointer */
    assert(((uintptr_t)p % 8) == 0);

    /* the block must be writable across its whole declared length */
    memset(p, 0xA5, 100);

    oevm_free(p);
    struct OevmCHeapStats c = oevm_cheap_stats();
    assert(c.frees == a.frees + 1);
    assert(c.out_bytes == a.out_bytes);       /* returned to baseline */
    assert(c.out_high == b.out_high);         /* high water never falls */

    /* realloc: grow, then shrink, and outstanding must track both ways */
    void *q = oevm_malloc(16);
    assert(q != NULL);
    q = oevm_realloc(q, 64);
    assert(q != NULL);
    struct OevmCHeapStats d = oevm_cheap_stats();
    assert(d.out_bytes == c.out_bytes + 64);
    q = oevm_realloc(q, 8);
    assert(q != NULL);
    struct OevmCHeapStats e = oevm_cheap_stats();
    assert(e.out_bytes == c.out_bytes + 8);
    oevm_free(q);
    assert(oevm_cheap_stats().out_bytes == c.out_bytes);

    /* oevm_free(NULL) is a no-op and must not move any counter */
    struct OevmCHeapStats f = oevm_cheap_stats();
    oevm_free(NULL);
    struct OevmCHeapStats g = oevm_cheap_stats();
    assert(g.frees == f.frees);
    assert(g.out_bytes == f.out_bytes);

    printf("test_cheap_stats OK\n");
    return 0;
}
```

- [ ] **Step 2: Register the test and run it to verify it fails**

In `nodus/tuvm/tools/run_c_tests.sh`, beside the existing `run test_heap_stats` line
(currently line 53), add:

```bash
# rete#190 sub-project A: the C-allocation seam's counters (oevm_malloc/free/realloc).
run test_cheap_stats "$ENG_A" -DOEVM_HEAP_SIZE=65536
```

Run: `cd ~/workspace/rete/nodus/tuvm && bash tools/run_c_tests.sh 2>&1 | grep cheap`

Expected: `test_cheap_stats FAIL` with a compile error — `unknown type name 'struct
OevmCHeapStats'` and `implicit declaration of function 'oevm_cheap_stats'`.

- [ ] **Step 3: Declare the seam in `oevm.h`**

Immediately after `struct OevmHeapStats oevm_heap_stats(void);` (currently `oevm.h:477`):

```c
/* rete#190: the C-allocation seam. Every malloc/realloc/free the VM performs goes
   through these three, so that (a) a NULL return is handled in ONE place instead of
   six, and (b) bytes-outstanding is exact rather than inferred. The VM made six
   allocations and exactly one free when this was written; the leak that motivated it
   was one wire-Symbol buffer per decoded Symbol (platform.c asSymbol).
   DELIBERATE BLIND SPOT: allocations made by lwIP, OpenThread or Zephyr itself are
   invisible here. `fails` still catches a NULL from ANY cause, because whoever
   exhausted the heap, the failing request is one the VM made. */
struct OevmCHeapStats {
    size_t out_bytes;   /* payload bytes currently outstanding (headers excluded) */
    size_t out_high;    /* max of out_bytes ever observed (monotonic since boot) */
    size_t allocs;      /* successful oevm_malloc + oevm_realloc calls since boot */
    size_t frees;       /* oevm_free calls that released a block (NULL is not counted) */
    size_t fails;       /* allocation attempts that returned NULL */
};
struct OevmCHeapStats oevm_cheap_stats(void);

void *oevm_malloc(size_t n);
void *oevm_realloc(void *p, size_t n);
void  oevm_free(void *p);
```

- [ ] **Step 4: Implement the seam in `gc.c`**

Add near the top of `gc.c`, beside the `heap_live_bytes` / `heap_high_water` counters
(currently `:62-64`) so the two accounting stories sit together. Note that these count the **C
heap**, which is a different heap from the GC arena those counters describe — the comment must
say so, because conflating the two is exactly the mistake that let this leak run for a week.

```c
/* rete#190: C-heap accounting. NOT the GC arena — that is heap_live_bytes above.
   The arena is statically sized and was never under pressure; the leak was here. */
static struct OevmCHeapStats cheap;

/* The size header. A union with uint64_t rather than a bare size_t: malloc returns
   8-aligned storage, and on a 32-bit target (RV32, the C6) a 4-byte header would
   hand back 4-aligned payloads. That misaligns every double stored through the
   pointer and faults on the TARGET ONLY — invisible on the 64-bit host, which is
   the worst possible failure shape. C99 has no max_align_t, hence uint64_t. */
union CHeapBlock {
    size_t   size;
    uint64_t _align;
};

void *oevm_malloc(size_t n) {
    union CHeapBlock *b = malloc(sizeof(union CHeapBlock) + n);
    if (b == NULL) { cheap.fails++; return NULL; }
    b->size = n;
    cheap.allocs++;
    cheap.out_bytes += n;
    if (cheap.out_bytes > cheap.out_high) cheap.out_high = cheap.out_bytes;
    return (void *)(b + 1);
}

void oevm_free(void *p) {
    if (p == NULL) return;
    union CHeapBlock *b = ((union CHeapBlock *)p) - 1;
    cheap.out_bytes -= b->size;
    cheap.frees++;
    free(b);
}

void *oevm_realloc(void *p, size_t n) {
    if (p == NULL) return oevm_malloc(n);
    union CHeapBlock *b = ((union CHeapBlock *)p) - 1;
    size_t old = b->size;
    union CHeapBlock *nb = realloc(b, sizeof(union CHeapBlock) + n);
    if (nb == NULL) { cheap.fails++; return NULL; }   /* `b` is still valid and still counted */
    nb->size = n;
    /* written as subtract-then-add, NOT `+= n - old`: the latter relies on size_t
       wraparound when shrinking, which is correct but reads as a bug */
    cheap.out_bytes = cheap.out_bytes - old + n;
    if (cheap.out_bytes > cheap.out_high) cheap.out_high = cheap.out_bytes;
    cheap.allocs++;
    return (void *)(nb + 1);
}

struct OevmCHeapStats oevm_cheap_stats(void) { return cheap; }
```

`gc.c` already includes `<stdlib.h>`; confirm, and add `<stdint.h>` if `uint64_t` is not
already in scope.

- [ ] **Step 5: Run the C test to verify it passes**

Run: `cd ~/workspace/rete/nodus/tuvm && bash tools/run_c_tests.sh 2>&1 | grep cheap`
Expected: `test_cheap_stats PASS`

- [ ] **Step 6: Convert all six call sites**

Mechanical. Replace the allocator, change nothing else — no NULL handling yet.

| File:line | Now | Becomes |
|---|---|---|
| `dispatch.c:1633` | `char *tmp = (char *)malloc(n + 1);` | `char *tmp = (char *)oevm_malloc(n + 1);` |
| `dispatch.c:1635` | `... free(tmp); ...` | `... oevm_free(tmp); ...` |
| `dispatch.c:1669` | `char *new_str = malloc(strlen(buf) + 1);` | `char *new_str = oevm_malloc(strlen(buf) + 1);` |
| `vm.c:1751` | `vt->bindings = realloc(vt->bindings, ...);` | `vt->bindings = oevm_realloc(vt->bindings, ...);` |
| `vm.c:1794` | `uint32_t *heap_insts = malloc(...);` | `uint32_t *heap_insts = oevm_malloc(...);` |
| `platform.c:1226` | `char *nm = malloc(len + 1);` | `char *nm = oevm_malloc(len + 1);` |
| `gc.c:342` | `struct FloatSlab *slab = malloc(...);` | `struct FloatSlab *slab = oevm_malloc(...);` |

**`vm.c:1751` is the one that is not a like-for-like swap.** `vt->bindings` may have been
allocated by a *previous* `realloc` from before this change within the same run — it cannot
have been, since all sites convert in one commit, but confirm by reading the surrounding
function that `vt->bindings` starts as `NULL` and is only ever grown here. If any other code
path assigns `vt->bindings` from a plain `malloc` or from static storage, `oevm_realloc` will
read a header that does not exist. Search before editing:

```bash
cd ~/workspace/rete/nodus/tuvm/src && rg -n 'bindings\s*=' vm.c dispatch.c kernel_boot.c
```

If any assignment gives `bindings` non-heap storage, stop and report it — that is a finding
this plan did not anticipate, not something to route around.

- [ ] **Step 7: Verify nothing regressed**

Run, one at a time, never concurrently:

```bash
cd ~/workspace/rete/nodus/tuvm && bash tools/run_c_tests.sh
cd ~/workspace/rete/nodus/nsl  && python3 tools/run_tests_oevm.py
```

Expected: all C tests PASS (including `test_cheap_stats`); the nsl suite reads
`Passed: 258 / Failed: 0` — unchanged, because this task adds no nsl-visible behaviour. **Read
the aux-gate line too**, not just the count.

- [ ] **Step 8: Commit**

```bash
cd ~/workspace/rete
git add nodus/tuvm/src/oevm.h nodus/tuvm/src/gc.c nodus/tuvm/src/dispatch.c \
        nodus/tuvm/src/vm.c nodus/tuvm/src/platform.c \
        nodus/tuvm/tests/test_cheap_stats.c nodus/tuvm/tools/run_c_tests.sh
git commit -m "feat(#190): six allocations and one free now go through one seam

The VM made six allocations and exactly one free. Routing all of them through
oevm_malloc/oevm_realloc/oevm_free makes bytes-outstanding exact and gives the
NULL return one place to be handled, instead of six that never were.

Behaviour is unchanged by design: callers that ignored a NULL still ignore it.
The header is a union with uint64_t, not a bare size_t — on RV32 a 4-byte header
would hand back 4-aligned payloads and misalign every double, faulting on the
target only and never on the 64-bit host."
```

---

## Task 2: An allocation failure becomes `#oom`, not a store through NULL

**Files:**
- Create: `nodus/tuvm/tests/test_cheap_oom.c`
- Modify: `nodus/tuvm/tools/run_c_tests.sh`
- Modify: `nodus/tuvm/src/dispatch.c:1633`, `:1669`; `nodus/tuvm/src/vm.c:1751`, `:1794`;
  `nodus/tuvm/src/platform.c:1226`; `nodus/tuvm/src/gc.c:342`

**Interfaces:**
- Consumes: `oevm_malloc`, `oevm_cheap_stats` from Task 1.
- Produces: no new symbols. The behavioural contract is that no VM allocation site
  dereferences a NULL return.

- [ ] **Step 1: Write the failing test**

Read `nodus/tuvm/tests/test_float_oom.c` first — it is the existing precedent for forcing
allocation failure with `-Wl,--wrap=malloc`, and its wrap-stub idiom is what to copy.

Create `nodus/tuvm/tests/test_cheap_oom.c`:

```c
/* rete#190 sub-project A: an allocation failure must be counted and survived,
   never dereferenced. Forces failure with --wrap=malloc, the test_float_oom
   precedent. Link set: Model A. */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "oevm.h"

static int fail_allocs = 0;
void *__real_malloc(size_t n);
void *__wrap_malloc(size_t n) { return fail_allocs ? NULL : __real_malloc(n); }

int main(void) {
    struct OevmCHeapStats a = oevm_cheap_stats();

    fail_allocs = 1;
    void *p = oevm_malloc(64);
    fail_allocs = 0;

    assert(p == NULL);
    struct OevmCHeapStats b = oevm_cheap_stats();
    assert(b.fails == a.fails + 1);
    assert(b.allocs == a.allocs);        /* a failure is not an allocation */
    assert(b.out_bytes == a.out_bytes);  /* and moves nothing outstanding */

    printf("test_cheap_oom OK\n");
    return 0;
}
```

- [ ] **Step 2: Register it and run it**

In `tools/run_c_tests.sh`, beside the existing `run test_float_oom` line (currently 128):

```bash
# rete#190: the seam counts a failed allocation and returns NULL rather than aborting.
run test_cheap_oom "$ENG_A" -Wl,--wrap=malloc
```

Run: `cd ~/workspace/rete/nodus/tuvm && bash tools/run_c_tests.sh 2>&1 | grep cheap_oom`
Expected: PASS already — Task 1's wrapper counts `fails` and returns NULL. **If it fails,
Task 1 is wrong and this is the test that caught it.** Fix Task 1's code, not this test.

- [ ] **Step 3: Make each of the six call sites NULL-safe**

This is the behavioural change, and each site needs its own judgement — do not apply one
pattern blindly. Read the surrounding function for each before editing.

The three that must not crash:

```c
/* dispatch.c:1633 — printString of a Symbol. THE CRASH SITE: `tmp[0] = '#'`
   stored through NULL and produced mcause 7 on three boards. */
char *tmp = (char *)oevm_malloc(n + 1);
if (tmp == NULL) return make_string("#?", 2);   /* a bounded truth beats a halt */

/* dispatch.c:1669 — asString building a Symbol name. */
char *new_str = oevm_malloc(strlen(buf) + 1);
if (new_str == NULL) return NULL;               /* nil, which nsl can test for */

/* platform.c:1226 — asSymbol from a wire String. */
char *nm = oevm_malloc(len + 1);
if (nm == NULL) { *out = NULL; return 1; }      /* answers nil to the nsl caller */
```

For `vm.c:1751` (vtable growth), `vm.c:1794` (method instructions) and `gc.c:342`
(`FloatSlab`), a NULL is not survivable in the local sense — a vtable that cannot grow cannot
dispatch. Read each and decide between returning a failure the caller already handles and
calling the VM's existing fatal path. **Whatever you choose, the choice must be visible in a
comment naming why**, and it must not be a silent store through NULL.

If any of the three is genuinely unrecoverable, say so in the commit message rather than
inventing a recovery this plan did not design.

- [ ] **Step 4: Verify**

```bash
cd ~/workspace/rete/nodus/tuvm && bash tools/run_c_tests.sh
cd ~/workspace/rete/nodus/nsl  && python3 tools/run_tests_oevm.py
```

Expected: all C PASS; nsl `Passed: 258 / Failed: 0`, aux line read.

- [ ] **Step 5: Commit**

```bash
cd ~/workspace/rete
git add nodus/tuvm/src nodus/tuvm/tests/test_cheap_oom.c nodus/tuvm/tools/run_c_tests.sh
git commit -m "fix(#190): an allocation failure is counted and survived, not dereferenced

dispatch.c:1633 is where nine crashes landed — tmp[0] = '#' stored through a NULL
that malloc(7) had just returned, giving mcause 7 Store/AMO on three boards. The
six sites now each answer a failure instead of trusting the pointer.

Forced with --wrap=malloc, the test_float_oom precedent."
```

---

## Task 3: `platform cheapStats` — the prim and the nsl class

**Files:**
- Modify: `nodus/tuvm/src/dispatch.c` (the prim enum tail ~`:853`, and `PRIM_NAMES` tail ~`:922`)
- Modify: `nodus/tuvm/src/prim_ids.h` (after `PRIM_pf_bootId`, currently `:487`)
- Modify: `nodus/tuvm/src/platform.c` (a new arm beside `PRIM_pf_heapStats`, currently `:1234`)
- Modify: `nodus/nsl/lib/kernel.nsl` (the `CHeapStats` class beside `HeapStats` at `:1080`; the
  accessor beside `heapStats` at `:3549`)
- Create: `nodus/tests/15_heap_stats/04_cheap_invariants.nsl`

**Interfaces:**
- Consumes: `oevm_cheap_stats()` from Task 1.
- Produces:
  - C: `PRIM_pf_cheapStats` (id **152**, to be confirmed by counting)
  - nsl: `platform cheapStats` → a `CHeapStats` instance with accessors `outBytes`,
    `outHigh`, `allocs`, `frees`, `fails`, and `CHeapStats fromList: anArray`

- [ ] **Step 1: Write the failing test**

Create `nodus/tests/15_heap_stats/04_cheap_invariants.nsl`. Copy the shape of the sibling
`01_invariants.nsl` exactly — the `(* expect: *)` block goes at the TOP, there is no
`.expected` file, and the class takes `main: platform args:`.

```
(*
expect:
true
true
true
true
true
*)
(* rete#190 sub-project A: C-heap accounting invariants. Absolute values vary
   run-to-run, so this asserts only what holds on every run and on BOTH paths
   (A solidified image, B .oimg): the high-water never trails what is currently
   outstanding, frees never exceed allocs, a failure count is a non-negative
   number, and something has in fact been allocated by the time an image is
   running. Exercises prim 152 -> the CHeapStats class end-to-end.
   NOTE: this is the C heap, NOT the GC arena that 01_invariants covers. The
   two being confused is what let a leak run for a week unseen. *)
class CHeapInvariantsTest packageUsing: manifest = () (
public main: platform args: args = (
  | s |
  s:: platform cheapStats.
  (s outHigh >= s outBytes) printString println.
  (s allocs >= s frees) printString println.
  (s fails >= 0) printString println.
  (s allocs > 0) printString println.
  (s outBytes >= 0) printString println.
)
) : (
)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd ~/workspace/rete/nodus/nsl && python3 tools/run-one.py ../tests/15_heap_stats/04_cheap_invariants.nsl`

Expected: FAIL — `cheapStats` does not resolve. **A SKIP proves nothing** and a CRASH discards
every line already printed, so read the actual output rather than trusting an exit code.

- [ ] **Step 3: Count `PRIM_NAMES`, then append the prim**

Ids are the 1-based position in the table and are arrived at by **counting**, never by
arithmetic. Count first:

```bash
cd ~/workspace/rete/nodus/tuvm/src && python3 - <<'EOF'
import re
s = open('dispatch.c').read()
body = re.search(r'static const char \*const PRIM_NAMES\[\] = \{(.*?)\n\};', s, re.S).group(1)
body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
print(len(re.findall(r'"((?:[^"\\]|\\.)*)"', body)))
EOF
```

Expected: `151`. The new prim is therefore **152**. If the count is not 151, use the count and
not this sentence.

In `dispatch.c`, append to the enum after `PRIM_pf_bootId` (currently `:853`):

```c
    /* rete#190 sub-project A: `platform cheapStats` — the C-allocation seam's
       counters (src/gc.c oevm_cheap_stats), the C-heap twin of heapStats (104),
       which reports the GC ARENA. Both exist because conflating them is what let
       a per-decoded-Symbol leak run for a week against a gauge that could not
       see it. Appended after bootId; no pre-existing id shifts. 152 was arrived
       at by COUNTING PRIM_NAMES (151 entries before this append), not by 151+1
       arithmetic, per the standing lesson above. Platform-dispatched (mirrored
       in prim_ids.h). */
    PRIM_pf_cheapStats,
```

In the same file, append `"cheapStats",` as the last entry of `PRIM_NAMES` (currently after
`"bootId",` at ~`:922`). **The enum and the string table must stay parallel** — index `i` maps
to id `i+1`.

In `prim_ids.h`, after `#define PRIM_pf_bootId 151` (currently `:487`), add the same comment
in the house style and:

```c
#define PRIM_pf_cheapStats        152
```

- [ ] **Step 4: Add the prim arm**

In `platform.c`, immediately after the `PRIM_pf_heapStats` arm (which ends at `:1252`):

```c
    case PRIM_pf_cheapStats: {
        /* rete#190: `platform cheapStats` — snapshot the C-allocation seam into a
           5-slot Array of SMIs, field order [outBytes, outHigh, allocs, frees,
           fails]. The nsl CHeapStats class names them. This is the C heap, NOT the
           GC arena that heapStats (104) reports.
           SMI range: outBytes/outHigh are bounded by the C heap (~200 KB on the C6)
           and the counts are per-boot totals — a 30 s cadence reaches ~10^4/day, so
           all five fit a 32-bit SMI on host and target alike.
           GC-safety: the whole struct is copied into the C local `st` BEFORE the
           single allocating call (indexable_new); the five stores are SMIs
           (immediates, no allocation), so no heap oop is held across an allocation
           and no gc_temp_root pin is required. args are ignored (0-arg selector). */
        struct OevmCHeapStats st = oevm_cheap_stats();
        oop r = indexable_new(5);
        ((struct ObjectInstance *)r)->slots[1] = TAG_SMI((intptr_t)st.out_bytes);
        ((struct ObjectInstance *)r)->slots[2] = TAG_SMI((intptr_t)st.out_high);
        ((struct ObjectInstance *)r)->slots[3] = TAG_SMI((intptr_t)st.allocs);
        ((struct ObjectInstance *)r)->slots[4] = TAG_SMI((intptr_t)st.frees);
        ((struct ObjectInstance *)r)->slots[5] = TAG_SMI((intptr_t)st.fails);
        *out = r; return 1;
    }
```

- [ ] **Step 5: Add the nsl class and accessor**

In `kernel.nsl`, immediately after the `HeapStats` class (which ends at `:1101`):

```
  class CHeapStats = ( | outBytes outHigh allocs frees fails | ) (
    (* rete#190: a read-only snapshot of C-allocation accounting (src/gc.c
       oevm_cheap_stats), surfaced by the native `platform cheapStats` leaf
       (prim 152) as a 5-element Array in field order. Construct via the
       class-side `fromList:`. This is the C heap — the GC arena is HeapStats. *)
    public outBytes = ( ^ outBytes )
    public outHigh  = ( ^ outHigh )
    public allocs   = ( ^ allocs )
    public frees    = ( ^ frees )
    public fails    = ( ^ fails )
    setFromList: a = (
      (* raw IndexableVtable Array, so basicAt: — the HeapStats precedent *)
      outBytes:: a basicAt: 1. outHigh:: a basicAt: 2. allocs:: a basicAt: 3.
      frees:: a basicAt: 4. fails:: a basicAt: 5. ^ self )
    public printString = (
      ^ 'out ', outBytes printString, ' / hw ', outHigh printString,
        ' / a ', allocs printString, ' / f ', frees printString,
        ' / fail ', fails printString )
  ) : (
    public fromList: a = ( ^ self new setFromList: a )
  )
```

And beside the `heapStats` accessor (currently `:3549`):

```
  public cheapStats        = ( ^ CHeapStats fromList: (platform cheapStats) )
```

Beware the accessor trap recorded for `Spans`/`SpanCapture` two lines above: sending an
instance-side selector to a class object silently NEWs the class and drops the message. The
`fromList:` form above is class-side and therefore safe, but read those comments before
deviating.

- [ ] **Step 6: Run the test to verify it passes**

```bash
cd ~/workspace/rete/nodus/nsl && python3 tools/run-one.py ../tests/15_heap_stats/04_cheap_invariants.nsl
```
Expected: five `true` lines, PASS.

Then the whole suite: `python3 tools/run_tests_oevm.py`
Expected: `Passed: <baseline + 1> / Failed: 0`, against the baseline you measured before
Task 1. Read the aux line.

- [ ] **Step 7: Commit**

```bash
cd ~/workspace/rete
git add nodus/tuvm/src nodus/nsl/lib/kernel.nsl nodus/tests/15_heap_stats/04_cheap_invariants.nsl
git commit -m "feat(#190): platform cheapStats — the C heap gets the gauge the arena had

heapStats (104) reports the GC arena, which is statically sized and was never
under pressure. The leak was in the C heap beside it, and nothing could see it.
cheapStats (152) is the twin, counted from PRIM_NAMES rather than computed.

Not Zephyr's heap: CONFIG_COMMON_LIBC_MALLOC's sys_heap is a static in lib/libc,
so reading it would need a second local patch to ~/zephyr-esp32/zephyr that a
west re-fetch silently loses. VM-side accounting reads identically on host and
target, so the nsl suite can exercise it."
```

---

## Task 4: The `CHeap` record on `sys/cheap`

**Files:**
- Modify: `nodus/nsl/lib/kernel.nsl` (the `cheap` builder beside `metrics` at `:3683`;
  `publishCheap` beside `publishMetrics` at `:2094`; the call at `:2065`)
- Create: `nodus/tests/22_spacesync/14_cheap_on_checkin.nsl`
- Modify: `nodus/tests/22_spacesync/09_batch_size.nsl` (the measured-size assertion)
- Modify: `protocol/PROTOCOL-NSL-RECORDS.md` (§4.4 table, after the `sys/metrics` row at `:380`)

**Interfaces:**
- Consumes: `platform cheapStats` → `CHeapStats` from Task 3.
- Produces: `Kernel>>cheap` → a `Record` tagged `#CHeap`; `publishCheap` writing the
  `sys/cheap` cell.

- [ ] **Step 1: Write the failing test**

Create `nodus/tests/22_spacesync/14_cheap_on_checkin.nsl`, modelled on the sibling
`../15_heap_stats/02_sys_metrics.nsl` — read that file first; it is the direct precedent for
asserting a record's structure and then round-tripping it through a cell.

```
(*
expect:
true
true
true
true
true
done
*)
(* rete#190 sub-project A: the sys/cheap story end-to-end — prim 152 (cheapStats)
   + the kernel CHeap record (schema pinned in PROTOCOL-NSL-RECORDS.md §4.4) +
   the sys/cheap CELL: a reader peeks current state by NAME, non-destructively.
   Absolute values vary run-to-run, so this asserts structural invariants only
   (the 15_heap_stats/02 precedent). Publishing is the app's act — this test IS
   the publisher. Both-path A==B gate. *)
class CheapOnCheckinTest packageUsing: manifest = () (
public main: platform args: args = (
  | c cell got |
  c:: platform cheap.
  (c tag = #CHeap) printString println.
  ((c at: #outHigh ifAbsent: [ -1 ]) >= (c at: #outBytes ifAbsent: [ 0 ])) printString println.
  ((c at: #allocs ifAbsent: [ -1 ]) >= (c at: #frees ifAbsent: [ 0 ])) printString println.
  ((c at: #fails ifAbsent: [ -1 ]) >= 0) printString println.

  cell:: platform space channel: 'sys/cheap'.
  cell put: '' value: c.
  got:: cell peek: '' ifAbsent: [ nil ].
  (got = c) printString println.
  'done' println.
)
) : (
)
```

- [ ] **Step 2: Run it to verify it fails**

```bash
cd ~/workspace/rete/nodus/nsl && python3 tools/run-one.py ../tests/22_spacesync/14_cheap_on_checkin.nsl
```
Expected: FAIL — `platform cheap` does not resolve.

- [ ] **Step 3: Add the record builder**

In `kernel.nsl`, immediately after the `metrics` method (which ends at `:3705` with
`^ Record tag: #Metrics fields: (Map fromPairs: pairs) )`):

```
  (* rete#190: the sys/cheap snapshot — prim 152 (cheapStats) as ONE flat immutable
     Record, the sys/cheap cell's wire schema (pinned in
     protocol/PROTOCOL-NSL-RECORDS.md §4.4). A SEPARATE record from Metrics, not
     five more Metrics fields: RECORDS §4.4 already records that lesson against
     LatencySpan — adding a concern adds a record, not a field, so this schema does
     not grow the way Metrics has. Metrics is 195 B of the 256 B cap and keeps all
     61 B of its headroom.
     The kernel only BUILDS the record; publishing it onto a cell is the app's act
     (G9), with the channel arriving as a capability. *)
  public cheap = ( | c pairs |
    c:: platform cheapStats.
    pairs:: OrderedCollection new.
    pairs add: #outBytes -> c outBytes.
    pairs add: #outHigh  -> c outHigh.
    pairs add: #allocs   -> c allocs.
    pairs add: #frees    -> c frees.
    pairs add: #fails    -> c fails.
    ^ Record tag: #CHeap fields: (Map fromPairs: pairs) )
```

- [ ] **Step 4: Publish it on each check-in**

Add beside `publishMetrics` (currently `:2094`):

```
    (* rete#190: the C-heap twin of publishMetrics. Same cadence, same rationale —
       one record per check-in, no escalation logic, no invented constants. *)
    publishCheap = (
      space put: 'sys/cheap' value: outer cheap.
      ^ self )
```

And call it in `checkIn`, immediately after `self publishMetrics.` (currently `:2065`):

```
      self publishMetrics.
      self publishCheap.
```

**The position matters and is not arbitrary.** The comment block above that call records that
`pushFrame` trims from the TAIL of an ascending-seq frame, so the last cell written before the
push is the first dropped under batch pressure. `sys/cheap` goes *after* `sys/metrics` because
both are rewritten every check-in and can afford to lose a sample, whereas `publishApps` above
them writes once per boot and cannot. Do not move it above `publishApps`.

- [ ] **Step 5: Run the test to verify it passes**

```bash
cd ~/workspace/rete/nodus/nsl && python3 tools/run-one.py ../tests/22_spacesync/14_cheap_on_checkin.nsl
```
Expected: five `true` lines then `done`, PASS.

- [ ] **Step 6: MEASURE the encoded size — do not estimate it**

RECORDS §4.4's `Metrics` (195 B) and `LatencySpan` (140 B) are both **measured** figures that
replaced estimates, and the design requires the same here. There is already a test whose whole
job is measuring these: `nodus/tests/22_spacesync/09_batch_size.nsl` prints
`metrics <bytes>` and `health <bytes>` through the real encoder. **Extend it rather than
writing a throwaway** — the number then stays measured on every suite run instead of once.

Add a line beside the existing ones (currently `:80-81`), matching their form:

```
  ('cheap ' , (cbor encode: (self cheap: platform)) byteSize printString) println.
```

The test builds its own record via a `self <name>: platform` helper rather than calling the
kernel directly — read how `self metrics: platform` is defined in that file and add the
`cheap:` twin the same way. Update the file's `(* expect: *)` block at the top with the
printed line; the whole point is that the number is asserted, so a schema change that grows
the record fails the suite instead of drifting silently.

Record the measured number — it goes into RECORDS in the next step and into the commit message.

- [ ] **Step 7: Pin the schema in RECORDS §4.4**

Add a row to the §4.4 table immediately after the `sys/metrics` row (currently `:380`),
matching the existing row format exactly:

```
| `sys/cheap` | **C (cell)** | `CHeap { outBytes: int, outHigh: int, allocs: int, frees: int, fails: int }` — rete#190; **NN B encoded, MEASURED 2026-08-10**. The C heap, **not** the GC arena that `sys/metrics` reports — a leak ran for a week against a gauge that could only see the arena. A separate record rather than five more `Metrics` fields, per the `LatencySpan` lesson in this section |
```

Replace `NN` with the measured number from Step 6.

- [ ] **Step 8: Run the full gates**

One at a time, never concurrently:

```bash
cd ~/workspace/rete/nodus/nsl  && python3 tools/run_tests_oevm.py     # expect 260 / 0
cd ~/workspace/rete/porta      && go test ./...
cd ~/workspace/rete/nodus/tuvm && bash tests/porta_sync_host.sh       # expect PORTA SYNC OK
cd ~/workspace/rete/nodus/tuvm && bash tools/run_c_tests.sh
```

The nsl count is now **baseline + 2** (Task 3's test and Task 4's), against the baseline you
measured before Task 1. Read the aux-gate line on the nsl run;
`footprint: SKIP (no arm-zephyr-eabi toolchain)` is expected.

`porta_sync_host.sh` matters here specifically: it exercises the node/porta exchange, and this
task added a cell to every check-in. If the extra cell pushes a frame over the 512 B batch
bound, this is the gate that will say so.

- [ ] **Step 9: Commit**

```bash
cd ~/workspace/rete
git add nodus/nsl/lib/kernel.nsl nodus/tests/22_spacesync/14_cheap_on_checkin.nsl \
        protocol/PROTOCOL-NSL-RECORDS.md
git commit -m "feat(#190): sys/cheap — one record per concern, published every check-in

A separate record rather than five more Metrics fields. RECORDS §4.4 already
records that lesson against LatencySpan: adding a concern adds a record, and
Metrics keeps all 61 B of its headroom.

Published after sys/metrics and after publishApps, which is not arbitrary —
pushFrame trims from the tail, so the last cell written is the first dropped
under batch pressure. Both metrics records can afford to lose a sample; the
write-once obs/apps/main above them cannot.

Encoded size MEASURED through the real encoder, not estimated, per the
precedent that replaced Metrics' estimate with its 195 B."
```

---

## Task 5: The hardware gate — does the gauge see the leak?

This is the task the whole plan exists for. **It is not a test that should pass; it is a
prediction that must come true.** A flat line here means the gauge is blind, and stage 2 would
then prove nothing.

**Files:** none modified. This task produces a reading and an issue comment.

**Board: `c6spare`**, at its 2 s check-in cadence — 44 minutes to a verdict rather than the
bell's ten and a half hours. **The bell stays halted** and is not used here; it is reserved as
the clean long-run witness for stage 2's flat line, where its known 10 h 41 m lifetime on the
old build makes the comparison direct.

- [ ] **Step 1: Establish what is actually on the bench**

Never assume last session's hardware is still attached, powered, or on the same port, and
never identify a board by a `ttyUSB` number.

```bash
ls -l /dev/serial/by-id/
```

Resolve `c6spare` through the collector env files under
`~/workspace/rete/nodus/tuvm/zephyr/tools/console/`, not through a table and not through this
plan. A by-id hash matching neither known entry means a third board is attached — stop and
report rather than flashing something unidentified.

- [ ] **Step 2: Build the C6 image on the UNFIXED VM**

The Symbol leak must still be present — that is the entire point. This plan changed no Symbol
code, so any build from this branch qualifies; confirm by checking that
`nodus/tuvm/src/oevm.h`'s `struct Symbol` still holds `const char *name`.

Build with `zephyr/run.sh` for the C6 target. Two standing traps:

- `run.sh` does not source the west environment — source it first.
- A west re-fetch of `~/zephyr-esp32/zephyr` loses the loader segment-walk fix and the C6
  radio branch. Confirm the tree is still on branch `tuvm-c6-radio-fix` before building:
  `git -C ~/zephyr-esp32/zephyr branch --show-current`

- [ ] **Step 3: Flash c6spare and confirm WHICH board was flashed**

Resolve the board that was actually flashed rather than assuming — this is the failure rete#170
fixed. Capture the boot banner and check the build id against what you just built. A C6 prints
**two** banners at boot; that is expected, not a double flash.

- [ ] **Step 4: Capture for at least 45 minutes**

One detached `setsid capture.py` holds the port. **Never open a second handle** — the C6 is the
one board a second open does not reboot, which makes a stolen port silent rather than obvious.
Check for an existing capture in its OWN Bash call:

```bash
pgrep -af 'capture[.]py'
```

A `pkill -f` whose pattern contains the log or port string kills the invoking shell too.

- [ ] **Step 5: Read `sys/cheap` out of porta and plot the line**

`porta` v2 is hand-started at `~/porta-v2` serving one shared store; restarting it costs every
node an outage, so read from the running instance rather than starting your own.

```bash
cd ~/porta-v2 && ./porta get <node> sys/cheap
cd ~/porta-v2 && ./porta log <node> --path sys/cheap
```

Confirm the exact flag spellings against `porta/docs/CLI.md`, which is stale in parts, and
against `porta --help`, which is not.

Segment by `obs/build` before attributing anything: one space holds builds from more than one
board, and node ids are slots rather than boards.

- [ ] **Step 6: Judge the prediction**

**The prediction:** `outBytes` **rises** monotonically, and the rise extrapolates to C-heap
exhaustion at c6spare's measured **43 m 54 s**.

| Reading | Verdict |
|---|---|
| `outBytes` rises, extrapolating to ~44 min | **Gauge confirmed.** Stage 2 may be planned |
| `outBytes` rises much faster or slower than 44 min | Gauge works but measures something else too — investigate before stage 2 |
| `outBytes` flat while the node still dies at ~44 min | **Gauge is blind.** Stage 2 is worthless until this is understood — the leak is outside the VM's six sites |
| Node does not die at ~44 min | The seam changed allocation behaviour. Find out how before trusting any of it |

Before believing any of the four, falsify the *instrument*: confirm the numbers are moving at
all, that they come from the board you think, and that the reading is not stale. A gate can go
green testing nothing, and success and silence must look different.

- [ ] **Step 7: Report on the issue**

Post the reading to rete#190 with the raw numbers, the board's by-id hash, the build id, the
capture window, and the verdict from the table above. State plainly if the prediction failed —
that outcome is more valuable than the one we expect, and it is the reason this stage ships
alone.

---

## Self-review

**Spec coverage (design §3, sub-project A):**

| Spec requirement | Task |
|---|---|
| `oevm_malloc`/`oevm_realloc`/`oevm_free` wrapper, six sites | 1 |
| Size header, `outBytes` exact | 1 |
| NULL check happens once; sites made NULL-safe | 1 (wrapper), 2 (sites) |
| `#oom` rather than a store through NULL | 2 |
| New prim, appended and counted | 3 |
| `CHeap { outBytes, outHigh, allocs, frees, fails }` at `sys/cheap` | 4 |
| Cell, same cadence as `sys/metrics` | 4 |
| Encoded size MEASURED, not estimated, into RECORDS §4.4 | 4 (steps 6–7) |
| Known blind spot stated | 1 (the `oevm.h` comment) |
| Stage 1 gate on c6spare; bell stays halted | 5 |

**Not covered here, by design:** inline Symbol bytes and the `.oimg` format change (stage 2),
`gw/**` and the sweep (stage 3), the panic note and loop breaker (stage 4). Each gets its own
plan.

**Type consistency:** the field order `[outBytes, outHigh, allocs, frees, fails]` is identical
in `struct OevmCHeapStats` (Task 1), the prim's five slots (Task 3), `CHeapStats>>setFromList:`
(Task 3), the `CHeap` record (Task 4) and the RECORDS row (Task 4). The C struct members are
snake_case (`out_bytes`) and the nsl/record names are camelCase (`outBytes`) — that split is
intentional and matches `OevmHeapStats`/`HeapStats`.

**Resolved during self-review:** an earlier draft invented a throwaway script to measure the
encoded size and guessed the encoder's selector. `22_spacesync/09_batch_size.nsl` already does
exactly this job with the real selector (`(cbor encode: X) byteSize`), so Step 6 extends it —
which also keeps the number asserted on every suite run rather than measured once.

**Also caught:** the new spacesync test was numbered `13_`, which `13_boot_id.nsl` already
holds; it is `14_`.
