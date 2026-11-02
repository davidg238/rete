# tuvm#21 — Distinguish posted `nil` from empty stream at `take:` — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a `nil` posted to a stream distinguishable from an empty stream at the pull-side read, by giving `space_take` a `found` out-param, wrapping the result Array-or-NULL at the `spaceTake:` prim, and exposing `Channel>>take:ifEmpty:` (removing the ambiguous bare `take:`).

**Architecture:** The C `space_take` stays non-allocating and gains a NULL-able `int *found` out-param (1 = an event was pulled, even a nil value; 0 = empty). The `spaceTake:` prim materializes the nsl-visible mechanism — `Array{value}` when found, `nil` when empty — the same Array-or-NULL shape `space_dequeue` already uses, allocating only in thread context. nsl `take:ifEmpty:` unwraps it, mirroring the cell's `peek:ifAbsent:`; there is no bare `take:`.

**Tech Stack:** C99 (tuvm VM), nsl (Smalltalk-family, `lib/kernel.ns`), the `.ns` test harness (`run_tests_oevm.py`).

**Design doc:** `nsl-tuvm/docs/design/2026-07-15-tuvm21-take-nil-vs-empty-design.md`

## Global Constraints

- **Two repos:** VM/C + C tests live in `~/workspace/tuvm`; nsl lib + `.ns` design/plan docs live in `~/workspace/nsl-tuvm`; `.ns` tests live in the sibling `~/workspace/nsl-tests` (referenced as `../nsl-tests/` from `nsl-tuvm`).
- **Scope:** `Channel>>take:` / `space_take` only. Do NOT touch `space_dequeue`, `space_peek`, or the cell `kind` byte — they already disambiguate.
- **`space_take` must not allocate.** The Array-or-NULL wrap happens in the `spaceTake:` prim (thread context), never in `space_take` — preserving `test_space.c`'s stress-safe drain loop (`while (space_take(...) != NULL) {}` under `OEVM_GC_STRESS`).
- **The prim's wrapper Array is a class-less indexable** (built by `indexable_new`, like `spaceDequeue`'s events) — read on the nsl side with **`basicAt:`**, never `at:`.
- **Suite must stay green after every task** (no long red window): keep/redefine bare `take:` until its call sites are migrated, then remove it last.
- **Never run the nsl suite concurrently with a `west build`** (stale-runner phantom failures). **Always restore `solidified_image.c`** after a single-`.ns` run: `git checkout -- src/solidified_image.c src/solidified_image.stmap`.
- **`run_tests_oevm.py` has no single-test filter** — a positional arg is ignored and it runs all ~204. Use the single-`.ns` recipe (Task 2) for fast iteration; run the full suite (both paths, ~10 min) at task boundaries from the `nsl-tuvm` cwd (first line must read `Discovered ~199 …`).

---

## Reference commands

**Build+run just `test_space` (fast C iteration), from `~/workspace/tuvm`:**
```bash
gcc -std=c99 -Isrc tests/test_space.c \
  src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c \
  src/platform.c src/serial.c src/udp.c src/mirror.c src/space.c src/handles.c \
  src/backend/posix/platform_posix.c src/backend/posix/serial_posix.c \
  src/backend/posix/udp_posix.c src/backend/posix/pulse_posix.c \
  src/backend/posix/timer_posix.c src/kernel_boot.c \
  -DOEVM_SPACE_MAX_CHANNELS=8 -DOEVM_SPACE_QUEUE_DEPTH=4 -Wall -Wextra -lm -o /tmp/ts \
  && /tmp/ts ; echo "exit: $?"
```

**Full C suite, from `~/workspace/tuvm`:** `bash tools/run_c_tests.sh 2>&1 | grep -E "test_space|test_pulse|FAIL"`
(sanitizers: prefix `OEVM_BUILD=ubsan` / `OEVM_BUILD=asan`; the ASan `test_enclosures` abort is pre-existing tuvm#12, ignore it.)

**Single `.ns` (Path A), from `~/workspace/nsl-tuvm`:**
```bash
python3 tools/ns_compile_oevm.py ../nsl-tests/09_channels/12_take_nil_vs_empty.ns \
  ~/workspace/tuvm/src/solidified_image.c
( cd ~/workspace/tuvm && gcc -Wall -std=c99 -Isrc -DOEVM_APPLIANCE \
  src/vm.c src/gc.c src/dispatch.c src/platform.c src/space.c src/handles.c \
  src/serial.c src/udp.c src/mirror.c \
  src/backend/posix/platform_posix.c src/backend/posix/serial_posix.c \
  src/backend/posix/udp_posix.c src/backend/posix/pulse_posix.c \
  src/backend/posix/timer_posix.c \
  src/solidified_image.c src/oevm_main.c -o /tmp/runner -lm && /tmp/runner ; echo "exit: $?" )
( cd ~/workspace/tuvm && git checkout -- src/solidified_image.c src/solidified_image.stmap )
```

**Full nsl suite (both paths), from `~/workspace/nsl-tuvm`:**
```bash
python3 tools/run_tests_oevm.py > /tmp/nsl_suite.out 2>&1   # ~10 min; poll /tmp/nsl_suite.out; do NOT wrap in `timeout` or pipe to head/tail
```
Expected tail: `Passed: 195  Failed: 0  Skipped: 10` (194 floor + the new gate).

---

## Task 1: C seam — `space_take` gains a `found` out-param

**Files:**
- Modify: `~/workspace/tuvm/src/space.h` (contract for `space_take`)
- Modify: `~/workspace/tuvm/src/space.c` (`space_take` signature/body; delete apologia in `space_post_id`)
- Modify: `~/workspace/tuvm/src/platform.c:406` (prim call site — pass `NULL` for now, behavior unchanged)
- Test: `~/workspace/tuvm/tests/test_space.c` (new property 10 + update existing call sites)

**Interfaces:**
- Produces: `oop space_take(const char *path, size_t len, int *found)` — returns the bare oldest value on the channel (may be `NULL`, a legal nil payload); sets `*found` (when non-NULL) to `1` iff an event was pulled, `0` if the channel was empty / never posted. Never allocates, never interns.

- [ ] **Step 1: Change the signature + all call sites + docs (compiles, `*found` not yet written → the new test will fail)**

In `src/space.h`, replace the `space_take` declaration (currently ~L82-91) with:
```c
/* Remove-if-present-now (design doc section 3, D-C2): the oldest
   queued-but-undispatched value on exactly this channel, removed from the
   queue. Returns the bare value, which MAY be nil (NULL) — a legal rank-0
   payload. Sets *found (when non-NULL) to 1 iff an event was pulled, 0 if the
   channel was empty or never posted: this is how a caller tells "took a nil"
   from "nothing queued" (tuvm#21). Never allocates (a drain loop stays
   stress-safe) and never interns; an over-length path answers not-found
   (found=0, NULL) rather than aborting — it can never have been interned, so
   absent is the correct answer for a query verb, whereas the same path
   reaching space_intern via space_post/space_register aborts. The nsl-visible
   Array-or-NULL wrap is materialized in the spaceTake: prim, not here. */
oop space_take(const char *path, size_t len, int *found);
```

In `src/space.c`, replace `space_take` (currently ~L237-251) with:
```c
oop space_take(const char *path, size_t len, int *found) {
    int chan = find_channel(path, len);
    if (chan < 0) { if (found) *found = 0; return NULL; }
    for (size_t k = 0; k < q_count; k++) {
        size_t idx = (q_head + k) % OEVM_SPACE_QUEUE_DEPTH;
        if (queue[idx].live && queue[idx].chan == chan) {
            oop value = queue[idx].value;
            queue[idx].live = 0;
            queue[idx].value = NULL;   /* drop the reference promptly for GC */
            if (idx == q_head) compact_head();
            return value;              /* *found intentionally NOT set yet (Step 3) */
        }
    }
    return NULL;                       /* *found intentionally NOT set yet (Step 3) */
}
```

In `src/space.c` `space_post_id`, delete the apologia comment (currently ~L223-226) and replace with:
```c
    /* nil (NULL) is a legal rank-0 value payload; the pull side distinguishes a
       stored nil from an empty channel via space_take's `found` out-param
       (tuvm#21), so it is stored here without ceremony. */
```

In `src/platform.c:406`, change the prim call to pass `NULL` (behavior unchanged — still returns the bare value):
```c
        { *out = space_take((const char *)p->bytes, p->length, NULL); return 1; }
```

In `tests/test_space.c`, update every existing `space_take(path, len)` call to `space_take(path, len, NULL)`. The 11 sites (bare-value asserts and drain loops are otherwise unchanged): lines ~226, 228, 230, 239, 241, 357, 400, 402, 405, 433, 477. Example — line 226/228/230 become:
```c
        oop t1 = space_take("a/b", 3, NULL);
        assert(t1 == v1 && "take must return the OLDEST queued value first");
        oop t2 = space_take("a/b", 3, NULL);
        assert(t2 == v2 && "take must return the second value next");
        oop t3 = space_take("a/b", 3, NULL);
        assert(t3 == NULL && "take on a drained channel must answer nil");
```
and the two drain loops become `while (space_take("a", 1, NULL) != NULL) {}` and `while (space_take("hw/probe", 8, NULL) != NULL) { }`.

- [ ] **Step 2: Add the failing property 10, run, verify it fails**

In `tests/test_space.c`, insert immediately before `printf("test_space: OK\n");` (currently ~L482):
```c
    /* --- Property 10 (tuvm#21): a posted nil is DISTINGUISHABLE from an empty
       channel. The bare value is NULL in both cases; space_take's `found`
       out-param is the only signal. Reuses the already-interned, freshly-drained
       "hw/probe" channel — no new registry entry under MAX_CHANNELS=8. --- */
    {
        int found = -1;
        while (space_take("hw/probe", 8, NULL) != NULL) { }   /* ensure empty */

        found = -1;
        oop e = space_take("hw/probe", 8, &found);
        assert(e == NULL && found == 0 && "empty channel: NULL value AND found==0");

        found = -1;
        space_post("hw/probe", 8, NULL);                       /* post a nil */
        oop v = space_take("hw/probe", 8, &found);
        assert(v == NULL && found == 1 && "posted nil: NULL value but found==1");

        found = -1;
        assert(space_take("hw/probe", 8, &found) == NULL && found == 0
               && "after taking the nil the channel is empty again: found==0");

        space_post("hw/probe", 8, TAG_SMI(7));
        assert(space_take("hw/probe", 8, NULL) == TAG_SMI(7)
               && "a NULL found arg still returns the value");
        printf("test_space: property 10 (tuvm#21 posted-nil vs empty distinguishable) OK\n");
    }
```
Run (the `test_space` build command from Reference commands). Expected: **FAIL** — the first assertion trips because `found` stays `-1` (Step 1 left `*found` unwritten). Confirms the test exercises the new behavior.

- [ ] **Step 3: Implement `*found`, run, verify it passes**

In `src/space.c` `space_take`, set `*found` on all three exits:
```c
oop space_take(const char *path, size_t len, int *found) {
    int chan = find_channel(path, len);
    if (chan < 0) { if (found) *found = 0; return NULL; }
    for (size_t k = 0; k < q_count; k++) {
        size_t idx = (q_head + k) % OEVM_SPACE_QUEUE_DEPTH;
        if (queue[idx].live && queue[idx].chan == chan) {
            oop value = queue[idx].value;
            queue[idx].live = 0;
            queue[idx].value = NULL;
            if (idx == q_head) compact_head();
            if (found) *found = 1;
            return value;
        }
    }
    if (found) *found = 0;
    return NULL;
}
```
Run the `test_space` build command. Expected: **PASS** — `test_space: property 10 ... OK` and `test_space: OK`.

- [ ] **Step 4: Full C suite green**

Run: `bash tools/run_c_tests.sh 2>&1 | grep -E "test_space|test_pulse|FAIL"`
Expected: `test_space PASS`, `test_pulse PASS`, no `FAIL`.

- [ ] **Step 5: Commit**

```bash
cd ~/workspace/tuvm
git add src/space.h src/space.c src/platform.c tests/test_space.c
git commit -m "feat(tuvm#21): space_take gains a found out-param (posted-nil vs empty)

space_take now sets a NULL-able int *found (1 = event pulled even if the value
is nil, 0 = empty/never-posted) and stays non-allocating. Existing bare-value
callers pass NULL and are unchanged; the spaceTake: prim (next commit) uses the
flag to materialize Array-or-NULL for nsl. Deleted the space_post_id apologia.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Prim Array-or-NULL wrap + nsl `take:ifEmpty:` (bare `take:` kept, routed through it)

**Files:**
- Modify: `~/workspace/tuvm/src/platform.c` (`PRIM_pf_spaceTake` case) — note `indexable_new` is *defined* here (L94) and `struct ObjectInstance`/`slots[]` are used throughout, so no extern/include is needed
- Modify: `~/workspace/nsl-tuvm/lib/kernel.ns` (`class Channel`: add `take:ifEmpty:`, redefine bare `take:`)
- Test: `~/workspace/nsl-tests/09_channels/12_take_nil_vs_empty.ns` (new gate)

**Interfaces:**
- Consumes: `space_take(path, len, &found)` from Task 1.
- Produces (nsl-visible): `platform spaceTake: pathString` answers `Array{value}` (a class-less indexable, read with `basicAt: 1`; `value` may be nil) when an event was pulled, or `nil` when the channel is empty.
- Produces (nsl): `Channel>>take: rel ifEmpty: aBlock` — the taken value (even nil) if present, else `aBlock value`. Bare `Channel>>take: rel` is redefined as `self take: rel ifEmpty: [ nil ]`.

- [ ] **Step 1: Write the failing gate test, run, verify it fails**

Create `~/workspace/nsl-tests/09_channels/12_take_nil_vs_empty.ns`:
```smalltalk
(*
expect:
posted-nil-is-nil
empty-ran-block
never-ran-block
*)
(* tuvm#21 — a stream that had `nil` posted to it is distinguishable from an
   empty stream. `take:ifEmpty:` yields the stored nil (the block does NOT run)
   when a nil was queued, and runs the block when nothing is queued. This is the
   pull-side read; bare one-arg `take:` no longer exists (removed in a later
   task). *)
class TakeNilVsEmpty packageUsing: manifest = () (
public main: platform args: args = (
  | space r |
  space:: platform space.
  space out: 'n' value: nil.                                 (* post a genuine nil *)
  r:: space take: 'n' ifEmpty: [ 'BUG:empty-on-posted-nil' ].
  (r == nil)
    ifTrue:  [ 'posted-nil-is-nil' out ]
    ifFalse: [ 'BUG:value-not-nil' out ].
  (space take: 'n'     ifEmpty: [ 'empty-ran-block' ]) out.  (* 'n' now drained *)
  (space take: 'never' ifEmpty: [ 'never-ran-block' ]) out.  (* never posted *)
) : (
)
```
Run the single-`.ns` recipe (Reference commands, with this file's path). Expected: **FAIL / DNU** — `take:ifEmpty:` is not defined yet (does-not-understand), so output does not match the expect block. (Restore `solidified_image.c` afterward.)

- [ ] **Step 2: Change the prim to wrap Array-or-NULL**

`indexable_new(intptr_t)` is defined in this same file (L94) and `struct ObjectInstance`/`slots[]` are used throughout — no extern or include is needed. Replace the `PRIM_pf_spaceTake` case (currently ~L396-407) with:
```c
    case PRIM_pf_spaceTake: {
        /* `platform spaceTake: path` — remove-if-present-now. Answers a 1-slot
           Array{value} when an event was pulled (the value MAY be nil), or nil
           when the channel is empty — the Array-or-NULL disambiguation nsl's
           take:ifEmpty: unwraps (tuvm#21), the same shape spaceDequeue uses.
           The wrap allocates HERE (thread context), keeping space_take itself
           non-allocating. */
        oop path = args[0];
        if (path == NULL || IS_SMI(path) || StringVtable == NULL ||
            obj_vtable(path) != StringVtable) {
            fprintf(stderr, "VM Error: platform spaceTake: expects a String path\n");
            abort();
        }
        struct String *p = (struct String *)path;
        int found = 0;
        oop v = space_take((const char *)p->bytes, p->length, &found);
        if (!found) { *out = NULL; return 1; }              /* empty stream -> nil */
        oop wrap = indexable_new(1);                         /* Array{value} */
        ((struct ObjectInstance *)wrap)->slots[1] = v;       /* v rooted: no alloc between read and store */
        *out = wrap;
        return 1;
    }
```

- [ ] **Step 3: Add `take:ifEmpty:` and redefine bare `take:` in nsl**

In `~/workspace/nsl-tuvm/lib/kernel.ns`, replace the current bare `take:` method (currently ~L1259-1263) with:
```smalltalk
    public take: rel = ( ^ self take: rel ifEmpty: [ nil ] )
    public take: rel ifEmpty: emptyBlock = (
      | p r |
      p:: self resolve: rel.
      (platform spaceKind: p) = self KIND_CELL ifTrue: [ self error: 'not a stream' ].
      r:: platform spaceTake: p.
      r == nil ifTrue: [ ^ emptyBlock value ].
      ^ r basicAt: 1 )
```

- [ ] **Step 4: Run the gate, verify it passes**

Run the single-`.ns` recipe for `12_take_nil_vs_empty.ns`. Expected: **PASS** — stdout is exactly:
```
posted-nil-is-nil
empty-ran-block
never-ran-block
```
(exit 0; then restore `solidified_image.c`).

- [ ] **Step 5: Full nsl suite green (both paths)**

```bash
cd ~/workspace/nsl-tuvm
python3 tools/run_tests_oevm.py > /tmp/nsl_suite.out 2>&1
```
Poll `/tmp/nsl_suite.out`. Expected tail: `Passed: 195  Failed: 0  Skipped: 10` (bare `take:` still routes through the new path, so the existing `09_channels` tests still pass; the new gate adds one).

- [ ] **Step 6: Commit (both repos)**

```bash
cd ~/workspace/tuvm && git add src/platform.c \
  && git commit -m "feat(tuvm#21): spaceTake: prim wraps Array-or-NULL for the pull side

The prim now materializes nsl's disambiguation: Array{value} when an event was
pulled (value may be nil), nil when empty — allocating in thread context so
space_take stays non-allocating. Same mechanism as spaceDequeue.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"

cd ~/workspace/nsl-tuvm && git add lib/kernel.ns \
  && git commit -m "feat(tuvm#21): Channel>>take:ifEmpty: — distinguish nil from empty

take:ifEmpty: yields the taken value (even nil) or runs the block when the
stream is empty, mirroring peek:ifAbsent:. Bare take: is redefined as
take: rel ifEmpty: [ nil ] for now; removed once call sites migrate.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"

cd ~/workspace/nsl-tests && git add 09_channels/12_take_nil_vs_empty.ns \
  && git commit -m "test(tuvm#21): gate — posted nil vs empty stream at take:ifEmpty:

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Migrate `take:` call sites to explicit `take:ifEmpty: [ nil ]`

**Files (all in `~/workspace/nsl-tests/09_channels/`, plus one flagged straggler):**
- Modify: `01_canonicalization.ns`, `02_confinement.ns`, `03_attenuation.ns`, `04_no_ambient.ns`, `05_take_semantics.ns`, `10_take_vs_dispatch.ns`
- Flag: `~/workspace/tuvm/review/demo-node.ns:174` (David's review tree, NOT suite-run)

**Interfaces:**
- Consumes: `Channel>>take:ifEmpty:` from Task 2.

**Migration rule (uniform, behavior-preserving):** bare `take:` always returned nil on empty, and none of these tests post nil, so `X take: R` → `X take: R ifEmpty: [ nil ]` is exactly equivalent. **Every `(* expect: *)` block stays unchanged.** The lossy collapse is now explicit at each call site.

- [ ] **Step 1: Rewrite each call site**

`01_canonicalization.ns:19`:
```smalltalk
  (capB take: '' ifEmpty: [ nil ]) printString out.
```
`02_confinement.ns` lines 20, 21, 23, 24:
```smalltalk
  (((space channel: 'sensors') take: '' ifEmpty: [ nil ]) == nil) printString out.
  ((space channel: 'sensor') take: '' ifEmpty: [ nil ]) printString out.
  (((space channel: 'a/bc') take: '' ifEmpty: [ nil ]) == nil) printString out.
  ((space channel: 'a/b') take: '' ifEmpty: [ nil ]) printString out.
```
`03_attenuation.ns` lines 21, 22, 23:
```smalltalk
  (capABC take: '' ifEmpty: [ nil ]) printString out.
  (((capAB channel: 'other') take: '' ifEmpty: [ nil ]) == nil) printString out.
  (((capA channel: 'x') take: '' ifEmpty: [ nil ]) == nil) printString out.
```
`04_no_ambient.ns` lines 21, 22:
```smalltalk
  (((space channel: 'sup/anything') take: '' ifEmpty: [ nil ]) == nil) printString out.
  ((space channel: 'node7/x') take: '' ifEmpty: [ nil ]) printString out.
```
`05_take_semantics.ns` lines 19, 21, 22, 25:
```smalltalk
  ((space take: 'x' ifEmpty: [ nil ]) == nil) printString out.
  (space take: 'x' ifEmpty: [ nil ]) printString out.
  ((space take: 'x' ifEmpty: [ nil ]) == nil) printString out.
  (space take: 'y' ifEmpty: [ nil ]) printString out.
```
`10_take_vs_dispatch.ns:20`:
```smalltalk
  (a take: '' ifEmpty: [ nil ]) out.
```

- [ ] **Step 2: Migrate the flagged straggler (review tree)**

`~/workspace/tuvm/review/demo-node.ns:174`:
```smalltalk
    ((space channel: 'node7/reading') take: '' ifEmpty: [ nil ]) printString out.
```
(Not suite-run, but keeps the demo valid once bare `take:` is removed in Task 4. If preferred, leave for David to confirm — note it in the task handoff either way.)

- [ ] **Step 3: Full nsl suite green (both paths)**

```bash
cd ~/workspace/nsl-tuvm
python3 tools/run_tests_oevm.py > /tmp/nsl_suite.out 2>&1
```
Expected tail: `Passed: 195  Failed: 0  Skipped: 10` — every migrated file keeps its exact expected output (bare `take:` still exists and is equivalent, so this proves the rewrites are behavior-identical before removal).

- [ ] **Step 4: Commit (both repos)**

```bash
cd ~/workspace/nsl-tests \
  && git add 09_channels/01_canonicalization.ns 09_channels/02_confinement.ns \
             09_channels/03_attenuation.ns 09_channels/04_no_ambient.ns \
             09_channels/05_take_semantics.ns 09_channels/10_take_vs_dispatch.ns \
  && git commit -m "refactor(tuvm#21): migrate take: call sites to take:ifEmpty: [ nil ]

Behavior-identical (bare take: returned nil on empty; none of these post nil),
so every expect block is unchanged — the lossy collapse is now explicit.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"

cd ~/workspace/tuvm && git add review/demo-node.ns \
  && git commit -m "refactor(tuvm#21): demo-node take: -> take:ifEmpty: [ nil ] (bare take: removal)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Remove bare `take:` (enforce explicit empty-handling)

**Files:**
- Modify: `~/workspace/nsl-tuvm/lib/kernel.ns` (`class Channel`: remove one-arg `take:`)

**Interfaces:**
- Consumes: all call sites migrated (Task 3).

- [ ] **Step 1: Remove the one-arg method**

In `~/workspace/nsl-tuvm/lib/kernel.ns`, delete the line:
```smalltalk
    public take: rel = ( ^ self take: rel ifEmpty: [ nil ] )
```
leaving only `take: rel ifEmpty: emptyBlock`.

- [ ] **Step 2: Full nsl suite green — the completeness proof**

```bash
cd ~/workspace/nsl-tuvm
python3 tools/run_tests_oevm.py > /tmp/nsl_suite.out 2>&1
```
Expected tail: `Passed: 195  Failed: 0  Skipped: 10`. A green suite here **proves the migration was complete** — any surviving one-arg `take:` would now DNU and surface as a failure. If a file fails, grep it for a `take:` without `ifEmpty:`, migrate it, and re-run.

- [ ] **Step 3: Commit**

```bash
cd ~/workspace/nsl-tuvm && git add lib/kernel.ns \
  && git commit -m "feat(tuvm#21): remove bare take: — a stream reader must confront empty

Symmetry with the cell's forced peek:ifAbsent: (no bare peek:). take:ifEmpty:
is now the only stream read; the green suite proves every call site migrated.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Close-out — sanitizers, CLAIMS promotion, close #21

**Files:**
- Modify: `~/workspace/tuvm/review/tests/CLAIMS.md` (promotion row)

- [ ] **Step 1: Sanitizer coverage on the C seam**

```bash
cd ~/workspace/tuvm
OEVM_BUILD=ubsan bash tools/run_c_tests.sh 2>&1 | grep -E "test_space|FAIL"
OEVM_BUILD=asan  bash tools/run_c_tests.sh 2>&1 | grep -E "test_space|FAIL"
```
Expected: `test_space PASS` under both. (An ASan `test_enclosures` abort is pre-existing tuvm#12 — unrelated; do not chase it.)

- [ ] **Step 2: Promote the claim to CLAIMS.md**

In `~/workspace/tuvm/review/tests/CLAIMS.md`, add a row after the tuvm#20 overflow row (before the "Event-source seam — host timer" row):
```markdown
| — | **A posted `nil` on a stream is distinguishable from an empty channel** — `take:ifEmpty:` yields the stored nil (block NOT run) vs runs the block when nothing is queued (tuvm#21) | `nsl-tests/09_channels/12_take_nil_vs_empty.ns` (post nil → value branch yields nil; drained/never-posted → block runs) + `tuvm/tests/test_space.c` property 10 (`space_take`'s `found` out-param: 1 for a pulled nil, 0 for empty); enforced by `tuvm/src/space.c` `space_take(path,len,int*found)` (non-allocating) + `tuvm/src/platform.c` `spaceTake:` prim's Array-or-NULL wrap + `nsl-tuvm/lib/kernel.ns` `Channel>>take:ifEmpty:` (the ONLY stream read — bare `take:` removed, mirroring the cell's forced `peek:ifAbsent:`). Design: `nsl-tuvm/docs/design/2026-07-15-tuvm21-take-nil-vs-empty-design.md` | DONE |
```

- [ ] **Step 3: Commit the promotion**

```bash
cd ~/workspace/tuvm && git add review/tests/CLAIMS.md \
  && git commit -m "docs(tuvm#21): promote — posted-nil vs empty stream claim to CLAIMS.md

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

- [ ] **Step 4: Push all three repos and close the issue**

```bash
cd ~/workspace/tuvm      && git push origin master
cd ~/workspace/nsl-tuvm  && git push origin master
cd ~/workspace/nsl-tests && git push origin master
cd ~/workspace/tuvm && gh issue close 21 -c "Fixed. space_take gained a non-allocating \`found\` out-param; the spaceTake: prim materializes Array{value}-or-nil (same mechanism as spaceDequeue); Channel>>take:ifEmpty: is the only stream read (bare take: removed, mirroring the cell's forced peek:ifAbsent:). Gate: nsl-tests/09_channels/12_take_nil_vs_empty.ns + test_space.c property 10. Suite 195/0/10; UBSan/ASan clean. Not re-graded (closed as fixed). Design: nsl-tuvm/docs/design/2026-07-15-tuvm21-take-nil-vs-empty-design.md"
```
(Skip the P0 re-grade per David — the issue closes as fixed.)

- [ ] **Step 5: Update the milestone-status memory**

Record: tuvm#21 CLOSED — no P0s remain; suite floor 195/0/10; the take:/nil ambiguity is gone (`take:ifEmpty:` only, bare `take:` removed).
