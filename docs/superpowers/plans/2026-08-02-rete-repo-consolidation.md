# rete Repo Consolidation — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Collapse `tuvm`, `nsl-tuvm`, `nsl-tests` and `porta` into one repository named `rete`, structured as `porta/` + `nodus/` + `protocol/`, preserving all git history, all 107 issue numbers and all 87 commit-SHA citations.

**Architecture:** Restructure `tuvm` **in place** rather than creating a fresh repo — this is what preserves the issue numbers and the 838 `#NN` references that the program navigates by. The other three repos are folded in with `git subtree add` (never `--squash`), which keeps their original commit SHAs resolvable. Each step ends with a green gate; the final gate is a **fresh clone into an empty parent directory**, because every script here resolves siblings by relative path and the stale local clones sit exactly where the old paths point.

**Tech Stack:** git (subtree, mv), `gh` CLI, bash, Python 3 (the nsl test harness), Go 1.26 (porta).

**Design record:** `~/workspace/tuvm/docs/design/2026-08-02-rete-repo-consolidation-design.md` — decisions D1–D13. Read §12 (the review record) before starting; it lists two errors an earlier draft made and one footgun.

## Global Constraints

- **NEVER pass `--squash` to `git subtree add`.** It collapses the incoming repo to one commit and destroys the history that D5 exists to preserve. Every subtree task carries an **ancestry** gate to catch it — `git merge-base --is-ancestor <source-head> HEAD`. A path-scoped `git log -- <prefix> | wc -l` is NOT that gate: it reads 1 whether or not `--squash` was used (Task 2 Step 3 explains why).
- **Never create a GitHub repo named `tuvm` again.** Doing so silently breaks the `tuvm` → `rete` rename redirect, and with it every old issue and clone URL.
- **Nothing in `review/` may be swept, tidied, renamed, reordered or deleted.** It moves as a whole tree via `git mv`, contents untouched. This is D10, granted by explicit permission on 2026-08-02.
- **The full nsl gate is `239 passed, 0 failed`** with aux gates grant / debugger-integration / portability-32 / boot-gc-asan / deepsleep all PASS. `footprint: SKIP (no arm-zephyr-eabi toolchain)` is pre-existing and expected.
- **The porta gate is** `go test ./...` green **and** `tests/porta_sync_host.sh` printing `PORTA SYNC OK`.
- Target layout is fixed by D2/D3/D4:
  ```
  rete/{README.md,CLAUDE.md,.github/,.gitignore,protocol/,porta/,nodus/{tuvm,nsl,tests,toit},review/,docs/,tools/}
  ```
- **`stic` stays outside `rete`** (D12). Do not move or reference `~/workspaceToit/stic` into the tree.
- Never run two nsl suites concurrently — they share `/tmp` paths.

---

## Task 0: Pre-flight — recovery points and the two unverified assumptions

**Files:**
- Create: none
- Modify: none

**Interfaces:**
- Produces: a `pre-rete` tag in all four repos (the recovery point every later task can reset to); a verified answer to whether `gh issue transfer` works public→private; a recorded baseline gate result.

- [ ] **Step 1: Confirm all four repos are clean, on master, and pushed**

```bash
for r in tuvm nsl-tuvm nsl-tests porta; do d=~/workspace/$r
  echo "$r: branch=$(git -C $d branch --show-current) unpushed=$(git -C $d log --oneline origin/master..master | wc -l) dirty=$(git -C $d status --porcelain | grep -v '^??' | wc -l)"
done
```

Expected: every line reads `branch=master unpushed=0 dirty=0`. If not, stop — land the outstanding work first (design §7.1).

- [ ] **Step 2: Tag a recovery point in each repo and push the tags**

```bash
for r in tuvm nsl-tuvm nsl-tests porta; do d=~/workspace/$r
  git -C $d tag -a pre-rete -m "State before the rete consolidation (#79)"
  git -C $d push origin pre-rete
done
```

Expected: four `* [new tag] pre-rete -> pre-rete` lines. This is the rollback: any task can `git reset --hard pre-rete`.

- [ ] **Step 3: Record the baseline gate, so a later regression is attributable**

```bash
cd ~/workspace/nsl-tuvm && python3 tools/run_tests_oevm.py 2>&1 | tail -8
cd ~/workspace/porta && go test ./... 2>&1 | tail -5
cd ~/workspace/tuvm && bash tests/porta_sync_host.sh 2>&1 | tail -3
```

Expected: `Passed: 239 / Failed: 0`, all aux gates PASS; `ok` for every Go package; `PORTA SYNC OK`. Save the output — later tasks compare against it, not against memory.

- [ ] **Step 4: Test whether `gh issue transfer` works PUBLIC → PRIVATE**

Design §5.2 flags this as unverified. Test it with one throwaway before betting 28 issues on it.

```bash
cd ~/workspace/porta
gh issue create --title "DELETE ME: transfer probe (#79)" --body "Throwaway. Testing public->private transfer before the rete migration." 
# note the number it prints, then:
gh issue transfer <NUM> davidg238/tuvm
```

Expected: the transfer succeeds and prints the new URL under `davidg238/tuvm`.
If it is **refused**, record the exact error and set `TRANSFER_WORKS=no` — Task 6 has a documented fallback that re-files by hand instead.

Then delete the probe: `gh issue delete <NEW_NUM> --repo davidg238/tuvm --yes`

- [ ] **Step 5: Rename the Toit repo so the name is not overloaded (D9)**

```bash
gh repo rename nodus-v1 --repo davidg238/nodus --yes
gh repo view davidg238/nodus-v1 --json name,url --jq '.name + " " + .url'
```

Expected: `nodus-v1 https://github.com/davidg238/nodus-v1`

- [ ] **Step 6: Commit nothing; record findings in the plan file**

No code changed. Append the Step 4 result to this plan under Task 6 so the executor of that task does not have to re-derive it.

---

## Task 1: Restructure `tuvm` in place

Move `tuvm`'s own contents down into `nodus/tuvm/`, hoist `review/`, `docs/` and the two program-level tools to the root, and fix the paths that break.

**Files:**
- Modify (move): `src/`, `zephyr/`, `tests/`, `tools/`, `oevm_image_main.c`, `bench.sh` → `nodus/tuvm/`
- Modify (move): `tools/docmap.sh`, `tools/diff_oracle.sh` → root `tools/`
- Modify (path fixes): `nodus/tuvm/tests/porta_sync_host.sh:22`, `nodus/tuvm/tests/hanoi_ab_host.sh:9`, `nodus/tuvm/tests/http_roundtrip_host.sh:14`, `nodus/tuvm/tests/test_portability_32.sh:13,17`, `nodus/tuvm/zephyr/run.sh:32`, `tools/docmap.sh:18`, `tools/diff_oracle.sh:53-54`, `review/tests/t0-gates/diff_oracle.sh:13-14`
- Unchanged in place: `review/`, `docs/`, `CLAUDE.md`

**Interfaces:**
- Consumes: the `pre-rete` tag from Task 0.
- Produces: a `rete`-shaped tree where `nodus/tuvm/` is the VM root. Later tasks assume `OEVM_DIR` defaults to `<repo>/nodus/tuvm` and that `nodus/nsl` and `nodus/tests` will be siblings of it.

- [ ] **Step 1: Create the directory and move the VM tree**

```bash
cd ~/workspace/tuvm
mkdir -p nodus/tuvm
git mv src zephyr tests oevm_image_main.c bench.sh nodus/tuvm/
git mv tools nodus/tuvm/tools
mkdir -p tools
git mv nodus/tuvm/tools/docmap.sh nodus/tuvm/tools/diff_oracle.sh tools/
git status --short | head -20
```

Expected: renames only, no deletions. `review/`, `docs/`, `CLAUDE.md` untouched at root.

- [ ] **Step 2: Verify `review/` is byte-identical (D10 is a hard constraint)**

```bash
git diff --cached --stat -- review/ | tail -3
```

Expected: **empty output** — `review/` has not been touched at all in this step. If anything appears, `git reset` and redo Step 1 without it.

- [ ] **Step 3: Fix the absolute `$HOME/workspace/...` references**

These are the ones that actually break; the relative sibling references are handled in Task 3. Replace `$HOME/workspace/nsl-tuvm` with `$HOME/workspace/tuvm/nodus/nsl` — an interim value, corrected to a repo-relative form in Task 3 Step 4.

```bash
cd ~/workspace/tuvm
sed -i 's|\$HOME/workspace/nsl-tuvm|\$HOME/workspace/tuvm/nodus/nsl|g' \
  nodus/tuvm/tests/porta_sync_host.sh \
  nodus/tuvm/tests/hanoi_ab_host.sh \
  nodus/tuvm/tests/http_roundtrip_host.sh \
  nodus/tuvm/zephyr/run.sh \
  tools/docmap.sh tools/diff_oracle.sh review/tests/t0-gates/diff_oracle.sh
sed -i 's|\$HOME/workspace/nsl-tests|\$HOME/workspace/tuvm/nodus/tests|g' \
  tools/diff_oracle.sh review/tests/t0-gates/diff_oracle.sh
grep -rn 'workspace/nsl-tuvm\|workspace/nsl-tests' nodus/ tools/ review/tests/ | grep -v '\.md:'
```

Expected: the final `grep` prints nothing.

> `review/tests/t0-gates/diff_oracle.sh` is edited here. That is a **path correction inside a script**, not a sweep of `review/` — D10 forbids moving, deleting and tidying, not fixing a path that would otherwise be broken. Its content is otherwise untouched.

- [ ] **Step 4: Fix the relative sibling refs in `test_portability_32.sh`**

```bash
cd ~/workspace/tuvm
sed -i 's|^NS=\.\./nsl-tuvm$|NS=../nsl|; s|^SRC=\.\./nsl-tests/|SRC=../tests/|' nodus/tuvm/tests/test_portability_32.sh
grep -n '^NS=\|^SRC=' nodus/tuvm/tests/test_portability_32.sh
```

Expected:
```
13:NS=../nsl
17:SRC=../tests/04_somns_basic/01_method_call.nsl   # same minimal test as Task 10
```

- [ ] **Step 5: Run the gate, pointing the harness at the new VM root**

The harness still lives in the unmerged `~/workspace/nsl-tuvm`; `OEVM_DIR` is exactly the knob for this.

```bash
cd ~/workspace/nsl-tuvm
OEVM_DIR=~/workspace/tuvm/nodus/tuvm python3 tools/run_tests_oevm.py 2>&1 | tail -8
```

Expected: `Passed: 239`, `Failed: 0`, and

```
grant: PASS  debugger-integration: PASS  portability-32: FAIL  boot-gc-asan: PASS  deepsleep: PASS  footprint: SKIP
```

> **`portability-32: FAIL` is REQUIRED here, not tolerated.** An earlier version of
> this plan demanded all aux gates PASS at Task 1, which is impossible by
> construction: `test_portability_32.sh` resolves `NS=../nsl` and
> `SRC=../tests/...` relative to the VM root, which is now `nodus/tuvm` — so it
> reaches for `nodus/nsl` (created in Task 2) and `nodus/tests` (created in
> Task 3). Before Task 1 it passed only because the VM root was the repo root,
> where `../nsl-tuvm` hit the real sibling repo.
>
> The failure must be exactly `No such file or directory` on
> `nodus/nsl/tools/ns_compile_oevm.py`. **Any other portability-32 failure is a
> real regression** — check Step 4. `Failed: 0` on the 239 tests is not
> negotiable and is what proves the move itself is sound.
>
> **`portability-32` first passes at Task 3**, which is the first fully green
> gate in this plan.

- [ ] **Step 6: Commit**

```bash
cd ~/workspace/tuvm
git add -A
git commit -m "refactor(rete): tuvm's own tree moves to nodus/tuvm (#79)

review/, docs/ and the two program-level tools (docmap.sh, diff_oracle.sh)
hoist to the root; everything else drops into nodus/tuvm/. Absolute
\$HOME/workspace/nsl-{tuvm,tests} references are repointed; the relative
sibling refs in test_portability_32.sh follow the nsl-tuvm -> nsl and
nsl-tests -> tests renames.

review/ moved as a whole tree with contents untouched, per D10. The one
edit inside it is a broken path in t0-gates/diff_oracle.sh.

Gate: 239 passed / 0 failed with OEVM_DIR pointed at nodus/tuvm.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Fold in `nsl-tuvm` → `nodus/nsl`

**Files:**
- Create: `nodus/nsl/` (from the `nsl-tuvm` repo, full history)

**Interfaces:**
- Consumes: the tree shape from Task 1.
- Produces: `nodus/nsl/tools/run_tests_oevm.py` — the harness every later gate invokes. Its `OEVM_DIR` default becomes `nodus/nsl/tools/../../tuvm` = `nodus/tuvm`, which is **correct without modification**.

- [ ] **Step 1: Add the remote and fetch**

```bash
cd ~/workspace/tuvm
git remote add nsl-src ~/workspace/nsl-tuvm
git fetch nsl-src
git log --oneline nsl-src/master | wc -l
```

Expected: a commit count well above 1. **Record this number** — Step 3 asserts against it.

- [ ] **Step 2: Subtree-add, WITHOUT `--squash`**

```bash
cd ~/workspace/tuvm
git subtree add --prefix=nodus/nsl nsl-src master
```

Expected: `Added dir 'nodus/nsl'`. No `--squash`. This is the Global Constraint.

- [ ] **Step 3: Verify history survived — the anti-`--squash` gate**

```bash
cd ~/workspace/tuvm
git merge-base --is-ancestor 639666a HEAD && echo "ANCESTRY OK" || echo "D5 FAILURE"
git rev-list 639666a | wc -l
git log --oneline -1 639666a
```

Expected: `ANCESTRY OK`; the `rev-list` count matches Step 1's count; and `639666a` — the nsl-tuvm `#108` merge — still resolves by SHA, which is what D5 exists for.

If ancestry fails, `git reset --hard HEAD~2` and redo Step 2 without `--squash`.

> **Do NOT use `git log --oneline -- nodus/nsl | wc -l` as the anti-`--squash` gate.** It returns **1** on a correct, non-squashed subtree add, so it cannot distinguish the two cases. `git subtree add` merges the source commit with its files at their *original* paths (`tools/…`); the `nodus/nsl/` prefix exists only in the merge's result tree, so no historical commit touches that pathspec and both plain and `--full-history` path logs stop at the merge. Verified empirically at Task 2 (2026-08-02) and reproduced in an isolated scratch repo. Ancestry is the real gate: `--squash` produces a commit with **no** link to the source head, so `merge-base --is-ancestor` fails exactly when history was destroyed.

- [ ] **Step 4: Point the moved shell scripts at the in-repo path**

Task 1 Step 3 left an interim absolute path. Now that `nsl` is in the repo, make it repo-relative.

```bash
cd ~/workspace/tuvm
# The three tests/*.sh already `cd "$(dirname "$0")/.."` into the repo first,
# so a plain `git rev-parse --show-toplevel` is both correct and robust there.
sed -i 's|\$HOME/workspace/tuvm/nodus/nsl|\$(git rev-parse --show-toplevel)/nodus/nsl|g' \
  nodus/tuvm/tests/porta_sync_host.sh \
  nodus/tuvm/tests/hanoi_ab_host.sh \
  nodus/tuvm/tests/http_roundtrip_host.sh
# run.sh has no preceding cd, but it already has an absolute $ZDIR on line 30.
sed -i 's|\$HOME/workspace/tuvm/nodus/nsl|\$(git -C "\$ZDIR" rev-parse --show-toplevel)/nodus/nsl|g' \
  nodus/tuvm/zephyr/run.sh
grep -n 'nodus/nsl' nodus/tuvm/tests/porta_sync_host.sh | head -2
```

Expected: the `NSL=` line derives from `git rev-parse --show-toplevel`, so a fresh clone anywhere works — which is what the Task 7 gate tests. Do **not** use `git -C "$(dirname "${BASH_SOURCE[0]}")"` in the three `tests/*.sh`: their preceding `cd` re-bases that relative path against the new cwd, so `bash nodus/tuvm/tests/hanoi_ab_host.sh` from the repo root dies with `fatal: cannot change to 'nodus/tuvm/tests'`.

- [ ] **Step 5: Run the gate from the in-repo harness**

```bash
cd ~/workspace/tuvm/nodus/nsl
python3 tools/run_tests_oevm.py 2>&1 | tail -8
```

Expected: this **will fail** — `run_tests_oevm.py` looks for `nsl-tests` at `tools/../../nsl-tests` = `nodus/nsl-tests`, which does not exist yet. Expected failure text: the suite finds 0 tests, or a path error naming `nodus/nsl-tests`.

This is the correct state at this point; Task 3 fixes it. Do not "fix" it here by creating a symlink.

- [ ] **Step 6: Commit**

```bash
cd ~/workspace/tuvm
git add -A
git commit -m "refactor(rete): fold nsl-tuvm into nodus/nsl, history intact (#79)

git subtree add without --squash, so every original SHA stays resolvable
(D5) — the docs cite 87 of them.

The harness needs no edit: run_tests_oevm.py resolves OEVM_DIR as
tools/../../../tuvm, which becomes nodus/nsl/tools/../../tuvm = nodus/tuvm.
Correct by accident, because nsl and tuvm remain siblings. Same for
test_supervisor_grant.py and test_debugger_integration.py.

The shell scripts move off Task 1's interim absolute path onto
git rev-parse --show-toplevel, so a fresh clone works anywhere.

The suite does not pass yet: nsl-tests is not folded in until the next
commit. That is expected, not a regression.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Fold in `nsl-tests` → `nodus/tests`, and the node side is whole

**Files:**
- Create: `nodus/tests/` (from the `nsl-tests` repo, full history)
- Modify: `nodus/nsl/tools/run_tests_oevm.py:161,397,578`, `nodus/nsl/tools/run-one.py:14`, `nodus/nsl/tools/test_deepsleep_node.py:13`, `nodus/nsl/tools/test_supervisor_grant.py:16`, `nodus/nsl/tools/test_debugger_integration.py:31`

**Interfaces:**
- Consumes: `nodus/nsl/` from Task 2.
- Produces: **the first fully green gate** — `239 passed, 0 failed` run entirely from inside the repo, with nothing reaching outside it.

- [ ] **Step 1: Subtree-add, WITHOUT `--squash`**

```bash
cd ~/workspace/tuvm
git remote add tests-src ~/workspace/nsl-tests
git fetch tests-src
SRC=$(git rev-parse tests-src/master)
git log --oneline tests-src/master | wc -l   # record this
git subtree add --prefix=nodus/tests tests-src master
git merge-base --is-ancestor $SRC HEAD && echo "ANCESTRY OK" || echo "D5 FAILURE"
git rev-list $SRC | wc -l                    # must match the recorded count
```

Expected: `ANCESTRY OK` and the two counts match. If ancestry fails, `--squash` slipped in — reset and redo. (Do **not** substitute `git log -- nodus/tests | wc -l`; it reads 1 even when history is intact — see the note at Task 2 Step 3.)

- [ ] **Step 2: Repoint the three `nsl-tests` lookups in the harness**

```bash
cd ~/workspace/tuvm
sed -i 's|parent\.parent\.parent / "nsl-tests"|parent.parent.parent / "tests"|g' nodus/nsl/tools/run_tests_oevm.py
grep -n '"tests"\|"nsl-tests"' nodus/nsl/tools/run_tests_oevm.py
```

Expected: three hits on `/ "tests"` (lines ~161, ~397, ~578), zero on `"nsl-tests"`.

> These resolve to `nodus/nsl/tools/../../tests` = `nodus/tests`. Correct.

- [ ] **Step 2b: Repoint the two aux gates' own nsl-tests paths**

```bash
cd ~/workspace/tuvm
sed -i 's|"\.\.", "nsl-tests"|"..", "tests"|' \
  nodus/nsl/tools/test_supervisor_grant.py \
  nodus/nsl/tools/test_debugger_integration.py
```

Expected: zero hits for `grep -rn 'nsl-tests' nodus/nsl/tools/*.py`. These two gates hardcode their own corpus path in a different expression from run_tests_oevm.py's, so Step 2's sed does not reach them. Missing this shows up as `grant: FAIL  debugger-integration: FAIL` with the tests themselves at 239/0 — a green test count next to two red aux gates, which is easy to skim past.

- [ ] **Step 3: Fix the absolute path in the deep-sleep aux gate**

```bash
cd ~/workspace/tuvm
sed -i 's|Path.home() / "workspace/tuvm/zephyr/|Path(__file__).resolve().parent.parent.parent / "tuvm" / "zephyr" / "|' nodus/nsl/tools/test_deepsleep_node.py
sed -n '13p' nodus/nsl/tools/test_deepsleep_node.py
```

Expected: the line derives from `__file__`, not `Path.home()`. An absolute `~/workspace/tuvm/...` would silently resolve to the *repo root* after the rename, where `zephyr/` no longer lives — a gate that reads green while testing a path that no longer exists.

- [ ] **Step 4: Fix the stale usage docstring in `run-one.py`**

```bash
cd ~/workspace/tuvm
sed -i 's|tools/run-one.py \.\./nsl-tests/|tools/run-one.py ../tests/|' nodus/nsl/tools/run-one.py
sed -n '14p' nodus/nsl/tools/run-one.py
```

Expected: `#   python3 tools/run-one.py ../tests/01_expressions/01_addition.nsl`

- [ ] **Step 5: Run the full gate — everything in-repo**

```bash
cd ~/workspace/tuvm/nodus/nsl
python3 tools/run_tests_oevm.py 2>&1 | tail -8
```

Expected:
```
  Passed:  239
  Failed:  0
  grant: PASS  debugger-integration: PASS  portability-32: PASS  boot-gc-asan: PASS  deepsleep: PASS  footprint: SKIP
```

Compare against the Task 0 Step 3 baseline. Any difference is a regression introduced by this migration — investigate before proceeding, do not proceed and hope.

- [ ] **Step 6: Confirm the tree is clean (issue #106 watch)**

```bash
cd ~/workspace/tuvm && git status --porcelain | grep -v '^??'
```

Expected: possibly `M nodus/tuvm/src/solidified_image.c` — that is #106, pre-existing and unrelated. Restore it: `git checkout nodus/tuvm/src/solidified_image.c`

- [ ] **Step 7: Commit**

```bash
cd ~/workspace/tuvm
git add -A
git commit -m "refactor(rete): fold nsl-tests into nodus/tests — the node side is whole (#79)

Three harness lookups move from nsl-tests to tests; run-one.py's usage line
follows. test_deepsleep_node.py's Path.home() absolute path becomes
__file__-relative — after the rename that absolute would resolve to the repo
ROOT, where zephyr/ no longer lives, i.e. a gate green against a path that
does not exist.

Gate: 239 passed / 0 failed, all aux gates PASS, run entirely from inside
the repo with nothing reaching outside it.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Fold in `porta`, hoist the protocol, scope the CI

**Files:**
- Create: `porta/` (from the `porta` repo, full history), `protocol/`, `protocol/v1/`, root `.github/workflows/`
- Modify: root `.gitignore`

**Interfaces:**
- Consumes: the tree from Task 3.
- Produces: `protocol/PROTOCOL-NSL.md` and `protocol/PROTOCOL-NSL-RECORDS.md` as the contract both sides cite; `porta/` as a self-contained Go module run from its own directory.

- [ ] **Step 1: Subtree-add, WITHOUT `--squash`**

```bash
cd ~/workspace/tuvm
git remote add porta-src ~/workspace/porta
git fetch porta-src
SRC=$(git rev-parse porta-src/master)
git log --oneline porta-src/master | wc -l   # record
git subtree add --prefix=porta porta-src master
git merge-base --is-ancestor $SRC HEAD && echo "ANCESTRY OK" || echo "D5 FAILURE"
git rev-list $SRC | wc -l                    # must match the recorded count
```

Expected: `ANCESTRY OK` and the two counts match. (Not a path-scoped `git log` count — see the note at Task 2 Step 3.)

- [ ] **Step 2: Hoist the protocol documents (D3)**

```bash
cd ~/workspace/tuvm
mkdir -p protocol/v1
git mv porta/docs/PROTOCOL-NSL.md porta/docs/PROTOCOL-NSL-RECORDS.md protocol/
git mv porta/docs/PROTOCOL.md porta/docs/PROTOCOL-NSL-draft-2026-07-11.md protocol/v1/
ls protocol protocol/v1
```

Expected: `protocol/` holds the two live documents; `protocol/v1/` holds the two superseded ones, kept as evidence per D9/§4.3. `ARCHITECTURE.md`, `CLI.md`, `DEVSDK.md`, `GETTING-STARTED.md` stay in `porta/docs/` — they are gateway-specific.

- [ ] **Step 3: Move porta's CI to the root and scope it (D13)**

```bash
cd ~/workspace/tuvm
mkdir -p .github/workflows
for f in porta/.github/workflows/*; do git mv "$f" ".github/workflows/porta-$(basename $f)"; done
rmdir -p porta/.github/workflows 2>/dev/null
ls .github/workflows
```

Then edit each moved workflow: add a `paths:` filter and a working directory, so a `kernel.nsl` edit does not run Go tests.

```yaml
on:
  push:
    paths: ['porta/**', 'protocol/**', '.github/workflows/porta-*']
  pull_request:
    paths: ['porta/**', 'protocol/**', '.github/workflows/porta-*']

defaults:
  run:
    working-directory: porta
```

Read the moved file first and merge these into its existing `on:` block rather than replacing it wholesale.

- [ ] **Step 4: Merge the `.gitignore`s, re-rooting every pattern**

```bash
cd ~/workspace/tuvm
cat nodus/nsl/.gitignore nodus/tests/.gitignore porta/.gitignore 2>/dev/null
```

Read each, then hand-merge into the root `.gitignore` with each pattern prefixed by its new location (e.g. porta's `/porta` binary becomes `/porta/porta`). Delete the merged child files with `git rm`. Add the artifacts D-table names as not-migrated: `/porta/porta`, `/porta/porta.db`, `/porta/porta-soak.log`, `/nodus/tuvm/oevm_bench`, `/nodus/tuvm/oevm_runner`.

```bash
git rm --cached porta/porta porta/porta.db porta/porta-soak.log 2>/dev/null || true
git rm --cached nodus/tuvm/oevm_bench nodus/tuvm/oevm_runner 2>/dev/null || true
```

(`--cached` only — the local files stay, they just stop being tracked.)

- [ ] **Step 5: Fix the protocol-document references now that the docs moved**

```bash
cd ~/workspace/tuvm
grep -rln 'docs/PROTOCOL-NSL' porta/ nodus/ review/ docs/ protocol/ | head -20
```

For each hit, repoint `porta/docs/PROTOCOL-NSL*.md` → `protocol/PROTOCOL-NSL*.md` and `porta/docs/PROTOCOL.md` → `protocol/v1/PROTOCOL.md`. Edit by hand — some are prose sentences, not paths, and a blind `sed` would mangle them.

- [ ] **Step 6: Run both gates**

```bash
cd ~/workspace/tuvm/porta && go test ./... 2>&1 | tail -6
cd ~/workspace/tuvm/nodus/tuvm && bash tests/porta_sync_host.sh 2>&1 | tail -4
```

Expected: `ok` for every Go package; `PORTA SYNC OK`.

`porta_sync_host.sh` builds a `porta serve` and drives the nsl node against it over UDP loopback, so it is the one gate that exercises **both children and the protocol between them**. If it passes here, the merge did not break the seam the whole restructure exists to protect.

- [ ] **Step 7: Re-run the node gate, to prove Task 4 broke nothing upstream**

```bash
cd ~/workspace/tuvm/nodus/nsl && python3 tools/run_tests_oevm.py 2>&1 | tail -4
```

Expected: `Passed: 239 / Failed: 0`.

- [ ] **Step 8: Commit**

```bash
cd ~/workspace/tuvm
git add -A
git commit -m "refactor(rete): fold porta in, hoist the protocol to its own child (#79)

The contract belongs to neither side (D3), so PROTOCOL-NSL.md and
PROTOCOL-NSL-RECORDS.md move to protocol/, and v1's two superseded
documents to protocol/v1/ — deleted as code, kept as evidence, which is
useless if it is only reachable in an archived repo nobody opens.

porta's CI moves to the root with a paths: filter (D13), or a kernel.nsl
edit would run Go tests. The five .gitignores merge with every pattern
re-rooted, and the tracked build artifacts are untracked.

The goldens are NOT extracted here — they are still duplicated inline in
porta/internal/cval/cval_test.go and nodus/tests/14_cbor/02_golden_hex.nsl.
protocol/ gives them a home; filling it is a separate issue (design §9).

Gates: go test ./... green, PORTA SYNC OK, and 239/0 still green.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Rename the repo, transfer the open issues, rewrite the references

**Files:**
- Modify: the 42 `porta#NN` sites (9 files) **and** the 46 `nsl-tuvm#NN` sites
  (26 files) across `review/`, `docs/`, `protocol/`, `porta/` and `nodus/`.
  Both source repos have their open issues transferred by Step 3, so both need
  a captured list, a mapping and a rewrite. `nsl-tests#NN` has 0 sites and 0
  open issues — nothing to do. Counts measured 2026-08-02.

**Interfaces:**
- Consumes: Task 0 Step 4's answer on whether public→private transfer works.
- Produces: one live tracker; every `#NN` in the docs resolving to the right issue.

- [ ] **Step 1: Rename the GitHub repo**

```bash
cd ~/workspace/tuvm
gh repo rename rete --repo davidg238/tuvm --yes
git remote set-url origin git@github.com:davidg238/rete.git
git remote -v
gh repo view davidg238/rete --json name,visibility --jq '.name + " " + .visibility'
```

Expected: `rete PRIVATE`. GitHub keeps a permanent redirect from `tuvm` — **never create a repo named `tuvm` again** (Global Constraints).

- [ ] **Step 2: Capture the pre-transfer issue list, so the mapping can be built**

**Both source repos, not just `porta`.** Step 3 transfers from `porta` AND
`nsl-tuvm`; a list captured for only one of them yields a mapping for only one
of them, and the other repo's references get silently skipped in Step 5.

```bash
cd ~/workspace/porta
gh issue list --state open --limit 100 --json number,title --jq '.[] | "\(.number)\t\(.title)"' > /tmp/porta-open-before.tsv
wc -l /tmp/porta-open-before.tsv
cd ~/workspace/nsl-tuvm
gh issue list --state open --limit 100 --json number,title --jq '.[] | "\(.number)\t\(.title)"' > /tmp/nsltuvm-open-before.tsv
wc -l /tmp/nsltuvm-open-before.tsv
```

Expected: 17 lines for `porta`, 11 for `nsl-tuvm` (or whatever the current
counts are — record both). 17 + 11 = the 28 of Step 3.

- [ ] **Step 3: Transfer the 28 open issues**

If Task 0 Step 4 recorded `TRANSFER_WORKS=yes`:

```bash
cd ~/workspace/porta
while IFS=$'\t' read -r num title; do
  echo "transferring porta#$num — $title"
  gh issue transfer "$num" davidg238/rete
done < /tmp/porta-open-before.tsv

cd ~/workspace/nsl-tuvm
while IFS=$'\t' read -r num title; do
  echo "transferring nsl-tuvm#$num — $title"
  gh issue transfer "$num" davidg238/rete
done < /tmp/nsltuvm-open-before.tsv
```

Transfers **renumber** (proved in Task 0). Never delete a transferred issue —
that destroys its redirect. Note Task 0 Step 4 told you to delete the probe;
that was the one action documented to kill a redirect, and it should not be
repeated for a real issue.

If Task 0 Step 4 recorded `TRANSFER_WORKS=no`, use the fallback: re-file each open issue by hand in `rete` with its original body plus a first line `Transferred from davidg238/porta#NN`, then close the original with a comment pointing at the new number.

- [ ] **Step 4: Build the old→new mapping**

```bash
cd ~/workspace/tuvm
gh issue list --repo davidg238/rete --state open --limit 200 --json number,title \
  --jq '.[] | "\(.number)\t\(.title)"' > /tmp/rete-open-after.tsv
join -t $'\t' -1 2 -2 2 -o 1.1,2.1 \
  <(sort -t $'\t' -k2 /tmp/porta-open-before.tsv) \
  <(sort -t $'\t' -k2 /tmp/rete-open-after.tsv) > /tmp/porta-map.tsv
join -t $'\t' -1 2 -2 2 -o 1.1,2.1 \
  <(sort -t $'\t' -k2 /tmp/nsltuvm-open-before.tsv) \
  <(sort -t $'\t' -k2 /tmp/rete-open-after.tsv) > /tmp/nsltuvm-map.tsv
wc -l /tmp/porta-map.tsv /tmp/nsltuvm-map.tsv
cat /tmp/porta-map.tsv /tmp/nsltuvm-map.tsv
```

Expected: 17 pairs in `porta-map.tsv` and 11 in `nsltuvm-map.tsv`, matched on
title. **The counts must equal the Step 2 line counts exactly.** Eyeball both —
a title that changed during transfer will not join, and a missing row means a
reference is rewritten wrongly or not at all. A short map is the failure that
looks like success: Step 5 simply skips what is not in it.

> Titles are the join key across BOTH maps against the same
> `rete-open-after.tsv`, so a title colliding between the two source repos
> would join wrong. Check for that before rewriting:
> `cut -f2 /tmp/porta-open-before.tsv /tmp/nsltuvm-open-before.tsv | sort | uniq -d`
> must print nothing.

- [ ] **Step 5: Rewrite the 42 `porta#NN` and 46 `nsl-tuvm#NN` sites**

```bash
cd ~/workspace/tuvm
for src in porta nsltuvm; do
  case $src in porta) prefix=porta;; nsltuvm) prefix=nsl-tuvm;; esac
  while IFS=$'\t' read -r old new; do
    grep -rl "$prefix#$old\b" review docs protocol porta nodus 2>/dev/null | while read f; do
      sed -i "s|$prefix#$old\b|#$new|g" "$f"
    done
  done < /tmp/$src-map.tsv
done
grep -rn 'porta#[0-9]\|nsl-tuvm#[0-9]' review docs protocol porta nodus | grep -v node_modules
```

Expected: the remaining hits are references to **closed** issues in either
repo, which are never transferred and stay in the archive. Rewrite each by hand
to a full URL: `porta#16` → `https://github.com/davidg238/porta/issues/16`,
`nsl-tuvm#4` → `https://github.com/davidg238/nsl-tuvm/issues/4`.

> **Why this must not be skipped, and why it is worse than a dead link.**
> `repo#NN` is not a GitHub autolink (only `owner/repo#NN` is) — it is a
> navigation convention this program reads by. `tuvm`'s own numbers are
> preserved by the in-place restructure, so after the merge the same integer
> names a real, unrelated issue rather than nothing: `nsl-tuvm#10` is the
> LD_SLOT codegen gotcha cited in `nodus/tuvm/zephyr/ThreadTimerNode.nsl:23`
> and `SoakNode.nsl:21`, while `tuvm#10` is a docs-sync task; `nsl-tuvm#4` is
> the oimg SMI-constant corruption cited in
> `nodus/tests/06_limit_tests/07_smi_boundary_constants.nsl:10`, while `tuvm#4`
> is the Integer bitop gap. Nothing errors. You just read the wrong issue.
> Counts measured 2026-08-02: `porta#NN` 42 sites in 9 files, `nsl-tuvm#NN` 46
> sites in 26 files, `nsl-tests#NN` 0. An earlier draft of this plan said
> "46 `porta#NN`" — that 46 was the *nsl-tuvm* count, and conflating the two is
> how `nsl-tuvm` fell out of Steps 2, 4 and 5 entirely.

- [ ] **Step 6: Verify a sample resolves**

Pick three rewritten references and confirm each names the issue the surrounding prose describes:

```bash
cd ~/workspace/tuvm
grep -rn '#[0-9]\+' review/program-sequence.md | tail -5
gh issue view <one-of-those-numbers> --json title --jq .title
```

Expected: the title matches what the sentence claims the issue is about. A mechanical rewrite that produces a valid-but-wrong number is worse than a broken link, because nothing flags it.

- [ ] **Step 7: Commit**

```bash
cd ~/workspace/tuvm
git add -A
git commit -m "refactor(rete): rename to rete, unify the tracker, rewrite the refs (#79)

The 107 tuvm issues keep their numbers, which is the whole reason for
restructuring in place — 838 bare #NN references keep resolving. The 28 open
issues from porta and nsl-tuvm transfer in; closed ones stay readable in the
archived repos, so the references citing them as history stay valid where
they are.

The 42 porta#NN and 46 nsl-tuvm#NN sites are rewritten from their transfer
mappings, joined on title. References to closed issues in either repo become
full URLs into the archive. Both repos are covered, not just porta: tuvm's own
numbers are preserved, so an unrewritten repo#NN does not dangle — it silently
names a different, real issue.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: One `CLAUDE.md`, a regenerated `DOCMAP`, and the orientation docs

**Files:**
- Modify: root `CLAUDE.md` (merging five), `review/DOCMAP.md`, `docs/next-steps-overview.md`, `tools/docmap.sh`
- Create: root `README.md`
- Delete: `nodus/nsl/CLAUDE.md`, `porta/CLAUDE.md` (contents merged)

**Interfaces:**
- Consumes: the finished tree.
- Produces: the orientation surface a future session reads first.

- [ ] **Step 1: Read all the CLAUDE.md files before merging any of them**

```bash
cd ~/workspace/tuvm
for f in CLAUDE.md nodus/nsl/CLAUDE.md porta/CLAUDE.md; do echo "=== $f"; cat "$f"; done
```

Merge into the root file. Keep every rule; do not drop one because it looks redundant — a rule that survived this long earned its place. Add three new lines:

- The tree map (`porta/`, `nodus/{tuvm,nsl,tests,toit}`, `protocol/`, `review/`).
- **"Never create a GitHub repo named `tuvm` — it silently breaks the rete rename redirect."**
- The corrected search recipe (see Step 3).

- [ ] **Step 2: Update `tools/docmap.sh` for the new single-root layout**

```bash
cd ~/workspace/tuvm
grep -n 'workspace' tools/docmap.sh
```

Replace the five doc-home paths with the in-repo ones (`docs`, `nodus/nsl/docs`, `nodus/tuvm/docs`, `porta/docs`, `protocol`, `review`) plus `~/workspaceToit/stic` — which **stays outside** per D12.

- [ ] **Step 3: Fix the cross-program search recipe in `review/DOCMAP.md`**

The header currently claims *"five doc homes across two workspace roots"*. That becomes **two**, not one — `~/workspaceToit/stic` survives the merge (design §5.3, corrected in review).

```
rg -i "<term>" ~/workspace/rete ~/workspaceToit/stic
```

- [ ] **Step 4: Regenerate the inventory**

```bash
cd ~/workspace/tuvm && bash tools/docmap.sh && git diff --stat review/DOCMAP.md
```

Expected: the generated inventory below the `BEGIN GENERATED INVENTORY` marker is rewritten with in-repo paths.

- [ ] **Step 4b: Sweep the stale path citations in the documents of record**

Tasks 1–4 repointed every path that *executes*. This sweeps the paths that are
merely *cited* — and only in the mainline documents of record, per David's
ruling of 2026-08-02. The historical record is deliberately left alone and is
filed as a deferral in Task 8 Step 2b.

Counts measured 2026-08-02 against the tree at `252ff1c`, with ONE combined
regex — `grep -rE 'nsl-tests/|nsl-tuvm/|~/workspace/(nsl-tuvm|nsl-tests|porta)'`.
Do not sum separate per-pattern greps: `~/workspace/nsl-tuvm/docs/x` matches two
of those alternatives at once, and summing them overcounts by ~35% (1,424 vs
the true 1,050). An earlier revision of this step made exactly that mistake.

**In scope here — 106 hits in 16 files:**

| target | hits | files | treatment |
|---|---|---|---|
| `review/` except `DOCMAP.md` | 101 | 14 | hand-edit (incl. `program-sequence.md` 19, `tests/CLAIMS.md` 37, `lib/**` 7) |
| `protocol/PROTOCOL-NSL-RECORDS.md` | 3 | 1 | hand-edit — this is the LIVE contract Task 4 hoisted; it is not history |
| `docs/next-steps-overview.md` | 2 | 1 | Step 5 covers it |
| `CLAUDE.md` | 0 | — | nothing to do |
| `review/DOCMAP.md` | 163 | 1 | **do not hand-edit** — Step 4 regenerates it |

Everything else — 781 hits in 77 files — is the historical record and is
deferred by Task 8 Step 2b. The three buckets sum to 1,050 across 94 files; if
your recount does not reconcile, find the remainder before editing. Doing that
reconciliation is what surfaced `protocol/PROTOCOL-NSL-RECORDS.md` sitting in
the wrong bucket.

```bash
cd ~/workspace/tuvm
grep -rnE 'nsl-tests/|nsl-tuvm/|~/workspace/(nsl-tuvm|nsl-tests|porta)' \
  review/ protocol/ docs/next-steps-overview.md CLAUDE.md \
  --include=*.md --include=*.html | grep -v '^review/DOCMAP.md'
```

Rewrite each hit: `nsl-tests/X` → `nodus/tests/X`, `nsl-tuvm/X` → `nodus/nsl/X`,
`~/workspace/nsl-tuvm` → `~/workspace/rete/nodus/nsl`, `~/workspace/nsl-tests` →
`~/workspace/rete/nodus/tests`, `~/workspace/porta` → `~/workspace/rete/porta`.

**By hand, not with `sed`** — the same reason Task 4 Step 5 says so, and it was
the right call there: many hits are prose, and some sit in shell blocks whose
paired `cd` must move with the path or the block becomes self-contradictory.
`review/` content is otherwise untouched — this is the D10 path-fix permission,
not licence to tidy.

Verification: re-run the grep above; expected zero hits outside `review/DOCMAP.md`.
Then confirm a sample of five rewritten paths actually exist:
`while read -r p; do [ -e "$p" ] || echo "DEAD: $p"; done`

- [ ] **Step 5: Update `docs/next-steps-overview.md`**

Add a line to Part 2 recording that the restructure landed and #56 is next. Replace any `~/workspace/{tuvm,nsl-tuvm,nsl-tests,porta}` path in the file with its `rete` equivalent.

- [ ] **Step 6: Write the root `README.md`**

Minimal and honest — this is **not** #79's front door (design §9 is explicit that the quickstart is out of scope). One paragraph on what rete is, the tree map, and how to run the two gates:

```markdown
# rete

An embedded fleet system: **nodus** (the device end of a protocol) and **porta**
(what orchestrates them), with the wire contract they share in `protocol/`.

    porta/       the orchestrator (Go)
    nodus/       the device end
      tuvm/      the VM (C) + Zephyr board ports
      nsl/       the dialect, compiler and kernel
      tests/     the nsl suite
    protocol/    the wire contract
    review/      the program ladder and claims ledger

Gates:

    cd nodus/nsl && python3 tools/run_tests_oevm.py     # 239 passed, 0 failed
    cd porta && go test ./...
    cd nodus/tuvm && bash tests/porta_sync_host.sh      # PORTA SYNC OK
```

- [ ] **Step 7: Commit**

```bash
cd ~/workspace/tuvm
git add -A
git commit -m "docs(rete): one CLAUDE.md, a regenerated DOCMAP, a root README (#79)

Five CLAUDE.md files merge into one, with every rule kept. Two new standing
rules: the tree map, and never create a repo named tuvm again — it silently
breaks the rename redirect and with it every old issue URL.

DOCMAP's header goes from five doc homes across two workspace roots to TWO,
not one: ~/workspaceToit/stic is a separate repo cited 13x in these docs and
stays outside rete on purpose (D12) — a harness that measures rete against
other VMs does not belong inside rete.

The README is deliberately minimal. It is not #79's front door; the quickstart
is explicitly out of scope (design §9).

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Task 7: Archive, delete the local clones, and the fresh-clone gate

This is the task that decides whether the whole exercise worked.

**Files:** none in the repo.

**Interfaces:**
- Consumes: everything.
- Produces: proof that no script still reaches outside the repository.

- [ ] **Step 1: Push everything**

```bash
cd ~/workspace/tuvm && git push origin master && git log --oneline -1
```

- [ ] **Step 2: Delete the local clones — BEFORE the gate, not after**

```bash
rm -rf ~/workspace/nsl-tuvm ~/workspace/nsl-tests ~/workspace/porta
ls -d ~/workspace/nsl-tuvm ~/workspace/nsl-tests ~/workspace/porta 2>&1
```

Expected: three "No such file or directory" errors.

> This ordering is the entire point. Every script here resolves siblings by relative path, and the stale clones sit *exactly where the old paths point*. Run the gate first and a half-migrated script finds the old clone, passes green, and proves nothing. Their history is already inside `rete` and pushed to their own origins — nothing is lost.

- [ ] **Step 3: Fresh clone into an empty parent**

```bash
rm -rf /tmp/rete-gate && mkdir -p /tmp/rete-gate
cd /tmp/rete-gate && git clone git@github.com:davidg238/rete.git
ls /tmp/rete-gate/rete
```

Expected: the tree from the Global Constraints, and **nothing else in `/tmp/rete-gate/`** — no siblings for a stray relative path to find.

- [ ] **Step 4: THE GATE — run everything from the fresh clone**

```bash
# BOOTSTRAP FIRST — see the note below. Without this the gate reads 0/239.
cd /tmp/rete-gate/rete/nodus/nsl/grammar && npm install
cd /tmp/rete-gate/rete/nodus/nsl && python3 tools/run_tests_oevm.py 2>&1 | tail -8
cd /tmp/rete-gate/rete/porta && go test ./... 2>&1 | tail -6
cd /tmp/rete-gate/rete/nodus/tuvm && bash tests/porta_sync_host.sh 2>&1 | tail -4
```

> **The `npm install` is mandatory and is not consolidation state.** The nsl
> parser `nodus/nsl/grammar/ns-parse.js` requires `ohm-js`, which lives in
> `grammar/node_modules/` — gitignored in the source repo and therefore in
> every clone, before and after this consolidation. `nodus/nsl/README.md:41`
> has always documented it. Discovered at Task 3 (2026-08-02): the first run
> of the Task 3 gate in a fresh worktree returned **0 passed / 239 failed**
> with every aux gate FAIL, purely for want of this one package. That failure
> mode is indistinguishable at a glance from "the consolidation broke
> everything", so bootstrap before you gate, and do not diagnose a 0/239 as a
> path regression until `node -e "require('ohm-js')"` succeeds from
> `nodus/nsl/grammar`.

Expected, all three:
```
  Passed:  239
  Failed:  0
  grant: PASS  debugger-integration: PASS  portability-32: PASS  boot-gc-asan: PASS  deepsleep: PASS  footprint: SKIP
ok  	github.com/davidg238/porta/...
PORTA SYNC OK
```

Any failure names a path that still reaches outside the repo. Fix it in `~/workspace/tuvm`, push, re-clone, re-run. **Do not fix it in `/tmp` and declare victory** — the clone is the instrument, not the workspace.

- [ ] **Step 5: Verify history and citations survived**

```bash
cd /tmp/rete-gate/rete
for s in 639666a a0ed88f bdbedf9 0fb9a21; do
  printf "%s: " "$s"; git log --oneline -1 "$s" 2>/dev/null || echo "MISSING"
done
for s in 639666a a0ed88f bdbedf9 0fb9a21; do
  printf "%s ancestry: " "$s"
  git merge-base --is-ancestor "$s" HEAD 2>/dev/null && echo OK || echo "D5 FAILURE"
done
```

Expected: all four SHAs resolve (they are cited in `review/program-sequence.md`) and all four are ancestors of `HEAD`. If any is missing or non-ancestral, `--squash` got in somewhere — that is a D5 failure and the subtree task must be redone. (Path-scoped `git log -- nodus/nsl | wc -l` is **not** a valid check here — see the note at Task 2 Step 3.)

- [ ] **Step 6: Archive the four old repos**

```bash
gh repo archive davidg238/nsl-tuvm --yes
gh repo archive davidg238/nsl-tests --yes
gh repo archive davidg238/porta --yes
gh repo archive davidg238/nodus-v1 --yes
for r in nsl-tuvm nsl-tests porta nodus-v1; do
  gh repo view davidg238/$r --json name,isArchived --jq '.name + " archived=" + (.isArchived|tostring)'
done
```

Expected: four `archived=true`. **Archived, never deleted** (D7) — deletion breaks every full-URL reference into the archive (the closed-issue citations Step 5 leaves behind), every permalink, and the redirects left by the 28 transferred issues.

`porta` and `nodus-v1` stay **public**; `nsl-tuvm` and `nsl-tests` stay private. Do not change visibility.

- [ ] **Step 7: Rename the working directory**

```bash
cd ~ && mv ~/workspace/tuvm ~/workspace/rete && ls -d ~/workspace/rete
```

Then update the scratchpad/session paths and the memory orientation file, which both name `~/workspace/tuvm`.

- [ ] **Step 8: Clean up and record the result**

```bash
cd ~/workspace/rete && git remote remove nsl-src && git remote remove tests-src && git remote remove porta-src
git remote -v
rm -rf /tmp/rete-gate
```

Then comment the fresh-clone gate output on #79 and update `review/program-sequence.md` with a rung entry — per the promotion rule, this work is not closed until a mainline document absorbs it.

- [ ] **Step 9: Commit**

```bash
cd ~/workspace/rete
git add -A
git commit -m "chore(rete): the consolidation closes — fresh-clone gate green (#79)

Four repos archived, never deleted (D7): deletion would break the 46
porta#NN references and every permalink. Local clones deleted BEFORE the
gate ran, not after — they sat exactly where the old relative paths point,
so a half-migrated script would have found them, passed green and proved
nothing.

Gate: a fresh clone into an empty parent directory, 239 passed / 0 failed
with every aux gate PASS, go test ./... green, PORTA SYNC OK. Four cited
SHAs still resolve and no subtree collapsed to a single commit, so D5 holds.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Task 8: File what was deferred

Design §9 lists what this change does **not** fix. An unfiled deferral is indistinguishable from an oversight.

- [ ] **Step 1: File the goldens-extraction issue**

```bash
cd ~/workspace/rete
gh issue create --title "protocol: the CBOR goldens are duplicated by hand in two test files, with no shared source" --body "$(cat <<'EOF'
The `protocol/` directory now exists as the contract both implementations answer to, but the goldens are not in it — because they do not exist as a shared artifact at all.

The vectors are duplicated inline, once per implementation:
- `porta/internal/cval/cval_test.go` (Go)
- `nodus/tests/14_cbor/02_golden_hex.nsl` (nsl)

Nothing checks one against the other. They stay in sync because someone remembers to update both.

This matters more than ordinary duplication. The reason nsl and Go agreed on the **first** protocol run is that plan 1 golden-tested `cval` against the VM's own CBOR rather than against the prose spec. That property — the thing that made the rewrite work — is currently held together by two hand-maintained copies.

The fix is a shared vector file under `protocol/goldens/` that both test suites read. It changes test code on both sides, which is why it was deliberately deferred out of the consolidation rather than absorbed into it.

Refs #79. Design: `docs/design/2026-08-02-rete-repo-consolidation-design.md` §2, §9.
EOF
)"
```

- [ ] **Step 2: Comment the surviving deferrals on their existing issues**

- **#63** (nine enumerated source lists): comment that it is now a single-repo problem — still nine lists, not fixed, but no longer spanning two repos.
- **#79**: comment that the merge landed and the front door remains unbuilt, so #79 stays open on its actual blocker.

- [ ] **Step 2b: File the historical stale-path-citation deferral**

Task 6 Step 4b sweeps the documents of record. It deliberately leaves the
historical record, and that decision must be recorded rather than absorbed —
this task exists because a decision nobody wrote down is indistinguishable from
an oversight.

```bash
cd ~/workspace/rete
gh issue create --title "Stale path citations in the historical record after the rete consolidation (#79)" --body "$(cat <<'EOF'
781 path citations across 77 files still name the pre-merge repo roots
(`nsl-tuvm/`, `nsl-tests/`, `~/workspace/porta`). Measured 2026-08-02 against
`252ff1c` with one combined regex — not by summing per-pattern greps, which
double-counts any line matching two alternatives:

| area | hits | files |
|---|---|---|
| `docs/superpowers/plans/` | 356 | 20 |
| `nodus/nsl/docs/` | 305 | 31 |
| `docs/design/` | 50 | 15 |
| `nodus/tuvm/` + `nodus/tests/` | 25 | 6 |
| `porta/docs/` | 24 | 2 |
| `docs/MILESTONE_{A,D,E}.md` | 21 | 3 |

**These were left deliberately, not missed.** The documents of record — 106
hits in 16 files: `review/` except DOCMAP, `protocol/PROTOCOL-NSL-RECORDS.md`,
`docs/next-steps-overview.md` — were swept in Task 6 Step 4b, and
`review/DOCMAP.md`'s 163 are regenerated from `tools/docmap.sh`. What remains is the
historical record: dated design docs and closed execution plans. Per the
`CLAUDE.md` docs convention the dated design doc *is* the historical "why", so
rewriting its paths would falsify what was true when it was written.

**The cost of leaving them.** The three local clones were deleted in Task 7
Step 8, so these citations no longer resolve on disk. Following one from a
closed plan now leads nowhere. The mapping is mechanical if it is ever wanted:
`nsl-tuvm/X` → `nodus/nsl/X`, `nsl-tests/X` → `nodus/tests/X`,
`~/workspace/porta` → `~/workspace/rete/porta`.

**Do not fix this with a blind `sed`.** Task 4 Step 5 and Task 6 Step 4b both
required hand-editing for the same reason: many hits are prose, and some sit in
shell blocks whose paired `cd` must move with the path or the block becomes
self-contradictory.

Legitimate outcomes include closing this as WONTFIX — the point is that the
choice is on the record.

Refs #79.
EOF
)"
```

- [ ] **Step 3: Verify no deferral is silent**

```bash
cd ~/workspace/rete
gh issue list --state open --search "goldens OR consolidation" --limit 10
```

Expected: the goldens issue exists. Every item in design §9 now has either a fix or an issue number.

---

## Self-Review

**Spec coverage.** Every design section maps to a task: §3 tree + §4 fates → Tasks 1–4; §4.2 `review/` → Task 1 Steps 1–2 (with a byte-identical check); §4.3 `protocol/v1/` → Task 4 Step 2; §4.4 CI → Task 4 Step 3; §5.1 subtree-not-filter-repo → Tasks 2/3/4 Step 1 with anti-`--squash` gates; §5.2 tracker → Task 5; §5.3 archive-not-delete → Task 7 Step 6; §6 visibility → Task 7 Step 6 (explicitly does not change it); §7.1 preconditions → Task 0; §7.3 seams → Tasks 1–3; §8 fresh-clone gate → Task 7; §9 deferrals → Task 8; §10 sequencing → after this plan, #56.

**Gaps found and fixed while executing (2026-08-02).** Recorded here because
each was a defect in this document, not in the work:

1. **The anti-`--squash` gate did not work.** Tasks 2/3/4 Step 1 and Task 7
   Step 5 all used `git log --oneline -- <prefix> | wc -l`, which returns **1**
   on a *correct* non-squashed `git subtree add` — the imported commits touch
   the source's original paths, so none match the new prefix. It could never
   detect the thing it existed to detect. Replaced everywhere with
   `git merge-base --is-ancestor <source-head> HEAD`.
2. **Task 1's gate was impossible as written** — it demanded all aux gates PASS
   while `portability-32` cannot pass until Task 3 creates `nodus/tests`.
   Corrected to require the failure and pin its exact form.
3. **The npm bootstrap was missing.** `nodus/nsl/grammar` needs `ohm-js` from a
   gitignored `node_modules`, so a fresh checkout gates at **0 passed / 239
   failed with every aux gate FAIL** — a signature indistinguishable from "the
   consolidation broke everything". Task 7 Step 4 now bootstraps first.
4. **Task 3's Modify list was incomplete.** `test_supervisor_grant.py` and
   `test_debugger_integration.py` hardcode their own corpus path in a different
   expression from the one Step 2's `sed` targets. Added as Step 2b.
5. **Task 5 rewrote references for only one of the two repos it transfers.**
   Steps 2/4/5 covered `porta#NN` but not `nsl-tuvm#NN` (46 sites, 26 files),
   even though Step 3 transfers both. The plan's "46 `porta#NN`" was itself the
   *nsl-tuvm* count; porta has 42. Both are now captured, mapped and rewritten.
6. **Nothing swept the stale path citations.** 1,050 across 94 files. The 106
   in the documents of record are now swept in Task 6 Step 4b; the 781
   historical ones are deliberately left and filed in Task 8 Step 2b.
7. **This section's own first count was wrong**, and the task reviewer caught
   it. Item 6 originally said "~1,400 across ~100 files", which was the sum of
   five separate per-pattern greps — a line reading `~/workspace/nsl-tuvm/docs/x`
   matches two of those patterns, so the sum overcounts by ~35%. One combined
   regex gives 1,050. Re-measuring to make the buckets reconcile is also what
   surfaced `protocol/PROTOCOL-NSL-RECORDS.md` — the live contract Task 4 had
   just hoisted — sitting in the historical bucket instead of the swept one.
   The lesson is not "count with a command"; that was done. It is that
   overlapping patterns do not sum, and a table whose parts do not add up to
   its total is telling you something.

**Placeholder scan.** No TBD/TODO. Two steps intentionally require reading before editing rather than a blind command — Task 4 Step 3 (merging into an existing `on:` block) and Task 4 Step 5 (prose references a `sed` would mangle) — and both say so and say why.

**Type consistency.** Path names are consistent throughout: `nodus/tuvm`, `nodus/nsl`, `nodus/tests`, `porta`, `protocol`, `protocol/v1`. `OEVM_DIR` is the only environment knob and is used identically in Task 1 Step 5 and nowhere after, because Task 2 makes the default correct.

**One gap found and fixed during review:** Task 2 Step 5 originally asserted a green suite immediately after folding in `nsl-tuvm`, which is impossible — `nsl-tests` is not merged until Task 3. It now states the expected *failure* and forbids papering over it with a symlink.
