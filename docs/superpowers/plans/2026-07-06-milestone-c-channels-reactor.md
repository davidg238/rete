# Milestone C — Capability-Scoped Channels + Reactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the degenerate (exact-channel) tuple space: C substrate (registry + FIFO queue + reaction storage), the rooted-prefix Channel capability, the three verbs, the drain reactor, and the fsm.ns Protocol — proving T-C C.1–C.10.

**Architecture:** C owns storage and queueing (`src/space.c`, new engine file in every link set); nsl owns every decision (path canonicalization, capability resolution, value validation, the drain loop) as Kernel-module sibling classes reaching non-public helpers via the `outer` slot-0 idiom. Spec: `docs/superpowers/specs/2026-07-06-milestone-c-channels-reactor-design.md` (RATIFIED 2026-07-06).

**Tech Stack:** C99 (tuvm engine), nsl (kernel), Python harness (unchanged).

## Global Constraints

- Frozen crash strings (spec §7): `not a value`, `bad channel path`; `outside capability prefix` is FROZEN-RESERVED (no thrower exists in Milestone C — do NOT invent a use). NEW, controller-proposed pending David: `channel registry full`, `channel queue full`, `channel reactions full` (static-capacity exhaustion, Tier-2 crash from C).
- `take:` on empty = nil (the single sanctioned nil return, errors.md L5). Posting to a reaction-less channel = silent drop.
- Dispatch semantics are the ratified D-C1..D-C5: one global FIFO queue; event dequeued exactly once; fan-out to ALL reactions on the exact channel in registration order; no dedupe; reaction-list snapshot taken at dequeue.
- Registrations carry an owner tag (spec §6.2). No unregister surface for jobs (Q2).
- Exact-channel dispatch ONLY. No wildcards, no prefixes at dispatch, no persistence.
- Path grammar: `/`-separated segments; NO reserved characters; canonicalization rejects (crash `bad channel path`): empty rel is LEGAL (means the root); leading `/`, trailing `/`, empty segment (`//`), and any segment equal to `.` or `..` are ILLEGAL.
- Conformance harness: `cd nsl-tests && OEVM_DIR=<abs>/tuvm OEVM_PATH=AB python3 <abs>/nsl-tuvm/tools/run_tests_oevm.py` — FOREGROUND, Bash timeout 600000, no waiter, no second run, sequential only.
- Before ANY tuvm commit: `git status` must show `src/solidified_image.c` and `src/solidified_image.stmap` restored (`git restore src/solidified_image.c src/solidified_image.stmap` after harness runs).
- tuvm `src/prim_ids.h` mirrors dispatch.c prim ids by hand: this plan only APPENDS ids 73–76 (no renumber needed — MB Task 3 precedent). If you find yourself INSERTING, stop and escalate.
- Escalate-first is HARD LAW for any VM/C change a task does not explicitly authorize.
- Branches: work on `milestone-c` in all three repos (exists in tuvm/nsl-tuvm; create in nsl-tests off master first task that touches it).
- Baseline: conformance 124/0/12 both paths, 4 gates PASS, C suite 20/2.

---

### Task 1: C substrate — `src/space.c` + platform prims 73–76 + link-set integration

**Files:**
- Create: `tuvm/src/space.h`, `tuvm/src/space.c`
- Modify: `tuvm/src/dispatch.c` (PRIM enum + PRIM_NAMES: 4 appended entries), `tuvm/src/prim_ids.h` (append 73–76), `tuvm/src/platform.c` (4 `platform_dispatch` cases), `tuvm/src/gc.c` (root-scan calls `space_gc_trace()`)
- Modify (link sets — EVERY place the engine file list appears): `tuvm/tools/run_c_tests.sh` (ENG_A and ENG_B), `tuvm/tools/footprint.sh` (compile list + `size` list, add `space.o`), `nsl-tuvm/tools/run_tests_oevm.py` (Path A build cmd ~line 388, Path B build cmd ~line 110, boot-gc-asan gate build ~line 159 — grep `"src/vm.c"` to find every build list and add `src/space.c` to each)
- Test: `tuvm/tests/test_space.c`; register in `tuvm/tools/run_c_tests.sh` (Model-B link set + space.c)

**Interfaces:**
- Consumes: `indexable_new(intptr_t n)` (platform.c:38) for the dequeue snapshot Array; existing `platform_dispatch` switch shape (platform.c:75); gc.c root-scan (find where `RawBytesVtable`/`StringVtable` roots are traced — Task-3/MB precedent — and call `space_gc_trace()` beside them).
- Produces (C, in space.h — later tasks and the D milestone rely on these exact names):
  - `int space_intern(const char *path, size_t len)` — registry id, crash `channel registry full` when static table exhausted
  - `void space_post(const char *path, size_t len, oop value)` — the C-callable enqueue (THE ISR-pulse seam, spec §6.1); crash `channel queue full` on ring overflow
  - `oop space_take(const char *path, size_t len)` — oldest queued-undispatched on that exact channel, removed; NULL (nil) if none
  - `void space_register(const char *path, size_t len, oop block, oop owner)` — append reaction; crash `channel reactions full`
  - `oop space_dequeue(void)` — NULL if queue empty; else a 2-slot heap Array `{value, blocksSnapshotArray}` where the snapshot is a fresh Array of the channel's reaction blocks AT THIS MOMENT (D-C5), registration order (D-C3)
  - `void space_gc_trace(void)` — marks every queued value oop + every registered block/owner oop
  - Static capacities (overridable `-D`): `OEVM_SPACE_MAX_CHANNELS 32`, `OEVM_SPACE_MAX_PATH 64`, `OEVM_SPACE_QUEUE_DEPTH 64`, `OEVM_SPACE_MAX_REACTIONS 8` (per channel)
- Produces (prims, appended — spellings the Task-2/3 kernel code sends to `platform`):

| id | prim_ids.h name | PRIM_NAMES selector | platform_dispatch behavior |
|---|---|---|---|
| 79 | `PRIM_pf_spacePost` | `spacePost:value:` | args[0]=String path, args[1]=value → `space_post`; answers args[1] |
| 80 | `PRIM_pf_spaceRegister` | `spaceRegister:block:owner:` | → `space_register`; answers platform |
| 81 | `PRIM_pf_spaceTake` | `spaceTake:` | → `space_take`; answers value or nil (NULL) |
| 82 | `PRIM_pf_spaceDequeue` | `spaceDequeue` | → `space_dequeue`; answers Array or nil |

(PLAN CORRECTION at Task-1 landing: this table originally said 73–76 — a controller
arithmetic error. dispatch.c's PRIM enum positions 73–78 were already held by the MB
Task-4 predicate prims, which prim_ids.h deliberately does not mirror. True append =
79–82, as landed. nsl reaches prims by selector, never numeric id — no downstream change.)

Path Strings arrive as `struct String*` (StringVtable-gated in the platform cases, mirroring how `basicNewBytes:` gates its args); crash strings print via `fprintf(stderr, "...\n"); abort();` exactly like the MB `index out of bounds` precedent (dispatch.c ~1262).

Implementation sketch (space.c internals — final shape is the implementer's, these invariants are not):

```c
struct SpaceReaction { oop block; oop owner; };
struct SpaceChannel  { char path[OEVM_SPACE_MAX_PATH]; size_t len; int used;
                       struct SpaceReaction reactions[OEVM_SPACE_MAX_REACTIONS]; int n_reactions; };
struct SpaceEvent    { int chan; oop value; int live; };
/* one static channel table; one static ring buffer (head/tail), FIFO;
   dequeue scans from head for the oldest live event, marks it dead,
   snapshots that channel's reactions into indexable_new(n) BEFORE returning;
   take scans the same ring for oldest live event with matching chan. */
```

- [ ] **Step 1: Write the failing C test** `tuvm/tests/test_space.c` (self-contained main, Model-B link set + space.c; follow test_raw_bytes.c's header-comment + assert style). Properties, each printing `test_space: property N (...) OK`:
  1. intern: same path → same id; different path → different id; `sensor` vs `sensors` distinct; fill to `OEVM_SPACE_MAX_CHANNELS` is fine at boundary (do NOT test overflow abort here — see property 8).
  2. post/take FIFO: post v1,v2 on `a/b`; take → v1, take → v2, take → NULL.
  3. take exact-match: post on `a/b`; take `a/bc` → NULL; take `a/b` → value.
  4. dequeue: register r1 on `a`; post e1 on `a`; dequeue → Array{e1, [r1]}; dequeue → NULL.
  5. snapshot-at-dequeue (D-C5): post e1, e2 on `a` with r1 registered; dequeue (gets e1, snapshot [r1]); register r2; dequeue (gets e2) → snapshot [r1, r2] in registration order.
  6. C-callable seam: `space_post("a", 1, v)` from the test (no VM dispatch in the loop) then dequeue sees it — this IS the ISR-pulse seam proof (spec §6.1).
  7. GC: post a heap value + register a heap block-stand-in (any heap oop works — use a String), run a full collection (mirror test_raw_bytes property 8's GC-survival scaffolding), then take/dequeue and verify contents intact.
  8. overflow aborts: fork-and-expect-SIGABRT (test_raw_bytes has the exact fork+pipe+WIFSIGNALED pattern to copy) for queue overflow (`channel queue full` on stderr) and registry overflow (`channel registry full`).
- [ ] **Step 2: Run it — expect link failure** (space.c doesn't exist): `cd tuvm && bash tools/run_c_tests.sh` after adding the test line → FAIL.
- [ ] **Step 3: Implement** space.h/space.c, the 4 platform cases, PRIM enum/PRIM_NAMES/prim_ids.h appends, gc.c trace call, and EVERY link-set addition listed in Files. `grep -rn "src/vm.c" tuvm/tools nsl-tuvm/tools` and verify each hit got `src/space.c`.
- [ ] **Step 4: Verify** `bash tools/run_c_tests.sh` → 21 PASS / 2 SKIP (test_space PASS). Full conformance suite both paths (exact Global-Constraints command, FOREGROUND) → 124/0/12 + 4 gates (proves the link-list edits didn't break the harness builds).
- [ ] **Step 5: Restore byproducts, commit** (tuvm + the nsl-tuvm harness edit, separate repos): tuvm `feat(space): exact-channel registry + FIFO event queue + owner-tagged reactions + C-callable post (Milestone C substrate, prims 73-76)`; nsl-tuvm `chore(harness): add src/space.c to Path A/B and asan-gate build lists`.

---

### Task 2: Kernel — path canonicalization + Channel/Space capabilities

**Files:**
- Modify: `nsl-tuvm/lib/kernel.ns` (new module-level helper + two sibling classes + platform accessor)
- Test: `nsl-tests/09_channels/01_canonicalization.ns`, `01b`–`01e` (one crash each), `02_confinement.ns`, `03_attenuation.ns`, `04_no_ambient.ns` (create `nsl-tests` branch `milestone-c` off master first)

**Interfaces:**
- Consumes: Task 1 prims (spellings in Task 1's table); String `at:` (1-based, answers SMI byte 0–255, dispatch.c ~1247) and `size`; String `,` concatenation; the `outer` slot-0 idiom for non-public helpers (kernel.ns comment above `rankOf:`); `outer checkValue:` (crash `not a value`).
- Produces (later tasks + demo-node rely on these exact spellings):
  - module-level NON-public `checkPath: aString` — answers aString if canonical-relative, else crash `bad channel path`
  - `class Channel = ( | root | )` with `setRoot:` (non-public), `public channel: rel` (attenuation → new Channel), `public out: rel value: v`, `public whenever: rel do: aBlock`, `public take: rel` (verbs land in Task 3 — THIS task lands the class, `setRoot:`, `channel:`, and the shared `resolve: rel`)
  - non-public `resolve: rel = ( checkPath-validated; '' → root; root '' → rel; else root , '/' , rel )`
  - `class Space = Channel ( )` adding `public drain` (Task 3)
  - Kernel accessor `public space = ( ^ Space new setRoot: '' )` (stateless facade — the substrate is the C singleton; beside `public ByteArray` ~kernel.ns:750)

`checkPath:` (byte-walk; `/` = 47, `.` = 46; complete logic):

```
checkPath: p = (
  | n i segLen dots |
  n:: p size.
  n = 0 ifTrue: [ ^ p ].                                   (* '' = the root, legal *)
  ((p at: 1) = 47 or: [ (p at: n) = 47 ]) ifTrue: [ self error: 'bad channel path' ].
  i:: 1. segLen:: 0. dots:: 0.
  [ i <= n ] whileTrue: [
    (p at: i) = 47
      ifTrue: [
        (segLen = 0 or: [ dots = segLen ]) ifTrue: [ self error: 'bad channel path' ].
        segLen:: 0. dots:: 0 ]
      ifFalse: [
        segLen:: segLen + 1.
        (p at: i) = 46 ifTrue: [ dots:: dots + 1 ] ].
    i:: i + 1 ].
  (segLen = 0 or: [ dots = segLen ]) ifTrue: [ self error: 'bad channel path' ].
  ^ p )
```

(`dots = segLen` rejects `.` and `..` — and also `...`+: stricter than A1 demands, deliberate: dot-only segments have no legitimate use; note this in the commit message. Segment bytes are otherwise unrestricted except `/`.)

- [ ] **Step 1: Failing tests.** `01_canonicalization.ns`: derive `space channel: 'a/b'`, then `channel: 'c/d'`, print resolved roots via a `debugRoot` probe? NO — no debug surface; instead prove by behavior: post on the attenuated channel and `take:` it from an equal full path derived independently (e.g. `(space channel: 'a') channel: 'b/c'` posts; `space channel: 'a/b/c'` takes → value arrives — same canonical channel). `01b`: `space channel: '/a'` → `expect-runtime-fail: bad channel path`; `01c`: `'a//b'`; `01d`: `'a/./b'` (also covers `..` — one extra file `01e`: `'a/../b'`); each one crash per file. `02_confinement.ns` (C.1): cap rooted `sensor` posts on rel `''`; independent take on `sensors` → nil, take on `sensor` → value (segment collision cannot alias); same for `a/b` vs `a/bc`. `03_attenuation.ns` (C.3): `a` → `a/b` → `a/b/c` chain; the leaf posts land under `a/b/c` only (prove via takes at exact paths; a take at `a/b/other` → nil). `04_no_ambient.ns` (C.4): a "job" block granted ONLY `(space channel: 'node7')` builds every path it can — all start `node7/`; prove a post it makes CANNOT be taken at `sup/anything` (nil) but CAN at `node7/x`; assert the capability object has no widening selector by sending `parent` → `expect-runtime-fail:` DNU contains `parent` (one crash file `04b`).
- [ ] **Step 2: Verify each fails** (class doesn't exist yet): run each file individually via the single-file procedure (compile with `ns_compile_oevm.py` per memory/harness precedent) or run the full suite — new tests FAIL.
- [ ] **Step 3: Implement** `checkPath:`, `Channel` (`setRoot:`/`resolve:`/`channel:` + STUB verbs crashing `self error: 'verbs land in Task 3'`? NO — implement `out:value:`/`take:` thin: `out: rel value: v = ( outer checkValue: v. platform spacePost: (resolve: rel) value: v. ^ self )`, `take: rel = ( ^ platform spaceTake: (resolve: rel) )`, `whenever: rel do: b = ( platform spaceRegister: (resolve: rel) block: b owner: self. ^ self )` — they are 3 one-liners consumed by this task's own tests; Task 3 owns `drain` + the dispatch-semantics tests), `Space`, `public space`.
- [ ] **Step 4: Full suite both paths** → 124 + 8 new = 132/0/12 (01,01b–e,02,03,04,04b = 9 files; 3 pure-crash files still count as tests → recount: expect 124+9=133; verify against actual and reconcile honestly in the report), 4 gates, C 21/2.
- [ ] **Step 5: Restore byproducts; commit** nsl-tuvm `feat(kernel): Channel/Space rooted-prefix capabilities + path canonicalization (Milestone C, A1)` and nsl-tests `test(channels): canonicalization + segment-aligned confinement + attenuation + no-ambient (T-C C.1-C.4)`.

---

### Task 3: Reactor — `drain` + core dispatch tests (C.5–C.7)

**Files:**
- Modify: `nsl-tuvm/lib/kernel.ns` (Space>>drain only)
- Test: `nsl-tests/09_channels/05_take_semantics.ns`, `06_run_to_completion.ns`, `07_registration_lifecycle.ns`

**Interfaces:**
- Consumes: Task 1 `spaceDequeue` (nil | Array{value, blocks}); Task 2 verbs; raw-indexable `basicAt:`/`basicSize` (see correction below).
- Produces: `Space>>public drain` — dispatch until queue empty (the host reactor; supervisor idle-wait is Milestone D).

```
public drain = (
  | ev v bs i |
  [ ev:: platform spaceDequeue. (ev == nil) not ] whileTrue: [
    v::  ev basicAt: 1.
    bs:: ev basicAt: 2.
    i:: 1.
    [ i <= bs basicSize ] whileTrue: [ (bs basicAt: i) value: v. i:: i + 1 ] ].
  ^ self )
```

(PLAN CORRECTION at Task-3 landing: this body originally used `at:`/`size` per the
List-internals precedent — the flagged known-unknown resolved against it. Probe-proven:
`space_dequeue`'s result is built by bare `indexable_new` (udp `basicReceive:` precedent),
carries the class-less `IndexableVtable`, so `at:`/`size` DNU; only the shape-gated
`basicAt:`/`basicSize` reach it. `basicAt:`/`basicSize` are kernel.ns's own underlying
vocabulary (`Array>>at: = ( ^ self basicAt: i )`) and the raw array never escapes drain —
selector spelling only, zero semantic change. Alternative rejected: re-plumbing
`space_dequeue` through the vtable-stamped `basicNewIndexable` path would rewrite
ratified Task-1 C on a task that authorizes no C change.)

- [ ] **Step 1: Failing tests.** `05_take_semantics.ns` (C.5): take on never-posted channel → print nil handling via `ifAbsent:`-style guard? NO — `take:` answers nil by contract: print `(space take: 'x') == nil` → `true`; post then take → value once; second take → nil again. `06_run_to_completion.ns` (C.6): reaction on `a` that, when the carried value is `#first`, posts `#second` to `a` and THEN prints `handled-first`; a second... (single reaction, self-post): expected print order `handled-first` then `handled-second` — recursion would invert or nest; also post `#first` twice before draining → FIFO order of prints pins queue order. `07_registration_lifecycle.ns` (C.7): post on `a` with NO reaction, drain (nothing happens — provable because a reaction registered AFTER that drain, followed by a fresh post + drain, fires exactly once: the dropped signal did not queue for the new registrant); reaction registered during a firing fires for a subsequently-posted event.
- [ ] **Step 2: Verify fail** (drain undefined). **Step 3: Implement** drain verbatim above. **Step 4: Full suite both paths** → previous total +3, 4 gates, C 21/2.
- [ ] **Step 5: Restore byproducts; commit** nsl-tuvm `feat(kernel): Space drain — FIFO run-to-completion reactor (Milestone C)` + nsl-tests `test(channels): take/run-to-completion/registration-lifecycle (T-C C.5-C.7)`.

---

### Task 4: Dispatch-semantics pins — fan-out, snapshot, take-vs-dispatch (C.8–C.10)

**Files:**
- Test only (expected): `nsl-tests/09_channels/08_fanout_order.ns`, `09_snapshot_at_dequeue.ns`, `10_take_vs_dispatch.ns`. If a test exposes a substrate bug, the fix is a Task-1-file change → own revertible commit, isolation-proven at the pre-task baseline (MB Task-8/10 discipline), escalate to controller first.

**Interfaces:** Consumes Tasks 1–3 surfaces only.

- [ ] **Step 1: Failing-or-passing tests (these PIN ratified semantics — write them to the spec, then run; a mismatch is a Task-1/3 bug, not a test to adjust):**
  - `08_fanout_order.ns` (D-C3/D-C4): register r1 printing `r1-<value>`, r2 printing `r2-<value>`, then r1's block AGAIN (same variable — the pathological dup, D-C4), then a third distinct r3 printing `r3-<value>` on channel `a`; post the event; drain. Expected prints, exactly: `r1-e`, `r2-e`, `r1-e`, `r3-e` (registration order, dup fires twice). (PLAN CORRECTION at Task-4 landing: the original sequence [r1, r2, r1] is a palindrome under reversal — reverse-order fan-out would print the identical stream, so the file pinned no-dedupe but NOT registration order. The appended r3 breaks the symmetry; reverse would print `r3-e` first. Reviewer-caught.)
  - `09_snapshot_at_dequeue.ns` (D-C5): r1 on `a` posts... precisely: r1's body registers r2 on `a` (guarded by a once-flag) and prints `r1`. Post `#e1` and `#e2` to `a`; drain. Expected: `r1` (e1: snapshot had only r1; r2 registered mid-firing, does NOT see e1), then for e2: `r1`, `r2` (snapshot at e2's dequeue includes both). Total prints: `r1`,`r1`,`r2`.
  - `10_take_vs_dispatch.ns` (D-C2): register r on `a`; post `#e1`, `#e2`; `take: 'a'` → answers `#e1` (oldest) — then drain → r fires ONCE, with `#e2` only. A taken event never fires.
- [ ] **Step 2: Run all three both paths.** Expected: PASS against the Task-1 substrate as specified. Any FAIL → stop, escalate to controller with the diff between observed and D-C* text.
- [ ] **Step 3: Full suite both paths** → +3, gates, C. **Step 4: Restore byproducts; commit** nsl-tests `test(channels): pin fan-out order, snapshot-at-dequeue, take-vs-dispatch (D-C2..D-C5; T-C C.8-C.10)`.

---

### Task 5: Land fsm.ns — Protocol (the fiber replacement)

**Files:**
- Modify: `nsl-tuvm/lib/kernel.ns` (land `class Protocol` + its `Transition` per the staged file's structure, adapted to landed kernel idioms)
- Source: `tuvm/review/lib/fsm.ns` (staged, complete — follow its class shape; it needs Dictionary [landed 3c-2] + the channels API [Tasks 2–3])
- Test: `nsl-tests/10_fsm/01_protocol.ns`, `01b_unhandled.ns`, `01c_validate.ns`

**Interfaces:**
- Consumes: staged `fsm.ns` surface — `Protocol named:`, `state:on:do:then:`, `state:onEntry:`, `onUnhandled:`, `startIn:`, `handle:`, `currentState`, `restore:`, `validate`, `trace:`; Dictionary; channels (a handler may post).
- Produces: `platform Protocol` accessor (same one-liner idiom as `public Map`, kernel.ns ~747) — demo-node uses bare `Protocol` inside its own module, but tests reach it via `platform Protocol`.

- [ ] **Step 1: Failing tests.** `01_protocol.ns`: build the staged header's 3-state machine (idle/converting on #start/#tick/#timeout), drive `handle:` through a full cycle, print `currentState` after each — derive expected states from the staged transitions (idle→converting→idle), plus one channel integration: a transition action posts to a channel, drain fires an observer (FSM ⇄ reactor seam). `01b`: unhandled event → `onUnhandled:` block runs (print), state unchanged. `01c`: `validate` on a machine wired to a `then:` target state that has no transitions and no entry action → per the staged file's validate contract (READ the staged body; if validate crashes on that, expect-runtime-fail with its exact string; if it warns, expect the print — the staged file is the spec, run first and freeze ACTUAL behavior only if the staged text is ambiguous, flagging it).
- [ ] **Step 2: Verify fail. Step 3: Land** Protocol into kernel.ns following the staged file (PLAN-DRAFT CAUTION, standing rule: staged drafts have carried real bugs — run the staged logic mentally against your test before trusting it; fix + flag divergences, never distort the implementation to match a broken draft). Add `public Protocol = ( ^ Protocol )`.
- [ ] **Step 4: Full suite both paths** → +3, gates, C 21/2. **Step 5: Restore byproducts; commit** nsl-tuvm `feat(kernel): Protocol state machine — the fiber replacement lands (Milestone C, L4)` + nsl-tests `test(fsm): protocol transitions, unhandled hook, validate (L4)`.

---

### Task 6: Gates, review-dir sync, close-out

**Files:**
- Modify: `tuvm/review/tests/CLAIMS.md` (row 5 + the reactor-semantics row → DONE with landed pointers), `tuvm/review/tests/README.md` (tc-channels landed-reality map), `tuvm/review/lib/README.md` (fsm.ns row → landed), `tuvm/tools/footprint-after.txt` (re-bank)
- Verify-only: full gates.

- [ ] **Step 1: Controller-grade verification:** full conformance both paths (expect ~139/0/12 — reconcile the exact count from Tasks 2–5 honestly); C suite 21 PASS/2 SKIP; ASAN/UBSan spot-runs (`test_space` + one channels-heavy .ns — `09_snapshot_at_dequeue.ns` — Path A `-fsanitize=address,undefined`); `oevm_heap_stats` sanity on a drain loop with ≥1000 posts (bounded, GC reclaims dead events — the queue slots recycle); footprint re-bank with honest deltas (space.o is a NEW object in the table; text delta = space.c + 4 platform cases + PRIM_NAMES growth).
- [ ] **Step 2: Mechanical review-dir sync** (plan-mandated `tuvm/review/` write exception, same scope discipline as MB Task 12): CLAIMS rows for C, tc-channels staged→landed map in tests/README.md (staged `01_segment_confinement`→`02_confinement` etc. — map ALL seven staged tc files to their landed names or note not-landed-because), lib/README fsm row.
- [ ] **Step 3: Ledger + close-out report to David:** proof lines (confinement incl. A1 collision cases ✓ / run-to-completion+FIFO ✓ / fan-out+snapshot pins ✓ / C-callable post seam ✓), gate numbers, footprint deltas, the three NEW exhaustion strings for ratification, roll-up triage. **NO merge, NO push — his word.**

---

## Plan Self-Review (run before execution)

1. **Spec coverage:** §2 concepts (T2), §3 verbs (T2/T3), §4 P1–P5 (T2 confinement tests + T3 C.6/C.7), §5 D-C1..5 (T1 substrate properties 2/4/5 + T4 pins), §5b (no new surface — WD is D), §6 seam incl. C-callable post (T1 property 6) + owner tag (T1 register signature), §7 strings (T1 aborts, T2 `bad channel path`; `outside capability prefix` reserved — no task throws it, matching the spec note), §8 tests C.1–C.10 (T2: C.1–C.4; T3: C.5–C.7; T4: C.8–C.10), fsm landing (T5), §9 Q1 respell already landed pre-plan, close-out (T6). ✓
2. **Known unknowns flagged, not hidden:** Array `size`/`at:` availability on raw indexables (T3 consumes the List-internals precedent — implementer probes first); staged fsm.ns `validate` contract (T5 Step 1 says read-then-freeze-with-flag); exact post-task test counts (T2/T6 say reconcile honestly). Each has an instruction, none blocks.
3. **Type consistency:** prim spellings (`spacePost:value:` / `spaceRegister:block:owner:` / `spaceTake:` / `spaceDequeue`) identical in T1 table, T2 verb bodies, T3 drain; `resolve:`/`checkPath:`/`setRoot:`/`channel:`/`drain` defined once, consumed by name; dequeue Array layout {value, blocks} identical in T1 property 4/5 and T3 drain. ✓
