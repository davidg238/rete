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
2. **`expect-crash:`** header: the program must terminate with a contained error whose
   text contains the substring, AFTER emitting any preceding `expect:` lines. Needed
   widely in T-B (mutation/depth crashes) pre-Milestone-D; after D, in-language
   observation replaces most uses.
3. **Sanitizer builds:** `OEVM_BUILD=asan|ubsan` build configs (T0.1).
4. **Oracle mode:** run shared tests on frozen `~/workspace/oevm` AND seeded tuvm,
   diff stdout (T0.2) — see `t0-gates/diff_oracle.sh`.

## ASSUMED API (single source for every file here — adjust mechanically if the
## agent lands different spellings; semantics are the spec's and do not change)

**Messages (Milestone B — from `../lib/messages.md`):**
`List with:…`, `List fromCollection:`, `Map empty`, `Map fromPairs:`,
`Record tag:fields:`; reads `at:`, `at:ifAbsent:`, `size`, `do:`, `tag`, `fields`;
predicates `isValue`, `isSymbol`, `isList`, `isMap`, `isRecord`, `isBytes`;
structural `=` / `hash`. Depth bound assumed **32**.

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
