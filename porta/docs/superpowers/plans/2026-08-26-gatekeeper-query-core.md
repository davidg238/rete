# Gatekeeper query core (#198) — implementation plan

> **RENAMED 2026-08-29: `gatekeeper` is now `custos`** (Latin *guardian*, to sit with
> *porta*, the gate). The package is `porta/internal/custos`, the binary
> `porta/cmd/custos`. This document keeps the old name throughout because it is a
> dated record and the issues and commits that cite it resolve by that name; read
> "gatekeeper" below as "custos". The current picture is
> `porta/docs/ARCHITECTURE.md`.

**Goal:** the §10 acceptance work of `porta/docs/design/2026-08-22-gatekeeper-first-cut-design.md`
(D3 ratified as (b)) — build `porta/internal/gate`, the schema-blind-porta / schema-aware-gate
query surface, and answer the five questions against a committed fixture snapshot **and** a
recorded run against the real store (`soak-2026-08-04.db`, gw85224-01).

**Falsifiable prediction under test (design §10):** (2) and (5) already answerable; (1) and
(3) need value projection **and** the reading rules; (4) needs build segmentation. If the
prediction fails, the design is wrong, not merely incomplete.

## Progress (2026-08-27 — Tasks 1-3 landed; the pipeline (Task 4) is next)

The survey that opened this work produced five build-input corrections, recorded in the
design's 2026-08-26 addendum (A1–A5): the completeness-gate authority is **MANIFEST.cddl,
not the nsl builders** (builder order ≠ wire order); three §4.2 field-list drifts
(`obs_health` +`rej`, `obs_build` +`man`/−`sdk`/−`image`, `sys_metrics` wire order); the
`tel/log` `lvl`/`txt` **wire-contract bug filed as rete#267**; the ±(2^64−1) materializer
policy (loud error, no field reaches the band today); the authorizer's 3-string callback.

**Landed and tested this session:**
- Task 2 — **`porta snapshot --out`** (`Store.SnapshotInto`, VACUUM INTO, refuses existing
  dest; tested for immutable-open under `chmod a-w`, source untouched, no `-wal`/`-shm`).
- Task 1 infra — **`manifest.Entry.Fields`** wire-ordered field retention + `Entries()` /
  `Path()`, so the completeness gate reuses the machine-normative parser (A1's resolution).

**That deferral is now closed.** The descriptor table and the completeness gate were held
back until rete#267 (tel/log `lvl`/`txt`) landed, so the table would hold the wire truth
rather than enshrining a known-false pair; #267 landed at `555922e` and Task 1 followed at
`fd3bfb1`+.

**Landed 2026-08-27 (Task 1):** `protocol/descriptors/records.yaml`,
`porta/internal/gate/descriptors.go` and the completeness gate, with `gopkg.in/yaml.v3` as
the one new dependency (pure Go, no transitive deps; a hand-rolled YAML subset would have
been the larger mechanism). Three findings came out of authoring it and are recorded in the
design's 2026-08-27 addendum: the per-rule keying correction, `status: live | specified`
(eight of twenty shapes have no producer in this tree), and the `sumUsHi`/`sumUsLo` DECIMAL
split, which a binary reading gets wrong by orders of magnitude.

## Global constraints

- porta stays schema-blind; ALL record decode lives in `gate` (D3(b)). One CBOR codec
  (`internal/cval`) — a second would be a defect.
- The query surface is stdio/CLI only in this cut; no network route (G5 deferred, §9).
- Copyright header `// Copyright (c) 2026 Ekorau LLC` on every new Go file; `gofmt -l .`
  clean before commit.
- Two packages, one boundary (2026-08-26 addendum): `gatekeeper` = the shell (listener,
  docs, proxy); `gate` = the query core (pipeline, authorizer, descriptors, views).
- The materializer's table/column names come ONLY from the descriptor table, never from
  query text (§2.3 — W is the one injection surface).
- `tel/log` text is node-authored: data, never instruction; label it on the way out (§2.3).

## Tasks

### Task 1: the descriptor table + completeness gate — **DONE 2026-08-27**
- `protocol/descriptors/records.yaml` — 20 record shapes (81 fields) + 4 scalar rows, each
  field carrying name, meaning, unit, kind, and — where there is a real trap — validity,
  segment and refs (design §4.4 shows the `lfb` exemplar).
  **Keyed by CDDL RULE, not by bare field name** (the §4.4 exemplar's flat key collides:
  `bootId` is in Panic and Reset, `image` in App/AppObs/Build, `n` in LatencySpan and
  SpanCapture) and **authored from `protocol/MANIFEST.cddl`, not from `kernel.nsl`** —
  addendum A1's resolution, which the bullets here predated.
- `porta/internal/gate/descriptors.go` — load YAML → schema (`Columns(rule)` in wire order),
  `Note(rule, key)` for the reading rule, `TableName(path)` derived and never authored.
  Closed vocabularies for `unit`/`kind`/`status`, and required non-empty `meaning`, so a
  descriptor cannot exist and say nothing.
- **Completeness gate (Go test, runs under `go test ./...`):** `CheckCompleteness` walks
  `manifest.Table.Entries()` — the machine-normative wire-order authority — and fails on a
  missing field, an extra field, a transposed pair, an unclaimed path, an invented path, an
  orphan rule, or a descriptor naming the wrong record tag. This is D5 made honest.
- **Proven RED before green**, eight in-memory drift cases plus one end-to-end run against a
  deliberately-drifted `MANIFEST.cddl` (a field added to `Health`: caught, named, restored).
- Extra, not planned: `TestDescriptorOrderIsWireOrder` re-derives the bytewise sort from
  `cval.Encode` and checks it against the authored order, so the manifest header's
  "key order == wire order" is now a checked property rather than an assertion two documents
  could agree on and both be wrong about.

### Task 2: `porta snapshot --out`
- Subcommand in `porta/internal/portacli/` — `VACUUM INTO` a consistent point-in-time copy
  off the live WAL without blocking the writer (§2). Test: snapshot a synthesized store,
  reopen `immutable=1`, assert it reads and writes nothing.

### Task 3: gate bindings + the three-connection topology (§2.2) — **DONE 2026-08-27**
- `porta/internal/gate/bindings.go` — `gate.Open(snapshotPath, desc)` /
  `gate.OpenLive(storePath, desc)`; R (immutable=1 / mode=ro), and a per-query `scratch`
  holding W (on disk, fresh, read-write) and Q (`_query_only` + authorizer). The `snap`
  binding offers `cells events exchanges gw_runs nodes` uncapped; `live` offers
  `cells exchanges nodes` with `exchanges` capped at 24 h/node and NO `events`, so the
  boundedness is structural rather than a rule a query could be written around.
- The authorizer is built here because it is part of constructing Q, and it is already the
  planner half: it permits `SQLITE_READ`/`SELECT`/`FUNCTION`, denies everything else BY
  ACTION CODE, and records the `(table, column)` read set Task 4 materializes from. Q gets
  its own `sqlite3.SQLiteDriver` instance through a `driver.Connector` rather than a
  registered driver name, so the hook closes over one scratch's authorizer and two
  concurrent queries cannot see each other's plan.
- **Added beyond the design, and it closes a silent failure:** `Open` REFUSES a path with a
  `-wal`/`-shm` sibling. §2.1 records that `immutable=1` on a live store asserts something
  false and yields silently wrong reads rather than errors — so the one input that produces
  that outcome is rejected by name instead of trusted.
- **Every guard proven separately, because a passing test over two stacked guards cannot say
  which one acted:** store tables are unreachable from Q (`no such table`, the topology
  talking, not the authorizer); eleven non-read statements denied by action code including
  `ATTACH` of the snapshot's own path; `_query_only` alone refuses a write with no
  authorizer present — **and `_query_only` alone ALLOWS `ATTACH`, demonstrated by attaching a
  second db and reading it**, which is exactly §2.2's stated reason the authorizer is not
  optional given the topology; the scratch directory does not survive `Close`.

### Task 4: the authorizer-as-planner pipeline (§3) — **DONE 2026-08-27** (`9fd84a2`)
- Landed as `porta/internal/gate/pipeline.go` + `pipeline_test.go`; `go test ./...` ok.
- Three corrections recorded in the design's 2026-08-27 addendum (C1–C3): the uniform
  identity prefix and the `Overrun.path` collision that makes it `src_path`; decoded-table
  PROVENANCE (`snap` = full history from events, `live` = latest-value-per-path) riding out
  in `Result.Sources`; and `hard_heap_limit` deferred because it is process-global in SQLite
  and unexposed by go-sqlite3.
- Beyond the design: `Result.Sources` carries rows+elapsed per table, so §7's measurement
  needs no new code, and each decoded record's `status`, so B2's producerless shapes explain
  their own empty tables.
- Build schema-only scratch (every logical table empty; columns from descriptors — this
  step IS the completeness gate at runtime).
- Prepare user SQL, authorizer records (table,column) reads and REJECTS non-reads
  (permit only SQLITE_READ/SELECT/FUNCTION; deny ATTACH/DETACH/PRAGMA/load_extension).
- Materialize only referenced tables — scan+decode from R via `cval`.
- Re-prepare and run; result carries `notes` (descriptor rules for every column touched).
- Resource guards (§2.3): wall-clock deadline → interrupt, SetLimit, row/byte caps.

### Task 5: decoded record tables + views (§4.2, §4.3, §5) — **DONE 2026-10-04** (design addenda D1–D5)
- Decoded tables: sys_metrics, sys_cheap, obs_health, obs_build, obs_liveness, sys_reset,
  tel_log, sys_latency — columns from descriptors, one row per stored record.
- `cells` gains a `stale` column computed exactly as `porta get`'s isStale (§4.1).
- Views: gc_cycle / gc_envelope (group by node,build,gcc; max lfb, min lb; envelope
  discards first ~5 cycles), boot, device (derived lineage §5.3), role (goal/name events),
  convergence (live: goal/X vs obs/X), staleness (live: why = never/refused/wedged via
  obs_health.ref), device_conflict (§6: overlapping boot sequence ⇒ two devices).

### Task 6: the CLI fronts (§9) — **sql/tables/describe DONE 2026-10-04** as `custos <cmd>`; `mcp` not built
- `gate sql "<q>" [--binding snap|live] [--snapshot PATH]`, `gate describe <key|record>`,
  `gate tables` in `porta/cmd/gatekeeper`. `gate mcp` deferred unless cheap.

### Task 7: the five-question acceptance (§10) — **DONE 2026-10-04**
- Landed as `porta/internal/gate/acceptance_test.go` over the committed snapshot
  `testdata/acceptance.db` (regenerated by `-update`; `TestAcceptanceFixtureIsCurrent` fails
  on drift) and five committed queries, `testdata/acceptance/q*.sql` — the same text Task 8
  runs. The prediction held on the fixture: Q2/Q5 answer on `live` as they stand; Q1/Q3 need
  `gc_envelope` and the notes (the naive median reads ~1/3 of the real one; the unsegmented
  slope reads a leak+fix as free memory RISING); Q4 needs `boot` × `obs_build`.
- Q3's "refuse under ~5 h" is the reader's, from lfb's validity note — the query withholds the
  slope; no mechanism in the gate refuses. Witnessed red: with the rule at 1 h, a 2 h boot
  reads as a 1500 B/h leak; without the warm-up discard, the fix reads as a 2600 B/h leak.
- Found: `custos sql` reads a query beginning `-- comment` as a flag; pass `--` before it.
- A committed fixture snapshot (synthesized: multi-build, multi-boot, multi-node, with a
  case where a naive aggregate is wrong — the rete#190 leak+fix shape).
- One test per question, asserting the design's prediction as an expectation:
  1. median free mem → gc_envelope gauge + must carry lfb's validity note.
  2. nodes disagreeing with goal/ → live convergence.
  3. GC arena grew over last soak → gc_envelope by boot+build, refuse a <~5h window.
  4. what changed A→B → boot × obs_build.
  5. stale nodes and why → live staleness, why distinguishing the three.
- device_conflict invariant test; completeness-gate test (both instruments, §10).

### Task 8: the real-store recorded run + close-out — **run recorded 2026-10-04** (design addendum E)
- Ran against `snap-20261004.db` and the live store (`mode=ro`) on gw85224-01 instead of
  copying `soak-2026-08-04.db` here — the store kept that name and is now 2.5 GB. The prediction
  held on both. ARCHITECTURE already carries the custos/gate boundary. **Owed:** the #198
  close-out comment and the next-steps entry (David's call on closing #198).
- Copy `soak-2026-08-04.db` (static archive) gw85224-01 → workstation (read-only; no porta
  restart, no live touch). Run the five queries, record results, confirm/refute the §10
  prediction.
- Promote: the design doc is the record; `porta/docs/ARCHITECTURE.md` gets the gate/gatekeeper
  boundary; #198 close-out comment with the recorded results; next-steps entry.
