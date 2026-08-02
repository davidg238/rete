# Milestone B — the message algebra (design)

**Status:** designed 2026-07-04 (brainstormed with David; all four sections approved).
Refines the main spec's §4 Milestone B (`2026-07-03-nsl-tuvm-first-step-design.md`) and
ratifies the API surface staged in `tuvm/review/lib/messages.md` and
`tuvm/review/lib/bytes.ns`, whose semantics were already pinned there. Inputs honored:
G3 decision M1 (big buffers in C) and M3 (design for the exact-size bins); the L7
integer decision (the algebra's `int` is that integer); the demo-node.ns ergonomic
findings (readme-driven development doing its job).

## 1. Scope

Milestone B delivers the **closed, deeply-immutable value algebra** — nothing else:

- The 10 algebra types, with construction-time validation (`not a value`,
  `message depth exceeded` at depth 32), structural typed equality, canonical
  ordering, and structural hashing.
- The **bytes work order**: `ByteArray` (mutable workbench, not a value) + `Bytes`
  (immutable value) over five new C leaf primitives.
- **Association construction sugar** (decided here; see §3).
- **Codec-proofing invariants** (decided here; see §5) — the decisions that would be
  breaking to retrofit when the CBOR codec lands. No codec code in B.
- The T-B test suite landed as `nsl-tests/08_messages/`.

Explicitly out: channels/TupleSpace/reactor (Milestone C), supervisor/crash boundary
(Milestone D), the CBOR codec itself (post-C, `tuvm/review/lib/cbor.md`), freeze-as-
neuter optimization (§8 of the main spec).

## 2. Types

| algebra type | provenance |
|---|---|
| `nil`, Boolean, Integer (L7), Float, String, Symbol | reused — already immutable in the VM |
| `List`, `Map`, `Record` | new kernel classes (nsl) |
| `Bytes` (+ `ByteArray` workbench, NOT a value) | new kernel classes over 5 C primitives |

**Not** in the algebra: Array, OrderedCollection, Dictionary, ByteArray, Association,
user objects — placing one in a message is a `not a value` crash at construction,
at every nesting level (T-B.6).

### Implementation locus (decided): nsl-first with an evidence clause

`List`/`Map`/`Record` are ordinary kernel classes in `lib/kernel.ns`, each backed by a
plain VM Array held in a **private slot that never escapes**. Newspeak slot privacy IS
the immutability enforcement: the classes define no mutating selectors (mutation is
`doesNotUnderstand`, reflective `perform:` finds no method — T-B.2), and no reflective
path reaches the backing store (mirror confinement, already designed). No immutable
bit, no write barrier, no freeze primitive for containers.

Validation, depth tracking, equality, and hashing are interpreted nsl. The spec's own
sizing note governs: messages are small, depth ≤ 32. **Evidence clause (pinned so it
is not re-litigated ad hoc):** structural `hash`/`=` may migrate to a C primitive
ONLY on measured evidence (T-B.5 generative timings or real-workload telemetry), the
same decide-on-evidence shape as G3/M5.

The classes land **inside `kernel.ns`** (~250 lines added to 290). A separate baked
`lib/messages.ns` module was rejected: the compile pipeline bakes exactly
kernel + supervisor + user program today, and a third baked module is pipeline work
that buys nothing at this size. Module split is a later refactor if growth warrants.

## 3. Construction

Bottom-up, the only way; no freeze for containers (per the main spec §5):

```
l := List with: 1 with: 'two' with: #three.
l := List fromCollection: anOrderedCollection.
m := Map with: #seq -> seq with: #milliC -> milliC.     "sugar (decided)"
m := Map fromPairs: aListOfPairs.                        "programmatic path"
r := Record tag: #reading fields: m.
r := Record tag: #reading with: #seq -> s with: #avg -> a.   "sugar"
b := aByteArray freeze.
```

### Association sugar (decided: adopt)

A minimal `Association` kernel class (`key`/`value`) plus `->` sugar. Verified:
`->` parses in the nsl grammar today (`grammar/ns.ohm:104-105`, binaryChar+) — this is
the standard Smalltalk/Newspeak pair idiom, zero grammar work.

**AMENDED 2026-07-04 (David, after MB Task 2's premise correction): `a -> b` is a
compile-time rewrite to `Association key: a value: b` at the emit site — NOT a method
on Object.** Task 2 established that no builtin vtable delegates to kernel Object
(all hardcode `delegate = NULL`; structural under the decoupled-XIP model — firmware
cannot point into app-compiled vtables; the mechanism for builtin receivers is a
per-type C switch in dispatch.c's PRIM cases, per solidify.py's own comment). An
`Object>>->` method would therefore be unreachable from Integer/Float/Symbol/String/
LargeInteger receivers — `3 -> 7` would DNU. The rewrite is receiver-agnostic (covers
builtins and heap instances uniformly), matches this section's own framing of `->` as
constructor sugar that is not a value, and follows the Milestone A precedent of the
`detect:ifNone:` rewrite off NLR. `Object>>->` is NOT defined (the rewrite covers all
send sites syntactically; a dispatched fallback would be dead code).
Rejected alternatives (recorded so this isn't re-litigated):
- *Generic dispatch fallback* (builtin receiver + lookup miss → kernel-Object vtable
  lookup): uniform dispatch for every future Object method, but new machinery on the
  hot send path, bought for one selector's benefit — Task 4 already answers the
  predicates/rank for builtins in C.
- *Per-selector C fast-path for `->`*: C cannot construct an instance of the
  app-compiled Association class under XIP decoupling — the same reason the delegate
  pointers are NULL.

**Association is constructor-side sugar only, NOT an algebra value.** Pair-taking
constructors copy key/value out and the Association dies (exact-size bin churn, G3/M3
friendly). A bare Association inside a List/Map value position is `not a value`. The
algebra remains exactly the 10 types; wire format, validation, and equality never see
Association.

Why adopted (recorded): `fromPairs:` ships regardless (programmatic construction, the
future CBOR decoder). The sugar removes the 6-lines-per-3-field-record verbosity every
publish site pays (demo-node.ns evidence) AND deletes an error class — in the
`List with: (List with: #k with: v)` spelling, a dropped nesting level constructs a
structurally-valid-but-wrong shape caught only at runtime; `#k -> v` is a syntactic
atom that cannot mis-nest. Rejected: `{…}` compiler literals (grammar+compiler surgery
mid-milestone; a permanent nsl-only dialect extension on top of the documented no-NLR
deviation, for a marginal reading gain over the sugar); fromPairs:-only (one-time
saving of ~30 kernel lines, paid for by every hand-written construction site forever).

### Validation & the depth cache

Every constructor validates each element: `isValue` false → `not a value` crash;
depth > 32 → `message depth exceeded` crash. **Every container caches its depth in a
slot** (leaf types answer 0; a container is 1 + max child depth), so per-construction
validation is O(children), never O(tree). The 32 bound is messages.md's: deep enough
for any sane message, shallow enough that a future C-side recursive codec is safe on
a 2–8 KB Zephyr stack (nsl recursion itself runs on the heap-side language stack).

Acyclicity needs no check: construction is bottom-up over already-immutable values; a
cycle is unrepresentable (T-B.4 pins it).

`isValue` on Object answers false; overridden true on the 10 types. Type predicates
`isSymbol`/`isList`/`isMap`/`isRecord`/`isBytes` per messages.md (fsm.ns dispatches
on them).

## 4. Access (total reads; misses take continuations)

Per messages.md, unchanged: `size`, `at:` (List), `at:ifAbsent:`, `keysDo:`, `do:`,
the read-only combinator set; `r tag`, `r at:ifAbsent:`, `r fields`. No `at:put:`, no
`add:` — the selectors do not exist on these classes.

## 5. Canonical order, equality, hash (codec-proofing — decided)

**Map storage is a sorted association array** (messages.md's hint, now normative),
sorted by key in **value order** — a total order over all algebra values:

1. **Type rank:** `nil < Boolean < Integer < Float < String < Bytes < Symbol < List
   < Map < Record`.
2. **Within type:** numeric for Integer and for Float; byte-lexicographic for
   String/Bytes/Symbol; element-wise lexicographic (recursive; terminating, depth
   ≤ 32) for List; sorted-entry lexicographic for Map; tag-then-fields for Record.

**NaN is not a value** (self-review pin): a NaN Float would break the total order
(no consistent rank), so constructing a message containing NaN crashes `not a value`
like any non-algebra object. The VM already aborts on the common producers (division
by zero); this closes the remaining routes (e.g. libm results). ±0.0 needs no pin:
they compare equal and order consistently.

One decision, three payoffs: **deterministic iteration** (`keysDo:` walks sorted
order — identical on host and target, protecting differential testing),
**deterministic structural hash**, and a **deterministic future wire encoding** (the
codec walks storage order; no sort at encode time).

Honesty note (pinned): RFC 8949 canonical CBOR sorts by *encoded key bytes*, which
will not coincide with value order. Canon here is **nsl value order**; porta treats
the wire as deterministic-but-nsl-defined. Revisit only if a third-party CBOR consumer
demands RFC-canonical ordering.

### Typed structural equality (decided; the subtle pin)

Container-level structural `=` is **typed**: values of different algebra types are
never equal — specifically **Integer 1 ≠ Float 1.0 inside the algebra**, even though
leaf-level nsl `1 = 1.0` remains true (oevm numeric equality, unchanged). Why:

- The sorted array requires order-consistent-with-equality; 1 and 1.0 sort to
  different type ranks, so they must be distinct keys or the invariant breaks.
- CBOR agrees: int 1 and float 1.0 are different encodings; a map holding both
  round-trips faithfully.
- The divergence is scoped (whole-message compares and mixed-type Map keys only) and
  fails toward MORE distinction, never silent merging:
  `(Map with: 1 -> 'a') at: 1.0 ifAbsent: [nil]` → `nil`, by design.

Rejected: numeric-merging keys (1 and 1.0 one key) — breaks the sorted invariant or
forces order hacks, and loses CBOR round-trip fidelity. Rejected: restricting Map keys
to Symbol/String/Integer — needless divergence from CBOR's any-value keys; typed
equality makes the general case sound.

**Hash:** structural fold over the canonical form (type rank + children, FNV-style
combine in nsl); leaves use existing hashes (Integer's is SMI/boxed-consistent per
L7). Records compare and hash tag first. Depth-bounded → terminating. Equal values
hash equal, on both platforms.

## 6. Bytes family and the C work order

Per bytes.ns, ratified: `ByteArray` mutable workbench (register payloads, packet
parsing) — **not a value**; `Bytes` immutable algebra value. `freeze` **copies**
(neutering is the deferred §8 optimization; API identical — demo-node's `buf freeze`
already assumes copy semantics). `Bytes freeze` answers self; `asByteArray` is the
explicit thaw-copy. Shared read protocol (`u8At:`, LE-default `u16At:`, spelled-out
`u16BeAt:`, …); write selectors exist only on ByteArray.

**C work order (leaf-only, deliberately minimal):** five platform primitives —
`basicNewBytes:` (zero-filled), raw `byteAt:` / `byteAt:put:` (0–255; bounds policed
in nsl), `byteSize`, `freezeBytes:`. Implementation note: both C objects take
String's existing layout shape (header + length + bytes, pointer-free leaf) with
their own vtables — GC already sweeps that shape, no new scan logic. No Bytes
literals exist in nsl source, so there is **zero** serializer/loader/solidify work on
either image path. Integer `bitAnd:/bitOr:/bitXor:/<</>>` are assumed present;
verified at implementation, added to the work order only if missing. Big long-lived
buffers stay in C arenas per G3/M1 — `Bytes` in messages are small copies by design.

## 7. Error strings (frozen API; errors.md discipline)

| condition | string |
|---|---|
| non-value element at construction (any depth) | `not a value` |
| depth > 32 at construction | `message depth exceeded` |
| byte index out of range | `index out of bounds` |
| byte value outside 0–255 | `byte out of range` |
| write attempt on Bytes/List/Map/Record | (no string — DNU; the selector does not exist) |

Total reads take `ifAbsent:` continuations; everything else crashes, per house
philosophy. `immutable` from the staged errors.md is deliberately unused: with
immutability by construction there is no write path to intercept.

## 8. Testing & gates

**Staged suite lands** (`tuvm/review/tests/tb-messages/` → `nsl-tests/08_messages/`)
with the variants the staged files call for: `01b` map-nesting depth; `02` expanded to
the full (type × mutating selector) matrix, generated from a selector list kept beside
the suite; `06b/c/d` (user object / unfrozen ByteArray / Dictionary, nested deep).
`05` generative (seeded in-image LCG, 200 trees, built twice, `=` + hash equal) stays
reusable for future CBOR round-trips.

**New tests for this design's decisions:** Association sugar ≡ `fromPairs:` (equal
Maps); bare Association is `not a value`; canonical iteration order independent of
construction order (identical output both backends); the typed-equality pin (1 vs 1.0
distinct keys; whole-message `=` distinguishes); depth-cache boundary at 32 from both
List- and Map-nesting sides.

**Harness: expected zero new work.** The staged tests name an `expect-crash:` header;
Milestone A already landed `expect-runtime-fail:` with those semantics (substring
after preceding `expect:` lines, both paths). Step one of implementation verifies
that and mechanically renames the staged headers (their README anticipates mechanical
adjustment). Any real gap is a small TDD'd harness patch.

**Gates:** full conformance suite both paths (90 → ~105+); C unit suite; ASAN/UBSan
on the bytes primitives; footprint — engine text moves only by the five primitives
(kernel.ns growth lands in the image object, which `footprint.sh` correctly
excludes), RAM per A2. Diff-oracle does not apply to new surface (no frozen-oevm
counterpart) — recorded so its silence is not mistaken for coverage.

**Proof line (main spec §4B, unchanged):** nested messages construct; a mutation
attempt is a compile/runtime rejection; a message shared between two references
cannot be mutated — plus the pins above.

## 9. Follow-ups out of this design (not B-blocking)

- Update `tuvm/review/demo-node.ns` and staged lib files to the ratified sugar
  surface (mechanical; the review README anticipates it).
- `FOOTPRINT_CONFIG=mcu` RAM line (pending sync point) — wanted before B's gates bank
  RAM numbers; independent tooling task.
- nsl-tuvm#1 (CompileError line numbers) before B *implementation* starts writing
  many failing-compile tests.
- The CBOR codec milestone consumes §5's invariants and `cbor.md` as written.

## Addendum 2026-07-05 — VM dispatch decisions forced by implementation (David-approved)

Two pre-existing VM gaps surfaced during Tasks 6-7 that the spec's nsl-first architecture
silently depended on. Both fixed in tuvm, own revertible commits, David-approved:

1. **`PRIM_eq` heap fall-through (tuvm c452fd5).** dispatch.c's `=` handler ended in an
   unconditional C identity compare — no heap class's `=` could EVER dispatch (not even
   `Object>>=`; proven empirically before fixing). Decision: builtin receivers (nil/SMI/
   String/Float/Symbol/LargeInteger/RawBytes) keep their C semantics unchanged (String
   content-eq, exact integer, mixed-numeric, identity for the rest — Symbols are interned
   and cannot reach nsl); genuine heap objects now `break` to vtable dispatch, where
   `Object>>= ( ^ self == other )` preserves identity for classes without a custom `=`.
   §5's typed structural `=` on List/Map/Record is IMPLEMENTABLE because of this. Rejected:
   hardcoding container equality per-type in C (contradicts nsl-first; re-done per type).
   Reviewer-proven non-regressing: the break-set equals PRIM_isNil's long-working heap set;
   `==`/PRIM_eqeq stays pure C identity (no regress). NOTE: `Object>>=` is now LIVE code.
2. **`PRIM_error` String messages (tuvm 0af499d).** `self error:` printed `Error: ?` for
   String args (only Symbols rendered), and the grammar admits no multi-word Symbol
   literals — this spec's frozen multi-word crash strings (`not a value`, `message depth
   exceeded`) were unsatisfiable. Fix mirrors PRIM_out's String/Symbol dual-print.

Related ratifications recorded elsewhere: `->` as compile-time rewrite (this spec's
Association section, amended 2026-07-04); the `outer <keyword>:` non-public-helper reach
idiom (kernel.ns:340-346 comment; ADR line due in Task 12 close-out).

## Addendum 2026-07-05 — Task 8 (Map) & Task 9 (Record) decisions (David-approved)

All pure-nsl (no VM/C change). Tasks 8-9 committed + merged to master + pushed 2026-07-05.

1. **`valueHashOf:` leaf-mask completion (nsl-tuvm 7bb3fc5, own revertible commit).** §5's hash
   fold stated "30-bit mask keeps every intermediate SMI-safe" but the *leaf* fallback
   (`(v hash) bitXor: r`) was unmasked — a bare Symbol's native hash (~6.4e12) overflowed
   LargeInteger's 2^64 magnitude when folded into a container's `* 16777619` before the
   container's own `bitAnd` ran. Latent since Task 7 (its test never folded a Symbol into a
   container); surfaced by Map with Symbol keys. Fix masks the leaf too, completing the stated
   invariant. Behavior-preserving (uniform mask; equal→equal holds); proven non-regressing at
   98/0/12 in isolation before the Map commit. Touches ratified Task-7 code → David-ratified.
2. **Map `keyAt:`/`valueAt:` (Task 8).** Minimal indexed read accessors, added so the
   module-level `mapCompare:with:` can walk two Map instances in lockstep by index
   (`keysAndValuesDo:` iterates only one receiver). Read-only, never expose the backing Array.
   David-accepted as sanctioned surface (List>>at: precedent). Record (Task 9) needs no
   analogue — it has exactly one tag + one fields, so `recordCompare:with:` uses the plain
   `tag`/`fields` accessors and adds NO new surface.
3. **Map last-wins via stable-sort + compaction (Task 8).** Duplicate construction keys keep
   the LAST entry (CBOR-decoder overwrite semantics). Implemented as: stable insertion-sort
   (strict `> 0` shift never reorders equal keys) then keep-last-of-run compaction. Covered by
   `05d_map_last_wins.ns` (adjacent + non-adjacent duplicates).
4. **`fromPairs:` contract consequence (Task 8).** A bare Association is `not a value`, so an
   algebra List can never legally hold Associations as elements. Therefore `Map>>fromPairs:`'s
   Association branch is reachable ONLY via an unvalidated collection (e.g. OrderedCollection),
   never an algebra List. The plan's draft `05_map.ns` (which fed `fromPairs:` a `platform List
   with: (#j -> 7) ...`) was unrunnable and was corrected (plan doc 94cb9d8).
5. **Record validation + Symbol rendering (Task 9).** `tag isSymbol` / `fields isMap` gate
   construction (else `not a value`) — both predicates are safe to send to any receiver
   (builtins answer in C, heap via Object defaults). Symbol `printString` renders WITH the
   leading `#` (`#reading`); the plan draft's bare `reading` was corrected in `06_record.ns`
   and Record was NOT altered — a no-`#` human render is deferred to tuvm#7 (`displayString`).

## Addendum 2026-07-06 — Task 12 close-out: the `outer` slot-0 helper-isolation invariant (ADR)

Pinned here (per the Task-6 roll-up minor) so a compiler change can't silently break it and
so it isn't re-litigated. **Decision (David-ratified 2026-07-05, first use Task 6, inherited
unchanged by Map/Record/Bytes in Tasks 8–10):** kernel value classes reach the module-level
NON-public helper families — `rankOf:`/`isValueObject:`/`depthOf:`/`checkValue:`/`checkDepth:`
(Task 6) and `valueCompare:with:`/`valueHashOf:`/the per-rank compare arms (Tasks 7–10) —
exclusively via `outer <keyword>:` sends: the sibling class's lexical `outer` link (instance
slot 0), which for classes nested directly in the Kernel module resolves to the Kernel module
instance. **Isolation guarantee:** an application module's own `outer` never points at the
Kernel instance, so these helpers are structurally unreachable from app code — no `private`
enforcement machinery exists or is needed. **Invariant on `ns_compile_oevm.py`:** `outer`
is an OBJECT slot, not activation-Context machinery — `collect_slots` appends `"outer"` as
slot 0 of every non-root nested class (~L1494) and `resolve_name`'s slot path reads it via
`OP_LD_SLOT` (~L867–895, incl. the literal `outer = slot 0` walk); `OP_MK_CONTEXT`/`OP_LD_CTX`
serve captured lexical variables on a different axis and are not involved. Any change to the
slot-0 placement, the `resolve_name` slot path, or `OP_LD_SLOT` semantics that makes sibling
`outer` sends unresolvable breaks the kernel loudly (List/Map/Record constructors funnel
through `outer checkValue:`, and every value class's `=`/`hash` — Bytes included — goes
through `outer`; the conformance suite fails wholesale),
but a change that made kernel helpers REACHABLE from app modules would be silent — nothing
tests app-side unreachability today. This ADR plus the kernel.ns module-helpers comment
(above `rankOf:`) are the pin; a dedicated negative test is a welcome future addition.
Rejected: `private` visibility enforcement in the compiler (machinery Milestone B doesn't
need — lexical structure already provides the boundary); public helper surface on the
Kernel module (would hand app code the raw validators and freeze their spellings as API).
