# tuvm#21 — a posted `nil` must be distinguishable from an empty stream at `take:`

**Date:** 2026-07-15
**Issue:** tuvm#21 (P0 — "space seam: posted nil indistinguishable from empty channel at take:/dequeue")
**Status:** design approved, pre-implementation

## Problem

On the **stream** read side, `Channel>>take:` (→ prim `spaceTake:` → C `space_take`)
returns the pulled value directly. A channel that had `nil` posted to it and a channel
that has nothing queued **both answer `nil`**. Any handler that legitimately publishes
`nil` (clear-a-value, optional field) is silently conflated with an empty channel. The
code already documents the hazard (`space.c` `space_post_id`, the apologia comment at
~L223-226).

### What is NOT in scope (already correct)

Two sibling read surfaces already disambiguate and are left untouched:

- **`space_dequeue`** returns a non-NULL 3-slot `Array{value, blocksSnapshot, path}`
  whenever `q_count > 0`, and `NULL` **only** when the queue is truly empty. A posted
  `nil` rides in the array's value slot; the reactor's `Space>>drain` loop keys off
  Array-vs-nil, so it never conflates the two.
- **Cells** (`space_peek` / `Channel>>peek:ifAbsent:`) disambiguate via the `kind` byte:
  `space_kind` reports `CELL`-or-not as a **property**, so "wrote nil" (`kind == CELL`,
  `value == NULL`) is distinct from "never written" (`kind == UNSET`) by construction.
  Pinned by `nsl-tests/16_cells/01_put_peek.ns`.

The issue comment thread once feared this bug would become load-bearing for a cell's
three states (holds-nil / holds-nothing / tombstoned); rung 9 removed that — David
deleted the tombstone ("there is no clear; there is a start button and a separate stop
button, and stop always wins"), so a cell needs two states, answered by `kind`. The
residual defect is exactly `take:` and nothing more.

## Chosen approach

**Align `take:` to `space_dequeue`'s Array-or-NULL mechanism**, keeping the cell's `kind`
mechanism as-is. Rationale (decision record):

- **`take:` introduces zero new disambiguation mechanism** — it reuses the stream-side
  Array-or-NULL pattern that already ships in `space_dequeue`. The seam goes from
  "two mechanisms + one broken surface" to "two mechanisms, each matched to the question
  it answers": Array-or-NULL for stream occupancy, `kind` for cell type/ever-written.
- **The cell's `kind` is deliberately NOT unified into Array-or-NULL.** Cell "absent" is a
  *property* question (was this name ever written / is it a cell); stream "empty" is a
  *transient occupancy* question. Forcing both through one mechanism would rework shipped,
  test-pinned cell code to make two genuinely different questions look identical — false
  alignment.
- **The nsl API shape aligns with the cell anyway:** `take:ifEmpty:` reads just like
  `peek:ifAbsent:` — a block for the absent case, the value (even `nil`) otherwise — even
  though the C seam underneath differs by necessity.

### Rejected alternatives

- **Empty-sentinel singleton** (C `space_take` returns a unique non-nil marker when empty):
  zero-allocation, but a *third* distinct mechanism the codebase doesn't otherwise use —
  loses the alignment this fix is chosen for.
- **Ban `nil` as a channel payload** (error at `out:` time): pushes a runtime landmine to
  every producer; the message algebra legitimately includes null. Rejected in the issue.
- **Full unification** (rework `space_peek` to Array-or-NULL too): most uniform, but reworks
  shipped/test-pinned cell code and conflates "never written" with "empty now".

## The allocation constraint (why the wrap lives in the prim, not the C seam)

The naive reading of "reuse dequeue's mechanism" — make C `space_take` itself return an
allocated `Array` — is **wrong here**, because:

1. `test_space.c:432` documents and relies on **`space_take` never allocating**, so a
   `while (space_take(...) != NULL) {}` drain loop is stress-safe under `OEVM_GC_STRESS`.
2. Making a **pull-side read allocate** changes the seam contract: a `take:` could trigger
   GC. That is a meaningful regression for embedded / ISR-adjacent reasoning, where the
   pull side is expected to be non-allocating.

Therefore the allocation is materialized at the **prim boundary** (thread context, exactly
where `space_dequeue`'s prim already allocates), and the C `space_take` stays
non-allocating.

## Design

### C seam — `space.c` / `space.h`

`space_take` gains a NULL-able out-parameter and stays non-allocating:

```c
/* Remove-if-present-now: the oldest queued-but-undispatched value on exactly
   this channel, removed from the queue; returns the bare value (which MAY be
   nil — a legal payload). Sets *found (when non-NULL) to 1 iff an event was
   pulled, 0 if the channel was empty / never posted — this is how the caller
   tells "took a nil" from "nothing queued" (tuvm#21). Never allocates, never
   interns; an over-length path answers not-found (see space_intern's asymmetry). */
oop space_take(const char *path, size_t len, int *found);
```

Implementation: same scan as today; on a hit, `if (found) *found = 1;` before returning
the value; the two miss paths (`chan < 0`, loop falls through) set `*found = 0` and return
`NULL`.

The apologia comment in `space_post_id` (nil-is-indistinguishable) is **deleted** — a nil
payload is now fully legal and the pull side distinguishes it. A one-line note remains that
`nil` is a legal value.

### Prim — `platform.c`

`PRIM_pf_spaceTake` calls the new signature and materializes the Array-or-NULL that nsl
sees:

```c
case PRIM_pf_spaceTake: {
    /* ... existing String-path validation ... */
    int found = 0;
    oop v = space_take((const char *)p->bytes, p->length, &found);
    if (!found) { *out = NULL; return 1; }         /* empty stream -> nil */
    oop wrap = indexable_new(1);                    /* Array{value}; value may be nil */
    ((struct ObjectInstance *)wrap)->slots[1] = v;  /* v rooted: no alloc between */
    *out = wrap;
    return 1;
}
```

GC note: `v` is a bare C local across the one `indexable_new` (which can collect under
stress). It is safe **only because nothing else allocates between reading `v` and storing
it into `wrap->slots[1]`** — the single-allocation analogue of `space_dequeue`'s multi-root
discipline. (If `v` needed to survive a *second* allocation it would need a temp root; it
does not.)

### nsl — `lib/kernel.ns` (`class Channel`)

```
public take: rel = ( ^ self take: rel ifEmpty: [ nil ] )
public take: rel ifEmpty: emptyBlock = (
  | p r |
  p:: self resolve: rel.
  (platform spaceKind: p) = self KIND_CELL ifTrue: [ self error: 'not a stream' ].
  r:: platform spaceTake: p.
  r == nil ifTrue: [ ^ emptyBlock value ].
  ^ r basicAt: 1 )
```

- The prim's wrapper Array is a class-less indexable (like `spaceDequeue`'s events), read
  with **`basicAt: 1`**, not `at:`.
- Bare `take:` stays lossy-nil (`ifEmpty: [ nil ]`) — every existing `09_channels` call
  site keeps its meaning (`(space take: 'x') == nil` when empty), so nothing breaks.
- The `KIND_CELL` guard is preserved (taking from a cell is a catchable error, unchanged).

## Testing (TDD)

### nsl behavioral gate — `nsl-tests/09_channels/12_take_nil_vs_empty.ns` (new)

- Post `nil` to a stream, then `take:ifEmpty:` → yields the **nil**, does NOT run the block.
- `take:ifEmpty:` on a never-posted channel → **runs the block** (empty).
- A subsequent `take:ifEmpty:` after the queue drains → runs the block again (empty is
  transient, re-observable).
- Bare `take:` still answers `nil` for both a posted-nil and an empty channel (documents
  the retained lossy convenience).

### C seam — `tests/test_space.c` (new property)

- `space_take` sets `found = 1` for a pulled `nil` value and `found = 0` on an empty /
  never-posted channel — the C-level proof of the distinction.
- Update existing `space_take(path, len)` call sites to the new 3-arg signature (pass
  `NULL` where the found bit is not needed; bare-value asserts are otherwise unchanged).
- The stress-safe drain loop at ~L432 remains `while (space_take("a", 1, NULL) != NULL)` —
  still non-allocating.

### Regression

- Full C suite green; `test_space` clean under UBSan and ASan (the new prim allocation is
  exercised under `OEVM_GC_STRESS` via the nsl gate through Path A/B).
- nsl suite floor holds (currently 194/0/10) plus the one new `09_channels` test.

## Close-out (promotion)

- Add a CLAIMS.md row: "A posted `nil` on a stream is distinguishable from an empty
  channel — `take:ifEmpty:` yields the nil, runs the block only when empty (tuvm#21)",
  proven by `09_channels/12_take_nil_vs_empty.ns` + `test_space.c` found-bit property,
  enforced by `space_take`'s `found` out-param + the `spaceTake:` prim's Array-or-NULL wrap
  + `Channel>>take:ifEmpty:`.
- Update the `space_take` contract in `space.h`; delete the `space_post_id` apologia.
- Consider the P0→P2/P3 re-grade David floated in the issue thread (this is a latent
  correctness fix, no longer gating a rung) — decide at close.

## Files touched

- `tuvm/src/space.c`, `tuvm/src/space.h` — `space_take` signature + doc; delete apologia.
- `tuvm/src/platform.c` — `spaceTake:` prim wraps Array-or-NULL.
- `tuvm/tests/test_space.c` — found-bit property + call-site signature updates.
- `nsl-tuvm/lib/kernel.ns` — `take:ifEmpty:`, bare `take:` redefined.
- `nsl-tests/09_channels/12_take_nil_vs_empty.ns` — new gate.
- `tuvm/review/tests/CLAIMS.md` — promotion row.
