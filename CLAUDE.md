# rete — project instructions

`rete` is one repository holding both halves of a two-sided embedded fleet system and the
wire contract between them. It was consolidated on 2026-08-02 from four repos — `tuvm`,
`nsl-tuvm`, `nsl-tests` and `porta` — by restructuring `tuvm` in place, which is what
preserved the issue numbers and the `#NN` references the program navigates by
(#79; design `docs/design/2026-08-02-rete-repo-consolidation-design.md`).

## Tree map

```
porta/       the orchestrator (Go) — the northbound half
nodus/       the device end
  tuvm/      the VM (C) + the Zephyr board ports
  nsl/       the dialect, its compiler and the kernel
  tests/     the nsl suite
protocol/    the wire contract both sides answer to (v1 frozen under protocol/v1/)
review/      David's review-agent tree: the program ladder and the claims ledger
docs/        the VM's own documents, the design records and the execution plans
tools/       repo-wide scripts (docmap.sh)
```

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
- **Remember the lineage: `st/berry` → `ns/berry` → `ns/oevm` → `nsl/tuvm`.** This system
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
- **Promotion rule — part of every close-out.** An issue / rung / milestone is not closed
  until its durable knowledge is promoted to a mainline document of record: the ladder
  (`review/program-sequence.md`), `review/tests/CLAIMS.md`, the owning doc under
  `review/lib/`, or — for porta — `protocol/PROTOCOL-NSL*.md` and
  `porta/docs/ARCHITECTURE.md`. The dated design doc stays behind as the historical "why".
  Every plan's close-out task must name which mainline doc absorbed the knowledge.

## Gates

```
cd nodus/nsl && python3 tools/run_tests_oevm.py     # 261 passed, 0 failed (2026-08-12)
cd porta      && go test ./...
cd nodus/tuvm && bash tests/porta_sync_host.sh      # PORTA SYNC OK
```

The nsl suite prints `Passed: N / Failed: 0` **and** a line of aux gates — read the aux
line, because it can be red while the count reads zero failures.
`footprint: SKIP (no arm-zephyr-eabi toolchain)` is pre-existing and expected. Never run
two nsl suites concurrently; they share `/tmp` paths. **The count above drifts upward as
tests land — treat a mismatch as staleness in this file, never as a missing test.** The
suite also prints `target gates: NOT RUN BY THIS SUITE`: the posix timer backend bypasses
the pulse mailbox, so coalescing, shedding and stale handles need `nodus/tuvm/tests/target/run.sh`
and a board.

**What `tests/target/run.sh` actually runs — seven scenarios, and a full pass is ~35–50 min of
board time**, so budget it rather than discovering it. `CLASS=disruptive` means the scenario
deliberately wedges, burns or reboots the node: **a disposable board only** (`c6spare`, which
is `TARGET_DEFAULT_BOARD` — resolve boards through the collector env files, never a table).

| scenario | class | what it gates |
|---|---|---|
| `bridge-health` | observe | the node half of the porta gate — obs acked, `goal/name` converged, nothing evicted (#streamDropped 0) |
| `bridge-forensics` | observe | the same workload under the forensics overlay (#222) |
| `pulse-coalesce` | disruptive | the coalescer does not destroy logical timer events (#151) |
| `stale-handle` | disruptive | a timer event survives a later unrelated registration (#151) |
| `wedge-note` | disruptive | the `RebootNote` survives a watchdog reset (#215, row 1) |
| `wedge-detect` | disruptive | the detector fires on a real wedge, dumps the condition, reboots, reports `#watchdog` (#215, row 3) |
| `wedge-quiet` | disruptive | **no false trip on a legitimately slow node** — one 10-minute timer, and the 600 s observation is why this run is long (#215, row 5) |

A scenario passing its TAPs is **not** the verdict: every one carries an observation window
that watches past it, because a gate that scores at tick 2 against a node that dies at tick 6
is a false green (#216).

---

# porta — the gateway half

`porta` (Latin "gate/door") is the **gateway project**. It is the northbound half of a
two-sided system whose unit of coupling is a **space**: every node owns a tree of paths,
and porta holds a mirror of that tree. Operators write intent under `goal/**`; the node
writes what it sees under `obs/**`, `tel/**` and `sys/**`. Convergence is not a mechanism —
`goal/X` versus `obs/X` is a tree diff, so the desired-vs-observed view and the self-heal
loop are the same query.

The two sides exchange the mirror over **plain UDP carrying CBOR**, in two node-initiated
idempotent messages: a `pull` that fetches writes porta has for the node, and a `push` that
delivers the node's entries. Both are bounded by a 512 B batch and a 256 B value, which are
radio constraints and not preferences (6LoWPAN fragments above ~102 B and a datagram
survives only if every fragment does).

There are **no verbs on the wire and none in the store.** The operator vocabulary is CLI
sugar over path writes and tree reads: `porta put` is a write to a `goal/` path, `porta get`
is a tree read, `porta log` is a query over the append-only event log.

> **The porta auto-memory namespace (`~/.claude/projects/-home-david-workspace-porta/memory/`)
> is v1-era and has NOT been reviewed against the rewrite.** It holds memories written
> against the command queue, TFTP delivery and the report schema — all deleted. Treat
> anything from it as a claim about a system that no longer exists until you verify it
> against the code. The memories this work depends on live in the **tuvm** namespace
> (`nsl-cbor-wire-shapes`, `rewrite-may-not-delete-a-capability`), which is the namespace
> `~/workspace/rete` sessions get.

## Status

v1 was deleted in one commit (design D7: demolition precedes construction, so no decision
is made with v1 still in the room). The last v1 tree is tagged **`porta-v1-final`**.

What v1 was — a command vocabulary and report schema, a command queue, container image
delivery over TFTP, and telemetry ingest — is gone, along with nine per-feature sqlite
tables. The **capability audit in
`porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md` §4.2 gives every v1 verb,
route and MCP tool a fate**, because a rewrite may delete a mechanism but may not silently
delete a capability. Three of v1's fourteen `nodes` columns turned out to have no successor
until they were enumerated; all three are closed.

The core is built and green: `cval`, `space` and `wire` all landed
(`porta/docs/superpowers/plans/2026-08-01-porta-space-sync-core.md`, DONE), the node side
landed (`docs/superpowers/plans/2026-08-02-porta-space-sync-node.md`, DONE), and the C6
hardware gate closed the rung 5/5. See `docs/next-steps-overview.md` Part 2 for what is
next.

## Layout

- `porta/cmd/porta/` + `porta/internal/portacli/` — the cobra CLI (`serve`, `get`, `put`,
  `log`). The operator verbs are sugar over path writes and tree reads.
- `porta/internal/cval/` — the nsl message algebra's CBOR codec, **hand-rolled**. Not
  layered on a CBOR library, and this is not a preference: the algebra's map keys are
  themselves tagged values (Symbols, tag 39), a Record's *name* is a Symbol rather than a
  text string, and decoding must be **closed** so a mis-versioned peer cannot smuggle a
  value through. The node's `Cbor` class in `nodus/nsl/lib/kernel.nsl` is the authority,
  not the RFC; the encoder is golden-tested against the node's own output.
- `porta/internal/space/` — the store: `nodes`, `cells` (latest value per path), `events`
  (append-only, both directions, with `actor`/`reason` provenance), `exchanges` (one row
  per request — two per check-in, a `pull` and a `push`, tagged by a `leg` column; the pull
  leg is the heartbeat and the replay index, since a node always sends it while a push can
  early-return on an empty frame). Four tables where v1 had nine, because the unit of
  extension is now a path: a new feature adds rows, not tables.
- `porta/internal/wire/` — pull/push framing as a pure `Handle([]byte) []byte`, wrapped by
  a thin UDP loop.
- `porta/internal/httpsrv/`, `porta/internal/serverstat/` — survived the demolition
  untouched. `httpsrv` now depends on a one-method `NodeCounter` interface rather than a
  concrete store, which is how it outlived v1's store and should outlive the next one.
- `porta/devsdk/` — the public dev-SDK (`exec`, `provision`; `apiclient` was deleted with
  v1's HTTP surface). All language/hardware toolchain lives on the node side, which imports
  `devsdk`; porta itself stays language-neutral.
- `porta/deploy/` — the deb build and systemd unit.

## Documents of record

- `protocol/PROTOCOL-NSL.md` — the current wire protocol: the space model, path namespaces,
  cells vs streams, the exchange. **§§1–4 are the conformance target.**
- `protocol/PROTOCOL-NSL-RECORDS.md` — the record catalogue: what shape rides at each pinned
  path, plus the size law (§2.1/§2.2).
- `porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md` — the rewrite's decision
  record (D1–D8), the store schema, the wire shapes, and the §4.2 capability audit.
- `porta/docs/ARCHITECTURE.md` — system architecture. **Stale in parts**: written for v1's
  command protocol and TFTP transport.
- `protocol/v1/PROTOCOL.md` — **v1. Deleted, historical only.** It describes the command
  verbs, JSON args and CRC32 payload delivery that no longer exist. Do not implement from
  it; read it only to understand what `porta-v1-final` did, and it is what the frozen Toit
  nodes still speak.
- `porta/docs/CLI.md`, `porta/docs/DEVSDK.md`, `porta/docs/GETTING-STARTED.md` — **stale**,
  written against v1's verb set.

## Key decisions & constraints (don't re-litigate)

- **Verbs live only at the CLI.** The store and the wire have none (D2). Adding a feature
  adds a path, not a table, a verb or an endpoint.
- **All values on the wire and in the store are raw deterministic CBOR** (D4.1); decode only
  for display. All timestamps are **milliseconds** (D4.2). All paths are **node-relative**
  (D4.5) — the `node/<id>/**` prefix is a view, never storage, so a captured session can be
  re-pointed at a different node.
- **One monotonic seq per space per side** (D5). Kind is a property of the leaf, set by the
  first write (D6); `KindStream = 1`, `KindCell = 2`, matching `SPACE_KIND_*` in
  `nodus/tuvm/src/space.h` — shared with the VM, do not renumber.
- **The integer band is ±(2^64−1)**, which exceeds `int64`. Represent out-of-range values
  exactly; never silently clamp.
- **There is no tombstone.** Stopped state is written explicitly as a value; genuine removal
  is an action on a `do/` stream.
- **A rewrite may delete a mechanism, but it may not silently delete a capability** (D8).
  Anything the old system could answer that the new one cannot is a regression wearing the
  costume of a simplification. This is why demolition was preceded by an audit.
- **Replay-from-0 is designed behaviour, and it is dangerous for streams.** Cold boot =
  cursor 0 = full replay is safe for cells (idempotent by construction) but replaying
  `goal/do/reboot` reboots the node again. See design §8.
- Bulk transfer (images, profiler blobs) is **deliberately off the space** and deferred to a
  CoAP blockwise plane; auth (G5) is out of this rung.
- Go 1.26, sqlite via `github.com/mattn/go-sqlite3`, cobra. Copyright header
  `// Copyright (c) 2026 Ekorau LLC` on every new Go file. `gofmt -l .` must print nothing
  before every commit.
