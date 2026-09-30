# rete — project instructions

`rete` is one repository holding both halves of a two-sided embedded fleet system and the
wire contract between them. It was consolidated on 2026-08-02 from four repos — `tuvm`,
`nsl-tuvm`, `nsl-tests` and `porta` — by restructuring `tuvm` in place, which is what
preserved the issue numbers and the `#NN` references the program navigates by
(#79; design `docs/design/2026-08-02-rete-repo-consolidation-design.md`).

@FOCUS.md

## Where truth lives (do not duplicate across channels)

| Kind | Home | Never in |
|---|---|---|
| Open work / status / bugs | GitHub issues, bands `P0`–`P4`, pinned tracker **#26** | memory, chat |
| What is top of mind right now | `FOCUS.md` (imported above) + `docs/status/dev-status.html` (live at `http://gw85224-01:6971/dev/status/dev-status.html`; refresh = edit + scp, recipe in `.claude/commands/conclude.md`) | memory |
| Direction | `docs/next-steps-overview.md`, `review/program-sequence.md` | dated design docs |
| Design decisions | `docs/design/` (porta: `porta/docs/design/`) | memory, chat |
| Session-to-session state | git + GitHub + this block; sessions end with `/conclude` | memory |

Memory store = David's working preferences and gotchas ONLY. "Save this for next session" means: update the issue, the doc, or the block above. **Session shape:** one focus issue per session; anything else found is filed P3 `found-during #N` and left for the weekly review.

## Tree map

```
porta/       the orchestrator (Go) — the northbound half
nodus/       the device end
  oevm/      the VM (C) + the Zephyr board ports
  nsl/       the dialect, its compiler and the kernel
  tests/     the nsl suite
protocol/    the wire contract both sides answer to (v1 frozen under protocol/v1/)
review/      David's review-agent tree: the program ladder and the claims ledger
docs/        the VM's own documents, the design records and the execution plans
tools/       repo-wide scripts (docmap.sh)
```

porta/, nodus/nsl/ and nodus/oevm/tests/target/ carry their own `CLAUDE.md`, loaded when working there.

**Being in one repo does not couple the two halves.** porta and the nodes are coupled
**only** over the wire; they never import each other's code, and `protocol/` is the whole
of the contract. A change that makes one side compile against the other is a bug, not a
convenience.

## Standing rules

- **Never create a GitHub repo named `tuvm`.** The repo was renamed `tuvm` → `rete`, and
  GitHub serves the old name as a redirect. Creating anything called `tuvm` again silently
  takes that redirect down, and with it every old issue URL and every clone URL in the
  historical record. There is no warning when this happens.
- **`review/` is David's review-agent tree — never sweep, move, tidy, reorder or delete
  files there.** Path fixes forced by a restructure are the one exception, and only with
  explicit permission (granted 2026-08-02 for #79).
- **Cite absolute paths** when referencing documents in conversation
  (e.g. `~/workspace/rete/review/program-sequence.md`, never a bare `review/`) — the
  program still spans two workspace roots, because `~/workspaceToit/stic` (the cross-VM
  benchmark harness) stays outside `rete` on purpose.
- **Two sibling repos are David's, adjacent, and NOT part of `rete`:**
  `~/workspace/vsa` (does hyperdimensional computing actually work? — Phases 0/1a/2a/2b/2c
  complete, and the answer so far is a series of clean negatives) and `~/workspace/cabot`
  (the wheeled house-pet robot: CAD, floor survey, the E-0..E-10 falsification plan, and
  VSA measurements at house-pet scale). Both are **private, Python, and worked on the
  z170** — a local clone here goes stale silently, so `git fetch` before citing any number
  from either. They are read as evidence, never imported; nothing in `rete` depends on them.
- Cross-repo document map: `review/DOCMAP.md` (regenerate with `tools/docmap.sh`).
- Content search across the program:

  ```
  rg -i "<term>" ~/workspace/rete ~/workspaceToit/stic
  ```

  Add `~/workspaceToit/nodus` to reach the frozen Toit v1 implementation (the GitHub repo
  is now `nodus-v1`; the local directory keeps its old name).

## Design posture (David, 2026-08-04)

How to make decisions here. These override the instinct to be thorough.

- **This is an evolving story, so expect the documents to contradict each other.** When
  two documents of record disagree, that is a finding, not an error to route around —
  resolve it and make them agree. Do not preserve both readings, and do not implement to
  whichever one you happened to read.
- **Resolve for simplicity, robustness and expressiveness, with a dash of elegance.**
  In that order. See also the standing preference for **mechanism minimization**: fewer
  distinct mechanisms and API symmetry beat convenience or low blast radius, and
  call-site churn is an acceptable price.
- **There is ZERO need to maintain backward compatibility, and no "fleet" to protect.**
  Both ends of the wire are reflashed together. A shape change is an edit to the
  documents and a rebuild — never a versioned field, a dual-read, or a deprecation
  window. This is what makes corrections possible instead of accretions: the usual way a
  record grows wrong is that each field was added *beside* what was there rather than
  *instead of* it.
- **Prefer progress, spikes and data over complex decisions made early.** Limit scope and
  get something working. **Do not invent tunables before there is data to choose them
  from** — if a design needs N invented constants before it can run once, ship the
  degenerate case whose cost can be measured, and let the measurement earn the knobs
  back. Record what would earn them, so the deferral is falsifiable rather than vague.
- **Remember the lineage: `st/berry` → `ns/berry` → `ns/oevm` → `nsl/tuvm` (now `rete`, the VM at `nodus/oevm`).** This system
  has been rewritten wholesale four times to get here. Nothing is built to last; it is
  built to teach. Design accordingly.

## Docs convention (2026-07-14)

- **Design specs are decision records** and live in `docs/design/` (moved from
  `docs/superpowers/specs/` on 2026-07-14). When a superpowers skill (e.g. brainstorming)
  says to save a design to `docs/superpowers/specs/`, save it to `docs/design/` instead —
  this instruction overrides the skill's default path. porta's own designs live in
  `porta/docs/design/` under the same rule.
- **Implementation plans** stay in `docs/superpowers/plans/` (porta's in
  `porta/docs/superpowers/plans/`) — they are execution scaffolding, historical once the
  work closes.
- **A document leaves the mainline by move + banner, never by deletion and never by a bare
  move** (2026-08-29, #79). Superseded documents go to the `archive/` of the subsystem that
  owns them (`nodus/oevm/docs/archive/`, `porta/docs/archive/`) with a
  `> **HISTORICAL — archived DATE.**` first line naming what replaced them and what was
  deleted since; then rewrite the in-tree citations to the new path (`review/` excepted —
  list those for David) and rerun `tools/docmap.sh`. Kept rather than deleted so that `#NN`
  issues and commits still resolve by path; the banner is what stops an archived spec being
  read as current, which is how `spec.md` described NLR for seven weeks after Milestone A
  deleted it.
- **Reference docs live with their subsystem; dated records live at the program level**
  (the 2026-08-02 consolidation's rule, executed 2026-08-29): `nodus/oevm/docs/` (start at
  `vm.md`), `nodus/nsl/docs/`, `porta/docs/`, each with a `README.md` map; the root `docs/`
  holds tutorials, status, `next-steps-overview.md`, and the dated `design/`, `results/`,
  `spikes/`, `superpowers/plans/` — which stay under their date because that is where the
  issue citations point. `docs/README.md` is the map of the whole.
- **Promotion rule — part of every close-out.** An issue / rung / milestone is not closed
  until its durable knowledge is promoted to a mainline document of record: the ladder
  (`review/program-sequence.md`), `review/tests/CLAIMS.md`, the owning doc under
  `review/lib/`, or — for porta — `protocol/PROTOCOL-NSL*.md` and
  `porta/docs/ARCHITECTURE.md`. The dated design doc stays behind as the historical "why".
  Every plan's close-out task must name which mainline doc absorbed the knowledge.

## Gates

```
cd nodus/nsl && python3 tools/run_tests_oevm.py     # 280 passed, 0 failed (2026-09-17)
cd porta      && go test ./...
cd nodus/oevm && bash tests/porta_sync_host.sh      # PORTA SYNC OK
```

- The nsl suite prints `Passed: N / Failed: 0` **and** a line of aux gates — read the aux line, it can be red while
  the count reads zero failures. A SKIP there is a finding, not a fixture. The count drifts upward; a mismatch is
  staleness in this file, never a missing test.
- Never run two nsl suites concurrently (shared `/tmp` paths), and never run `porta_sync_host.sh` beside one
  (`tutorial-porta` binds the same udp 16969/16970).
- Board scenarios (`nodus/oevm/tests/target/run.sh`, fourteen of them, ~70–100 min, `porta-*` need a hand-started
  injector) are NOT run by the suite. `CLASS=disruptive` means a disposable board only (`c6spare`); resolve boards
  through the collector env files, never a table.
- Detail loads with the directory: `nodus/nsl/CLAUDE.md` (the aux gates), `nodus/oevm/tests/target/CLAUDE.md`
  (the scenario table and the `CONF_EXTRA`/radio rule), `porta/CLAUDE.md` (the gateway half).
