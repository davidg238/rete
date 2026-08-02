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

Counted from the repos with `ls`, not from recall — a restructure that loses a capability
silently is the failure mode worth designing against.

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

`porta/docs/PROTOCOL.md` describes the v1 command protocol, and
`PROTOCOL-NSL-draft-2026-07-11.md` is the superseded draft. v1 is deleted, not deprecated —
but the standing rule is to read v1 as **evidence, not spec**, and that rule is useless if
the evidence is only reachable in an archived repo nobody thinks to open. They move to
`protocol/v1/`, clearly fenced, alongside the contract that replaced them.

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

### 5.3 What GitHub looks like afterwards

Nothing disappears. Nothing is deleted.

| repo | after |
|---|---|
| `tuvm` | **renamed `rete`** — GitHub keeps a permanent redirect, so old clone URLs and all 107 issue links resolve. Same repo, same issues, same numbers. |
| `nsl-tuvm`, `nsl-tests`, `porta` | **archived**, not deleted. Read-only banner; code browsable; issues readable; every permalink still resolves. Their code also now lives inside `rete` with its history. |
| `nodus` | renamed **`nodus-v1`** first — so the name is not overloaded with `rete/nodus/` — then archived. |

Subsumed as *code*, preserved as *tombstones*. That preservation is what keeps the 46
`porta#NN` refs and the 87 SHA citations alive.

**The two-workspace-root split disappears.** `~/workspaceToit/nodus` was the only reason
`~/workspaceToit` figured in this program. `CLAUDE.md`, `DOCMAP.md` and the cross-program `rg`
recipe all encode "five doc homes across two roots"; all three get simpler.

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
