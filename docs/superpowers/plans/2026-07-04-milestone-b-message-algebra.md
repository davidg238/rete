# Milestone B — Message Algebra Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The closed deeply-immutable 10-type value algebra (List/Map/Record/Bytes + reused leaves) as nsl kernel classes, with construction-time validation, canonical value order, typed structural equality/hash, Association sugar, and the T-B conformance suite.

**Architecture:** nsl-first per the design spec — kernel classes over private backing Arrays (Newspeak slot privacy IS immutability); C work is leaf-only (byte storage, three Integer bitops, one `valueRank:` prim, six predicate fast-paths, one delegate-wiring fix). No new pointer-bearing heap layouts, zero image-format work.

**Tech Stack:** nsl (`nsl-tuvm/lib/kernel.ns`), C99 (`tuvm/src/dispatch.c`, `platform.c`, `builtins.def`), conformance harness (`nsl-tuvm/tools/run_tests_oevm.py`), corpus (`nsl-tests/08_messages/`).

**Spec:** `docs/superpowers/specs/2026-07-04-milestone-b-message-algebra-design.md` — normative for all semantics. Staged references: `tuvm/review/lib/messages.md`, `tuvm/review/lib/bytes.ns`, `tuvm/review/tests/tb-messages/`.

## Global Constraints

- Frozen crash strings (exact): `not a value`, `message depth exceeded`, `index out of bounds`, `byte out of range`. Mutation on algebra types is DNU (no string — the selector must not exist).
- Depth bound: **32**. Leaf depth = 0; container = 1 + max child depth; depth cached in a slot.
- Value order type ranks: `nil`=0, Boolean=1, Integer=2, Float=3, String=4, Bytes=5, Symbol=6, List=7, Map=8, Record=9. **NaN is not a value** (rank −1 at the C prim).
- Typed structural equality: values of different ranks are never equal (Integer 1 ≠ Float 1.0 in the algebra); leaf-level `1 = 1.0` behavior is untouched.
- Association is constructor sugar, **not** a value.
- All corpus tests must pass BOTH backends (Path A solidified + Path B .oimg). Single-file check: `bash <scratchpad>/br.sh <file>` for Path A; `python3 tools/ns_compile_oevm.py <file> /tmp/t.oimg --format=oimg --layout=/tmp/oevm_layout.json && /tmp/oevm_img /tmp/t.oimg` for Path B (build `/tmp/oevm_img` per Task 1 Step 0 note).
- The conformance harness is NOT concurrency-safe: run suites strictly foreground/sequential with Bash timeout ≤ 600000 ms; never start a second run while one is going.
- Repos/branches: work on branch `milestone-b` in `tuvm`, `nsl-tuvm`, `nsl-tests` (create from master at Task 1). Commit per task. NO merge to master, NO push — David's word required.
- Per David's SDD flow: implementer subagents are **sonnet**; the controller re-runs gates independently.

## File Structure (locked by the design spec)

- `tuvm/src/dispatch.c` — Integer bitop prims; predicate fast-paths; RawBytes at/at:put:/size fast-paths (Tasks 1, 3, 4)
- `tuvm/src/platform.c` + `src/builtins.def` — `basicNewBytes:`/`freezeBytes:`/`valueRank:` prims; RawBytesVtable builtin (Tasks 3, 4)
- `nsl-tuvm/tools/builtins_registry.py` — mirror of builtins.def (Task 3)
- `nsl-tuvm/lib/kernel.ns` — Association, module-level value helpers, List, Map, Record, ByteArray, Bytes, accessors (Tasks 5–10)
- delegate-wiring fix location found by Task 2's investigation — RESOLVED: no delegate exists anywhere (all builtin vtables hardcode NULL); the fix landed as a String entry in dispatch.c's isNil/notNil per-type switch, and `->` moved to a compile-time rewrite (see Task 5 amendment)
- `nsl-tests/08_messages/` — the whole T-B corpus (Tasks 1–11)

---

### Task 1: Integer bitops — `bitAnd:`, `bitOr:`, `>>`

**Files:**
- Modify: `tuvm/src/dispatch.c` (PRIM enum ~line 602, selector-string table ~line 626, case arm next to `PRIM_bitXor` at ~line 843)
- Test: `nsl-tests/08_messages/00_int_bitops.ns`

**Interfaces:**
- Produces: Integer `bitAnd:`, `bitOr:`, `>>` selectors, semantics identical in shape to existing `bitXor:`/`<<` (defined for the values register math uses: non-negative operands ≤ 2^62−1; behavior for negatives follows whatever `bitXor:` already does — replicate, don't redesign).

- [ ] **Step 0 (once per session): create branches + Path B binary**

```bash
for r in tuvm nsl-tuvm nsl-tests; do (cd ~/workspace/$r && git checkout -b milestone-b); done
cd ~/workspace/tuvm && gcc -w -std=c99 -Isrc src/vm.c src/gc.c src/dispatch.c src/platform.c src/serial.c src/udp.c src/mirror.c src/backend/posix/platform_posix.c src/backend/posix/serial_posix.c src/backend/posix/udp_posix.c src/oimg_loader.c src/kernel_boot.c oevm_image_main.c -o /tmp/oevm_img -lm
```

- [ ] **Step 1: Write the failing corpus test**

Create `nsl-tests/08_messages/00_int_bitops.ns`:

```
(*
expect:
8
14
1
2147483648
255
*)
(* Milestone B Task 1 — Integer bitops the algebra hash and bytes.ns register
   helpers need. bitXor:/<< already exist; this adds bitAnd:/bitOr:/>>. *)
class IntBitops packageUsing: manifest = () (
public main: platform args: args = (
  ((12 bitAnd: 10) printString) out.        (* 8 *)
  ((12 bitOr: 6) printString) out.          (* 14 *)
  ((5 >> 2) printString) out.               (* 1 *)
  ((4294967296 >> 1) printString) out.      (* 2^32 >> 1 = 2147483648 *)
  ((4294967295 bitAnd: 255) printString) out.  (* u32 max masked *)
)
) : (
)
```

- [ ] **Step 2: Run to verify it fails** — Path A via `br.sh`; expect `Message 'bitAnd:' not understood` crash.

- [ ] **Step 3: Implement.** Read the existing `case PRIM_bitXor` arm (dispatch.c ~line 843) and its enum/string rows first. Add `PRIM_bitAnd, PRIM_bitOr, PRIM_shr` to the enum row at ~602 and `"bitAnd:", "bitOr:", ">>"` at the same positions in the string table at ~626 (positions must correspond 1:1 — that is the table's contract). Add three case arms that are byte-for-byte the `PRIM_bitXor` arm with the operator changed (`&`, `|`, and for `>>` an arithmetic shift mirroring `PRIM_shl`'s shape). Do not extend semantics beyond what bitXor/shl already handle.

- [ ] **Step 4: Verify** — test passes Path A AND Path B. Then full C suite: `bash tools/run_c_tests.sh` (19 PASS / 2 SKIP unchanged).

- [ ] **Step 5: Commit** in both repos: `feat(int): bitAnd:/bitOr:/>> primitives (Milestone B work order)`.

---

### Task 2: Fix the String→Object delegation gap

**Files:**
- Investigate then modify: the delegate wiring — grep `delegate` in `nsl-tuvm/tools/ns_compile_oevm.py`, `tools/solidify.py`, `tuvm/src/kernel_boot.c` to find where IntegerVtable/SymbolVtable/FloatVtable get their kernel-Object delegate; StringVtable is missing the same wire.
- Test: `nsl-tests/08_messages/00b_string_delegation.ns`

**Interfaces:**
- Produces: String receivers resolve kernel-Object nsl methods (`notNil`, and later `->`, `valueRank`). Verified fact driving this task: `5 notNil`, `#k notNil`, `3.5 notNil`, LargeInteger `notNil` all answer; `'s' notNil` is DNU today — String alone lacks the delegate.

- [ ] **Step 1: Failing test** — `00b_string_delegation.ns`:

```
(*
expect:
true
false
*)
class StringDeleg packageUsing: manifest = () (
public main: platform args: args = (
  ('s' notNil) printString out.
  ('s' isNil) printString out.
)
) : (
)
```

- [ ] **Step 2: Verify it fails** both paths (DNU `notNil`).
- [ ] **Step 3: Find + fix the wire.** Locate how the other builtin vtables acquire the kernel Object delegate (compiler-emitted patch or boot wiring); apply the identical mechanism to StringVtable. If the gap turns out intentional (a comment says so), STOP and report to the controller instead of forcing it.
- [ ] **Step 4: Verify** — test green both paths; **full conformance suite** (String dispatch is hot — regression risk): expect all prior tests still green.
- [ ] **Step 5: Commit**: `fix(dispatch): StringVtable delegates to kernel Object like every other builtin`.

---

### Task 3: Raw byte storage — builtin vtable + 5 platform primitives

**Files:**
- Modify: `tuvm/src/builtins.def` (append RawBytesVtable row — follow the LargeIntegerVtable row at index 16 as the pattern; take the next free index), `tuvm/src/kernel_boot.c` (const Vtable + registry entry, mirroring `Vtable_LargeIntegerVtable`), `tuvm/src/dispatch.c` (PRIM_at / new PRIM_at_put fast path / PRIM_size for RawBytes receivers, next to the existing String PRIM_at fast path at ~1162; plus `PRIM_pf_basicNewBytes`/`PRIM_pf_freezeBytes` enum+string rows), `tuvm/src/platform.c` (two prim cases, modeled on `PRIM_pf_string` at ~121)
- Modify: `nsl-tuvm/tools/builtins_registry.py` (mirror the new builtin row — keep registry_version consistent; run `python3 tools/test_gen_builtins.py` if present)
- Test: `tuvm/tests/test_raw_bytes.c` + run-line in `tools/run_c_tests.sh`; corpus smoke in Task 10.

**Interfaces:**
- Produces (consumed by Task 10's kernel classes):
  - `platform basicNewBytes: n` → raw byte object, n zero bytes, RawBytesVtable, String C layout (`struct String` reused: length + bytes; pointer-free leaf — GC already sweeps this shape).
  - `raw at: i` → SMI 0..255 (1-based; C bounds-check crashes `index out of bounds`).
  - `raw at: i put: v` → writes byte (C checks: index → `index out of bounds`, value 0..255 → `byte out of range`). **Only** RawBytesVtable receivers — a String receiver must NOT gain at:put:.
  - `raw size` → byte count.
  - `platform freezeBytes: raw` → fresh RawBytes copy (freeze-is-copy per spec §6).

- [ ] **Step 1: Failing C unit test** `tuvm/tests/test_raw_bytes.c` (follow `tests/test_heap_stats.c` idiom: heap init, asserts, PASS lines). Properties: alloc n=8 zero-filled; write/read roundtrip byte 1 and byte 8; out-of-bounds index and out-of-range value abort (fork-and-expect-abort pattern — copy from whichever existing C test checks aborts, or assert via the documented VM error path); freezeBytes yields a distinct object with equal bytes; writing the original does not change the frozen copy.
- [ ] **Step 2: Verify fail** — add run-line, `bash tools/run_c_tests.sh` shows the new test FAIL (missing symbols).
- [ ] **Step 3: Implement** per the interface block above. builtins.def row + kernel_boot mirror + dispatch fast paths + platform cases + `builtins_registry.py` sync.
- [ ] **Step 4: Verify** — C suite all green; full conformance suite both paths (builtin-table change touches image loading — must prove Path B unharmed); ASAN/UBSan run of test_raw_bytes.
- [ ] **Step 5: Commit** both repos: `feat(bytes): RawBytes leaf storage + basicNewBytes:/freezeBytes:/at:/at:put:/size (Milestone B)`.

---

### Task 4: `platform valueRank:` prim + predicate fast-paths for builtin receivers

**Files:**
- Modify: `tuvm/src/dispatch.c` (PRIM_pf_valueRank enum+string; six predicate selectors as pre-lookup fast paths for builtin receivers ONLY, modeled on the String PRIM_at fast path), `tuvm/src/platform.c` (valueRank case)
- Test: `nsl-tests/08_messages/00c_value_rank.ns`

**Interfaces:**
- Produces:
  - `platform valueRank: v` → SMI: nil→0; true/false→1; SMI/LargeInteger→2; Float→3 **but NaN→−1** (C: `v != v`); String→4; Symbol→6; RawBytes→−2 (internal, never a value); any other heap object→**−1** (meaning: ask the object in nsl).
  - Selectors `isValue`, `isSymbol`, `isList`, `isMap`, `isRecord`, `isBytes` answered in C for **builtin** receivers only (Symbol: isSymbol→true, isValue→true, others false; nil/Boolean/Integer/Float/String: isValue→true, the five type predicates→false; NaN Float: isValue→false). Heap instances fall through to normal nsl lookup (Object defaults land in Task 5).

- [ ] **Step 1: Failing corpus test** `00c_value_rank.ns`:

```
(*
expect:
2
6
4
true
true
false
false
*)
class ValueRank packageUsing: manifest = () (
public main: platform args: args = (
  (platform valueRank: 5) printString out.
  (platform valueRank: #k) printString out.
  (platform valueRank: 'k') printString out.
  (#k isSymbol) printString out.
  (5 isValue) printString out.
  (5 isSymbol) printString out.
  ('k' isList) printString out.
)
) : (
)
```

- [ ] **Step 2: Verify fail** both paths (`valueRank:` DNU on platform / `isSymbol` DNU).
- [ ] **Step 3: Implement** per interface block. The predicate fast paths go in the same pre-vtable-lookup switch that hosts the String `PRIM_at` case; key strictly on builtin vtables so heap instances fall through.
- [ ] **Step 4: Verify** green both paths + full suite.
- [ ] **Step 5: Commit**: `feat(algebra): valueRank: prim + builtin predicate fast-paths (Milestone B)`.

---

### Task 5: Association, `->`, and Object algebra defaults

> **AMENDED 2026-07-04 (David's decision after Task 2's premise correction — spec §Association
> sugar has the full decision record):** there is NO builtin→kernel-Object delegation (Task 2
> proved all builtin vtables hardcode `delegate = NULL`; `5 ifNil: [0]` DNUs). `->` is therefore
> a COMPILE-TIME REWRITE (`a -> b` → `Association key: a value: b` at the emit site, precedent:
> Milestone A's `detect:ifNone:` rewrite), NOT an Object method. Object algebra defaults still
> land in kernel.ns but serve HEAP-INSTANCE receivers only; builtin receivers get predicates/rank
> from Task 4's C fast-paths.

**Files:**
- Modify: `nsl-tuvm/lib/kernel.ns` — Object class (~line 18) + new sibling class + module accessors (~line 280)
- Modify: `nsl-tuvm/tools/ns_compile_oevm.py` — the `->` rewrite at the emit site (find the `detect:ifNone:` rewrite from Milestone A Task 2 as the pattern; both codegen paths). Investigate how compiled code references kernel module classes (how do existing sends reach kernel globals?) — if no clean reference path to Association exists from an arbitrary send site, STOP and escalate rather than inventing one.
- Test: `nsl-tests/08_messages/01_association.ns`

**Interfaces:**
- Produces (consumed by Tasks 6–10):
  - `Association` kernel class: `key`, `value` accessors; class-side `key:value:`.
  - `a -> b` rewritten at compile time to `Association key: a value: b` (receiver-agnostic — works for builtin AND heap receivers; no `Object>>->` is defined).
  - Object defaults (heap instances): `isValue = ( ^ false )`, `isSymbol/isList/isMap/isRecord/isBytes = ( ^ false )`, `valueRank = ( ^ -1 )`, `valueDepth = ( ^ 0 )`.
  - Module accessor `public Association = ( ^ Association )`.

- [ ] **Step 1: Failing test** `01_association.ns`:

```
(*
expect:
k
42
7
false
*)
class AssocTest packageUsing: manifest = () (
public main: platform args: args = (
  | a b |
  a:: #k -> 42.
  a key printString out.
  a value printString out.
  b:: 3 -> 7.                (* Integer receiver — compile-time rewrite, no dispatch *)
  b value printString out.
  (a isValue) printString out.   (* Association is NOT a value *)
)
) : (
)
```

- [ ] **Step 2: Verify fail** both paths (`->` DNU).
- [ ] **Step 3: Implement.** In ns_compile_oevm.py: the `->` emit-site rewrite (per the amendment above). In kernel.ns, Object additions (NO `->` method — the rewrite replaces it):

```
    public isValue = ( ^ false )
    public isSymbol = ( ^ false )
    public isList = ( ^ false )
    public isMap = ( ^ false )
    public isRecord = ( ^ false )
    public isBytes = ( ^ false )
    public valueRank = ( ^ -1 )
    public valueDepth = ( ^ 0 )
```

New sibling class (place after Dictionary):

```
  class Association = ( | k v | ) (
    public key = ( ^ k )
    public value = ( ^ v )
    public printString = ( ^ k printString , ' -> ' , v printString )
  ) : (
    public key: aKey value: aValue = ( | a | a:: self new. a setKey: aKey value: aValue. ^ a )
  )
```

(If the codebase's initialization idiom differs — check how Dictionary/Set instances initialize slots, `::=` defaults vs setter methods — follow that idiom exactly; the corpus's `04_initializers.ns` shows the pattern. Symbol/Integer receivers for `->` work because the rewrite never dispatches `->` at all; note `printString` on Symbol/String operands must already exist — it does, the corpus uses it.)

- [ ] **Step 4: Verify** both paths.
- [ ] **Step 5: Commit** (nsl-tuvm + nsl-tests): `feat(kernel): Association + -> sugar + Object algebra defaults (Milestone B)`.

---

### Task 6: List + module-level validation helpers

**Files:**
- Modify: `nsl-tuvm/lib/kernel.ns` (module-level helpers + List class + accessor)
- Test: `nsl-tests/08_messages/02_list.ns`, `03_list_guards.ns`

**Interfaces:**
- Produces:
  - Module-level (NOT public — lexically visible to sibling classes only):
    ```
    rankOf: v = ( | r | r:: platform valueRank: v. r = -1 ifTrue: [ ^ v valueRank ]. ^ r )
    isValueObject: v = ( ^ (rankOf: v) >= 0 )
    depthOf: v = ( (rankOf: v) >= 7 ifTrue: [ ^ v valueDepth ]. ^ 0 )
    checkValue: v = ( (isValueObject: v) ifFalse: [ self error: 'not a value' ]. ^ v )
    checkDepth: d = ( d > 32 ifTrue: [ self error: 'message depth exceeded' ]. ^ d )
    ```
    (Confirm `self error:` is the house crash idiom — grep kernel.ns/corpus for `error:`; if the idiom differs, e.g. a primitive crash call, use that. The crash text must be EXACTLY the frozen string.)
  - `List` class: instance `size`, `at:` (1-based, bounds-crash `index out of bounds`), `at:ifAbsent:`, `do:`, `inject:into:`, `anySatisfy:`, `isList = (^true)`, `isValue = (^true)`, `valueRank = (^7)`, `valueDepth = (^depth slot)`; class-side `with:`, `with:with:`, `with:with:with:`, `with:with:with:with:`, `fromCollection:`.
  - Backing: private slots `| items depth |`; `items` is a plain Array copied at construction, never returned, never stored from a caller-visible alias.

- [ ] **Step 1: Failing tests.** `02_list.ns` (happy path):

```
(*
expect:
3
two
6
1
true
*)
class ListBasics packageUsing: manifest = () (
public main: platform args: args = (
  | l L |
  L:: platform List.
  l:: L with: 1 with: 'two' with: #three.
  l size printString out.
  (l at: 2) out.
  (l inject: 0 into: [ :a :e | (platform valueRank: e) = 2 ifTrue: [ a + e ] ifFalse: [ a ] ]) printString out.
  ((L with: (L with: 9)) valueDepth) printString out.   (* nested: depth 1... outer is depth 1? outer=1+inner(1)=2 — see Step 1 note *)
  (l isList) printString out.
)
) : (
)
```

  **Depth arithmetic note (normative, from the spec):** leaf depth 0; `List with: 9` → depth 1; `List with: (List with: 9)` → depth 2. Fix the expected line above accordingly when writing the real file: the fourth expectation is `2`, and the third expectation sums integer elements = `1`. Work the expected outputs out BY HAND before running; the test is the oracle, not the implementation.

  `03_list_guards.ns` (crashes — two separate files since each crash ends the program; follow this template for the second):

```
(*
expect:
ok
expect-runtime-fail: not a value
*)
class ListGuardValue packageUsing: manifest = () (
public main: platform args: args = (
  | oc |
  'ok' out.
  oc:: platform OrderedCollection new.
  oc add: 7.
  platform List with: 1 with: oc.
  'unreached' out.
)
) : (
)
```

  and `03b_list_depth_guard.ns`: build depth 32 by looping `v:: L with: v` 32 times from leaf `0` (prints 32 via valueDepth), then one more `with:` → `expect-runtime-fail: message depth exceeded`.

- [ ] **Step 2: Verify all fail** both paths (List DNU on platform).
- [ ] **Step 3: Implement.** List construction validates each element via `checkValue:`, computes `depth:: 1 + (max over children of depthOf:)` then `checkDepth:`; copies elements into a fresh Array. `fromCollection:` iterates the argument with `do:` copying into the private Array — validation identical.
- [ ] **Step 4: Verify** both paths, all three files.
- [ ] **Step 5: Commit**: `feat(kernel): List — immutable algebra list + validation/depth helpers (Milestone B)`.

---

### Task 7: Value order, typed equality, structural hash

**Files:**
- Modify: `nsl-tuvm/lib/kernel.ns` (module-level `valueCompare:with:`, `valueHashOf:`; List `=` and `hash` overrides)
- Test: `nsl-tests/08_messages/04_typed_equality.ns`

**Interfaces:**
- Produces (consumed by Map/Record):
  - `valueCompare: a with: b` → −1/0/1 total order: rank compare first; rank-equal → within-type: Integer/Float via `<`; String/Symbol byte-wise via `at:` loop (shorter-is-less on prefix tie); Bytes via `u8At:` loop (Task 10 — until then the Bytes arm reads `at:` on the raw slot; write the arm when Bytes lands, leave a dispatch on rank 5 that crashes `not a value` until Task 10 replaces it — no, simpler: Task 10 adds the arm; until then rank 5 cannot occur since Bytes doesn't exist yet); List element-wise recursive, shorter-is-less; Map over sorted entries (key then value, pairwise); Record tag first then fields-Map compare.
  - `value: a equals: b = ( ^ (valueCompare: a with: b) = 0 )`.
  - `valueHashOf: v` → FNV-style fold: leaves → existing `hash` XOR'd with rank; containers → fold `h:: ((h bitXor: childHash) * 16777619) bitAnd: 1073741823` seeded from rank, over children in canonical order (List order; Map sorted entries; Record tag hash then fields).
  - List: `public = other` → `(other isList) and: [value: self equals: other]`-shaped (guard non-values/foreign types → false, never crash); `public hash = ( ^ valueHashOf: self )`.

- [ ] **Step 1: Failing test** `04_typed_equality.ns`:

```
(*
expect:
true
false
true
false
true
*)
class TypedEq packageUsing: manifest = () (
public main: platform args: args = (
  | L a b c |
  L:: platform List.
  a:: L with: 1 with: 2.
  b:: L with: 1 with: 2.
  c:: L with: 1.0 with: 2.
  (a = b) printString out.               (* structural, cross-construction *)
  (a = c) printString out.               (* typed: Integer 1 /= Float 1.0 in the algebra *)
  (a hash = b hash) printString out.
  (a = (L with: 1)) printString out.     (* length differs *)
  ((1 = 1.0)) printString out.           (* leaf numeric equality UNCHANGED *)
)
) : (
)
```

- [ ] **Step 2: Verify fail** (List `=` currently identity → first line false).
- [ ] **Step 3: Implement** per interface block. Recursion is depth-bounded by construction (≤32) — no cycle guard needed (spec: acyclicity by construction).
- [ ] **Step 4: Verify** both paths.
- [ ] **Step 5: Commit**: `feat(kernel): value order + typed structural =/hash for the algebra (Milestone B)`.

---

### Task 8: Map — sorted association array

**Files:**
- Modify: `nsl-tuvm/lib/kernel.ns` (Map class + accessor; extend `valueCompare:with:`/`valueHashOf:` Map arms if stubbed)
- Test: `nsl-tests/08_messages/05_map.ns`, `05b_map_canonical_order.ns`, `05c_map_typed_keys.ns`, `03c_map_depth_guard.ns`

**Interfaces:**
- Produces:
  - `Map` class over private `| keys vals depth |` (two parallel Arrays, entries sorted by `valueCompare:with:` on keys — parallel arrays, not Association storage: Associations die at the constructor).
  - Class-side: `fromPairs: aList` (each element must be a 2-element algebra List OR an Association — accept both; Association path copies key/value out), `with:` ×1..4 taking Associations.
  - Instance: `size`, `at:ifAbsent:` (binary search by valueCompare), `keysDo:`, `do:` (over values), `keysAndValuesDo:`, `isMap = (^true)`, `valueRank = (^8)`, `isValue = (^true)`, `valueDepth`, `=`, `hash` (via module helpers).
  - Duplicate keys in construction: **last wins** (pin it — CBOR decoders overwrite; record in a comment).
  - Keys and values both validated (`checkValue:`); depth = 1 + max over keys and values.

- [ ] **Step 1: Failing tests.** `05_map.ns`:

```
(*
expect:
2
42
0
true
true
*)
class MapBasics packageUsing: manifest = () (
public main: platform args: args = (
  | M m m2 |
  M:: platform Map.
  m:: M with: #k -> 42 with: #j -> 7.
  m size printString out.
  (m at: #k ifAbsent: [ 0 ]) printString out.
  (m at: #zzz ifAbsent: [ 0 ]) printString out.
  m2:: M fromPairs: (platform List with: (#j -> 7) with: (#k -> 42)).
  (m = m2) printString out.              (* sugar == fromPairs, order-independent *)
  (m hash = m2 hash) printString out.
)
) : (
)
```

  `05b_map_canonical_order.ns`: construct the same 3-symbol-key map in two insertion orders; `keysDo:` prints identical sequences (byte-lexicographic symbol order) — 6 expect lines.
  `05c_map_typed_keys.ns`: `(M with: 1 -> 'a') at: 1.0 ifAbsent: [ 'miss' ]` → `miss`; both keys coexist: `M with: 1 -> 'a' with: 1.0 -> 'b'` has size 2.
  `03c_map_depth_guard.ns`: map-nesting depth guard (mirror `03b`, nesting via values).

- [ ] **Step 2: Verify fail** both paths.
- [ ] **Step 3: Implement.** Construction: validate, copy into parallel arrays, insertion-sort by `valueCompare:with:` (messages are small — no need for anything smarter), enforce last-wins on compare==0, compute/check depth. `at:ifAbsent:` binary search.
- [ ] **Step 4: Verify** all four files, both paths.
- [ ] **Step 5: Commit**: `feat(kernel): Map — sorted immutable algebra map (Milestone B)`.

---

### Task 9: Record

**Files:**
- Modify: `nsl-tuvm/lib/kernel.ns` (Record class + accessor; Record arms in compare/hash)
- Test: `nsl-tests/08_messages/06_record.ns`

**Interfaces:**
- Produces: `Record` over `| tag fields depth |` — `tag` must be a Symbol (else `not a value`), `fields` a Map (else `not a value`). Class-side `tag:fields:`, `tag:with:` ×1..4 (association sugar building the Map internally). Instance: `tag`, `fields`, `at:ifAbsent:` (delegates to fields), `isRecord = (^true)`, `valueRank = (^9)`, `isValue = (^true)`, `valueDepth = fields depth + 1`, `=` (tag first, then fields), `hash`.

- [ ] **Step 1: Failing test** `06_record.ns`:

```
(*
expect:
reading
42
0
true
false
*)
class RecordBasics packageUsing: manifest = () (
public main: platform args: args = (
  | R r r2 r3 |
  R:: platform Record.
  r:: R tag: #reading with: #seq -> 42.
  r tag printString out.
  (r at: #seq ifAbsent: [ 0 ]) printString out.
  (r at: #zz ifAbsent: [ 0 ]) printString out.
  r2:: R tag: #reading fields: (platform Map with: #seq -> 42).
  (r = r2) printString out.
  r3:: R tag: #other with: #seq -> 42.
  (r = r3) printString out.               (* tag differs *)
)
) : (
)
```

- [ ] **Step 2: Verify fail**; **Step 3: Implement**; **Step 4: Verify both paths**.
- [ ] **Step 5: Commit**: `feat(kernel): Record — tagged immutable record (Milestone B)`.

---

### Task 10: ByteArray + Bytes kernel classes

**Files:**
- Modify: `nsl-tuvm/lib/kernel.ns` (shared read protocol per staged bytes.ns, ByteArray, Bytes, accessors; Bytes arm in `valueCompare:with:`/`valueHashOf:` — byte-wise)
- Test: `nsl-tests/08_messages/07_bytes.ns`, `07b_bytes_guards.ns`

**Interfaces:**
- Consumes: Task 3's `platform basicNewBytes:/freezeBytes:` + raw `at:/at:put:/size`.
- Produces: per staged `tuvm/review/lib/bytes.ns`, adapted to the private-raw-slot shape: `ByteArray` (mutable: `u8At:`, `u8At:put:`, `u16At:`/`u16At:put:` LE, `u16BeAt:`/`u16BeAt:put:`, `byteSize`, `freeze`, class-side `new:`, `fromList:`) — **not a value** (no isValue override → Object false); `Bytes` (same read protocol, `freeze = (^self)`, `asByteArray`, `isBytes = (^true)`, `isValue = (^true)`, `valueRank = (^5)`, `valueDepth = (^0)`, byte-wise `=`/`hash` via module helpers). Multi-byte accessors compose on `u8At:` in nsl; bounds/range crashes come from the C layer with the frozen strings.

- [ ] **Step 1: Failing tests.** `07_bytes.ns`: build a 3-byte ByteArray (`u8At:put:` + `u16BeAt:put:` — the demo-node TMP117 pattern), freeze, read back all bytes from the Bytes, mutate the original ByteArray, prove the frozen copy unchanged, put Bytes in a List (isValue true), print `(b = b2) printString` for two equal freezes. `07b_bytes_guards.ns` + `07c`: unfrozen ByteArray into a List → `expect-runtime-fail: not a value`; `u8At: 99` → `expect-runtime-fail: index out of bounds`; `u8At: 1 put: 300` → `expect-runtime-fail: byte out of range` (three files — one crash each).
- [ ] **Step 2: Verify fail**; **Step 3: Implement** (follow staged bytes.ns structure; storage = private raw slot from `platform basicNewBytes:`); **Step 4: Verify both paths**.
- [ ] **Step 5: Commit**: `feat(kernel): ByteArray workbench + Bytes algebra value (Milestone B)`.

---

### Task 11: Land the staged T-B suite + variants

**Files:**
- Create: `nsl-tests/08_messages/10_depth_bound.ns` … `16_mutable_boundary_*.ns` from `tuvm/review/tests/tb-messages/*` (renumbered; headers mechanically renamed `expect-crash:` → `expect-runtime-fail:`; bare class references qualified via `platform`), plus `12_mutation_matrix/` generated files and `14_generative.ns` (the seeded-LCG T-B.5 test, 200 trees, depth ≤ 4 — port the staged `05_generative_roundtrip.ns` completing its generator against the landed API), `16b/c/d` variants (user object / ByteArray / Dictionary nested deep).
- Create: `nsl-tests/08_messages/12_mutation_selectors.txt` — the selector list (`List at:put:`, `List add:`, `Map at:put:`, `Map removeKey:`, `Record at:put:`, `Bytes u8At:put:`) the matrix files are generated from, one small `.ns` per row (DNU crash: `expect-runtime-fail: <selector>` — the DNU message text contains the selector, per the staged 02 test).
- Verify first: `grep -n "expect_runtime_fail\|expect-runtime-fail" nsl-tuvm/tools/run_tests_oevm.py` — semantics = program exits nonzero, error text contains the substring, AFTER preceding `expect:` lines matched. If any staged test needs semantics the harness lacks, patch the harness TDD-style and note it in the commit.

- [ ] **Step 1:** Land + adapt each staged file; run each individually BOTH paths; fix expectations only where the staged file's assumed spelling differs from the landed API (semantics changes are FORBIDDEN — they're the spec's).
- [ ] **Step 2:** Full conformance suite both paths — everything green.
- [ ] **Step 3: Commit**: `test(messages): land T-B suite — depth/mutation-matrix/aliasing/cycle/generative/boundary (Milestone B)`.

---

### Task 12: Gates, review-dir sync, close-out

**Files:**
- Modify: `tuvm/review/demo-node.ns`, `tuvm/review/lib/*.ns` (mechanical: sugar surface `Map with: #k -> v …`, platform-qualified class refs), `tuvm/tools/footprint-after.txt` (re-bank)
- Verify-only: full gates.

- [ ] **Step 1:** Controller-grade verification: full conformance suite both paths (expect ~105+/0); C suite (20 PASS incl. test_raw_bytes / 2 SKIP); ASAN/UBSan spot-runs (test_raw_bytes + one algebra-heavy corpus file compiled `-fsanitize=address,undefined`); `oevm_heap_stats` sanity on the generative test (no runaway growth — M2 counters exist, use them); footprint re-bank with honest deltas (engine text: bitops + raw-bytes + fast-paths only; kernel growth lands in the image object, excluded by design).
- [ ] **Step 2:** Mechanical review-dir sync (demo-node.ns publish sites to sugar; staged libs' assumed-API notes updated to point at landed spellings).
- [ ] **Step 3:** Update `nsl-tuvm/.superpowers/sdd/progress.md` ledger; final commits all three repos. Report to David: proof-line status (§4B: nested construction ✓ / mutation rejection ✓ / shared-reference immutability ✓), gate numbers, footprint deltas. **NO merge, NO push — his word.**

---

## Plan Self-Review (run before execution)

1. **Spec coverage:** types (T5–T10), validation+depth (T6), canonical order+typed equality+hash (T7–T8), NaN pin (T4 valueRank), Association sugar (T5), bytes+work order (T3, T10), error strings (T6/T8/T10 guards + C), predicates (T4–T5), harness rename (T11), gates+follow-ups (T12). Bitops (T1) and String delegation (T2) are enabling work the spec's §6 and §3 imply (bytes.ns "add if kernel lacks"; `->` on String keys). ✓
2. **Known unknowns flagged for implementers, not hidden:** the crash idiom (`self error:` vs primitive — Task 6 Step 3 says grep first); slot-init idiom (Task 5); delegate wiring location (Task 2 investigates, stops if intentional); harness header verify (Task 11). Each has a fallback instruction.
3. **Type consistency:** `valueRank`/`valueDepth`/`isValue` selector spellings identical across Tasks 4–10; `valueCompare:with:`/`valueHashOf:`/`rankOf:`/`checkValue:`/`checkDepth:` defined once (T6/T7) and consumed by name (T8–T10). ✓
