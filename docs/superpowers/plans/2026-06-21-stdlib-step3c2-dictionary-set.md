# Std-lib step 3c-2 — Dictionary + Set Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `Dictionary` and `Set` to the Kernel library as open-addressing hash
tables built entirely in `lib/kernel.ns`, consuming the 3c-1 `hash`/`=` value
protocol.

**Architecture:** A new abstract `HashedCollection` (subclass of `Collection`)
owns the addressing/probe machinery over a `table` (keys) `Array`; `Set` uses it
as-is, `Dictionary` adds a parallel `vals` `Array`. Linear probing, load factor
0.75, cluster-rehash deletion (no tombstones). Pure source — no C/VM/`.oimg`
changes.

**Tech Stack:** Newspeak (`lib/kernel.ns`), the ns-oevm compiler
(`tools/ns_compile_oevm.py`), the OEVM C VM (sibling repo `../oevm`), conformance
tests (`../ns-tests`).

**Spec:** `docs/superpowers/specs/2026-06-21-ns-oevm-stdlib-step3c2-dictionary-set.md`

## Global Constraints

- **THREE sibling repos.** `ns-oevm` (compiler/lib/docs), `../oevm` (C VM),
  `../ns-tests` (conformance `.ns`). Commit per-repo; trailer required; **push
  only on explicit say-so**.
- **Run conformance from `ns-oevm` cwd:** `python3 tools/run_tests_oevm.py`
  (default `OEVM_PATH=AB` runs both paths). The runner has **no single-test
  filter** — run the full suite and read the relevant line(s).
- **Inside `lib/kernel.ns` NEVER write `Array new: n`** (compiles to fixed-slot
  `OP_NEW`, not an indexable). Allocate indexables via
  `platform basicNew: Array indexable: n` (`Array` is a sibling-slot read).
- **Tests are expect-output matched** (`(* expect: ... *)` diffed vs stdout).
  Iteration order over a hash table is **not** insertion order — assert only
  order-independent observations (sizes, membership, `at:` lookups, summed folds).
- **Subagents locate edits by QUOTED CODE**, not line numbers.
- Baseline suite: **57 pass / 0 fail / 3 skip, both paths.** Target after 3c-2:
  **59 pass / 0 fail / 3 skip, both paths.**

## Controller recon — already verified (do NOT re-litigate)

These were settled by reading/running the code during planning; the code blocks
below depend on them:

1. **Inherited slot initializers do NOT run for the bare `= Super` form.**
   `compile_initializer` step 0 only fires when a `superInit` keyword clause is
   present (grammar `SuperInit = (keyword BinarySend)+`); `ns_compile_oevm.py`
   ~1585. **Workaround (verified by running a 3-level hierarchy):** the abstract
   `HashedCollection` declares slot *names only* (`| table count |`) so its
   methods resolve them; each concrete subclass **re-declares `table`/`count`
   with initializers** (and `Dictionary` adds `vals`). `finalize_slot_layouts`
   (`ns_compile_oevm.py:1206-1229`) merges layouts to a fixpoint, so inherited
   methods read the child-initialized slots at identical indices.
2. **`whileTrue:` / `whileFalse:` are supported** (`../oevm/src/dispatch.c:1258`)
   as `[cond] whileTrue: [body]` (returns nil). Loops below use them.
3. **No `\\` / `bitAnd:` prim; `//` is genuine floored division.** Modulo =
   `h - (h // c * c)`, correct and non-negative for any sign of `h`.
4. **Pure source.** Dictionary/Set are ordinary fixed-slot heap objects, no
   literals → Path B serializes them via the existing generic class machinery; no
   `dump_layout.c`/registry/loader/`format_version` change. `../oevm` changes only
   in the regenerated committed default image (Task 2).

---

## Task 1: HashedCollection (abstract) + Set

**Files:**
- Modify: `lib/kernel.ns` (insert two classes after `OrderedCollection`, before
  the `public Object … OrderedCollection` accessor block; add a `Set` accessor)
- Create: `../ns-tests/04_somns_basic/35_set.ns`

**Interfaces:**
- Consumes: `Collection` (folds), `platform basicNew: Array indexable: n`,
  `Array` (`at:`/`at:put:`/`size`), the value protocol (`hash`, `=`).
- Produces (for Task 2): abstract `HashedCollection` with `capacity`, `size`,
  `isEmpty`, `indexFor:`, `slotFor:`, `containsKey:`, `atCapacity`,
  `growIfNeeded`, `rehashFrom:`, and the subclass hooks `grow` /
  `reinsertEntryAt:` / `do:`. `Dictionary` (Task 2) subclasses `HashedCollection`
  and overrides those hooks.

- [ ] **Step 1: Create feature branches in all three repos**

```bash
cd /home/david/workspace/ns-oevm && git checkout -b feat/stdlib-step3c2
cd /home/david/workspace/ns-tests && git checkout -b feat/stdlib-step3c2
cd /home/david/workspace/oevm && git checkout -b feat/stdlib-step3c2
cd /home/david/workspace/ns-oevm
```

- [ ] **Step 2: Write the failing test `35_set.ns`**

Create `../ns-tests/04_somns_basic/35_set.ns`:

```
(*
expect:
true
3
false
true
false
6
2
false
4
absent
20
true
210
20
20
3
true
true
true
false
*)
class SetTest packageUsing: manifest = () (
public main: platform args: args = (
    | s s2 s3 |
    s:: platform Set new.
    s isEmpty println.                              (* true  *)
    s add: 1. s add: 2. s add: 3. s add: 2.         (* last is a dup *)
    s size println.                                 (* 3     *)
    s isEmpty println.                              (* false *)
    (s includes: 2) println.                        (* true  *)
    (s includes: 9) println.                        (* false *)
    (s inject: 0 into: [ :a :e | a + e ]) println.  (* 6     *)
    s remove: 2.
    s size println.                                 (* 2     *)
    (s includes: 2) println.                        (* false *)
    (s inject: 0 into: [ :a :e | a + e ]) println.  (* 4     *)
    s remove: 9 ifAbsent: [ 'absent' println ].     (* absent *)
    1 to: 20 do: [ :i | s add: i ].                 (* forces grow; union w/ {1,3} *)
    s size println.                                 (* 20    *)
    (s includes: 17) println.                       (* true  *)
    (s inject: 0 into: [ :a :e | a + e ]) println.  (* 210   *)
    s2:: platform Set new. s2 addAll: s.
    s2 size println.                                (* 20    *)
    (s asArray) size println.                       (* 20    *)
    s3:: platform Set new.
    s3 add: 'foo'. s3 add: #bar. s3 add: 42.
    s3 size println.                                (* 3     *)
    (s3 includes: 'foo') println.                   (* true  — content equality *)
    (s3 includes: #bar) println.                    (* true  *)
    (s3 includes: 42) println.                      (* true  *)
    (s3 includes: #foo) println.                    (* false — Symbol #foo /= String 'foo' *)
)
) : (
)
```

- [ ] **Step 3: Run the suite; verify `35_set.ns` FAILS**

Run: `python3 tools/run_tests_oevm.py`
Expected: `35_set` FAILS (Set is not yet defined → `platform Set` is a
`doesNotUnderstand`). All 57 prior tests still pass.

- [ ] **Step 4: Implement `HashedCollection` + `Set` in `lib/kernel.ns`**

In `lib/kernel.ns`, find the end of the `OrderedCollection` class — the line:

```
  ) : (
  )

  public Object            = ( ^ Object )
```

Insert the two classes **between** `OrderedCollection`'s closing `) : ( )` and
the `public Object` accessor line, so the new block reads:

```
  class HashedCollection = Collection ( | table count | ) (
    (* Abstract open-addressing hash table over `table` (the keys/elements
       Array); `count` is the live-entry tally. Concrete subclasses re-declare
       `table`/`count` WITH initializers to set the initial capacity — the bare
       `= Super` inheritance form does not run an ancestor's slot initializers,
       and a per-target bounded variant would pick a different capacity here. *)
    capacity = ( ^ table size )
    public size = ( ^ count )
    public isEmpty = ( ^ count = 0 )

    (* 1-based home bucket for aKey. Floored `//` gives a correct non-negative
       modulo for any sign of hash; the value protocol supplies `hash`. *)
    indexFor: aKey = ( | h c | h:: aKey hash. c:: self capacity. ^ (h - (h // c * c)) + 1 )

    (* Linear probe from the home bucket (wrapping) to the slot holding aKey OR
       the first empty (nil) slot. Load factor < 1 guarantees an empty slot, so
       this terminates. The `and:` short-circuits, so `=` is only sent to a
       non-nil key. *)
    slotFor: aKey = ( | c i k |
      c:: self capacity. i:: self indexFor: aKey. k:: table at: i.
      [ k notNil and: [ (k = aKey) not ] ] whileTrue: [
        i:: i + 1. i > c ifTrue: [ i:: 1 ]. k:: table at: i ].
      ^ i )

    containsKey: aKey = ( ^ (table at: (self slotFor: aKey)) notNil )

    atCapacity = ( ^ count * 4 >= (table size * 3) )           (* load factor 0.75 *)
    growIfNeeded = ( self atCapacity ifTrue: [ self grow ]. ^ self )

    (* Cluster-rehash deletion (no tombstones): after a slot is vacated, walk the
       contiguous run that follows and re-place each entry, pulling forward any
       entry that had probed past the hole. Stops at the first empty slot. *)
    rehashFrom: start = ( | c j |
      c:: self capacity. j:: start + 1. j > c ifTrue: [ j:: 1 ].
      [ (table at: j) notNil ] whileTrue: [
        self reinsertEntryAt: j.
        j:: j + 1. j > c ifTrue: [ j:: 1 ] ].
      ^ self )

    grow = ( ^ self error: 'subclassResponsibility' )
    reinsertEntryAt: j = ( ^ self error: 'subclassResponsibility' )
    public do: aBlock = ( ^ self error: 'subclassResponsibility' )
  ) : (
  )

  class Set = HashedCollection ( | table ::= platform basicNew: Array indexable: 8. count ::= 0. | ) (
    public add: anObject = ( anObject isNil ifTrue: [ ^ self error: 'nil element not allowed' ]. self place: anObject. self growIfNeeded. ^ anObject )
    public addAll: aCollection = ( aCollection do: [ :e | self add: e ]. ^ aCollection )
    public remove: anObject = ( ^ self remove: anObject ifAbsent: [ self error: 'element not found' ] )
    public remove: anObject ifAbsent: aBlock = ( | i |
      i:: self slotFor: anObject.
      (table at: i) isNil ifTrue: [ ^ aBlock value ].
      table at: i put: nil. count:: count - 1. self rehashFrom: i. ^ anObject )
    public includes: anObject = ( ^ self containsKey: anObject )
    public do: aBlock = ( 1 to: self capacity do: [ :i | | e | e:: table at: i. e isNil ifFalse: [ aBlock value: e ] ]. ^ self )
    place: anObject = ( | i | i:: self slotFor: anObject. (table at: i) isNil ifTrue: [ table at: i put: anObject. count:: count + 1 ]. ^ self )
    reinsertEntryAt: j = ( | e | e:: table at: j. table at: j put: nil. count:: count - 1. self place: e. ^ self )
    grow = ( | old oldCap | old:: table. oldCap:: table size. table:: platform basicNew: Array indexable: oldCap * 2. count:: 0. 1 to: oldCap do: [ :i | | e | e:: old at: i. e isNil ifFalse: [ self place: e ] ]. ^ self )
  ) : (
  )

```

Then add a `Set` accessor in the accessor block at the bottom of `Kernel`. Find:

```
  public OrderedCollection = ( ^ OrderedCollection )
```

and insert immediately after it:

```
  public Set               = ( ^ Set )
```

- [ ] **Step 5: Run the suite; verify `35_set.ns` PASSES**

Run: `python3 tools/run_tests_oevm.py`
Expected: **58 pass / 0 fail / 3 skip, both paths.** Confirm `35_set` PASSES on
**both** Path A and Path B (the runner labels each). If output mismatches,
compare expected vs actual lines — every assertion is order-independent, so a
mismatch is a real logic bug, not iteration order.

- [ ] **Step 6: Commit (per repo)**

```bash
cd /home/david/workspace/ns-tests
git add 04_somns_basic/35_set.ns
git commit -m "test(stdlib): 35_set.ns — Set conformance (3c-2 task 1)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
cd /home/david/workspace/ns-oevm
git add lib/kernel.ns
git commit -m "feat(stdlib): HashedCollection + Set (3c-2 task 1)

Abstract open-addressing HashedCollection<Collection (linear probe, load
factor 0.75, cluster-rehash deletion) + Set (keys only). Pure source;
consumes the 3c-1 hash/= value protocol. Suite 57 -> 58 both paths.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Dictionary + dialect row + default image

**Files:**
- Modify: `lib/kernel.ns` (insert `Dictionary` after `Set`; add a `Dictionary`
  accessor)
- Modify: `docs/NEWSPEAK_DIALECT.md` (append row #11)
- Create: `../ns-tests/04_somns_basic/34_dictionary.ns`
- Modify (regenerate): `../oevm/src/solidified_image.c` (committed default image)

**Interfaces:**
- Consumes: everything `HashedCollection` produces in Task 1 (`slotFor:`,
  `containsKey:`, `growIfNeeded`, `rehashFrom:`, `size`, `isEmpty`); `Collection`
  folds; `platform basicNew: Array indexable:`.
- Produces: `Dictionary` with `at:put:`, `at:`, `at:ifAbsent:`,
  `at:ifAbsentPut:`, `includesKey:`, `removeKey:`, `removeKey:ifAbsent:`, `keys`,
  `values`, `keysAndValuesDo:`, `do:` (over values).

- [ ] **Step 1: Write the failing test `34_dictionary.ns`**

Create `../ns-tests/04_somns_basic/34_dictionary.ns`:

```
(*
expect:
true
3
2
1
true
false
3
22
-1
1
4
4
4
30
22
3
false
no
8
3
3
30
289
900
9455
465
9455
9455
3
100
200
300
*)
class DictionaryTest packageUsing: manifest = () (
public main: platform args: args = (
    | d g m ksum vsum dsum |
    d:: platform Dictionary new.
    d isEmpty println.                                  (* true  *)
    d at: 'one' put: 1. d at: 'two' put: 2. d at: 'three' put: 3.
    d size println.                                     (* 3     *)
    (d at: 'two') println.                              (* 2     *)
    (d at: 'one') println.                              (* 1     *)
    (d includesKey: 'three') println.                   (* true  *)
    (d includesKey: 'nope') println.                    (* false *)
    d at: 'two' put: 22.                                (* update, not insert *)
    d size println.                                     (* 3     *)
    (d at: 'two') println.                              (* 22    *)
    (d at: 'missing' ifAbsent: [ -1 ]) println.         (* -1    *)
    (d at: 'one' ifAbsent: [ -1 ]) println.             (* 1     *)
    (d at: 'four' ifAbsentPut: [ 4 ]) println.          (* 4     *)
    (d at: 'four') println.                             (* 4     *)
    d size println.                                     (* 4     *)
    (d inject: 0 into: [ :a :v | a + v ]) println.      (* 30  — folds over values *)
    (d removeKey: 'two') println.                       (* 22  — removed value *)
    d size println.                                     (* 3     *)
    (d includesKey: 'two') println.                     (* false *)
    (d removeKey: 'x' ifAbsent: [ 'no' ]) println.      (* no    *)
    (d values inject: 0 into: [ :a :v | a + v ]) println.  (* 8  = 1+3+4 *)
    (d values) size println.                            (* 3     *)
    (d keys) size println.                              (* 3     *)
    g:: platform Dictionary new.
    1 to: 30 do: [ :i | g at: i put: i * i ].           (* forces grow *)
    g size println.                                     (* 30    *)
    (g at: 17) println.                                 (* 289   *)
    (g at: 30) println.                                 (* 900   *)
    (g inject: 0 into: [ :a :v | a + v ]) println.      (* 9455 = sum i^2, 1..30 *)
    ksum:: 0. vsum:: 0.
    g keysAndValuesDo: [ :k :v | ksum:: ksum + k. vsum:: vsum + v ].
    ksum println.                                       (* 465  = sum 1..30 *)
    vsum println.                                       (* 9455 *)
    dsum:: 0. g do: [ :v | dsum:: dsum + v ]. dsum println.  (* 9455 — do: over values *)
    m:: platform Dictionary new.
    m at: 'k' put: 100. m at: #k put: 200. m at: 7 put: 300.
    m size println.                                     (* 3 — String 'k' /= Symbol #k *)
    (m at: 'k') println.                                (* 100 — content key *)
    (m at: #k) println.                                 (* 200 *)
    (m at: 7) println.                                  (* 300 *)
)
) : (
)
```

- [ ] **Step 2: Run the suite; verify `34_dictionary.ns` FAILS**

Run: `python3 tools/run_tests_oevm.py`
Expected: `34_dictionary` FAILS (`platform Dictionary` → DNU). `35_set` and the
57 baseline tests still pass (58 pass total besides the new failure).

- [ ] **Step 3: Implement `Dictionary` in `lib/kernel.ns`**

In `lib/kernel.ns`, find the closing of the `Set` class added in Task 1:

```
    grow = ( | old oldCap | old:: table. oldCap:: table size. table:: platform basicNew: Array indexable: oldCap * 2. count:: 0. 1 to: oldCap do: [ :i | | e | e:: old at: i. e isNil ifFalse: [ self place: e ] ]. ^ self )
  ) : (
  )

```

Insert the `Dictionary` class immediately after `Set`'s closing `) : ( )`:

```
  class Dictionary = HashedCollection ( | table ::= platform basicNew: Array indexable: 8. count ::= 0. vals ::= platform basicNew: Array indexable: 8. | ) (
    public at: aKey put: aValue = ( aKey isNil ifTrue: [ ^ self error: 'nil key not allowed' ]. self place: aKey value: aValue. self growIfNeeded. ^ aValue )
    public at: aKey ifAbsent: aBlock = ( | i | i:: self slotFor: aKey. (table at: i) isNil ifTrue: [ ^ aBlock value ]. ^ vals at: i )
    public at: aKey = ( ^ self at: aKey ifAbsent: [ self error: 'key not found' ] )
    public at: aKey ifAbsentPut: aBlock = ( | i | i:: self slotFor: aKey. (table at: i) notNil ifTrue: [ ^ vals at: i ]. ^ self at: aKey put: aBlock value )
    public includesKey: aKey = ( ^ self containsKey: aKey )
    public removeKey: aKey = ( ^ self removeKey: aKey ifAbsent: [ self error: 'key not found' ] )
    public removeKey: aKey ifAbsent: aBlock = ( | i v |
      i:: self slotFor: aKey.
      (table at: i) isNil ifTrue: [ ^ aBlock value ].
      v:: vals at: i. table at: i put: nil. count:: count - 1. self rehashFrom: i. ^ v )
    public keys = ( | arr j | arr:: platform basicNew: Array indexable: count. j:: 0. 1 to: self capacity do: [ :i | | k | k:: table at: i. k isNil ifFalse: [ j:: j + 1. arr at: j put: k ] ]. ^ arr )
    public values = ( | arr j | arr:: platform basicNew: Array indexable: count. j:: 0. 1 to: self capacity do: [ :i | | k | k:: table at: i. k isNil ifFalse: [ j:: j + 1. arr at: j put: (vals at: i) ] ]. ^ arr )
    public keysAndValuesDo: aBlock = ( 1 to: self capacity do: [ :i | | k | k:: table at: i. k isNil ifFalse: [ aBlock value: k value: (vals at: i) ] ]. ^ self )
    public do: aBlock = ( 1 to: self capacity do: [ :i | | k | k:: table at: i. k isNil ifFalse: [ aBlock value: (vals at: i) ] ]. ^ self )
    place: aKey value: aValue = ( | i | i:: self slotFor: aKey. (table at: i) isNil ifTrue: [ table at: i put: aKey. count:: count + 1 ]. vals at: i put: aValue. ^ self )
    reinsertEntryAt: j = ( | k v | k:: table at: j. v:: vals at: j. table at: j put: nil. count:: count - 1. self place: k value: v. ^ self )
    grow = ( | oldK oldV oldCap | oldK:: table. oldV:: vals. oldCap:: table size. table:: platform basicNew: Array indexable: oldCap * 2. vals:: platform basicNew: Array indexable: oldCap * 2. count:: 0. 1 to: oldCap do: [ :i | | k | k:: oldK at: i. k isNil ifFalse: [ self place: k value: (oldV at: i) ] ]. ^ self )
  ) : (
  )

```

Then add the accessor. Find the `Set` accessor added in Task 1:

```
  public Set               = ( ^ Set )
```

and insert immediately after it:

```
  public Dictionary        = ( ^ Dictionary )
```

- [ ] **Step 4: Run the suite; verify `34_dictionary.ns` PASSES**

Run: `python3 tools/run_tests_oevm.py`
Expected: **59 pass / 0 fail / 3 skip, both paths.** Confirm `34_dictionary` and
`35_set` both PASS on Path A and Path B.

- [ ] **Step 5: Append dialect row #11 to `docs/NEWSPEAK_DIALECT.md`**

In `docs/NEWSPEAK_DIALECT.md`, the deviations table ends with row `| 10 |
**`String`** … |`. Add a new row immediately after row 10 (keep the table format
— five `|`-separated cells):

```
| 11 | **`Dictionary`/`Set` are open-addressing `HashedCollection`s with a reduced protocol** (std-lib step 3c-2) | `Dictionary` stores `Association`s; `Set`/`Dictionary` carry rich protocol; `select:` answers same-species. | Open-addressing tables over a `platform`-allocated `Array` (linear probe, load factor 0.75, cluster-rehash deletion). **No `Association` class** (`associationsDo:`/`associations` absent — keys and values live in parallel arrays, no per-entry object); **`nil` is forbidden as a key/element** (it is the empty-slot sentinel — insertion raises `error:`); `Dictionary>>do:` iterates **values**; inherited `collect:`/`select:` answer an `OrderedCollection`, not a `Dictionary`; `keys`/`values` answer an `Array`. | Parallel arrays + open addressing avoid per-entry allocation/fragmentation at embedded scope, and the reduced protocol is the YAGNI subset st-zephyr scripting needs. | A real `Association` / same-species `select:` / nil-key need. |
```

- [ ] **Step 6: Regenerate the committed default solidified image**

The conformance runner overwrites `../oevm/src/solidified_image.c` on every Path
A run; the committed default should be a representative current image. Regenerate
it from `34_dictionary.ns` (matching the 3b/3c-1 precedent):

```bash
cd /home/david/workspace/ns-oevm
python3 tools/ns_compile_oevm.py ../ns-tests/04_somns_basic/34_dictionary.ns ../oevm/src/solidified_image.c
```

Expected: `Compilation and solidification successful`.

- [ ] **Step 7: Final full-suite confirmation**

Run: `python3 tools/run_tests_oevm.py`
Expected: **59 pass / 0 fail / 3 skip, both paths.** (Re-running regenerates the
per-test images, so this also confirms the regenerated default compiles and the
VM is unaffected.)

- [ ] **Step 8: Commit (per repo)**

```bash
cd /home/david/workspace/ns-tests
git add 04_somns_basic/34_dictionary.ns
git commit -m "test(stdlib): 34_dictionary.ns — Dictionary conformance + value-protocol keys (3c-2 task 2)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
cd /home/david/workspace/oevm
git add src/solidified_image.c src/solidified_image.stmap
git commit -m "chore(stdlib): regenerate default solidified image from 34_dictionary (3c-2 task 2)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
cd /home/david/workspace/ns-oevm
git add lib/kernel.ns docs/NEWSPEAK_DIALECT.md
git commit -m "feat(stdlib): Dictionary + dialect #11 (3c-2 task 2)

Dictionary<HashedCollection with parallel vals array: at:put:/at:/at:ifAbsent:/
at:ifAbsentPut:/includesKey:/removeKey:/removeKey:ifAbsent:/keys/values/
keysAndValuesDo:/do:(values). No Association class, nil-forbidden keys,
keys/values return Array. Dialect row #11. Suite 58 -> 59 both paths.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## After both tasks

- [ ] **Opus whole-branch review** (per 3c-1 precedent): dispatch an opus review
  of the full `feat/stdlib-step3c2` diff across all three repos before merge.
  Focus areas: probe termination (load factor invariant guarantees a nil slot),
  cluster-rehash deletion correctness, the value protocol across mixed key types,
  and the `nil`-key/element guards.
- [ ] **Merge + push only on David's say-so** (memory: push only when asked).
  Merge order mirrors prior steps; delete `feat/stdlib-step3c2` branches after.

## Self-review (against the spec)

- **Spec coverage:** §3 hierarchy → Tasks 1+2 (`Collection`→`HashedCollection`→
  `Set`/`Dictionary`). §4 representation (`table`/`count`/`vals`, nil sentinel,
  power-of-two, load 0.75) → Task 1 `HashedCollection` + concrete slot decls.
  §5 mechanics (`indexFor:`/`slotFor:`/`grow`/cluster-rehash/`size`) → Task 1.
  §6 Set API → Task 1 `Set` + `35_set.ns`. §7 Dictionary API → Task 2 +
  `34_dictionary.ns`. §8 accessors → Task 1/2 accessor steps (HashedCollection
  intentionally not exposed). §9 dialect #11 → Task 2 Step 5. §10 testing →
  `35_set.ns`/`34_dictionary.ns` incl. mixed-type + String-content-key
  integration. §11 SDD shape (2 tasks, no Path-A/B split) → this plan.
- **Deletion vs spec §5 "backshift":** the plan implements cluster-rehash
  deletion — the same tombstone-free, O(cluster) guarantee, expressed with the
  available control flow and easier to review. Documented as such.
- **Placeholder scan:** none — every step has concrete code/commands/expected
  output. `grow`/`reinsertEntryAt:`/`do:` are `subclassResponsibility` on the
  abstract by design (overridden in both concretes).
- **Type/selector consistency:** abstract hooks (`grow`, `reinsertEntryAt:`,
  `do:`) are overridden in both `Set` and `Dictionary`; `place:` (Set) vs
  `place:value:` (Dictionary) are distinct selectors; the value-array slot is
  `vals` (not `values`) to avoid clashing with the `values` accessor method;
  `containsKey:`/`slotFor:`/`indexFor:`/`capacity`/`size`/`isEmpty`/`atCapacity`/
  `growIfNeeded`/`rehashFrom:` are defined once on `HashedCollection` and used by
  both subclasses.
```
