# review/tests — staged verification suites (T0, T-A…T-D)

**Status:** designed 2026-07-03 per `../test-suites-spec.md`. Staged here; land into
`nsl-tests/` (suites) and `tuvm/tools/` (gates) as each milestone's surface exists.
Test files follow the corpus conventions: `(* expect: … *)` header, module class
`…packageUsing: manifest`, entry `main: platform args: args`, output via
`printString out`.

## Landing map

| Dir | Lands when | Into |
|---|---|---|
| `t0-gates/` | now (A green for oracle) | `tuvm/tools/` + harness flags |
| `ta-deletion/` | Milestone A | `nsl-tests/07_no_nlr/` |
| `tb-messages/` | Milestone B | `nsl-tests/08_messages/` |
| `tc-channels/` | Milestone C | `nsl-tests/09_channels/` |
| `td-crash/` | Milestone D | `nsl-tests/10_supervision/` |

## Harness extensions required (small, listed once)

1. **`expect-compile-error:`** header: the file must FAIL to compile and the compiler
   diagnostic must contain the header's substring; the run PASSES on that match.
   Needed by `ta-deletion/01`, `tb-messages/02b`. (T-A.1 tests the diagnostic as API.)
   [2026-07-06 sync: `tb-messages/02b_reflective_mutation.ns` is now DEFERRED — see
   `CLAIMS.md` row 4a — until `perform:`/mirrors land, so this doesn't currently apply.]
2. **`expect-runtime-fail:`** header (landed spelling — this staged draft's rows below
   still say `expect-crash:`; renamed when Milestone B landed, see the headers in
   `nsl-tests/08_messages/`): the program must terminate with a contained error whose
   text contains the substring, AFTER emitting any preceding `expect:` lines. Needed
   widely in T-B (mutation/depth crashes) pre-Milestone-D; after D, in-language
   observation replaces most uses.
3. **Sanitizer builds:** `OEVM_BUILD=asan|ubsan` build configs (T0.1).
4. **Oracle mode:** run shared tests on frozen `~/workspace/oevm` AND seeded tuvm,
   diff stdout (T0.2) — see `t0-gates/diff_oracle.sh`.

## ASSUMED API (single source for every file here — adjust mechanically if the
## agent lands different spellings; semantics are the spec's and do not change)

**Messages (Milestone B — landed 2026-07-06 in `nsl-tuvm/lib/kernel.ns`; sync
below reflects the landed reality, staged prose in `../lib/messages.md` itself
left as-is per the sync brief):**
`List with:…` (up to 4) / `List fromCollection:`; `Map with: <Association>…`
(up to 4, duplicate keys LAST-WINS) / `Map fromPairs: <List of 2-elem Lists
or Associations>` (no zero-arg/`empty` constructor landed — spell "no fields"
via `fromPairs:` over an empty collection, see `lib/timers.ns`'s sync note);
`a -> b` compile-time Association sugar (works on any value operands,
including builtins); `Record tag:fields:` / sugar `Record tag:with:…` (up to
4, e.g. `Record tag: #reading with: #seq -> 42`); reads `at:`, `at:ifAbsent:`,
`size`, `do:`, `tag`, `fields`; predicates `isValue`, `isSymbol`, `isList`,
`isMap`, `isRecord`, `isBytes`; structural `=` / `hash`. Depth bound **32**,
confirmed landed (`kernel.ns checkDepth:`). `perform:` / `perform:withArguments:`
do NOT exist in the landed language (dispatch slot 11 reserved) — anything
assuming them (`tb-messages/02b`) is deferred until mirrors/perform land.

**tb-messages landed reality (2026-07-06 sync):**
- `01_depth_bound.ns` → landed `nsl-tests/08_messages/10_depth_bound.ns`
  (+ `03b_list_depth_guard.ns` / `03c_map_depth_guard.ns`).
- `02_mutation_rejection.ns` → superseded by
  `nsl-tests/08_messages/12_mutation_selectors.txt` +
  `12_mutation_matrix/01..06` (one DNU-crash test per type × mutating-selector).
- `02b_reflective_mutation.ns` → DEFERRED (`perform:`/`perform:withArguments:`
  absent from the landed language; dispatch slot 11 reserved).
- `03_aliasing.ns` → landed `nsl-tests/08_messages/13_aliasing.ns`.
- `04_cycle_impossible.ns` → landed `nsl-tests/08_messages/15_cycle_impossible.ns`.
- `05_generative_roundtrip.ns` → this staged draft has a REAL bug (double
  `nextRandom` draw in `randomBelow:`, see its own header sync warning); the
  landed FIXED reference is `nsl-tests/08_messages/14_generative.ns`.
- `06_mutable_boundary.ns` → landed `nsl-tests/08_messages/16_mutable_boundary.ns`,
  plus three added variants: `16b_mutable_boundary_user_object.ns` (user-defined
  object rejected), `16c_mutable_boundary_bytearray.ns` (unfrozen ByteArray
  nested deep — distinct from `07b`'s top-level case),
  `16d_mutable_boundary_dictionary.ns` (Dictionary rejected).

**Channels (Milestone C):** `platform space` → a fresh TupleSpace (test entry point).
- `space channel: 'a/b'` → root capability (grant; canonicalization errors here).
- `cap channel: 'c'` → attenuated child capability (root extended by one+ segments).
- Verbs, paths **relative to the capability root**, `''` = the root channel itself:
  `cap out: 'sub/path' value: msg` · `cap whenever: 'sub' do: [:msg | …]` ·
  `cap take: 'sub'` (nil when empty).
- Deterministic test scheduling: `space drain` dispatches queued firings FIFO, each to
  completion, until quiescent. (The supervisor's idle-wait loop is the production
  driver; `drain` is its test-mode equivalent.)

**Supervision (Milestone D):** a contained handler crash posts
`Record tag: #crash fields: {#error -> <text>, #channel -> <path>}` on the reserved
supervisor channel `%sys/crash` (root-only capability). Tests observe crashes with
`sys take: 'crash'`. Restart-policy stub behavior is whatever D lands — `td-crash/04`
pins it once known.

**Error strings** are API, from `../lib/errors.md` (`outside capability root`,
`immutable`, `not a value`, `message depth exceeded`, `invalid channel path`, …).

## CLAIMS.md

`CLAIMS.md` in this directory maps every §7 success criterion of the first-step spec to
the files here. Keep it current: a criterion without a green named test is unproven.
