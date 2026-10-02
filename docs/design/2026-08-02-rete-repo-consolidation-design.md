# rete — collapsing five repos into one system

**Status:** design, ratified in conversation 2026-08-02. Not yet executed.
**Issue:** #79 (the "read five repos" blocker), and the developer friction it turns out to
be tangled with.
**Supersedes nothing.** Adds a structural decision the program has never taken.

---

## 1. The problem, stated twice because it is two problems

#79 names one blocker in one sentence:

> Today there is no entry point that does not start with "read five repos".

That is the **newcomer** problem. Its fix is a front door — a quickstart, one board recipe,
one clone that ends with something blinking. An outsider does not care how many repos exist
if one of them gets them running.

Underneath it is a second, unnamed problem: the **developer** one. The five repos are not
five components. They are one program that happens to be stored in five places.

The evidence is not impressionistic:

- All four live repos last committed **2026-08-02**. They move in lockstep.
- Fixing #108 — a single change of about twenty lines — took **three repos, three branches,
  three commits**.
- #63 records that adding one `.c` file to the VM means editing **nine enumerated source
  lists across two repos**.
- The space-sync rewrite paid the tax at its worst: plan 1 (porta, Go) and plan 2 (the node,
  nsl) were separate repos implementing **one wire protocol**, and the CBOR goldens had to be
  hand-carried between them.
- There is no consumer of `nsl-tests` without `nsl-tuvm`, and none of `nsl-tuvm` without
  `tuvm`. Nobody defends these boundaries; they are an accident of the order things were
  built in.

**A monorepo fixes the second problem outright and merely *enables* a fix for the first.**
Keeping them distinct matters, because the same conflation between a public bar and a fleet
bar already cost this program clarity once (`docs/next-steps-overview.md` Part 2). This
document is about the second problem. The front door is deliberately out of scope, and §9
says so again.

---

## 2. The shape: porta and nodus are the two halves of a protocol

The organising insight is David's, and it reframes what the repos are for.

`nodus` is **not** the Toit repo. It is the **device end of a protocol** — the umbrella term
for every node that speaks it. The repo currently named `nodus` is strictly Toit, and that is
an artifact of it being the only implementation ever written, not an architectural statement.

Symmetrically, a fleet of devices with nothing orchestrating them is not a system. `porta` is
the orchestrator. Neither half is meaningful alone, which is exactly what makes them the two
top-level children.

**The protocol itself belongs to neither.** It is what *makes* them two sides. Today
`PROTOCOL-NSL.md` and `PROTOCOL-NSL-RECORDS.md` live under `porta/docs/`, which quietly
asserts the gateway owns the contract. It does not.

And the goldens are worse than misfiled — **they do not exist as a shared artifact at all.**
Checked rather than assumed: the vectors are duplicated by hand, inline, once per
implementation — `porta/internal/cval/cval_test.go` on the Go side and
`nsl-tests/14_cbor/02_golden_hex.nsl` on the nsl side. There is no single source either one
is checked against.

That matters because the reason nsl⇔Go agreed on the *first* protocol run is that plan 1
golden-tested `cval` against the VM's own CBOR rather than against prose. The property that
made the rewrite work is currently held together by two hand-maintained copies staying in
sync. Making it structural is the point of `protocol/`.

**But extraction is real work, not a `git mv`** — it changes test code on both sides and
deserves its own gate. This design **hoists the protocol documents** (a move) and **files the
goldens extraction as a follow-up issue** (§9). Deferred, explicitly, not absorbed.

### 2.1 Two axes, not one — and only one of them earns a directory

"Various os/language/silicon combinations" sounds like one axis. It is two, and they behave
completely differently:

- **Runtime axis** — nsl-on-tuvm, Toit, possibly st. These share *nothing but the wire*.
  Genuinely separate trees. **This is what earns `nodus/` its umbrella.**
- **Silicon axis** — esp32c6, nRF52840, rp2350. These share almost everything: one VM source
  tree, one `kernel.nsl`, N thin board ports. This is **already factored correctly** inside
  `tuvm/zephyr/boards/*`.

So a layout of `nodus/nsl-tuvm-zephyr-esp32/`, `nodus/nsl-tuvm-zephyr-nrf/`, … would be a
**regression**: it fans the silicon axis out to directory level and duplicates the VM once per
target. Silicon stays *inside* the runtime tree.

### 2.2 Three layers, not two

`tuvm` (the VM, in C) and `nsl-tuvm` (dialect, compiler, `kernel.nsl`) are not node
*implementations*. They are the substrate implementations are written on — one copy,
target-independent. The tree therefore has three levels, not two: system → half → substrate →
target.

---

## 3. The tree

```
rete/
  README.md            the front door (later — see §9)
  CLAUDE.md            one root instruction file
  protocol/            the wire contract, owned by neither side
    PROTOCOL-NSL.md, PROTOCOL-NSL-RECORDS.md
    v1/                PROTOCOL.md + the draft, kept as evidence (§4.3)
    goldens/           NOT created here — see §2 and §9
  porta/               the orchestrator (Go): cmd, internal, devsdk, deploy, scripts
  nodus/               the device end
    tuvm/              VM (C) + zephyr board ports (esp32c6, nrf52840, rp2350…)
    nsl/               dialect, compiler, grammar, kernel.nsl
    tests/             the nsl suite
    toit/              empty until a v2 Toit node exists (§4.1)
  review/              the program-level review tree — at ROOT (§4.2)
  docs/                program-level docs; child-specific docs stay with the child
  tools/               program-level tooling (docmap.sh, diff_oracle.sh)
```

### 3.1 The name

`rete` — Latin for *net*. It completes a set that was already implicit rather than inventing a
third name: a net is made of **nodi** (knots) and entered through a **porta** (gate). Four
characters, unambiguous to type, no significant software collision. It names the *topology*
rather than a product, which is honest about what this is.

Known caveat: `rete` is a live Italian word for "network", so it is SEO-crowded in Italian.
Accepted — `rete` is private for now (§6) and this is not a marketing decision.

---

## 4. A fate for every item

Counted from the repos with `ls` **and `git ls-files`**, not from recall — a restructure that
loses a capability silently is the failure mode worth designing against.

| from | item | fate |
|---|---|---|
| tuvm | `src`, `zephyr`, `oevm_image_main.c`, `bench.sh` | → `nodus/tuvm/` |
| tuvm | `tests`, `tools` | → `nodus/tuvm/`; `docmap.sh`, `diff_oracle.sh` → root `tools/` |
| tuvm | `review` | → **root** `review/` (§4.2) |
| tuvm | `docs` | split: program-level → root `docs/`; VM-specific → `nodus/tuvm/docs/` |
| tuvm | `oevm_bench`, `oevm_runner` | verify tracked-artifact status → `.gitignore`, not migrated |
| nsl-tuvm | `lib`, `grammar`, `tools`, `docs` | → `nodus/nsl/` |
| nsl-tests | suite dirs, `run_tests*.py`, `run_benchmarks.py` | → `nodus/tests/` |
| porta | `cmd`, `internal`, `devsdk`, `deploy`, `scripts`, `go.mod`, `go.sum` | → `porta/` |
| porta | `docs/PROTOCOL-NSL.md`, `PROTOCOL-NSL-RECORDS.md` | → **root** `protocol/` |
| porta | `docs/PROTOCOL.md` (v1), `PROTOCOL-NSL-draft-2026-07-11.md` | → `protocol/v1/` as evidence (§4.3) |
| porta | `docs/{ARCHITECTURE,CLI,DEVSDK,GETTING-STARTED}.md` | → `porta/docs/` — gateway-specific, stays with the child |
| porta + nsl-tests | the CBOR goldens (duplicated inline in two test files) | **not migrated** — extraction filed as a follow-up (§2, §9) |
| porta | `porta` (binary), `porta.db`, `porta-soak.log` | artifacts → `.gitignore`, not migrated |
| nodus (Toit) | everything | **archived, not imported** (§4.1) |
| all five | `CLAUDE.md` × 5 | merged into one root file + scoped children |
| all | `LICENSE`, `README.md` | one of each at root |
| all five | `.gitignore` × 5 | merged into one root file, every pattern re-rooted to its new prefix |
| porta | `.github/workflows/` (1 workflow) | → root `.github/workflows/porta-*.yml`, paths fixed **and a `paths:` filter added** so it does not fire on node-only commits (§4.4) |
| nodus | `.github/workflows/` (1 workflow) | not migrated — goes read-only with `nodus-v1` |

**The two `.github/` rows and the `.gitignore` row were missed on the first pass**, because
the table was built from a bare `ls`, which does not show dotfiles. Recovered by
`git ls-files | grep -oE '^\.[^/]*'` across all five repos. Recording the miss rather than
quietly fixing it: this is exactly the failure mode the "count the old surface with a command,
not from recall" rule exists for, and it went wrong here on the first try.

### 4.1 `nodus/toit/` starts empty — deliberately

The existing Toit repo implements the **scrapped v1 command protocol**. v1 is deleted, not
deprecated; the Toit and `st` kinds went with it. Importing that tree into `rete` would import
archeology and invite a reader to mistake it for a working second implementation.

It is archived read-only as `nodus-v1`, which keeps it greppable — and greppable is what it
is *for*: the standing rule is to read nodus/porta v1 as **evidence, not spec**. A v2 Toit
node is written fresh into `nodus/toit/` when someone writes one.

This is not a deferral of the second runtime. A second independent implementation is
**load-bearing** — it is what proves the protocol is a protocol and not one program's I/O
format. `nodus/` earns its umbrella on that basis, and the layout assumes siblings from day
one even while only one exists.

### 4.2 `review/` moves to the root, with explicit permission

`CLAUDE.md` says: *"`review/` is David's review-agent tree — never sweep, move, or delete
files there."*

A restructure moves it. This was raised explicitly rather than interpreted around, and David
approved it on 2026-08-02. The move is a `git mv` of the whole tree with **nothing dropped,
reordered, renamed or tidied** — categorically different from a sweep. It goes to the root
because the ladder (`program-sequence.md`) and `CLAIMS.md` span *both* children; filing them
under `nodus/` would misstate their scope.

The rule stands unchanged after the move.

### 4.3 `protocol/v1/` — v1 is deleted as *code*, kept as *evidence*

> **Addendum 2026-10-02:** `protocol/v1/` was moved to `protocol/archive/` with HISTORICAL
> banners (David: the Toit-era protocol is legacy and has no claim on rete's version
> numbering). Still kept as evidence; the paths below are the 2026-08-02 ones.

`porta/docs/PROTOCOL.md` describes the v1 command protocol, and
`PROTOCOL-NSL-draft-2026-07-11.md` is the superseded draft. v1 is deleted, not deprecated —
but the standing rule is to read v1 as **evidence, not spec**, and that rule is useless if
the evidence is only reachable in an archived repo nobody thinks to open. They move to
`protocol/v1/`, clearly fenced, alongside the contract that replaced them.

### 4.4 CI: one workflow, scoped

`porta` carries the program's only CI workflow. Merged naively it fires on every commit to
`rete`, including node-only ones, and its paths all assume repo-root == porta-root. It moves
to root `.github/workflows/porta-*.yml` with paths corrected and a
`paths: ['porta/**', 'protocol/**']` filter, so a `kernel.nsl` edit does not run Go tests.

The node side has **no** CI today; the merge neither adds nor removes that. Out of scope here
— but it does become a single-repo problem, which is the only thing that changes.

---

## 5. History and the tracker

### 5.1 Subtree-merge; do **not** rewrite history

Two ways to fold a repo in. One fact decides it: the documents cite **119 commit SHAs, 87 of
them distinct**, across `review/` and `docs/` — the ladder is dense with `0fb9a21`,
`79f1c7f`, `bc47c6f`.

- `git filter-repo --to-subdirectory-filter` then merge gives the prettier result — every
  historical commit rewritten as if the files had always lived at `nodus/nsl/…`, so `log`,
  `blame` and `bisect` are seamless. **But it rewrites every SHA**, and all 87 citations stop
  resolving inside `rete`.
- `git subtree add --prefix=…` **preserves SHAs exactly.** Every citation keeps resolving. The
  cost is that historical commits show the old paths; `git log --follow` handles it, and
  `blame` and `bisect` work.

**Decision: subtree.** Cosmetic history beauty is not worth invalidating 87 citations in the
documents this program navigates by.

> **Never pass `--squash`.** `git subtree add --squash` collapses the incoming repo into a
> single commit and discards exactly the history D5 exists to preserve — all 87 citations die
> and `blame` stops at the merge. It is also what most tutorials recommend, which is what makes
> it dangerous. The plan must state the bare form explicitly, and step 2's gate must include
> `git log --oneline -- nodus/nsl | wc -l` returning the incoming repo's real commit count,
> not 1.

### 5.2 The tracker is the status system, so it sets the constraint

| repo | issues (open / closed) |
|---|---|
| tuvm | 77 / 30 |
| porta | 17 / 16 |
| nsl-tuvm | 11 / — |
| nsl-tests | 0 / — |

`~/workspace/tuvm/{review,docs}` alone contains **838 bare `#NN` references** and **46
`porta#NN`**. The standing orientation rule is that status lives in
`docs/next-steps-overview.md` **and the GitHub tracker** — not in memory. So these references
are not bookkeeping; they are the navigation.

That inverts the intuition that a fresh repo is the clean option. A fresh repo is the
*expensive* one.

- **tuvm's 107 issues stay put, numbers unchanged.** All 838 refs keep resolving. This is the
  entire reason for restructuring in place rather than starting fresh.
- **Transfer the 28 open issues** from porta (17) and nsl-tuvm (11) into `rete`.
  `gh issue transfer` carries comments and history and leaves a redirect at the old URL, so
  existing *links* survive; the numbers change.
- **Leave closed issues** (16 in porta) in the archived repos. They are cited as history and
  archived repos stay readable, so those refs remain valid where they are. Accepted cost: a
  permanently split tracker for closed history.
- **Then rewrite the 46 `porta#NN` sites** from the transfer mapping — scripted, one pass,
  after the transfer, because the mapping is not known until then. Refs to closed issues
  become full URLs into the archive; refs to transferred issues become plain `#NNN`.

**Two assumptions here are unverified and must be tested before the plan relies on them:**

1. **Can `gh issue transfer` move an issue from a PUBLIC repo (`porta`) into a PRIVATE one
   (`rete`)?** GitHub requires push access to both and same ownership, which holds — but the
   public→private direction is not something this program has ever exercised. **Test it with
   one throwaway issue before transferring 28.** If it is refused, the fallback is to re-file
   the 17 open porta issues by hand and leave the originals as closed pointers; annoying, not
   blocking.
2. **The rename redirect is not unconditional.** GitHub redirects `tuvm` → `rete` permanently
   *unless a new repository named `tuvm` is created under the same account*, which silently
   breaks every redirect. **Never create a repo named `tuvm` again.** Worth a line in the root
   `CLAUDE.md`, because it will not be obvious to anyone who wasn't here.

### 5.3 What GitHub looks like afterwards

Nothing disappears. Nothing is deleted.

| repo | after |
|---|---|
| `tuvm` | **renamed `rete`** — GitHub keeps a permanent redirect, so old clone URLs and all 107 issue links resolve. Same repo, same issues, same numbers. |
| `nsl-tuvm`, `nsl-tests`, `porta` | **archived**, not deleted. Read-only banner; code browsable; issues readable; every permalink still resolves. Their code also now lives inside `rete` with its history. |
| `nodus` | renamed **`nodus-v1`** first — so the name is not overloaded with `rete/nodus/` — then archived. |

Subsumed as *code*, preserved as *tombstones*. That preservation is what keeps the 46
`porta#NN` refs and the 87 SHA citations alive.

**The two-workspace-root split does NOT disappear — an earlier draft of this document claimed
it would, and that was wrong.** Checked: `~/workspaceToit/stic` is its own git repo, cited
**13 times** across `review/` and `docs/` — including `review/DOCMAP.md` and
`review/performance_slides.html` — and it holds the published ESP32 benchmark table
(`stic/README.md`, cited 10× on its own) plus `performance.toit`. `~/workspaceToit/toit/src`
is cited too, as the cross-VM comparison source.

So `~/workspaceToit` survives the merge, and `stic` stays outside `rete` on purpose: it is a
**comparative benchmark harness measuring rete against other VMs**, not a part of the system.
Folding a thing that measures you into the thing being measured is the wrong boundary.

What does get simpler is smaller and should be stated as such: `CLAUDE.md`, `DOCMAP.md` and
the cross-program `rg` recipe go from **five doc homes across two roots** to **two** —
`~/workspace/rete` and `~/workspaceToit/stic`. Real, but not the clean collapse first claimed.

---

## 6. Visibility

Current state, which the merge forces into a single decision:

```
tuvm       PRIVATE      porta   PUBLIC
nsl-tuvm   PRIVATE      nodus   PUBLIC
nsl-tests  PRIVATE
```

**Decision: `rete` is PRIVATE.** David's reasoning, 2026-08-02: nobody is watching porta or
nodus, and public visibility is not wanted at this stage.

This is worth recording as a decision rather than a default, because it **resolves a genuine
conflict**. Subtree-merge preserves history, and `review/` carries `WHY.md`, the
revenue-and-positioning strategy doc and `slides.html`, while the tracker carries #79
(go-to-market, vendor routes) and the `revenue-gate` labels. A public `rete` would require
excising all of that *and its history* — and excision means `filter-repo`, which rewrites
SHAs, which is exactly the thing §5.1 decided against. Private `rete` dissolves the conflict:
**no excision, no rewrite, all 87 citations survive.**

`porta` and `nodus-v1` are archived **as-is, left public** — costs nothing and keeps their
links resolving without a login.

The public front door becomes a later, curated artifact — which is what it should have been
anyway, rather than a byproduct of where files happen to sit.

---

## 7. Execution

### 7.1 Preconditions

`#108` is on three unpushed branches (`fix/porta-bridge-reply-tag` in tuvm, nsl-tuvm,
nsl-tests). **Land those first.** Merging repos with in-flight branches is how work is lost.
All four repos clean and pushed before step 1.

### 7.2 Order, with a green gate at every step

| # | step | gate |
|---|---|---|
| 0 | rename `nodus` → `nodus-v1` on GitHub | — |
| 1 | in `tuvm`: `git mv` own contents → `nodus/tuvm/`; `review/`, `docs/` → root; fix internal paths | nsl suite + C tests green |
| 2 | `git subtree add --prefix=nodus/nsl` ← nsl-tuvm; fix paths | nsl suite green |
| 3 | `git subtree add --prefix=nodus/tests` ← nsl-tests | **239 passed / 0 failed**, all aux gates PASS |
| 4 | `git subtree add --prefix=porta` ← porta; protocol docs → `protocol/`, v1 docs → `protocol/v1/` | `go test ./...` green + `porta_sync_host.sh` → `PORTA SYNC OK` |
| 5 | rename repo → `rete`; transfer 28 open issues; scripted rewrite of the 46 `porta#NN` sites | links resolve |
| 6 | merge 5 × `CLAUDE.md`; regenerate `DOCMAP.md`; update `next-steps-overview.md` and the memory orientation file | — |
| 7 | archive the four old repos; **delete the local clones** | — |

### 7.3 The seams that need rewiring

All cross-repo coupling is sibling-relative and confined to about ten scripts — mechanical and
bounded, which is why this is a days-not-weeks change:

- `../nsl-tuvm`, `../nsl-tests`, `../tuvm/src/solidified_image.c` in `tools/run_tests_oevm.py`,
  `tools/run-one.py`, `zephyr/run.sh`, `tools/diff_oracle.sh`, `review/tests/t0-gates/diff_oracle.sh`,
  `tests/porta_sync_host.sh`, `tests/test_portability_32.sh`, `tests/hanoi_ab_host.sh`,
  `tests/http_roundtrip_host.sh`, `tools/run_c_tests.sh`, `tools/docmap.sh`.
- `OEVM_DIR`'s default (the knob itself stays — it is what makes branch worktrees work).
- The zephyr CMake reach into `../nsl-tuvm`.

`porta/go.mod` stays its own module (`github.com/davidg238/porta`); Go commands run from
`porta/`. Renaming the module path to `…/rete/porta` is cosmetic while `rete` is private —
**deferred deliberately, not forgotten.**

---

## 8. The gate that decides whether this worked

**A fresh clone of `rete` into an empty parent directory, then the full suite.** Not the
working copy.

This is the whole verification story, and it is not pedantry. Every script here resolves its
siblings by relative path, and this machine still has stale `~/workspace/nsl-tuvm`,
`~/workspace/nsl-tests` and `~/workspace/porta` sitting *exactly where those paths point*. A
half-migrated script will silently find the old clone, pass green, and prove nothing. That is
the same family as every other way a measurement lies in this program.

Which is why step 7 deletes the local clones **before** anyone celebrates.

The gate: fresh clone, empty parent —

- nsl suite **239 passed / 0 failed**, aux gates grant / debugger-integration / portability-32
  / boot-gc-asan / deepsleep all PASS (footprint SKIP without an arm toolchain is pre-existing)
- `go test ./...` green in `porta/`
- `tests/porta_sync_host.sh` → `PORTA SYNC OK`

---

## 9. What this does not fix

Stated explicitly so it is not assumed. A restructure that quietly absorbs credit for
unrelated problems is how a rewrite loses a capability.

- **The CBOR goldens are still duplicated by hand** in `porta/internal/cval/cval_test.go` and
  `nsl-tests/14_cbor/02_golden_hex.nsl`, with no shared source. `protocol/` gives them a home;
  this change does not move them into it. **File as a follow-up issue before executing**, so
  the hoisting of the protocol docs cannot be mistaken for having solved it.
- **#63** — nine enumerated source lists become a single-repo problem instead of a two-repo
  one. **Still nine lists. Not fixed.**
- **#79's front door** — untouched. The merge only makes it *possible* to write one clone's
  worth of quickstart. Deliberately deferred.
- **#106** — the nsl suite still leaves `src/solidified_image.c` dirty.
- **The C6 hardware gate** still needs hardware and a cold re-flash.
- **Visibility** — `rete` is private; nothing here advances a public release.

---

## 10. Sequencing

**Restructure first, then #56.**

#56 (debug/profiling/console over porta) spans porta + node + protocol — precisely the shape
that just cost three repos, three branches and three commits for a twenty-line fix. It is also
the largest piece of work yet queued, so the per-change tax multiplies. The space-sync rewrite
already demonstrated the failure mode: two implementations of one wire protocol in two repos,
with goldens hand-carried between them.

Doing the merge first means #56 lands as atomic commits, with the protocol change sitting
next to both implementations that must honour it.

Revised program order: porta→channels ✅ → **rete restructure** → **#56** → the #25 host slice.

---

## 11. Decisions, for the record

| # | decision | why, in one line |
|---|---|---|
| D1 | One repo, `rete`, restructured from `tuvm` in place | Preserves 107 issues, 838 refs, the URL; a fresh repo is the expensive option |
| D2 | Two top-level children, `porta/` and `nodus/` | The protocol boundary is the only load-bearing seam |
| D3 | `protocol/` is a third top-level child | The contract belongs to neither side |
| D3a | Protocol *docs* move now; *goldens* extraction is deferred to its own issue | Extraction changes test code on both sides and needs its own gate |
| D4 | Silicon stays *inside* the runtime tree; only the runtime axis gets directories | Otherwise the VM is duplicated once per target |
| D5 | `git subtree`, not `filter-repo` | Preserves the 87 SHA citations |
| D6 | Transfer open issues only; leave closed ones in the archives | Live tracker unified; historical refs stay valid where they are |
| D7 | Old repos archived, never deleted | Deletion breaks 46 `porta#NN` refs and every permalink |
| D8 | `rete` is private | Dissolves the excision-vs-SHA-rewrite conflict outright |
| D9 | `nodus/toit/` starts empty; v1 archived as evidence | Importing a scrapped protocol imports archeology |
| D10 | `review/` moves to root, by explicit permission | It spans both children; a whole-tree `git mv` is not a sweep |
| D11 | Fresh-clone gate, empty parent | The only thing that proves no path still reaches outside the repo |
| D12 | `stic` stays outside `rete`, in `~/workspaceToit` | A harness that measures rete against other VMs does not belong inside rete |
| D13 | porta's CI workflow moves to root with a `paths:` filter | Otherwise Go tests fire on every `kernel.nsl` edit |
| D14 | Sweep stale path citations in the documents of record only; leave the 1,266 in the historical record, filed as an explicit deferral | A dated design doc is the historical *why*; rewriting its paths falsifies what was true when it was written (§13.1) |

---

## 12. Review record

This document was reviewed against the repos on 2026-08-02, after being written. The review
found **two factual errors and one execution footgun**, all corrected in place and left
visible rather than silently patched:

1. **The fate table missed every dotfile** — `.github/` (porta's only CI workflow) and five
   `.gitignore`s. Cause: the table was built from a bare `ls`. Fixed by re-counting with
   `git ls-files`. This is the precise failure the "count with a command, not from recall"
   rule guards against, and it still happened.
2. **"The two-workspace-root split disappears" was false** (§5.3). `~/workspaceToit/stic` is a
   separate repo cited 13× in the program docs. The claim is now the smaller true one: five
   doc homes across two roots become two.
3. **`git subtree add --squash` would silently destroy D5** — it is the form most tutorials
   recommend, and it discards the history the whole SHA-preservation decision rests on. Now an
   explicit prohibition with a counting gate (§5.1).

Also added: two unverified GitHub assumptions (public→private issue transfer; the rename
redirect breaking if `tuvm` is ever recreated) are now flagged as things to test rather than
things to rely on.

---

## 13. Addendum — decisions made and corrected during execution (2026-08-02)

Written after Tasks 1–4 landed and the whole-branch review closed. §12's review
record was written *before* execution; this section records what execution
itself changed. The plan's Self-Review lists the seven defects found in the
plan; this section covers only what changed a **decision**.

### 13.1 D14 (new) — the stale-path citation sweep is scoped to the documents of record

Restructuring moved paths that thousands of documents cite. Measured at
`b60d828` with one combined regex: **1,666 citations across 105 files** name
pre-merge locations.

**Decision (David, 2026-08-02):** sweep the documents of record — `review/`
except `DOCMAP.md`, `protocol/PROTOCOL-NSL-RECORDS.md`,
`docs/next-steps-overview.md` — and regenerate `review/DOCMAP.md` from
`tools/docmap.sh` rather than hand-editing its generated inventory. The 1,266
citations in the historical record (`docs/design/`, `docs/superpowers/plans/`,
`nodus/nsl/docs/`, `docs/MILESTONE_*`, `porta/docs/`) are **deliberately left**.

**Rationale.** The docs convention in `CLAUDE.md` makes the dated design doc the
historical *why*. Rewriting its paths would falsify what was true when it was
written. A closed plan describing work done in `~/workspace/nsl-tuvm` is
accurate history; the same text with `nodus/nsl` substituted is a document
claiming something that never happened.

**Consequence, accepted:** Task 7 deletes the three local clones, after which
those 1,266 citations resolve to nothing. That is the price of not falsifying
the record, and it is filed as an explicit deferral (Task 8 Step 2b) rather than
absorbed silently — WONTFIX is a legitimate outcome, silence is not.

### 13.2 D5's verification method was wrong, and D5 itself is unchanged

D5 chose `git subtree` over `filter-repo` to preserve the 87 SHA citations. That
decision stands and is verified: all three source heads are ancestors of `HEAD`
and every imported tree is byte-identical to its source commit.

What was wrong was the **gate**. §5.1 and every subtree task asserted D5 with a
path-scoped commit count — `git log --oneline -- <prefix> | wc -l` — described
as a "counting gate". That check returns **1 on a correct, non-squashed
`git subtree add`**: the imported commits touch the source's *original* paths,
so none match the new prefix, and both plain and `--full-history` path logs stop
at the merge. It could never have detected `--squash`.

**The gate is now ancestry:** `git merge-base --is-ancestor <source-head> HEAD`.
`--squash` produces a commit with no link to the source head, so ancestry fails
exactly when history was destroyed and passes exactly when it was not.

§12's item 3 ("`--squash` would silently destroy D5 — it is the form most
tutorials show, so the plan needs an explicit prohibition **with a counting
gate**") is correct in its concern and wrong in its remedy. The prohibition was
right; the counting gate was decorative.

### 13.3 D2's "one `CLAUDE.md`" means no per-child copies

Ambiguity surfaced during the final review: the plan's Task 6 both said keeping
a standalone `porta/CLAUDE.md` was "a judgment call" and, in a `.gitignore`
comment, that Task 6 "is expected to `git add`" it. Neither matched the task's
own title or its deletion of `nodus/nsl/CLAUDE.md`.

**Resolved:** porta's rules merge into the root file and **no** `porta/CLAUDE.md`
is created — identical treatment to nsl's. A second copy is a rule set that
drifts from the first, and a later session cannot tell which governs.

Note the mechanism that nearly lost porta's rules entirely: porta's own
`.gitignore` contained `/CLAUDE.md`, so `git subtree add` correctly never
imported the file, and Task 4 faithfully re-rooted that rule as
`/porta/CLAUDE.md` — making it *un-addable*, silently. The rule is removed. The
only surviving copy is `~/workspace/porta/CLAUDE.md` and it must be read before
Task 7 deletes that clone.

### 13.4 The gate that decides whether this worked (§8) needed two corrections

§8 makes a fresh clone into an empty parent the deciding gate, on the reasoning
that stale sibling clones sit exactly where the old relative paths point. That
reasoning is sound and was vindicated twice — but the gate as specified would
have passed while proving less than it claimed:

1. It ran the nsl suite without `npm install`. `nodus/nsl/grammar` needs
   `ohm-js` from a gitignored `node_modules`, so a fresh clone reads **0 passed
   / 239 failed with every aux gate FAIL** — a signature indistinguishable from
   "the consolidation broke everything", against a tree that is fine.
2. `porta_sync_host.sh` defaulted `PORTA_DIR` to `$HOME/workspace/porta` and
   turned its absence into `exit 0`. Since the gate runs *after* the clones are
   deleted, the one check that exercises both children **and** the protocol
   between them would have reported success having done nothing.

Both are fixed. The generalisation worth keeping: **§8's instrument was itself
subject to §8's own thesis.** A gate that resolves anything by absolute path, or
that treats a missing dependency as a skip, is measuring the old world.

---

## 13. Execution addendum — decisions made while running the plan (2026-08-02)

Recorded here because they were made *during* Tasks 5–8, not before, and a decision
that only exists in a commit message is a decision the next reader has to reconstruct.

### D15 — `rete` stays PRIVATE, and porta's issues go private with it

Explicit call by David during Task 5. `porta` was public; `tuvm` (now `rete`) is private.
The moment the 17 open porta issues transferred, their public URLs began redirecting into
a private repository — so an anonymous visitor following an old `porta#NN` link sees
nothing. That is a consequence of the *merge*, not of the transfer, and it was accepted
knowingly rather than discovered later. Reversible by making `rete` public, but that
would also publish `nsl-tuvm` and `nsl-tests` content, which are private today. §9's
"visibility — nothing here advances a public release" still holds.

### D16 — the generated inventory emits in-repo paths repo-relative

`tools/docmap.sh` emitted `$HOME`-relative absolutes. That was about to bake
`~/workspace/tuvm/...` into ~300 citations **one task before** Task 7 renamed that
directory, and it already read wrong from a branch worktree. In-repo entries are now
repo-relative; only the two `~/workspaceToit` homes stay `~`-absolute. Verified after
the rename: regenerating produced **zero** path churn, only date changes.

The date changes are the cost, and they are filed rather than hidden: 197 of 351 entries
now read `2026-08-02` because that is when the move touched them. `--follow` returns
*empty* across a subtree import, so it is worse, not better. Caveat in the DOCMAP header;
issue for the real fix.

### D17 — the Step 4b sweep was widened past its own regex

The plan's regex covered `nsl-tuvm/`, `nsl-tests/` and the absolute repo roots. It did
not cover bare relative `tuvm/` citations — `tuvm/src/space.c`, `tuvm/docs/MILESTONE_E.md`
— 66 more hits in the same documents of record, several sharing a line with a hit the
regex did catch. Fixing one and leaving its neighbour is worse than fixing neither, so
the sweep was widened. D10 compliance was then checked rather than asserted: re-deriving
each of the 162 changed `review/` lines from its original by path substitution alone
reproduces 161 exactly, the one exception being a citation of `nsl-tests/CLAIMS.md`, a
path that never existed.

### D18 — an archived repo is where a doomed commit goes

porta's orphan branch `chore/b4-brainstorm-kickoff` was held in `rete` only by the
`porta-src` remote-tracking ref. Task 7 Step 2 deletes the clone and Step 8 removes that
remote; between them, `2e4372a` becomes unreachable and is GC'd, with `porta` archived
and no copy anywhere. Nothing in the plan would have reported it.

Pushed to `porta`'s origin before archiving. The generalisation: **D7's "archive, never
delete" protects refs that exist on the remote — it does not protect work that only ever
existed locally.** Before archiving a repo, push what is only local. #123 is now a
decision rather than a loss.

### Two things `git mv` cannot carry, discovered at Task 7

Both are consequences of the same fact — the restructure moves *tracked* files — and both
would have been silent:

1. **Gitignored local config.** Four Thread/wifi `.conf` files existed only at the old
   `zephyr/` path and exist nowhere else. Copied to `nodus/tuvm/zephyr/` before the stale
   tree was deleted; without that, the next C6 or ESP32 build comes up missing its dataset
   with nothing pointing at the cause.
2. **The agent memory namespace.** `~/.claude/projects/` is keyed by working-directory
   path, so `mv ~/workspace/tuvm ~/workspace/rete` orphans all 44 memories — every
   standing order and hardware gotcha — behind a namespace no future session opens.
   Copied across and repointed. Not a repo concern, which is exactly why the plan had no
   step for it.
