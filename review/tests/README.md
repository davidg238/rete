# review/tests — staged verification suites (T0, T-A…T-D)

**Status:** designed 2026-07-03 per `../test-suites-spec.md`. Staged here; land into
`nodus/tests/` (suites) and `nodus/tuvm/tools/` (gates) as each milestone's surface exists.
Test files follow the corpus conventions: `(* expect: … *)` header, module class
`…packageUsing: manifest`, entry `main: platform args: args`, output via
`printString out`.

## Landing map

| Dir | Lands when | Into |
|---|---|---|
| `t0-gates/` | now (A green for oracle) | `nodus/tuvm/tools/` + harness flags |
| `ta-deletion/` | Milestone A | `nodus/tests/07_no_nlr/` |
| `tb-messages/` | Milestone B | `nodus/tests/08_messages/` |
| `tc-channels/` | Milestone C | `nodus/tests/09_channels/` |
| `td-crash/` | Milestone D | `nodus/tests/11_supervision/` (landed numbering shifted by one — `10_supervision/` was never used; see the sync note above) |

## Harness extensions required (small, listed once)

1. **`expect-compile-error:`** header: the file must FAIL to compile and the compiler
   diagnostic must contain the header's substring; the run PASSES on that match.
   Needed by `ta-deletion/01`, `tb-messages/02b`. (T-A.1 tests the diagnostic as API.)
   [2026-07-06 sync: `tb-messages/02b_reflective_mutation.ns` is now DEFERRED — see
   `CLAIMS.md` row 4a — until `perform:`/mirrors land, so this doesn't currently apply.]
2. **`expect-runtime-fail:`** header (landed spelling — this staged draft's rows below
   still say `expect-crash:`; renamed when Milestone B landed, see the headers in
   `nodus/tests/08_messages/`): the program must terminate with a contained error whose
   text contains the substring, AFTER emitting any preceding `expect:` lines. Needed
   widely in T-B (mutation/depth crashes) pre-Milestone-D; after D, in-language
   observation replaces most uses.
3. **Sanitizer builds:** `OEVM_BUILD=asan|ubsan` build configs (T0.1).
4. **Oracle mode:** run shared tests on frozen `~/workspace/oevm` AND seeded tuvm,
   diff stdout (T0.2) — see `t0-gates/diff_oracle.sh`.

## ASSUMED API (single source for every file here — adjust mechanically if the
## agent lands different spellings; semantics are the spec's and do not change)

**Messages (Milestone B — landed 2026-07-06 in `nodus/nsl/lib/kernel.ns`; sync
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
- `01_depth_bound.ns` → landed `nodus/tests/08_messages/10_depth_bound.ns`
  (+ `03b_list_depth_guard.ns` / `03c_map_depth_guard.ns`).
- `02_mutation_rejection.ns` → superseded by
  `nodus/tests/08_messages/12_mutation_selectors.txt` +
  `12_mutation_matrix/01..06` (one DNU-crash test per type × mutating-selector).
- `02b_reflective_mutation.ns` → DEFERRED (`perform:`/`perform:withArguments:`
  absent from the landed language; dispatch slot 11 reserved).
- `03_aliasing.ns` → landed `nodus/tests/08_messages/13_aliasing.ns`.
- `04_cycle_impossible.ns` → landed `nodus/tests/08_messages/15_cycle_impossible.ns`.
- `05_generative_roundtrip.ns` → this staged draft has a REAL bug (double
  `nextRandom` draw in `randomBelow:`, see its own header sync warning); the
  landed FIXED reference is `nodus/tests/08_messages/14_generative.ns`.
- `06_mutable_boundary.ns` → landed `nodus/tests/08_messages/16_mutable_boundary.ns`,
  plus three added variants: `16b_mutable_boundary_user_object.ns` (user-defined
  object rejected), `16c_mutable_boundary_bytearray.ns` (unfrozen ByteArray
  nested deep — distinct from `07b`'s top-level case),
  `16d_mutable_boundary_dictionary.ns` (Dictionary rejected).

**Channels (Milestone C — landed 2026-07-06 in `nodus/nsl/lib/kernel.ns` `class
Channel`/`class Space`, ~L574-651, atop the C substrate `nodus/tuvm/src/space.c`,
Milestone C Task 1; sync below reflects the landed reality):**
`platform space` → a fresh TupleSpace (test entry point).
- `space channel: 'a/b'` → root capability (grant; canonicalization errors here).
- `cap channel: 'c'` → attenuated child capability (root extended by one+ segments).
- Verbs, paths **relative to the capability root**, `''` = the root channel itself:
  `cap out: 'sub/path' value: msg` · `cap whenever: 'sub' do: [:msg | …]` ·
  `cap take: 'sub'` (nil when empty).
- Deterministic test scheduling: `space drain` dispatches queued firings FIFO, each to
  completion, until quiescent. (The supervisor's idle-wait loop is the production
  driver; `drain` is its test-mode equivalent.)
- Path-validation crash string is landed as **`bad channel path`** (`kernel.ns`
  `checkPath:`, ~L798-813) — corrects this section's earlier `invalid channel
  path` placeholder, which was never the implemented spelling.
- The four C-level exhaustion strings (`channel registry full`, `channel queue
  full`, `channel reactions full`, `channel path too long`, all in
  `nodus/tuvm/src/space.c`) are ALL David-ratified (2026-07-06, the 4th via tuvm
  `1d7abf6`) — landed, not pending ratification.

**tc-channels landed reality (2026-07-06 sync) — all seven staged files map to
landed `nodus/tests/09_channels/` counterparts; landed also adds three files with
no staged precedent (08/09/10 below):**
- `01_segment_confinement.ns` → landed `02_confinement.ns` (same segment-aligned
  collision proof: `sensor`/`sensors`, `a/b`/`a/bc`). Numbering shifted because
  landed inserted the canonicalization family as `01`/`01b-01e` ahead of it.
- `02_canonicalization.ns` → landed as the whole `01`-family: `01_canonicalization.ns`
  (new positive test — canonical join is the same channel regardless of
  attenuation path, not staged) + `01b_leading_slash.ns` (leading `/`) +
  `01c_empty_segment.ns` (`a//b`, the staged file's own crash shape) +
  `01d_dot_segment.ns` (`a/./b`) + `01e_dotdot_segment.ns` (`a/../b`). Staged's
  own comment anticipated a 5th shape, trailing `/` (its planned `02d`) — NOT
  LANDED-BECAUSE no `nodus/tests/09_channels/*.ns` file exercises a trailing-slash
  path (grep-confirmed); left unmapped rather than guessed.
- `03_attenuation.ns` → landed `03_attenuation.ns` (same number/name; the
  staged file's `..`-escape clause moved into the canonicalization family
  above, `01e`).
- `04_no_ambient.ns` → landed `04_no_ambient.ns` (same number/name) +
  `04b_no_ambient_no_parent.ns` (new: the staged comment's "verify by protocol
  review when landing" note on the no-widening-selector claim is now a real
  DNU crash test).
- `05_take_semantics.ns` → landed `05_take_semantics.ns` (same number/name,
  extended with D-C4: a taken event never fires a reaction).
- `06_run_to_completion.ns` → landed `06_run_to_completion.ns` (same
  number/name, same run-to-completion + FIFO pin).
- `07_registration_lifecycle.ns` → landed `07_registration_lifecycle.ns` (same
  number/name, extended with a second reaction registered mid-stream).
- Landed-only additions, no staged file: `08_fanout_order.ns` (fan-out order +
  no-dedupe, D-C3/D-C4), `09_snapshot_at_dequeue.ns` (snapshot-at-dequeue,
  D-C5), `10_take_vs_dispatch.ns` (take: consumes before the next dispatch,
  D-C2).

**Supervision (Milestone D — landed 2026-07-07 in `nodus/nsl/lib/kernel.ns`; sync below
reflects the landed reality):** a contained handler crash posts
`Record tag: #crash with: #text -> <text> with: #channel -> <path>` (landed field is
`#text`, not the staged `#error`; `#channel` is unchanged) on the reserved channel
`sys/crash` (landed spelling — plain `sys/`, not the staged `%sys/`; there is no `%`
sigil anywhere in the landed reserved-channel spelling, `sys/timer` included). Tests
observe crashes with a `sys whenever: 'crash' do: [...]` reaction, NOT `take:` — `drain`
dispatches (and would drop) the transient crash records, same reasoning as the
run-to-completion reactor itself (see `nodus/tests/11_supervision/01_containment.ns`'s own
header note). Restart-policy stub behavior landed as log + restart + quarantine-after-3
(`Supervisor`/`SupervisedJob`, Milestone D Task 5), pinned by
`nodus/tests/11_supervision/05_restart_quarantine.ns` (staged `td-crash/04` superseded).

**Exceptions (Milestone E — landed 2026-07-07 in `nodus/tuvm/src/vm.c`/`dispatch.c` +
`nodus/nsl/lib/kernel.ns`; phase-2 rung 1):** full `on:do:`/`ensure:` built beside D's
`vm_protected_fire` boundary (not reworking it). Four verbs — `self error:`/`error:tag:`,
`e raise`, `[prot] on:[:e|pred] do:[:e|body]`, `[prot] ensure:[cleanup]` — as ordinary
prim-sends (ids 90-93, append-only). The in-flight value is the crash `Record` (`#tag`/`#text`);
handlers read `e tag` / `e at: #text ifAbsent: […]`. Terminate-only (no resume/retry); handler
runs after unwind; `^` in a handler/predicate/cleanup is a compile error (`non-local return`,
`07_no_nlr`); a crash never crosses a dispatch boundary (C5). **Supersedes the D `#crash` tag:**
`sys/crash` now carries the **raise tag** (decision (a); `#crash` retired — the supervisor matches
jobs by `#channel`, never the tag). A bare `self error:` uses the image-**interned** `#error`
default tag, so `[:e| e tag = #error]` matches (F1 fix). Landed suite:
`nodus/tests/12_exceptions/01..14`; the driver-shaped Tier-2 recovery `11_td_crash_recover.ns`
realizes `td-crash/04`'s once-`policy TBD` cycle (that staged stub was already superseded by
`11_supervision/05_restart_quarantine.ns` at D's close). See `docs/MILESTONE_E.md`.

**Error strings** are API, from `../lib/errors.md` (`outside capability root`,
`immutable`, `not a value`, `message depth exceeded`, `bad channel path` [landed
spelling — `../lib/errors.md` still says `invalid channel path`, which was never
implemented; see the Channels section's correction above], …).

## CLAIMS.md

`CLAIMS.md` in this directory maps every §7 success criterion of the first-step spec to
the files here. Keep it current: a criterion without a green named test is unproven.
