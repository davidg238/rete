# Milestone B — nsl-facing message algebra API (proposed surface)

**Status:** proposed 2026-07-03. Milestone B's *implementation* is the active agent's
work per the main spec; this file pins the **nsl-facing constructor/accessor surface**
so the T-B tests (../tests/tb-messages/) and later library code (bytes.ns, cbor.md,
fsm.ns) are written against one API. If the agent lands different selectors, update the
tests mechanically — the semantics below are the spec's, not negotiable per §2.

## Types

Algebra: `nil, Boolean, Integer, Float, String, Bytes, Symbol, List, Map, Record` —
existing kernel types are reused where already immutable; `List`, `Map`, `Record`,
`Bytes` are the new deeply-immutable family. **Not** in the algebra: Array,
OrderedCollection, Dictionary, ByteArray, user objects — placing one in a message is a
`not a value` crash at construction.

## Construction (bottom-up; the only way — no freeze operation, per spec §5)

```
l := List with: 1 with: 'two' with: #three.
l := List fromCollection: anOrderedCollection.     "validates each element is a value"
m := Map fromPairs: (List with: (List with: #k with: 42)).
r := Record tag: #reading fields: m.               "tagged record: name + field map"
```

- Every constructor validates: element is an algebra value (`not a value` crash),
  resulting depth ≤ bound (`message depth exceeded` crash).
- Acyclicity needs no check — construction is bottom-up over already-immutable values;
  a cycle is unrepresentable. (T-B.4 pins this.)

## Access (total reads; misses take a continuation, per errors.md)

```
l size.  l at: 2.  l do: [:e | …].  l anySatisfy: […].    "full read-only combinator set"
m at: #k ifAbsent: [ 0 ].  m keysDo: […].  m size.
r tag.   r at: #field ifAbsent: [ nil ].  r fields.
```

No `at:put:`, no `add:` — the selectors do not exist on these classes (mutation is
`doesNotUnderstand`, and a reflective `perform: #at:put:` finds no method — T-B.2).

## Type predicates

`isValue` (true for every algebra value, false for Array/user objects/etc. — the
constructor's validation test, exposed), plus `isSymbol`, `isList`, `isMap`, `isRecord`,
`isBytes` on Object (default false, overridden true on the type). fsm.ns dispatches on
`isSymbol`/`isRecord`.

## Equality & hashing

Structural `=` and `hash` over the whole tree (depth-bounded, so terminating). Records
compare tag first. Required for Map keys and channel-message dedup later.

## VM shape (informative, for the C side)

- One immutable-bit (or distinct vtables) — `List`/`Map`/`Record`/`Bytes` have no
  mutating primitives compiled in at all; immutability is by construction, not a
  runtime flag check on the write path.
- `Map` may be a sorted association array under the hood at first (messages are small);
  hash table is an optimization, not semantics.
- Depth bound: recommend 32 (deep enough for any sane message, shallow enough that the
  recursive validator/codec can use the C stack safely on a 2–8 KB Zephyr stack).
