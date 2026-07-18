# `.ns` → `.nsl` source-extension migration — design

**Status:** designed 2026-07-17 (David's calls captured below). Precursor to Arc 1 of the
next-arc sequence (`~/workspace/tuvm/review/program-sequence.md`, addendum 2026-07-17) —
"before we write any more code, do the file-extension change." A hard cut across the live
nsl repos, executed as its own spec → plan → implementation cycle.

## Why

`.ns` is the **Newspeak** source extension generally. nsl has diverged from Newspeak
(seed-and-delete, −10% text, no NLR, immutable message algebra, reactive run-to-completion)
and now warrants its own extension, `.nsl`, so a file's dialect is unambiguous at a glance
and so tooling can discriminate nsl sources from the frozen Newspeak/oracle corpora that
still share the workspace.

## Decisions (all ratified 2026-07-17)

1. **Hard cut**, not accept-both. The toolchain moves to `.nsl` exclusively for live nsl
   sources; the whole point is to *end* the nsl-vs-Newspeak ambiguity, and a lingering
   dual-accept defeats it. The five open worktrees were verified **all merged and clean**
   (see "Worktrees" below), so the hard cut carries no unmerged-branch conflict cost.
2. **Single `.nsl` source for the test corpus** — no duplication. The primordialsoup
   differential oracle keeps working with a *single glob edit*, because
   `~/workspace/nsl-tests/run_tests_soup.py` reads each test's **content** and writes it to
   a fresh temp `.ns` file (lines 132–139) before handing it to primordialsoup — the
   original file's extension only matters for **discovery** (`glob("**/*.ns")`, line 204).
   So renaming the corpus to `.nsl` and flipping that one discovery glob keeps every test
   cross-checked against Newspeak, with no doubled corpus and no ongoing sync burden.
   *(The originally-floated "duplicate `.ns` + `.nsl` with a provenance README" was dropped
   once the temp-file mechanism was confirmed — duplication bought nothing the glob edit
   doesn't.)*
3. **Sweep comment/docstring references too, discriminating.** Update every comment or
   docstring that cites a **live** nsl filename (e.g. `kernel.ns` in `dispatch.c` comments)
   to `.nsl`, while leaving frozen-oracle / third-party citations as `.ns`. Stale filename
   comments rot; the sweep is mechanical but must obey the discriminating rule below.

## The discriminating rule (the one thing that prevents corruption)

`.ns` plays **two roles** in the tooling. A blind global replace corrupts the second:

- **Live nsl sources → `.nsl`:** references to `nsl-tests`, `nsl-tuvm` (incl. `lib/`),
  tuvm zephyr nodes.
- **Frozen / third-party → stay `.ns`:** references to `ns-tests`, `oevm`, `ns-oevm` (the
  frozen oracles — "never touched"), and `~/workspace/primordialsoup/newspeak/*.ns` (the
  genuine Newspeak std lib the soup oracle compiles against).

Concrete traps this rule resolves (each must be decided per-reference, not by pattern):

| reference | target | action |
|---|---|---|
| `~/workspace/tuvm/bench.sh:4` → `../ns-tests/00_perf/fib.ns` | frozen | keep `.ns` |
| `~/workspace/tuvm/tests/test_oimg_blob.sh:7` → `../ns-tests/...` | frozen | keep `.ns` |
| `~/workspace/tuvm/tests/test_load_run.sh:14` → `../ns-tests/...` | frozen | keep `.ns` |
| `~/workspace/tuvm/tests/test_portability_32.sh:17` → `../nsl-tests/...` | live | → `.nsl` |
| `~/workspace/tuvm/tests/hanoi_ab_host.sh` → `zephyr/HanoiAB.ns` | live | → `.nsl` |
| `~/workspace/nsl-tests/run_tests_soup.py:145` → `SOUP_STD_LIB.glob("*.ns")` | third-party | keep `.ns` |
| `~/workspace/nsl-tests/run_tests_soup.py:204` → `NS_TESTS_DIR.glob("**/*.ns")` | live | → `.nsl` |

## Scope — files renamed (`git mv`)

- **`~/workspace/nsl-tests`** — 209 `*.ns` → `*.nsl`.
- **`~/workspace/tuvm`** — 14 zephyr node sources (`TimerNode`, `PulseNode`, `HanoiAB`,
  `NetNode`, `NetBridgeNode`, `ThreadEchoNodeDK`, and the rest under `zephyr/`).
  **Excludes `~/workspace/tuvm/review/`** (31 `.ns` files — David's tree, never swept/moved).
- **`~/workspace/nsl-tuvm`** — `lib/kernel.ns`, `lib/supervisor.ns` → `.nsl`.

## Load-bearing tooling edits (must land in lockstep with the renames)

- **Discovery globs** `**/*.ns` → `**/*.nsl` (LIVE `nsl-tests` targets only — all resolved by audit):
  - `~/workspace/nsl-tests/run_tests.py:195`
  - `~/workspace/nsl-tests/run_tests_soup.py:204` (test discovery only; std-lib glob line 145 stays `.ns`)
  - `~/workspace/nsl-tests/run_benchmarks.py:180` (test discovery; SOUP std-lib glob line 133 stays `.ns`)
  - `~/workspace/nsl-tuvm/tools/run_tests_oevm.py:560` (`test_dir` = `nsl-tests`, confirmed :375/:554)
- **Discovery globs that STAY `.ns`** (resolved: they target the FROZEN `ns-tests`, not `nsl-tests`):
  - `~/workspace/tuvm/tools/run_tests_oevm.py:136` (`test_dir = .../ns-tests`, :130)
  - `~/workspace/nsl-tuvm/tools/snapshot_images.py:15` (`TESTS = .../ns-tests`, :10)
  - `~/workspace/nsl-tuvm/tools/test_source_map.py`, `test_compile_oimg.py` (`NS_TESTS = .../ns-tests`)
- **Hardcoded stdlib paths:** `~/workspace/nsl-tuvm/tools/ns_compile_oevm.py:2180,2192`
  (`kernel.ns`, `supervisor.ns` → `.nsl`). *(The compiler's `src` arg is extension-agnostic —
  it reads whatever path it's given — so only these two hardcoded stdlib names matter.)*
- **`~/workspace/tuvm/zephyr/run.sh`:** line 70 `${NODE%.ns}.oimg` and line 7 `NODE=TimerNode.ns` default → `.nsl`.
- **Hardcoded test paths in the Python drivers — LIVE (`nsl-tests`) only → flip:**
  `~/workspace/nsl-tuvm/tools/run_tests_oevm.py:154`, `test_supervisor_grant.py:16`,
  `test_debugger_integration.py:31` (all `nsl-tests/04_somns_basic/*.ns`).
  **Frozen (`ns-tests`) hardcoded paths → keep `.ns`:** `test_source_map.py:59,85`,
  `test_compile_oimg.py:17`.
- **Grammar:** `~/workspace/nsl-tuvm/grammar/ns-parse.js:307` usage string; the `.ns` temp
  names in `grammar/test/program.test.js` (cosmetic — tmp files, but sweep for consistency).
- **`~/workspace/porta`** — 2 textual refs (trivial; audit which role).
- **Editor/VCS associations** — audit for `.gitattributes`, tree-sitter file-type globs, or
  syntax-highlight configs keyed to `.ns`; add `.nsl` where a live-source association exists.

## Execution order (so the suite never goes red)

The compiler's `src` arg is extension-agnostic, which de-risks cross-repo ordering; the true
lockstep is *within* each repo (a discovery glob must flip in the same commit as the files it
discovers). Order:

1. **nsl-tuvm** — rename `lib/kernel`+`supervisor`, fix `ns_compile_oevm.py` hardcoded paths,
   flip its tool globs, fix its hardcoded live-corpus test paths.
2. **nsl-tests** — `git mv` all 209; flip `run_tests.py` and `run_tests_soup.py` discovery globs.
3. **tuvm** — rename the 14 zephyr nodes; fix `run.sh`; apply the discriminating tooling +
   comment sweep (audit each `ns-tests`-vs-`nsl-tests` reference).
4. **porta** — 2 refs.

## Verification

- After the tuvm/nsl-tuvm/nsl-tests edits, run the **full nsl suite Path B (.oimg)** — per the
  standing no-Path-A rule (`~/workspace/tuvm/... memory`) — and confirm the current floor
  (~193/0/10) holds unchanged. A pure rename must not move the count.
- Run `~/workspace/nsl-tests/run_tests_soup.py` to confirm the differential oracle still
  discovers and cross-checks the corpus post-rename.
- Grep sweep: `grep -rI '\.ns\b'` across the three live repos (excluding `review/` and any
  legitimate frozen/third-party reference) returns only intended survivors.

## Out of scope / non-goals

- `~/workspace/tuvm/review/` — David's tree; its `.ns` files and citations are left as-is.
- Frozen oracles (`oevm`, `ns-oevm`, `ns-tests`) and primordialsoup — never touched.
- The five worktrees (below) — not edited; optional cleanup only.

## Worktrees (David's question, answered)

All five open worktrees are stale checkouts of **already-merged, clean** branches — nothing
to conflict with the hard cut:

| worktree | branch | commits not in master | dirty |
|---|---|---|---|
| `tuvm-wt-a` | `perf/activation-trim` | 0 | 0 |
| `tuvm-wt-ab` | `perf/activation-plus-ops` | 0 | 0 |
| `tuvm-wt-b` | `perf/op-cmp-at` | 0 | 0 |
| `tuvm-wt-iram` | `perf/iram-interp` | 0 | 0 |
| `nsl-tuvm-wt-b` | `perf/op-cmp-at` | 0 | 0 |

Recommendation: `git worktree remove` all five (optional housekeeping, independent of this
migration).

## Risks & mitigations

- **Corrupting a frozen-oracle reference** — the highest risk. Mitigation: the discriminating
  rule + a per-reference audit in the plan; no `sed -i` global replace.
- **A missed hardcoded path fails silently** (test discovers 0 files → "green" by vacuity).
  Mitigation: assert the post-rename suite count equals the pre-rename floor, not merely "0 fail".
- **Comment sweep touches a frozen citation** — mitigation: the sweep is discriminating and
  reviewed as part of the tuvm step.

## Close-out / promotion

The durable decision (hard cut, single-source corpus, discriminating rule) is recorded here
and pointed at from the ladder's 2026-07-17 next-arc addendum. On completion: if the suite
floor is unchanged, no `CLAIMS.md` row moves; note the completed migration in the ladder as
the Arc-1 precursor closing, and record the `.nsl` convention wherever `.ns` was documented
as the source extension.
