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
- `protocol/cval/` (outside porta, in the shared `protocol` module since #338; porta,
  custos and nothing else import it) — the nsl message algebra's CBOR codec, **hand-rolled**. Not
  layered on a CBOR library, and this is not a preference: the algebra's map keys are
  themselves tagged values (Symbols, tag 39), a Record's *name* is a Symbol rather than a
  text string, and decoding must be **closed** so a mis-versioned peer cannot smuggle a
  value through. The node's `Cbor` class in `nodus/nsl/lib/kernel.nsl` is the authority,
  not the RFC; the encoder is golden-tested against the node's own output.
- `porta/internal/space/` — the store: `nodes`, `cells` (latest value per path), `events`
  (append-only, both directions, with `actor`/`reason` provenance), `exchanges` (one row
  per request — two per check-in, a `pull` and a `push`. **There is no `leg` column**: it
  was dropped with the cadence inference rete#231 D4 deleted, which was its only reader
  (and which had to count pulls alone or infer a period half the node's real one, #217
  C1). What the table is for now — the replay index and the `last_seen` heartbeat — wants
  every request rather than one leg, and **both** legs stamp `nodes.last_seen_ms` via
  `RecordExchange`). Four tables where v1 had nine, because the unit of
  extension is now a path: a new feature adds rows, not tables.
- `porta/internal/wire/` — pull/push framing as a pure `Handle([]byte) []byte`, wrapped by
  a thin UDP loop.
- `porta/internal/httpsrv/`, `porta/internal/serverstat/` — survived the demolition
  untouched; the allowlist they admit by is `protocol/allowlist`, shared with custos. `httpsrv` now depends on a one-method `NodeCounter` interface rather than a
  concrete store, which is how it outlived v1's store and should outlive the next one.
- `porta/devsdk/` — the public dev-SDK (`exec`, `provision`; `apiclient` was deleted with
  v1's HTTP surface). All language/hardware toolchain lives on the node side, which imports
  `devsdk`; porta itself stays language-neutral.
- `porta/deploy/` — the deb build and systemd unit.
- **custos is not under porta** (#338): it is `custos/`, its own module. porta and custos
  share only `protocol/`'s Go module; porta's store builders for custos's tests are
  `internal/space/fixtures_test.go`, written to `protocol/fixtures/store/`.

## Documents of record

- `protocol/PROTOCOL-NSL.md` — the current wire protocol: the space model, path namespaces,
  cells vs streams, the exchange. **§§1–4 are the conformance target.**
- `protocol/PROTOCOL-NSL-RECORDS.md` — the record catalogue: what shape rides at each pinned
  path, plus the size law (§2.1/§2.2).
- `protocol/MANIFEST.cddl` — **the machine-normative half of the catalogue** (#243,
  2026-08-25): path → kind + class + dir + shape, enforced by the nsl suite's
  `nsl-tools-py` aux gate (formerly `manifest-py`), porta's store (all three write doors), and the seam gate.
  Editing it has hard zcbor rules — see the `manifest-cddl-editing-rules` memory or the
  file's own header.
- `porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md` — the rewrite's decision
  record (D1–D8), the store schema, the wire shapes, and the §4.2 capability audit.
- `porta/docs/ARCHITECTURE.md` — system architecture. **Rewritten for v2 2026-08-29**
  (#279) and current: the three planes, both UDP transports, modes and app lifecycle as
  paths, the manifest gate, the store's two open doors, one-repo topology. §7 records the
  Toit lineage as history.
- `protocol/archive/PROTOCOL.md` — **v1. Deleted, historical only.** It describes the command
  verbs, JSON args and CRC32 payload delivery that no longer exist. Do not implement from
  it; read it only to understand what `porta-v1-final` did, and it is what the frozen Toit
  nodes still speak.
- `porta/docs/CLI.md` — **the v2 command reference and current** (rewritten 2026-08-29,
  #279). Gated: `porta/docs/cli-gate.sh` walks the binary's own `--help` and fails if a
  command or flag named on the page does not exist, or if one that exists is unnamed.
  Names only — it does not score the prose, and the transcripts on it are dated evidence.
- `porta/docs/GETTING-STARTED.md` — **rewritten end to end 2026-08-29** (#279) and
  current: install, confirm, point a node at it, first operations, and `custos`. Prose
  and NOT gated — it says so itself and defers to `CLI.md` and tutorial 3 on any
  disagreement.
- `porta/docs/DEVSDK.md` — **stale in parts**, written against v1's verb set. `exec` and
  `provision` survive; `apiclient` went with v1's HTTP surface, and `provision`'s
  `firmware.config["porta"]` shape is Toit-era.

## Key decisions & constraints (don't re-litigate)

- **Verbs live only at the CLI.** The store and the wire have none (D2). Adding a feature
  adds a path, not a table, a verb or an endpoint.
- **All values on the wire and in the store are raw deterministic CBOR** (D4.1); decode only
  for display. All timestamps are **milliseconds** (D4.2). All paths are **node-relative**
  (D4.5) — the `node/<id>/**` prefix is a view, never storage, so a captured session can be
  re-pointed at a different node.
- **One monotonic seq per space per side** (D5). Kind is a property of the leaf, set by the
  first write (D6); `KindStream = 1`, `KindCell = 2`, matching `SPACE_KIND_*` in
  `nodus/oevm/src/space.h` — shared with the VM, do not renumber.
- **The integer band is ±(2^64−1)**, which exceeds `int64`. Represent out-of-range values
  exactly; never silently clamp.
- **There is no tombstone.** Stopped state is written explicitly as a value; genuine removal
  is an action on a `do/` stream.
- **A rewrite may delete a mechanism, but it may not silently delete a capability** (D8).
  Anything the old system could answer that the new one cannot is a regression wearing the
  costume of a simplification. This is why demolition was preceded by an audit.
- **Replay-from-0 is designed behaviour, and since 2026-08-26 it is safe per kind.** Cold
  boot = cursor 0 replays cells as latest-value (idempotent); streams are guarded — porta
  never re-serves a row the node has acked as applied (`nodes.cursor_hw`, bounded by
  `served_hw`), so `goal/do/*` cannot re-execute across reboots. See
  `porta/docs/design/2026-08-26-space-sync-durability-trio-design.md` and
  `PROTOCOL-NSL.md` §3.
- Bulk transfer (images, profiler blobs) is **deliberately off the space** and deferred to a
  CoAP blockwise plane; auth (G5) is out of this rung.
- Go 1.26, sqlite via `github.com/mattn/go-sqlite3`, cobra. Copyright header
  `// Copyright (c) 2026 Ekorau LLC` on every new Go file. `gofmt -l .` must print nothing
  before every commit.

