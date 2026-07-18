# `.ns` → `.nsl` Source-Extension Migration — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rename all live nsl source files from `.ns` to `.nsl` across the live repos and update every load-bearing reference, without disturbing the frozen Newspeak/oracle corpora that legitimately keep `.ns`.

**Architecture:** A hard cut. Live nsl sources (`nsl-tests`, `nsl-tuvm/lib`, `tuvm/zephyr`) become `.nsl`; discovery globs and hardcoded paths that target *live* dirs flip in lockstep; everything targeting the frozen `ns-tests`/`oevm`/`ns-oevm` oracles or third-party primordialsoup Newspeak std lib **stays `.ns`**. The nsl compiler's `src` argument is extension-agnostic, so cross-repo ordering is low-risk; the true lockstep is *within* each repo (a discovery glob must flip in the same logical change as the files it discovers).

**Tech Stack:** Python 3 test harnesses, bash tooling, C VM, git across four repos (`nsl-tests`, `nsl-tuvm`, `tuvm`, `porta`).

**Design of record:** `~/workspace/tuvm/docs/design/2026-07-17-ns-to-nsl-extension-migration-design.md`

## Global Constraints

- **The discriminating rule (never violate):** references to `nsl-tests` / `nsl-tuvm` / `tuvm/zephyr` live sources → `.nsl`; references to `ns-tests`, `oevm`, `ns-oevm` (frozen oracles), primordialsoup `newspeak/*.ns` (third-party), `tuvm/review/**` (David's tree), and `tuvm/docs/spikes/**` (archived snapshots) → **stay `.ns`**. Never `sed -i` a global replace across a repo.
- **No vacuous green:** a pure rename must not change the suite count. Assert the post-migration count *equals the captured baseline*, not merely "0 failures" (a broken glob discovers 0 files and reports green).
- **Path B only** for the nsl suite (standing rule: no per-test gcc rebuild / Path A). The harness default is already Path B.
- **Commit per repo** (four separate git histories). Do not stage across repos.
- **Do not touch** `tuvm/review/`, `~/workspace/oevm`, `~/workspace/ns-oevm`, `~/workspace/ns-tests`, `~/workspace/primordialsoup`, or the five stale worktrees.

---

### Task 0: Capture the baseline suite count

**Files:**
- Modify: none (read-only baseline)

- [ ] **Step 1: Run the live nsl suite (Path B) and record the count**

Run:
```bash
cd ~/workspace/nsl-tuvm && python3 tools/run_tests_oevm.py 2>&1 | tail -5
```
Expected: a summary line like `... 193 passed, 0 failed, 10 skipped` (the ladder floor). **Record the exact pass/fail/skip numbers** — this is the equality target for Task 2 and Task 5. If the VM is not found, set `OEVM_DIR=~/workspace/tuvm` and re-run.

- [ ] **Step 2: Record the pre-migration `.ns` inventory (for a final diff)**

Run:
```bash
cd ~/workspace
{ find nsl-tests tuvm/zephyr -name '*.ns' -not -path '*/.git/*'; \
  find nsl-tuvm/lib -name '*.ns'; } | sort > /tmp/nsl-rename-inventory.txt
wc -l /tmp/nsl-rename-inventory.txt
```
Expected: 224 lines (209 nsl-tests + 13 tuvm/zephyr + 2 nsl-tuvm/lib). Keep this file for Task 5's verification.

---

### Task 1: nsl-tuvm stdlib rename + compiler paths (checkpoint stays green)

Renames only the two stdlib files and the compiler's hardcoded loads. The suite still globs `*.ns` and the corpus is still `.ns`, so this task alone must leave the count unchanged.

**Files:**
- Rename: `~/workspace/nsl-tuvm/lib/kernel.ns` → `kernel.nsl`, `supervisor.ns` → `supervisor.nsl`
- Modify: `~/workspace/nsl-tuvm/tools/ns_compile_oevm.py:2180,2192,2104`

- [ ] **Step 1: Rename the stdlib files**

```bash
cd ~/workspace/nsl-tuvm
git mv lib/kernel.ns lib/kernel.nsl
git mv lib/supervisor.ns lib/supervisor.nsl
```

- [ ] **Step 2: Fix the compiler's hardcoded stdlib loads**

In `~/workspace/nsl-tuvm/tools/ns_compile_oevm.py`:
- Line ~2180: `"..", "lib", "kernel.ns"` → `"..", "lib", "kernel.nsl"`
- Line ~2192: `"..", "lib", "supervisor.ns"` → `"..", "lib", "supervisor.nsl"`
- Line ~2104 (usage string): `<src.ns>` → `<src.nsl>`

- [ ] **Step 3: Verify the suite is unchanged**

Run:
```bash
cd ~/workspace/nsl-tuvm && python3 tools/run_tests_oevm.py 2>&1 | tail -5
```
Expected: **identical** pass/fail/skip to Task 0. (Corpus still `.ns`, glob still `.ns`, only the stdlib the compiler loads changed name.) If the count dropped, the compiler is not finding `kernel.nsl` — check the two path edits.

- [ ] **Step 4: Commit**

```bash
cd ~/workspace/nsl-tuvm
git add lib/kernel.nsl lib/supervisor.nsl tools/ns_compile_oevm.py
git commit -m "refactor(nsl): rename stdlib kernel/supervisor .ns -> .nsl"
```

---

### Task 2: nsl-tests corpus rename + all live discovery globs (cross-repo, verify == baseline)

The atomic core. Renames the 209-file corpus and flips every discovery glob/path that targets `nsl-tests` — in both `nsl-tests` (its own runners) and `nsl-tuvm` (the live gate). These MUST land together: after the corpus is `.nsl` but before the globs flip, the gate discovers 0 files (the vacuous-green trap).

**Files:**
- Rename: `~/workspace/nsl-tests/**/*.ns` → `*.nsl` (209 files)
- Modify: `~/workspace/nsl-tests/run_tests.py:195` (+ msg :200); `run_tests_soup.py:204` (NOT :145); `run_benchmarks.py:180` (NOT :133)
- Modify: `~/workspace/nsl-tuvm/tools/run_tests_oevm.py:560` (glob), `:154` (hardcoded path)

- [ ] **Step 1: Rename the corpus**

```bash
cd ~/workspace/nsl-tests
for f in $(find . -name '*.ns' -not -path './.git/*'); do git mv "$f" "${f%.ns}.nsl"; done
find . -name '*.ns' -not -path './.git/*' | wc -l   # expect 0
find . -name '*.nsl' | wc -l                        # expect 209
```

- [ ] **Step 2: Flip nsl-tests's own discovery globs (keep the Newspeak std-lib globs)**

In `~/workspace/nsl-tests/run_tests.py`:
- Line ~195: `NS_TESTS_DIR.glob("**/*.ns")` → `NS_TESTS_DIR.glob("**/*.nsl")`
- Line ~200: message text `No Newspeak (.ns) test files` → `No nsl (.nsl) test files` (cosmetic)

In `~/workspace/nsl-tests/run_tests_soup.py`:
- Line ~204: `NS_TESTS_DIR.glob("**/*.ns")` → `**/*.nsl`
- **Leave line ~145 `SOUP_STD_LIB.glob("*.ns")` unchanged** (third-party Newspeak std lib)
- **Leave lines ~132/137 `{class_name}.ns` temp files unchanged** (primordialsoup consumes a `.ns` temp)

In `~/workspace/nsl-tests/run_benchmarks.py`:
- Line ~180: `dir_path.glob("*.ns")` → `dir_path.glob("*.nsl")`
- **Leave line ~133 `SOUP_STD_LIB.glob("*.ns")` unchanged**

- [ ] **Step 3: Flip the live gate in nsl-tuvm**

In `~/workspace/nsl-tuvm/tools/run_tests_oevm.py`:
- Line ~560: `test_dir.glob("**/*.ns")` → `test_dir.glob("**/*.nsl")`
- Line ~154: `... "nsl-tests" / "04_somns_basic" / "01_method_call.ns"` → `01_method_call.nsl`
- Lines ~375/~554 (`test_dir = .../nsl-tests`) have no extension — no change.

- [ ] **Step 4: Verify the suite count equals the baseline**

Run:
```bash
cd ~/workspace/nsl-tuvm && python3 tools/run_tests_oevm.py 2>&1 | tail -5
```
Expected: **exactly** the Task 0 numbers. If it reports "0 tests" or a much lower count, a glob or the corpus rename is inconsistent — do not proceed.

- [ ] **Step 5: Verify the soup oracle still discovers the corpus**

Run:
```bash
cd ~/workspace/nsl-tests && python3 run_tests_soup.py 2>&1 | tail -5
```
Expected: the discovery line reports the corpus count (non-zero); pass/fail matches its pre-migration behavior (soup only runs the Newspeak-compatible subset — the point is that discovery is non-zero, i.e. the glob flip worked). If primordialsoup isn't built, the runner prints a build hint and exits — that is acceptable (it's the runner working, just no VM).

- [ ] **Step 6: Commit (two repos, two commits)**

```bash
cd ~/workspace/nsl-tests
git add -A
git commit -m "refactor(tests): rename nsl corpus .ns -> .nsl; flip discovery globs (soup std-lib stays .ns)"

cd ~/workspace/nsl-tuvm
git add tools/run_tests_oevm.py
git commit -m "refactor(nsl): point live test gate at *.nsl corpus"
```

---

### Task 3: tuvm live source renames + run.sh + live scripts (frozen refs untouched)

**Files:**
- Rename: `~/workspace/tuvm/zephyr/*.ns` → `*.nsl` (13 files)
- Modify: `~/workspace/tuvm/zephyr/run.sh:7,70`; `tests/test_portability_32.sh:17`; `tests/hanoi_ab_host.sh:16,17`; `zephyr/CMakeLists.txt:7`
- **Do NOT modify:** `bench.sh:4`, `tests/test_oimg_blob.sh:7`, `tests/test_load_run.sh:14`, `tools/run_tests_oevm.py:130-145` (all target frozen `ns-tests`); `docs/spikes/**` (archive)

- [ ] **Step 1: Rename the zephyr nodes**

```bash
cd ~/workspace/tuvm/zephyr
for f in *.ns; do git mv "$f" "${f%.ns}.nsl"; done
ls *.ns 2>/dev/null | wc -l   # expect 0
ls *.nsl | wc -l              # expect 13
```

- [ ] **Step 2: Fix run.sh**

In `~/workspace/tuvm/zephyr/run.sh`:
- Line ~7: `NODE=TimerNode.ns` → `NODE=TimerNode.nsl`
- Line ~70: `OIMG="$ZDIR/${NODE%.ns}.oimg"` → `OIMG="$ZDIR/${NODE%.nsl}.oimg"`

- [ ] **Step 3: Fix the live-corpus test scripts (leave frozen ones alone)**

- `~/workspace/tuvm/tests/test_portability_32.sh:17`: `../nsl-tests/04_somns_basic/01_method_call.ns` → `.nsl`
- `~/workspace/tuvm/tests/hanoi_ab_host.sh:16,17`: `HanoiAB.ns` → `HanoiAB.nsl` (both the echo text and the `python3 "$NSL" zephyr/HanoiAB.nsl ...` invocation)
- `~/workspace/tuvm/zephyr/CMakeLists.txt:7`: comment `PulseNode.ns` → `PulseNode.nsl`

- [ ] **Step 4: Verify — discriminating grep + run.sh name resolution**

Run:
```bash
cd ~/workspace/tuvm
# Live refs should be gone from zephyr/ and the two live scripts; frozen refs must remain:
grep -rn '\.ns\b' zephyr/run.sh zephyr/CMakeLists.txt tests/test_portability_32.sh tests/hanoi_ab_host.sh | grep -v '\.nsl'   # expect no output
grep -n 'ns-tests' bench.sh tests/test_oimg_blob.sh tests/test_load_run.sh   # expect the 3 frozen refs still present
# run.sh computes the right oimg name:
bash -c 'NODE=TimerNode.nsl; echo "${NODE%.nsl}.oimg"'   # expect TimerNode.oimg
```
Expected: as annotated. (Optional device confirmation: `zephyr/run.sh --model B --node TimerNode.nsl` with `DO_FLASH=0` bakes+builds — needs the ESP32 west workspace; skip if unavailable.)

- [ ] **Step 5: Commit**

```bash
cd ~/workspace/tuvm
git add zephyr/ tests/test_portability_32.sh tests/hanoi_ab_host.sh
git commit -m "refactor(zephyr): rename node sources .ns -> .nsl; run.sh + live scripts (ns-tests refs untouched)"
```

---

### Task 4: Discriminating comment/docstring sweep + porta ref

Updates non-load-bearing filename citations in comments/docstrings that reference **live** files, leaving frozen/third-party/review/archive citations as `.ns`. This is the "sweep all, discriminating" decision.

**Files (sweep, each edit reviewed against the discriminating rule):**
- `~/workspace/tuvm/src/dispatch.c`, `src/platform.c:77`, `src/timer_backend.h:17`, `src/backend/posix/timer_posix.c:17`, `src/backend/zephyr/platform_zephyr.c:38`, `zephyr/src/main.c:6,73,77` — citations of `kernel.ns`/`Object.ns`/`PulseNode(DK).ns`/`ThreadTimerNode.ns` (all live) → `.nsl`
- `~/workspace/nsl-tuvm/lib/kernel.nsl` — internal comments: `05b_map_canonical_order.ns`, `05_restart_quarantine.ns` (nsl-tests, live) → `.nsl`; **but** `review/lib/combinators.ns`, `review/lib/fsm.ns`, `review/lib/timers.ns`, `bytes.ns` cited as `review/lib/...` → **keep `.ns`** (review tree)
- `~/workspace/nsl-tuvm/tools/`: `run_tests_oevm.py` comments (:19,37,341,396,494), `stmap.py:6` (`<basename.ns>`), `test_stmap.py` synthetic literals, `test_platform_routing.py:12` / `test_capture_analysis.py:12` (`frag.ns` tmp), `test_supervisor_grant.py:16`, `test_debugger_integration.py:4,31` (all live) → `.nsl`
- `~/workspace/nsl-tuvm/grammar/ns-parse.js:307` (`<file.ns>`), `grammar/test/program.test.js` (tmp `.ns` names) → `.nsl`
- `~/workspace/porta/docs/PROTOCOL-NSL-RECORDS.md:19` — `nsl-tuvm/lib/kernel.ns` (live) → `kernel.nsl`
- **Leave `.ns`:** `snapshot_images.py` (:2 docstring + :15 glob → ns-tests), `test_source_map.py` (:59,85 → ns-tests), `test_compile_oimg.py:17` (ns-tests), `run_benchmarks.py:30` if it names the frozen `ns-oevm` frontend, and anything under `tuvm/docs/spikes/`.

- [ ] **Step 1: Sweep the tuvm C-source citations**

Edit each listed `tuvm/src/**` and `tuvm/zephyr/src/main.c` comment citing a live `.ns` file to `.nsl`. These are comments only — no behavior change.

- [ ] **Step 2: Sweep the nsl-tuvm comments/docstrings (discriminating on review/ vs nsl-tests)**

Edit `kernel.nsl` internal comments and the `tools/*.py` + `grammar/*` citations per the file list above. For `kernel.nsl`, keep every `review/lib/*.ns` citation as `.ns`; flip only nsl-tests test-file citations.

- [ ] **Step 3: Fix the porta doc reference**

`~/workspace/porta/docs/PROTOCOL-NSL-RECORDS.md:19`: `nsl-tuvm/lib/kernel.ns` → `nsl-tuvm/lib/kernel.nsl`.

- [ ] **Step 4: Verify — the discriminating survivor grep**

Run (each block's output must match its annotation):
```bash
cd ~/workspace
# Live source trees: only legitimate frozen/review/archive citations may still say .ns
echo "--- tuvm live .ns survivors (expect ONLY ns-tests / review / docs/spikes) ---"
grep -rIn '\.ns\b' tuvm/src tuvm/zephyr tuvm/tests tuvm/bench.sh tuvm/tools 2>/dev/null | grep -v '\.nsl' | grep -vE 'ns-tests|/review/|docs/spikes'   # expect no output
echo "--- nsl-tuvm live .ns survivors (expect ONLY ns-tests / review/lib) ---"
grep -rIn '\.ns\b' nsl-tuvm/tools nsl-tuvm/lib nsl-tuvm/grammar 2>/dev/null | grep -v '\.nsl' | grep -vE 'ns-tests|review/lib|node_modules'   # expect no output
```
Expected: no output from either filtered grep. Any line printed is a missed live citation or a wrongly-flipped frozen one — inspect and fix.

- [ ] **Step 5: Commit (per repo)**

```bash
cd ~/workspace/tuvm && git add src zephyr/src && git commit -m "docs(comments): update live .ns citations to .nsl (frozen/review refs kept)"
cd ~/workspace/nsl-tuvm && git add tools lib grammar && git commit -m "docs(comments): update live .ns citations to .nsl (ns-tests/review refs kept)"
cd ~/workspace/porta && git add docs/PROTOCOL-NSL-RECORDS.md && git commit -m "docs: kernel.ns -> kernel.nsl citation"
```

---

### Task 5: Final verification + ladder close-out

**Files:**
- Modify: `~/workspace/tuvm/review/program-sequence.md` (close-out note only — David's tree; edit is the requested arc-precursor bookkeeping, not a sweep)

- [ ] **Step 1: Full suite equals baseline**

```bash
cd ~/workspace/nsl-tuvm && python3 tools/run_tests_oevm.py 2>&1 | tail -5
```
Expected: **exactly** the Task 0 pass/fail/skip.

- [ ] **Step 2: Inventory closed out — no live `.ns` remains**

```bash
cd ~/workspace
# Every file recorded in the baseline inventory now exists as .nsl:
while read f; do [ -e "${f%.ns}.nsl" ] || echo "MISSING: ${f%.ns}.nsl"; done < /tmp/nsl-rename-inventory.txt   # expect no output
# No stray live .ns files remain (review/ and docs/spikes excluded):
find nsl-tests nsl-tuvm/lib tuvm/zephyr -name '*.ns' -not -path '*/.git/*'   # expect no output
```

- [ ] **Step 3: Note completion in the ladder**

Append a one-line note under the 2026-07-17 next-arc addendum in `~/workspace/tuvm/review/program-sequence.md` recording the Arc-1 precursor as done (suite floor unchanged, so no `CLAIMS.md` row moves). Keep it to one or two lines; the design doc holds the detail.

- [ ] **Step 4: Commit the close-out note**

```bash
cd ~/workspace/tuvm && git add review/program-sequence.md docs/design/2026-07-17-ns-to-nsl-extension-migration-design.md docs/superpowers/plans/2026-07-17-ns-to-nsl-extension-migration.md
git commit -m "docs: .ns->.nsl migration close-out (ladder + design/plan)"
```

- [ ] **Step 5 (optional): Remove the stale worktrees**

```bash
cd ~/workspace/tuvm && for w in tuvm-wt-a tuvm-wt-ab tuvm-wt-b tuvm-wt-iram; do git worktree remove ~/workspace/$w; done
cd ~/workspace/nsl-tuvm && git worktree remove ~/workspace/nsl-tuvm-wt-b
```
(All five are merged + clean — verified during design. Optional housekeeping.)

---

## Self-Review

**Spec coverage:** hard cut (all tasks); single `.nsl` source + soup glob flip (Task 2 step 2, keeps `:145`); discriminating comment sweep (Task 4); the `.ns` dual-role rule + concrete keep/flip lists (Global Constraints + Tasks 2–4); worktrees (Task 5 step 5); verification == baseline not just 0-fail (Tasks 0/2/5). Covered.

**Placeholder scan:** every edit names a file:line with a before→after; every verification is a runnable command with expected output; the one irreducibly-judgment step (the comment sweep) ships an explicit keep/flip file list plus a survivor-grep gate. No "TBD"/"handle edge cases".

**Type/name consistency:** `TimerNode.nsl`, `kernel.nsl`, `supervisor.nsl`, `01_method_call.nsl`, `HanoiAB.nsl`, `*.nsl` globs used consistently; `SOUP_STD_LIB.glob("*.ns")` (`:133`/`:145`) and the primordialsoup temp `.ns` are consistently held back across Task 2.
