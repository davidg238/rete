# Gatekeeper query core (#198) — implementation plan

**Goal:** the §10 acceptance work of `porta/docs/design/2026-08-22-gatekeeper-first-cut-design.md`
(D3 ratified as (b)) — build `porta/internal/gate`, the schema-blind-porta / schema-aware-gate
query surface, and answer the five questions against a committed fixture snapshot **and** a
recorded run against the real store (`soak-2026-08-04.db`, gw85224-01).

**Falsifiable prediction under test (design §10):** (2) and (5) already answerable; (1) and
(3) need value projection **and** the reading rules; (4) needs build segmentation. If the
prediction fails, the design is wrong, not merely incomplete.

## Progress (2026-08-26 — the foundation landed; query core is the next session)

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

**Deliberately deferred to keep tonight correct:** the descriptor table (`records.yaml`) and
the completeness gate are NOT built yet — they must hold the wire truth, so **rete#267
(tel/log lvl/txt) lands first**, then the descriptor is authored against corrected MANIFEST.
Building on `text`/`level` now would enshrine a known-false pair the decoder cannot use.

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

### Task 1: the descriptor table + completeness gate
- `protocol/descriptors/records.yaml` — one entry per wire field: record, path, name,
  meaning, unit, kind, validity, segment, refs (design §4.4 shows the `lfb` exemplar).
  Authored from the record field order in `nodus/nsl/lib/kernel.nsl`.
- `porta/internal/gate/descriptors.go` — load YAML → schema (record→ordered columns),
  notes lookup by key.
- **Completeness gate (Go test, runs under `go test ./...`):** parse the record builders in
  `kernel.nsl`, extract field symbols in wire order, fail if any wire field lacks a
  descriptor entry OR the order disagrees. This is D5 made honest.

### Task 2: `porta snapshot --out`
- Subcommand in `porta/internal/portacli/` — `VACUUM INTO` a consistent point-in-time copy
  off the live WAL without blocking the writer (§2). Test: snapshot a synthesized store,
  reopen `immutable=1`, assert it reads and writes nothing.

### Task 3: gate bindings + the three-connection topology (§2.2)
- `gate.Open(snapshotPath)` / `gate.OpenLive(storePath)` — R (immutable=1 / mode=ro), W
  (scratch, on disk, fresh per query), Q (_query_only + authorizer). The `snap` binding
  (unbounded, all history) and `live` binding (bounded tables only: cells, nodes,
  exchanges capped 24h/node).

### Task 4: the authorizer-as-planner pipeline (§3)
- Build schema-only scratch (every logical table empty; columns from descriptors — this
  step IS the completeness gate at runtime).
- Prepare user SQL, authorizer records (table,column) reads and REJECTS non-reads
  (permit only SQLITE_READ/SELECT/FUNCTION; deny ATTACH/DETACH/PRAGMA/load_extension).
- Materialize only referenced tables — scan+decode from R via `cval`.
- Re-prepare and run; result carries `notes` (descriptor rules for every column touched).
- Resource guards (§2.3): wall-clock deadline → interrupt, SetLimit, row/byte caps.

### Task 5: decoded record tables + views (§4.2, §4.3, §5)
- Decoded tables: sys_metrics, sys_cheap, obs_health, obs_build, obs_liveness, sys_reset,
  tel_log, sys_latency — columns from descriptors, one row per stored record.
- `cells` gains a `stale` column computed exactly as `porta get`'s isStale (§4.1).
- Views: gc_cycle / gc_envelope (group by node,build,gcc; max lfb, min lb; envelope
  discards first ~5 cycles), boot, device (derived lineage §5.3), role (goal/name events),
  convergence (live: goal/X vs obs/X), staleness (live: why = never/refused/wedged via
  obs_health.ref), device_conflict (§6: overlapping boot sequence ⇒ two devices).

### Task 6: the CLI fronts (§9)
- `gate sql "<q>" [--binding snap|live] [--snapshot PATH]`, `gate describe <key|record>`,
  `gate tables` in `porta/cmd/gatekeeper`. `gate mcp` deferred unless cheap.

### Task 7: the five-question acceptance (§10)
- A committed fixture snapshot (synthesized: multi-build, multi-boot, multi-node, with a
  case where a naive aggregate is wrong — the rete#190 leak+fix shape).
- One test per question, asserting the design's prediction as an expectation:
  1. median free mem → gc_envelope gauge + must carry lfb's validity note.
  2. nodes disagreeing with goal/ → live convergence.
  3. GC arena grew over last soak → gc_envelope by boot+build, refuse a <~5h window.
  4. what changed A→B → boot × obs_build.
  5. stale nodes and why → live staleness, why distinguishing the three.
- device_conflict invariant test; completeness-gate test (both instruments, §10).

### Task 8: the real-store recorded run + close-out
- Copy `soak-2026-08-04.db` (static archive) gw85224-01 → workstation (read-only; no porta
  restart, no live touch). Run the five queries, record results, confirm/refute the §10
  prediction.
- Promote: the design doc is the record; `porta/docs/ARCHITECTURE.md` gets the gate/gatekeeper
  boundary; #198 close-out comment with the recorded results; next-steps entry.
