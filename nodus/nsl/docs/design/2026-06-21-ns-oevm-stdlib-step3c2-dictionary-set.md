# ns-oevm std-lib step 3c-2 — Dictionary + Set (HashedCollection)

**Status:** ratified design (brainstorm w/ David, 2026-06-21). Next: writing-plans → SDD.
**Predecessor:** 3c-1 (String + universal value protocol), merged + pushed.
**Successor:** 3c-3 (Streams + Serial).
**Specs cross-ref:** `2026-06-20-ns-oevm-stdlib-design.md` (capability spine),
`2026-06-21-ns-oevm-stdlib-step3c1-string-booleans.md` (the `hash`/`=` protocol this consumes),
`docs/NEWSPEAK_DIALECT.md` (divergence governance).

---

## 1. Goal

Add `Dictionary` and `Set` to the Kernel library as the first real consumers of
the 3c-1 universal value protocol (`hash` + content `=`). Both are
open-addressing hash tables built **entirely in `lib/kernel.ns`** on top of
existing primitives — no VM/C changes, no `.oimg` layout work.

Scope is calibrated to st-zephyr embedded scripting (subset, YAGNI), consistent
with the whole std-lib effort.

---

## 2. Footprint: pure-source step (verified during brainstorm)

3c-2 touches **only** `lib/kernel.ns`, conformance tests, `docs/NEWSPEAK_DIALECT.md`,
and the regenerated committed default `tools/solidified_image.c`. No C, no
primitives, no registry, no loader, no `format_version`. Four findings establish
this:

1. **Value protocol is live.** `hash` returns SMI for `SmallInteger` (self),
   FNV-1a content hash for `String`, and falls through to overridable
   `Object>>hash` (= `identityHash`) for heap objects (`../oevm/src/dispatch.c`
   `PRIM_hash`, lines ~958-973). Content `=` for `String`, identity `=` for
   `Symbol`/booleans. Exactly what key comparison needs across all key types.

2. **Modulo needs no new prim.** The VM exposes `//` as *genuine floored*
   integer division (confirmed: tests `17_object_equality_intops`,
   `20_exact_division`; there is **no** `\\` or `bitAnd:` prim). For floored
   `//` and positive `n`, `h - (h // n * n)` is a correct bucket index in
   `0..n-1` for any sign of `h`. So hash-bucket indexing is expressible in pure
   `.ns`.

3. **Subclass-slots-over-superclass-slots is supported.** The compiler's
   `finalize_slot_layouts` computes `layout(C) = layout(super(C)) + own slots`
   to a fixpoint, order-independently and across multi-level chains
   (`tools/ns_compile_oevm.py:1206-1229`). So `Dictionary = [slots, count,
   values]` over `HashedCollection = [slots, count]` works, with inherited
   methods hitting `slots`/`count` at identical indices on both `Set` and
   `Dictionary`. (No existing class exercises this — `OrderedCollection` has
   slots but `Collection` has none — so it is a machinery point the SDD
   controller verifies first, but the mechanism is confirmed present.)

4. **No literals → nothing for Path B to serialize specially.** The grammar has
   no Dictionary/Set literal syntax, and instances are ordinary fixed-slot heap
   objects with no inline byte payload (unlike `String`). They are picked up by
   the existing generic class/vtable/method serialization on both paths; no
   `dump_layout.c` entry, no registry bump, no GC leaf case.

---

## 3. Class hierarchy

```
Collection                  (existing — abstract functional folds)
 └ HashedCollection         (NEW — abstract; open-addressing machinery)
    ├ Set                   (keys only)
    └ Dictionary            (adds a parallel values array)
```

`Set`/`Dictionary` are **real `Collection`s** — normal heap objects with an
`outer`→Kernel link — so they legitimately inherit `Collection`'s folds and reach
`platform` lexically through the `Kernel` outer chain, exactly as
`OrderedCollection` does today. (Contrast `Array`/`String`, which are bare
indexables with no `outer` link and therefore *cannot* be `Collection`s — dialect
#3.)

**Rationale for the shared abstract superclass (rejected alternatives):**
- *Set built on Dictionary* — implementation inheritance: a `Set` is not an
  `is-a Dictionary`, and every `Set` would drag an unused `values` array. LSP
  violation + wasted memory.
- *Two independent impls* — duplicates the probe/grow logic.
- *HashedCollection (chosen)* — factors the genuine commonality (the hash-table
  mechanics over a keys array) into a true abstract base; `Set` and `Dictionary`
  are siblings that each legitimately `is-a HashedCollection`.

---

## 4. Representation

- `HashedCollection` slots:
  - `slots` — the open-addressing key table, an `Array` (initial capacity **8**).
  - `count` — live-entry count, a `SmallInteger`.
- `Dictionary` adds slot:
  - `values` — an `Array` parallel to `slots` (value at index *i* belongs to the
    key at `slots` index *i*).
- **Empty-slot sentinel = `nil`.** Consequently **`nil` is forbidden as a
  key/element** — attempting to insert `nil` raises `error:`. (dialect #11)
- Capacity is **power-of-two** (8 → 16 → 32 → …).
- **Load factor 0.75**: grow (double capacity) + rehash when `count` would reach
  `3/4` of capacity.

Both arrays are allocated capability-purely via
`platform basicNew: Array indexable: cap` (never `Array new:` — see kernel.ns
header note). Slot defaults are initialized like `OrderedCollection`'s
(`slots ::= platform basicNew: Array indexable: 8. count ::= 0.`, plus
`values ::= platform basicNew: Array indexable: 8.` on `Dictionary`), so the
zero-arg `new` constructor (default `OP_NEW` + initializers) suffices — no
explicit `new:`.

---

## 5. Core mechanics (on `HashedCollection`)

All probing operates on the `slots` (keys) array, so the machinery is shared
unchanged by `Set` and `Dictionary`; `Dictionary` overrides only the points that
must also touch `values`.

- `indexFor: aKey` → `| h | h:: aKey hash. ^ (h - (h // self capacity * self
  capacity)) + 1` (bucket in `0..cap-1`, then `+1` for 1-based `Array`).
  `capacity` = `slots size`.
- `findSlotFor: aKey` → linear probe from `indexFor:`, wrapping at capacity,
  comparing `(slots at: i) = aKey`. Returns the index of the matching key **or**
  the first `nil` slot encountered (the insertion point). Sole consumer of the
  value protocol (`hash` + `=`).
- Insertion (`Set>>add:`, `Dictionary>>at:put:`): forbid `nil`; locate via
  `findSlotFor:`; if the slot already holds an equal key it is an update
  (Dictionary writes `values`; Set is a no-op); otherwise store the key
  (Dictionary also stores the value), bump `count`, and grow if over load factor.
- `grow` → allocate 2× key array (and 2× values array for Dictionary), re-insert
  every live entry by fresh probe into the new arrays, then swap.
- **Removal = backward-shift deletion (Knuth Algorithm R).** After clearing the
  matched slot, walk forward shifting back any entry whose ideal bucket lies at
  or before the hole, until a `nil` slot is reached. No tombstones; probe chains
  stay compact — preferred for long-lived embedded tables. Dictionary shifts the
  parallel `values` entry alongside each key.
- `size` overrides `Collection`'s O(n) fold to return `count`.
- `isEmpty` → `count = 0`.
- `do:` is **abstract** on `HashedCollection` (subclassResponsibility): `Set`
  iterates elements, `Dictionary` iterates values.

---

## 6. Set API

| Selector | Behaviour |
|---|---|
| `add: e` | forbid `nil`; insert `e` if absent (dedup no-op if present); answer `e`. |
| `addAll: aColl` | `aColl do: [:e | self add: e]`; answer `aColl`. |
| `remove: e` | backshift-remove `e`; `error:` if absent. |
| `remove: e ifAbsent: aBlock` | remove, else `^ aBlock value`. |
| `includes: e` | hash lookup (overrides `Collection`'s O(n) `do:`-scan). |
| `do: aBlock` | iterate live elements (skip `nil` slots). |
| `size` / `isEmpty` | inherited from `HashedCollection`. |
| `asArray` | inherited from `Collection` (`do:` + `size` already defined). |

Inherited `Collection` folds (`inject:into:`/`collect:`/`select:`/`reject:`/
`detect:`) run over the elements via `do:`.

---

## 7. Dictionary API

| Selector | Behaviour |
|---|---|
| `at: k put: v` | forbid `nil` key; insert/update; answer `v`. |
| `at: k` | answer the value; `error: 'key not found'` if absent. |
| `at: k ifAbsent: aBlock` | answer the value, else `^ aBlock value`. |
| `at: k ifAbsentPut: aBlock` | answer the value, else insert `aBlock value` under `k` and answer it. |
| `includesKey: k` | hash lookup → boolean. |
| `removeKey: k` | backshift-remove; `error:` if absent; answer the removed value. |
| `removeKey: k ifAbsent: aBlock` | remove (answer value), else `^ aBlock value`. |
| `keys` | fresh `Array` (size `count`) of the keys, filled by live-slot scan. |
| `values` | fresh `Array` (size `count`) of the values. |
| `keysAndValuesDo: aBlock` | `aBlock value: key value: value` per live entry. |
| `do: aBlock` | iterate **values** (Smalltalk convention). |
| `size` / `isEmpty` | inherited. |

Inherited `Collection` folds run over **values** (via `do:`). Consequently
`collect:`/`select:` answer an `OrderedCollection`, **not** a `Dictionary`
(dialect #11); `includes:` (inherited) tests **value** membership (Smalltalk
convention), distinct from `includesKey:`.

`keys`/`values` return **`Array`** (fixed snapshot, sized exactly to `count`, one
allocation). Rationale: size is known up front, so `OrderedCollection`'s
growability is pure overhead; the snapshot is iterated and discarded, not
appended to; and it mirrors `Collection>>asArray`. The only cost — the snapshot
is not appendable (`keys add:` → DNU) — is a non-use in practice and reversible.

---

## 8. Kernel accessors

Add hand-written accessors (dialect #6 pattern) at the bottom of `Kernel`:

```
public Set        = ( ^ Set )
public Dictionary = ( ^ Dictionary )
```

`HashedCollection` remains an internal abstract with **no** accessor (exposing an
abstract base buys nothing; reversible if a real subclassing need appears). The
app constructs instances via `platform Set new` / `platform Dictionary new`.

---

## 9. Dialect doc — new row #11

Add to `docs/NEWSPEAK_DIALECT.md` (append-only):

> **#11 — `Dictionary`/`Set` are open-addressing `HashedCollection`s with a
> reduced protocol (std-lib step 3c-2).** Full Newspeak/Smalltalk: `Dictionary`
> stores `Association`s, `Set`/`Dictionary` answer rich protocol, `select:`
> answers same-species. ns-oevm: open-addressing tables over a `platform`-
> allocated `Array` (linear probe, load factor 0.75, backshift deletion); **no
> `Association` class** (`associationsDo:`/`associations` absent — keys and
> values live in parallel arrays, no per-entry object); **`nil` is forbidden as a
> key/element** (it is the empty-slot sentinel); `Dictionary>>do:` iterates
> values; inherited `collect:`/`select:` answer an `OrderedCollection`, not a
> `Dictionary`; `keys`/`values` answer an `Array`. *Why:* parallel arrays + open
> addressing avoid per-entry allocation/fragmentation at embedded scope, and the
> reduced protocol is the YAGNI subset st-zephyr scripting needs. *Revisit:* a
> real `Association`/same-species-`select:`/nil-key need.

---

## 10. Testing (conformance `.ns`, expect-output; ~57 → ~61 both paths)

All under `../ns-tests/04_somns_basic/`. Each is `(* expect: ... *)`-matched and
must pass `OEVM_PATH=AB` (both paths). Run from `ns-oevm` cwd:
`python3 tools/run_tests_oevm.py`.

- **`34_dictionary.ns`** — full Dictionary API: `at:put:`/`at:`/`at:ifAbsent:`/
  `at:ifAbsentPut:`/`includesKey:`/`removeKey:`/`keys`/`values`/`keysAndValuesDo:`/
  `do:`/`size`/`isEmpty`. **Mixed key types** in one dictionary — `String`
  (content-keyed), `Symbol` (identity), `SmallInteger` (self) — exercising the
  value protocol. **Collision + grow**: insert > 8 entries to force at least one
  rehash and assert all survive. **Key update** (re-`at:put:` an existing key
  changes value, not `size`). **`removeKey:` then re-insert** (backshift
  correctness: a key whose probe chain crossed the hole is still findable).
- **`35_set.ns`** — `add:`/dedup (adding an equal element twice leaves `size`
  unchanged)/`addAll:`/`remove:`/`remove:ifAbsent:`/`includes:`/`do:`/`size`/
  `isEmpty`/`asArray`. Grow past capacity 8. Mixed element types.
- **String-key value-protocol integration** (in `34` or its own file) — `d at:
  'foo' put: 1`, then retrieve with a **different** `'foo'` literal instance and
  get `1` (content equality, not identity). The headline real-world validation of
  3c-1's content `hash`/`=`. Also assert a `String` key and an equal-named
  `Symbol` key do **not** collide (distinct `hash`/`=`).

Regenerate the committed default `tools/solidified_image.c` from one of the new
tests (e.g. `34_dictionary.ns`), per the 3b/3c-1 precedent.

---

## 11. SDD shape (finalized in writing-plans)

Pure-source means no Path-A-then-Path-B split (contrast 3c-1's
A-flip-then-B-serialize). Provisional ~2 tasks, each implementer→reviewer, then an
opus whole-branch review:

1. **`HashedCollection` + `Set`** — abstract machinery (`indexFor:`/`findSlotFor:`/
   `grow`/backshift-remove/`size`/`isEmpty`/abstract `do:`) + `Set` concrete API +
   `Set` accessor + `35_set.ns`. Controller verifies subclass-slots-over-
   superclass-slots up front (§2 finding 3) with a smoke insert/grow before the
   full API.
2. **`Dictionary`** — parallel `values` array + full Dictionary API + accessor +
   `34_dictionary.ns` + string-key integration assertions + dialect #11 +
   regenerated default `solidified_image.c`.

Suite target: **57 → ~61 pass, 0 fail, 3 skip, both paths.**

---

## 12. Open / deferred (recheck later)

- **Bounded per-target variant.** The growable table here is the Linux/default
  variant; a fixed-capacity Zephyr variant (no grow; `error:`/evict on full) is
  the per-target `platform`-binding seam (capability spine §3). Deferred until a
  Zephyr target is actually built — same protocol, different boot-time binding.
- **`Association`/same-species `select:`/nil keys** — dialect #11 revisit
  triggers; not needed at current scope.
- **`hash` quality.** Relies on 3c-1's FNV-1a (String) + identity/self (others);
  no secondary hashing/perturbation. Adequate for embedded table sizes; revisit
  only if pathological clustering is observed.
</content>
</invoke>
