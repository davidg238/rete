# Gatekeeper — the first cut (design, 2026-08-22)

> **RENAMED 2026-08-29: `gatekeeper` is now `custos`** (Latin *guardian*, to sit with
> *porta*, the gate). The package is `porta/internal/custos`, the binary
> `porta/cmd/custos`. This document keeps the old name throughout because it is a
> dated record and the issues and commits that cite it resolve by that name; read
> "gatekeeper" below as "custos". The current picture is
> `porta/docs/ARCHITECTURE.md`.

**Status:** decision record, closed on every fork it opens. Successor to
`2026-08-11-gatekeeper-and-the-introspectable-space-design.md`, which is now history: its
D3 is ratified, its §9 "does not decide" list is decided, and one of its five reading
rules turned out to be understated. Read that note for the *why*; read this one for the
*what*.

**This is a design, not a plan.** The implementation plan follows separately.

---

## 1. What was ratified (David, 2026-08-22)

| ref | decision |
|---|---|
| D1 | porta is OPERATIONS; gatekeeper is the query and authority surface. **Taken 2026-08-11.** |
| D2 | the meta level is answered by the same read as the object level. **Taken 2026-08-11.** |
| D3 | **(b)** — the query layer decodes; the store stays opaque. (c) held in reserve, (a) rejected. |
| D4 | the payload is the READING RULES, not the schema. |
| D5 | schema and rules are per-BUILD, held northbound, generated not written. |
| D6 | the agent is the client, not the help desk. |
| §9.1 | gatekeeper is a **separate process**. |
| §9.3 | **the compact machine surface is primary**; the human view is a rendering of the descriptor table. This settles rete#197's display question. |
| §9.4 | **retention: retain until there is a reason to delete.** No retention bound, so every aggregate is unbounded and §7's measurements are what keep that honest. |

Two things the 2026-08-11 note got wrong, corrected here rather than preserved alongside:

- **D5's "generated not written" cannot cover the reading rules.** The field *skeleton* is
  genuinely generatable — `Kernel>>metrics` in `nodus/nsl/lib/kernel.nsl` lists the wire
  symbols in wire order, and that is the authority, not the prose. But *meaning*, *unit*,
  *validity* and *segmentation* exist only as narrative. §4 below replaces "generated" with
  **generated skeleton + authored rules + a completeness gate**, which is stronger than
  either half: a wire field with no descriptor entry fails the build.
- **§6's fourth reading rule is understated.** It says "a node id is a slot, not a board".
  The store now also demonstrates the converse — *a board is not one id*. See §5.

---

## 2. Topology: the interface is a snapshot file

**The porta↔gatekeeper interface is a file, not a database handle.** This is the load-bearing
piece the 2026-08-11 note flagged and declined to settle.

- `porta snapshot --out snap-<ts>.db` — porta owns the store, so producing a consistent
  point-in-time copy is porta's job and an operations job (D1). `VACUUM INTO` yields a
  compacted, self-contained file off a live WAL database without blocking the writer.
- gatekeeper opens that snapshot with **`immutable=1`**, which is *true* of a snapshot and
  false of a live store. The file needs no `-shm` write, so it can be `chmod a-w` or owned
  by another user.

### 2.1 Why not simply open porta's store read-only

Measured 2026-08-22, and the result is why this section exists at all:

| test | result |
|---|---|
| `mode=ro` on a WAL db, directory writable | reads fine — **and creates `-shm` and `-wal`** |
| OS-level read-only (`chmod a-w` on file *and* directory) | `attempt to write a readonly database` — **cannot read at all** |
| same, plus `immutable=1` | reads, and writes nothing |

So: **SQLite's `mode=ro` is a promise SQLite keeps, not a capability the process lacks** —
the process still holds write permission on the file. A genuine OS-level boundary cannot
read a live WAL store, because a WAL reader must write `-shm`. And `immutable=1` buys the
boundary by asserting something false about a live store, which yields silently wrong reads
rather than errors.

A snapshot dissolves all three: the assertion is true, the boundary is real, and the file
can live anywhere. **Gatekeeper therefore does not have to run on the gateway** — which
removes the tension between D1's "porta sheltered, gatekeeper exposed" and any design where
the exposed process needs a handle on the sheltered one.

Two further hazards a read-only handle would not have addressed:

- **Contention.** A long scan holds a read snapshot, which blocks WAL checkpointing. The
  live store's WAL is already 152 MB against a 400 MB database. An unbounded historical
  query against the operational store is an operational hazard *caused by reading*.
- **Availability.** `select * from events a, events b` is 2.8 × 10¹² rows, entirely legal
  and entirely read-only. Read-only is no guard here whatsoever.

### 2.2 Three connections, and why "delete all" is unreachable rather than blocked

| conn | target | mode | who writes statements |
|---|---|---|---|
| **R** | snapshot (or, for `live`, porta's store) | `immutable=1` / `mode=ro` | gatekeeper's materializer only |
| **W** | scratch db, fresh per query, **on disk** | read-write, private | gatekeeper's materializer only |
| **Q** | the same scratch db | `_query_only` + authorizer | **the user / the agent** |

The load-bearing guard is the topology: the only database reachable from a query anyone
types is a scratch db built for that one query and discarded after it. `DELETE FROM events`
is not denied — there is no `events` on that connection, and no path from Q to any store.

On top of the topology, the authorizer on Q permits only `SQLITE_READ`, `SQLITE_SELECT` and
`SQLITE_FUNCTION`. `ATTACH`, `DETACH`, `PRAGMA` and `load_extension` are denied by action
code; `ATTACH` is the one that would otherwise reach back out to the filesystem, and it is
why the authorizer is not optional even given the topology.

Scratch goes **on disk, never `:memory:`**: gw85224-01 has 3.7 GB of RAM with 182 MB free
and 1.7 TB of disk. OOMing the box that runs the fleet is a worse outcome than a slow query.

### 2.3 Resource guards, which read-only never provided

- a wall-clock deadline per query → `sqlite3_interrupt`
- `SetLimit` on expression depth, compound-select and VDBE ops; `hard_heap_limit` on Q
- a row cap on results and a byte cap on the scratch file
- the materializer's table and column names come **only from the descriptor table, never
  from query text** — W is read-write by design, and it is the one place injection matters

`tel/log` text is node-authored and rides into result sets. It is data, never instruction,
and gatekeeper labels it as such on the way out — the MCP front means an agent writes the
SQL, so this is a live concern rather than a theoretical one.

### 2.4 Two bindings

| binding | source | contents | why |
|---|---|---|---|
| `snap` | immutable snapshot | `events` (1.7 M), decoded record tables, all history | unbounded; must not touch the live store |
| `live` | live store, `mode=ro`, bounded tables only | `cells` (89 rows), `nodes` (6), `exchanges` **capped at the last 24 h per node** | "stale **right now**" must not be answered from a file |

The split is from the data, not from taste. It is also not hypothetical caution: rete#225 is
the failure where `gw/liveness` read `#live` for ten hours and six minutes while `c6-bell`
missed every check-in. A staleness answer computed from yesterday's snapshot reproduces that
bug by construction. The `live` binding exposes only bounded tables, so no query written
against it can scan 1.7 M rows — the boundedness is structural, as in §2.2. `exchanges` is
the only unbounded table on this binding, so it is materialized with a hard 24 h/node cap
rather than exposed whole; a query needing more history belongs on `snap`.

---

## 3. The query pipeline: the authorizer is also the planner

D3(b) means gatekeeper must decode before it can aggregate, and a general SQL surface means
it does not know *what* to decode until the query is parsed. That is circular — except
SQLite already solves it. The authorizer fires during `prepare` and reports every
`(table, column)` a statement reads.

1. **Build a schema-only scratch db** — every logical table, empty, columns generated from
   the descriptor table. This step *is* §4's completeness gate.
2. **Prepare the user's SQL against it**, authorizer recording. Two jobs at once: it rejects
   anything that is not a read, and it yields the exact table/column set the query needs.
3. **Materialize only those**, scanning and decoding from R.
4. **Re-prepare and run.**

**No predicate pushdown in the first cut.** Materialize the whole referenced slice and
measure it (§7). Pushdown is earned by a measurement, not assumed.

### 3.1 Every answer carries its reading rules

Because step 2 knows exactly which columns a result touched, **every answer returns the
descriptor notes for every column it touched** — not a lookup the caller must remember, not
a heuristic refusal, but riding with the data:

```
{ "binding": "snap", "snapshot_ms": 1787446515059,
  "columns": [...], "rows": [...],
  "notes": [ { "column": "lfb",
               "validity": "invalid as a point sample; use max() per gcc bucket, discard the first ~5 cycles",
               "segment":  ["obs/build"],
               "refs":     ["rete#153"] } ],
  "truncated": false }
```

This is D4's whole payload delivered structurally. `select median(lfb) …` returns the median
*and* the reason the median is not the gauge. A help desk that returns data without the rule
for reading it industrialises error at machine speed; this is the cheapest thing that does
not.

The dictionary is queryable by the same read as everything else —
`select * from descriptors where key = 'lfb'` — which is **D2 landing rather than being
asserted**.

---

## 4. Tables, views and descriptors

### 4.1 Structural tables

`events`, `cells`, `exchanges`, `nodes`, `gw_runs` pass through near-verbatim. `cells` gains
a **`stale` column** computed exactly as `porta get`'s `isStale` does, so the rete#210
mixed-read hazard is a column rather than a banner a pipe can eat.

### 4.2 Decoded record tables

One per record-bearing path, columns generated from descriptors:

```
sys_metrics (node_id, ts_ms, boot_id, seq, hsz lb hhw lfb gcc hnl hnhw qhw ovr pdr pco pst)
sys_cheap   (node_id, ts_ms, boot_id, seq, ob ohw alc fre fal)
obs_health  (node_id, ts_ms, boot_id, seq, up bid wak pto sdr ref)
obs_build   (node_id, ts_ms, boot_id, chip build sdk image)
obs_liveness(node_id, ts_ms, boot_id, cad bud)
sys_reset   (node_id, ts_ms, boot_id, reason code bootId)
tel_log     (node_id, ts_ms, level, text)
sys_latency (node_id, ts_ms, boot_id, span n fails minUs maxUs sumUsLo sumUsHi b0..b5)
```

### 4.3 Views — §6's payload made executable rather than quotable

- **`gc_cycle` / `gc_envelope`** — group by `(node, build, gcc)`, `max(lfb)`, `min(lb)`;
  `gc_envelope` additionally discards the first ~5 cycles per boot. rete#153's three rules
  as SQL nobody can forget to apply.
- **`boot`** — `(node_id, boot_id, first_ms, last_ms, build, reset_reason)`. The
  segmentation unit, and rete#208's lesson made structural.
- **`device`** — the identity lineage, §5. **Derived, not authored.**
- **`role`** — `(role, node_id, from_ms, to_ms, actor, reason)`, derived from `goal/name`
  events. §5.
- **`convergence`** (`live`) — `goal/X` vs `obs/X`, which is already a tree diff.
- **`staleness`** (`live`) — with `why` separating *never checked in* / *refused* / *wedged*.
  The discriminator is `obs_health.ref`, because rete#199 established that `misses` counts
  no-reply, so a uniformly refused node reads `misses 0` all the way to its death.
- **`device_conflict`** — §6.

`alc − fre` gets no view. It gets a **validity rule** on both columns, because
`oevm_realloc` bumps `allocs` and never `frees`, so the subtraction is inflated rather than
wrong in a way a view could fix.

### 4.4 The descriptor table

Authored at **`protocol/descriptors/records.yaml`** — beside the catalogue it annotates,
because adjacency is what keeps the drift gate obvious. (A judgement call; the file is
northbound-only, which argues for `porta/`, and it describes wire records, which argues for
`protocol/`. Reversible.)

```yaml
lfb:
  record:   Metrics
  path:     sys/metrics
  name:     largestFreeBlock
  meaning:  largest contiguous free block in the GC arena
  unit:     bytes
  kind:     instantaneous
  validity: >
    INVALID as a point sample. The GC fires only on arena exhaustion, so the sample
    before a collection reports what the FAILING allocation left behind. Use max()
    per gcc bucket; discard the first ~5 cycles of a boot; do not fit a trend to a
    window shorter than ~5 h. min(lfb) is not a gauge at all.
  segment:  [obs/build]
  refs:     [rete#153, "review/tests/CLAIMS.md"]
```

**The completeness gate is a Go test**, not a new script: it parses the record builders in
`nodus/nsl/lib/kernel.nsl`, extracts the field symbols in wire order, and fails if any lacks
a descriptor entry or if the order disagrees. It runs under `go test ./...`, which is already
a gate — no new gate to remember, which is the whole point.

This is what makes D5 honest: the skeleton is generated and cannot drift; the rules are
authored once, in one place, and cannot be silently incomplete.

---

## 5. Identity: device, role, lineage

### 5.1 The finding

**The pre-#186 ids were roles.** `c6-bench` is "the bench soak node", `c6-bell` is "the ships
bell", `c6-timerless` is a liveness scenario. #186 replaced a role-shaped id with a
device-shaped one, which is why the history split on 2026-08-22. The two-level model was not
missing; the system had level one and traded it for level two.

And `goal/name` — porta-authored, node-echoed at `obs/name`, append-only in `events`, already
carrying `actor` and `reason` — is level one's path, currently set to a copy of the id and
therefore carrying no information at all.

### 5.2 The mechanism: role is `goal/name`, and it is optional

- **Device id** stays `platform nodeId`, build-time, self-asserted (#186). A developer
  flashes a board and it appears with a space. **Commissioning cannot get in the way of
  bring-up because it is not on the path** — the answer is not to add a step, it is to make
  the step optional.
- **Commissioning is one write:** `porta put c6bench goal/name "bathroom-1"`. Whether it took
  is `goal/name` vs `obs/name` — the convergence diff that already exists.
- **Uncommissioned is a legal, honest state.** Absent `goal/name` means "this device has no
  role", the same posture as `obs/build`'s omitted `sdk`: absence means the system cannot
  say. Dev boards live there permanently and nothing complains.
- **Role history is the `goal/name` rows in `events`.** A board dies, the role is written to
  its replacement, and the log records when and by whom. **No new table, no new path, no new
  mechanism** — which is what #186 ruled out ("a table that disagrees with the collector's is
  #170").

Roles are also the right **federation key**: `bathroom-1` means something across gateways;
`c6bench` does not.

**Uniqueness** — two devices holding one role is a commissioning error, detected by a
`role_conflict` view rather than enforced at write. Enforcing means porta learning that one
path is special, which is what D2 forbids. Revisit if a conflict ever produces a wrong answer.

### 5.3 The lineage is derived, and here is the evidence

The persisted per-board boot counter is a **device fingerprint**. Merging `exchanges.boot_id`
by arrival time over `soak-2026-08-04.db`:

```
118 c6-bench      121 c6-bench      124 c6-bench      125 c6spare
119 c6-timerless  122 c6-timerless                    126 c6spare
120 c6-timerless  123 c6-timerless
```

118 → 126 is **gapless, interleaved across three node ids, and never overlapping in time**.
Likewise `c6-bell` boot 8 (last 17:36 MST) → `c6` boot 9 (first 17:37). And `c6bench` runs
boots 1/2/3 *concurrently* with `c6spare`'s 125/126.

| board | ids it has answered to |
|---|---|
| A | `c6-bench` → `c6-timerless` → `c6spare` |
| B | `c6-bell` → `c6` |
| C | `c6bench` (fresh counter, no prior history) |

Three boards, matching the three on the bench. **The invariant: merged sequence gapless and
non-overlapping ⇒ one device; overlapping ⇒ two.**

Because this is derived, the `device` view is computed rather than curated, and #186's
no-mapping-table rule holds with no exception.

---

## 6. Duplicate device detection

Because the id is baked from `--board`, flashing one board's image onto another makes it
announce the same id — which is #186's own damage ("36,153 events in one space from at least
two boards").

**`device_conflict` is a view, and it ships in this rung.** Its rule is §5.3's invariant run
in the other direction: a node id whose merged boot sequence *overlaps in time* is two
devices. Corroborated by `obs/build` and `last_addr` alternating within a window. It costs
nothing, it catches duplicates regardless of who flashed — including ones predating porta —
and it is what found #186's damage in the first place.

**Flash-time prevention is a later layer, never a replacement.** When porta orchestrates
flashing it will know which baked id went to which USB `by-id` serial, and a second flash of
one id to a different serial is catchable. But it only sees flashes porta performed; a
hand-flashed board or one already in service is invisible to it. Shipping only that produces
a green light that is not warranted.

**A hardware-rooted id would make duplicates impossible rather than detected**, and §5.2
removed #186's main objection to one. That is **rete#242**, filed as its own decision because
it is a wire change. This rung does not depend on its outcome: detection ships either way.

---

## 7. Measurements

All taken 2026-08-22 against the live `soak-2026-08-04.db` (400 MB, 1,696,916 events,
897,490 exchanges, 89 cells, 6 nodes) on gw85224-01.

| step | cost |
|---|---|
| `VACUUM INTO` snapshot | **5.6 s**, 400 MB → 352 MB, source WAL untouched |
| open snapshot `immutable=1` under `chmod a-w` | reads; **wrote nothing** |
| scan worst slice (`c6-bench` / `sys/metrics`, 404,790 rows) | **0.83 s** |
| fetch all 404,790 blobs (47.9 MB) | **1.27 s** on the gateway, 745 ms on the workstation |
| **decode 404,790 records through `cval`** | **419 ms** — 1.035 µs/record, 4,567,955 fields |
| **fetch + decode, end to end** | **1.16 s** |
| copy the 352 MB snapshot gateway → workstation | 8.5 s (~41 MB/s over Tailscale) |

The decode run was executed **on the workstation, against a copied snapshot file**, which is
the end-to-end confirmation of §2.1's claim that gatekeeper does not have to run on the
gateway.

**A snapshot is a background chore, not a query cost.** Hourly is trivial; on demand is fine.

**D3(b) holds and (c) stays unbuilt.** The 2026-08-11 note recorded the trigger for (c) as
"a real query, over a real month of a real node's history, that (b) cannot answer inside an
interactive latency budget". That query has now been run against the worst slice that exists,
and the answer is **1.16 s** — decode is 419 ms of it, so decode was never the cost the note
worried it might be. The reserve is not needed and the deferral was correct.

**Decode is 36% of the cost and I/O is 64%**, which also says where pushdown would pay if it
is ever earned: narrowing the scan, not speeding the codec.

**The scaling trigger, quantified.** 404,790 rows is `c6-bench` at a **2 s developer
cadence**, not a deployment one. Fifty nodes at the bell's 30 s cadence is ~4.3 M rows per
record-path per month per gateway — about 10× today, and the first point at which predicate
pushdown or D3(c) is earned. It is synthesizable now, so that trigger is measurable rather
than vague. Not built.

---

## 8. Federation

A gateway serves a cluster of <50 nodes and produces snapshot files. Gatekeeper federates by
reading N snapshots, or by fanning out to N gatekeepers and merging. **Aggregation and
federation, not a godzilla-sized database in the sky** (David, 2026-08-22).

The snapshot interface is what makes this possible rather than aspirational: a snapshot is a
file, so a federation member is an argument, not an integration. It also gives attenuation
its shape — an attenuated capability is a **narrower snapshot**, not a filter applied at read
time.

Not built in this rung. Named so the first cut does not foreclose it.

---

## 9. Surface

One query core; two thin fronts; **separate process** (§9.1), **same Go module**
(`porta/cmd/gatekeeper` + `porta/internal/gate`) so it shares `cval`. A second CBOR codec
here would be a defect, not a convenience — the algebra's map keys are tagged values and the
decode is deliberately closed.

```
gate sql "<query>" [--binding snap|live] [--snapshot PATH]
gate describe <key|record>          # sugar over: select * from descriptors
gate tables                         # the logical schema
gate mcp                            # serve MCP over stdio
```

MCP tools, three: `space_sql`, `space_tables`, `space_describe`. Stdio only, so there is no
network surface and **G5/auth stays deferred honestly rather than half-built**. When it
lands it lands here, per D1, and the nsl capability model — rooted path prefixes that can
only be attenuated — is the shape to reuse.

Per §9.3 the compact keys are the surface. `gate describe` renders the human view *from the
descriptor table*, which is what makes rete#197's rename a display concern rather than a wire
concern.

---

## 10. Acceptance

The five questions from the 2026-08-11 note §10, each a test against a small committed
fixture snapshot **and** a recorded run against the real store:

1. Median free memory for node XYZ over the last month → must return the `gc_envelope`
   gauge, and must carry `lfb`'s validity note.
2. Which nodes disagree with their `goal/` tree right now → `live` binding, `convergence`.
3. Did any node's GC arena grow over its last soak → `gc_envelope`, segmented by `boot` and
   `obs/build`, and must refuse to fit a trend to a window under ~5 h.
4. What changed between build A and build B on the same node → `boot` × `obs_build`.
5. Which nodes are stale, and why → `live`, `staleness`, with `why` distinguishing the three.

**The note's prediction was that (2) and (5) were already answerable, (1) and (3) needed
projection plus the reading rules, and (4) needed build segmentation.** That prediction is
recorded as a test expectation, not as prose: if it fails, this design is wrong and not
merely incomplete.

Tests first, per the project's TDD posture. The completeness gate (§4.4) and the
`device_conflict` invariant (§6) each get their own test, because both are instruments and
an untested instrument reads green while dead.

---

## 11. What this does not decide

- **Auth (G5).** It has a home (§9) and a model to reuse, and no implementation.
- **Whether the device id becomes hardware-rooted** — rete#242.
- **Predicate pushdown and D3(c).** Triggers quantified in §7; neither built.
- **Federation mechanics** — §8 names the shape, nothing more.
- **Whether `protocol/descriptors/` is the right home** for the descriptor file (§4.4).
- **Retention.** Ratified as "retain until there is a reason to delete", so there is no
  mechanism here — and §7's arithmetic is what will eventually supply the reason.

---

**Refs:** `porta/docs/design/2026-08-11-gatekeeper-and-the-introspectable-space-design.md`
(the opened note this closes), rete#198 (the tracking issue), rete#242 (hardware-rooted id),
rete#186 (build-time id, and the cut-over), rete#197 (wire labels; §9.3 settles its display
question), rete#153 (`largestFreeBlock`), rete#199 (`ref` vs `misses`), rete#208 (per-boot
segmentation), rete#210 (mixed reads), rete#225 (a frozen liveness verdict), rete#170 (a
table that disagrees with the collector's),
`porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md` (D2, D4.1, D4.5, D8),
`protocol/PROTOCOL-NSL-RECORDS.md` §4.

---

## Addendum 2026-08-26 — the process shell exists, with a network surface for DOCS only

**What happened:** David directed (2026-08-26) that the dev status surface built that
morning belonged to gatekeeper, not porta, and that gatekeeper should stand up as a process
now. It did (`4e1469d`): `porta/cmd/gatekeeper` runs on gw85224-01:**6971**, serving the
repo docs tree at `/dev` (the dev status page at `/`) and **proxying** `/health` and
`/manifest` to porta's sheltered :6970. Deployed the same day — a separate process (§9.1)
restarts freely, so it did not wait on a porta restart.

**What this does and does not change about §9:**

- §9's "no network surface" sentence is about the QUERY surface, and it stands: the shell
  decodes nothing, reads no store, and exposes no query. The network routes are static docs
  plus a proxy of two routes porta already exposed on :6970 behind the same allowlist
  (`httpsrv.DefaultAllowCIDR`, now shared so the two surfaces cannot drift). G5 stays
  honestly deferred: when the query surface lands, it lands stdio-MCP-first per §9, and any
  future network query route waits for G5.
- **Naming:** the shell landed as `porta/internal/gatekeeper` (the process: listener,
  docs, proxy). §9's `porta/internal/gate` remains the name for the QUERY core (pipeline,
  authorizer, descriptors) when it is built — two packages, one boundary: `gate` answers,
  `gatekeeper` fronts.

**Status correction propagated:** rete#198's body still reads "ratify D3, then run the
five-question spike" — it predates this note, which ratified D3=(b) on 2026-08-22. The
five-question spike is therefore §10's acceptance work, blocked on nothing. A correcting
comment was posted on #198 the same day as this addendum.

---

## Addendum 2026-08-26 — build-input corrections found while opening the query core

Building `porta/internal/gate` began with a full survey of the surfaces §2–§5 name. Five
findings correct this design **before** the query core is built to it — recorded here so the
"resolve contradictions, don't route around them" rule (CLAUDE.md) is honoured at the design
level rather than papered over in code.

### A1 — the completeness-gate authority is `MANIFEST.cddl`, not the nsl builders (corrects §4.4)

§4.4 says the completeness gate "parses the record builders in `nodus/nsl/lib/kernel.nsl`,
extracts the field symbols **in wire order**". **Builder order is not wire order.**
`cval.Encode` sorts a record's map keys **bytewise over their encoded bytes**
(`porta/internal/cval/encode.go`, "This is NOT the Map's own iteration order") — shorter
names first, then lexicographic. `Kernel>>metrics` is authored `hsz lb hhw lfb gcc …`; the
wire carries `lb gcc hhw hnl hsz lfb …`. A gate keyed on builder order fails on every
multi-length-key record. Worse, three of the eight records (`Health`, `Reset`, and the
per-app `Health` copies) are not built in `kernel.nsl` at all.

**Resolution:** the authority is `protocol/MANIFEST.cddl` — it postdates this design (#243,
2026-08-24), declares itself **machine-normative for path/kind/class/dir/shape**, and its
header states **key order == wire order**. `porta/internal/manifest` already parses it; it
now also retains the ordered field-key list per record row (`manifest.Entry.Fields`,
`Entries()`, `Path()` — landed 2026-08-26). The completeness gate is a reuse of that parser,
not a second parser over nsl source.

### A2 — three catalogue drifts in §4.2's column lists

§4.2 predates the MANIFEST pin and is wrong on three records. The corrected wire-order fields
(from MANIFEST, hand-verified against the bytewise-sort rule):

- **`obs_health` has SEVEN fields, not six:** `up bid pto ref rej sdr wak`. §4.2 omits
  **`rej`** (the #232 rider — a counted-and-dropped inbound entry, permanent under the replay
  guard, so it must be visible).
- **`obs_build` needs `man` and does not need `sdk`/`image` from any live builder:** the
  shipping `publishBuild:` writes `chip`, `build`, and `man` (the #243 truncated manifest
  pin) when pinned; `sdk`/`image` are optional and unwritten today. §4.2 lists
  `chip build sdk image` and omits `man`.
- **`sys_metrics` wire order is** `lb gcc hhw hnl hsz lfb ovr pco pdr pst qhw hnhw` (§4.2's
  authored order `hsz lb hhw lfb gcc …` is builder order — see A1).

### A3 — `tel/log`'s field names are a genuine WIRE-CONTRACT BUG, filed separately

Every shipping producer writes `#lvl`/`#txt` (`kernel.nsl:2769` `telLog:`, and all six
per-app bridges), and the node's own tests read `at: #lvl`. But `MANIFEST.cddl`'s `log` rule,
the golden generator (`manifest_goldens.nsl`), `PROTOCOL-NSL-RECORDS.md` §4, and this design
§4.2 all say `text`/`level`. **No gate catches it:** porta's manifest gate checks only the
record *head* bytes (tag + map header), never the field keys, and the golden validates
against the CDDL it agrees with — not against the producer. This is **rete#267**
(a wire-contract fix with zcbor + golden-regeneration steps), and the descriptor table and
`tel_log` materializer should land **after** it, so they hold the wire truth (`lvl`/`txt`)
rather than enshrining the documented-but-false pair.

### A4 — the ±(2^64−1) band has no SQLite representation (a materializer policy, §3)

`cval.Decode` yields `cval.Uint` / `cval.NegUint` for integers outside `int64`
(`value.go`). SQLite's INTEGER is `int64`, so the materializer cannot round-trip the band
into a scratch column silently. **Today no catalogue field reaches the band** (every metric
is a small `uint`), so the cheap correct policy is a **loud error on an out-of-band value**,
never a truncation — recorded here so the materializer is written that way from the start.

### A5 — the authorizer callback drops SQLite's 4th string (a §3 mechanism note)

`go-sqlite3 v1.14.40`'s `RegisterAuthorizer(func(op int, arg1, arg2, arg3 string) int)`
forwards **three** strings, not the four the pipeline sketch assumed: `arg3` is the database
name and the trigger/view-name is dropped. For `SQLITE_READ` this is exactly `(table,
column, dbname)` — sufficient for §3 step 2's referenced-column recording. It only bites if
the authorizer ever needs to reason about view/trigger provenance, which the first cut does
not. Reached via a **named** `ConnectHook` driver so porta's plain `"sqlite3"` is untouched;
the wall-clock deadline is `context.WithTimeout`+`QueryContext` (no exported `Interrupt()`),
backstopped by `SQLITE_LIMIT_VDBE_OP`.

### Landed 2026-08-26 (the foundation, ahead of the query core)

- **`porta snapshot --out`** (`Store.SnapshotInto`, `VACUUM INTO`, refuses an existing dest)
  — the §2 interface primitive; tested to open `immutable=1` under an OS-level `chmod a-w`
  boundary, leave the source writable, and write no `-wal`/`-shm` sidecar.
- **`manifest.Entry.Fields`** — wire-ordered field retention (A1's resolution), with
  `Entries()` and `Path()` accessors.

The descriptor table (`protocol/descriptors/records.yaml`), the completeness gate, the
materializer/authorizer pipeline, the views, and the five-question acceptance remain — a
clean next session with the schema authority (A1), the field sets (A2), and the mechanism
realities (A4/A5) now pinned, and A3 sequenced first.

## Addendum 2026-08-27 — A3 resolved; the network-health view is a gatekeeper client (a §9 note)

**A3 is closed**: rete#267 landed at `555922e`, ahead of the descriptor table as sequenced.
`MANIFEST.cddl`, the manifest golden and RECORDS §4.3 now carry the wire truth
(`lvl`/`txt`), and the golden gate was proven red-then-green against the drift — so the
descriptor table can be built from the manifest without the caveat A3 recorded.

**Where network health lives, decided while updating the landing page.** David asked
whether the network-health view (v1's web `/status` with the wifi-vs-Thread transport
breakdown; §4.2 audit fate "deferred → operator surface") should return on porta or on
gatekeeper. D1's one-door rule answers it: **a gatekeeper page, fed from porta snapshots
through this design's query surface — never porta HTML.** Porta stays sheltered and
schema-blind (the same reason the `--dev-docs` flag was removed before it deployed). The
per-node last-seen / transport / convergence view therefore waits on the descriptor table
and query pipeline, which is what makes it correct rather than another bespoke door.

The degenerate case shipped the same day (`fd3bfb1`, deployed live, no restart): the
landing page at :6971 carries a client-side `/health` stats strip (status, nodes, version,
uptime, `blocks_plane_down` when present, loud unreachable fallback) and a
documents-of-record index. Zero new server mechanisms — a static file plus the proxy the
shell already had. What earns the fuller page back: the descriptor table landing, at which
point the strip's own limits (no per-node rows, no transport, no goal/obs diff) name the
first three queries it should serve.

## Addendum 2026-08-27 — three corrections from authoring the descriptor table (§4.4)

Task 1 landed: `protocol/descriptors/records.yaml` (20 record shapes, 81 fields, 4 scalar
rows), `porta/internal/gate/descriptors.go`, and the completeness gate as a Go test under
`go test ./...`. Three things the design got wrong or did not know, corrected here rather
than papered over in the code.

### B1 — the descriptor table is keyed by RECORD SHAPE, not by field name (corrects §4.4)

§4.4's exemplar is a flat map keyed on the bare field: `lfb:` at the top level, with
`record` and `path` as attributes inside. **That key collides.** `bootId` rides in both
`Panic` and `Reset`; `image` in `App`, `AppObs` and `Build`; `n` in `LatencySpan` and
`SpanCapture`; `monoMs` in `Panic` and `Metric`; `mode`, `triggers`, `lifecycle` and
`runlevel` each in two shapes. A flat table silently keeps one and loses the other.

**Resolution:** the table is keyed by CDDL rule — the shape — and `paths` is a list, because
one shape legitimately rides several paths (`goal/mode` + `obs/mode`; `sys/latency` +
`diag/spans/<span>`). Field entries live in an ordered list under the shape. This also makes
§4.2's per-path tables fall out cleanly: the table name is *derived* from the path
(`TableName`, never authored, so it cannot drift from the path it holds) and its columns
come from the shape's descriptors.

### B2 — `status: live | specified`, because eight of twenty shapes have no producer

Authoring against MANIFEST surfaced something no document said: **`Mode`, `Debug`,
`Profile`, `Reboot`, `Overrun`, `Print`, `Metric` and `Reset` are catalogued shapes that
nothing in this tree builds or consumes.** (`sys/reset` is the sharpest case: every bridge
`subscribeStream:`s it, so the delivery path is wired — and no code anywhere constructs a
`Reset`, so the channel is drained and never fed.) An agent querying those paths gets
silence, and silence at a path is otherwise indistinguishable from a dead node.

**Filed as rete#268**, because some of the eight are deferred design (`Mode`) and some are
gaps (`Reset` has a wired delivery path and no source), and the catalogue does not
distinguish the two. So each record carries `status`. `live` means something builds it
today; `specified` means the shape is catalogued and the silence is expected. This is authored, not derived — a
producer scan across nsl and Go would be a second parser over source, which is exactly what
A1 rejected. It is checked only for vocabulary; it goes stale the day a producer lands, and
the cost of that staleness is a query surface that under-promises.

### B3 — `sumUsHi`/`sumUsLo` split DECIMALLY, and nothing said so

`LatencySpan` carries the window's total microseconds across two fields. Every instinct says
a 2^32 split. `nodus/oevm/src/span.c:85` writes `sum % 1000000000` to `sumUsLo` and
`sum / 1000000000` to `sumUsHi` — **decimal**, so the total is
`sumUsHi * 1e9 + sumUsLo`. A reader assuming binary is wrong by orders of magnitude, and
every mean (`total / n`) derived from it is wrong with it. Neither `PROTOCOL-NSL-RECORDS.md`
§4.4 nor `MANIFEST.cddl` says which, because neither is in the business of saying — this is
precisely the class of fact D4 exists to carry, found by authoring the rule rather than by
anyone hitting the bug. `PROTOCOL-NSL-RECORDS.md` §4.4's `sys/latency` row now carries the
catalogue half of it.

### What the gate now checks, and what witnessed it red

`CheckCompleteness(manifest.Table, *Descriptors)` fails on: a wire field with no descriptor,
a descriptor for a field not on the wire, a transposed pair, a manifest path no descriptor
claims, a descriptor claiming a path the manifest lacks, an orphan rule, and a descriptor
naming the wrong record tag (checked against the manifest's pinned encoded head). The
loader separately refuses an unknown `unit`/`kind`/`status`, an empty `meaning`, a duplicate
field, two shapes claiming one path, and — via `KnownFields` — a misspelt attribute.

**Green on the first run is the shape of a false green**, so the red was witnessed before the
green was believed: eight in-memory drift cases as permanent negative controls, plus one
end-to-end run with a field genuinely added to `Health` in `MANIFEST.cddl` (caught by name,
manifest then restored to its deployed md5).

One property was added that the design did not ask for. The manifest's header *asserts* key
order == wire order; `TestDescriptorOrderIsWireOrder` re-derives the bytewise sort from
`cval.Encode` and checks the authored order against it. Without it, a mis-authored CDDL hands
the completeness gate a wrong order and is agreed with — two documents consistent with each
other and both wrong, which is the failure mode this whole rung exists to close.

## Addendum 2026-08-27 — three corrections from building the pipeline (§3, §4.2)

Task 4 landed at `9fd84a2` (`porta/internal/gate/pipeline.go`). §3's loop is as designed and
needed no change. Three things the design got wrong or left open, found by building it:

### C1 — the decoded tables' identity prefix is uniform, and it is `src_path` not `path`

§4.2 lists a different identity prefix per table (`tel_log` has no `boot_id`/`seq`,
`obs_build` no `seq`). That variation is shorthand rather than design: one prefix for every
decoded table is the smaller mechanism, and the descriptor table is already the authority for
the rest of the columns. So every decoded table is
`(node_id, src_path, ts_ms, boot_id, seq)` + the record's fields in wire order.

**`src_path`, not `path`, because `Overrun.path` is a real wire field** meaning *the path whose
entries were dropped* — not the path the row was stored at. A natural `path` column would
silently mean two things in one table. `checkIdentityCollisions` closes the CLASS rather than
this instance: any future descriptor field colliding with an identity column fails at schema
build, naming it. Proven red against a doctored table.

### C2 — a decoded table's PROVENANCE changes what its rows mean, so it rides out with them

§2.4 splits the bindings by which structural tables they expose and does not say what that
does to the decoded ones. It is load-bearing: `snap` offers `events`, so a decoded table is the
**full history**; `live` does not, so it is **latest-value-per-path and nothing more**. Same
table name, same columns, different meaning. `Result.Sources[].From` carries `events` or
`cells` out with every answer, and both halves are tested so the difference is measured rather
than being one binding's quirk.

### C3 — `hard_heap_limit` is deferred, and here is the reason

§2.3 lists it beside `SetLimit`. It cannot be done as specified: `sqlite3_hard_heap_limit64`
is **process-global**, and go-sqlite3 does not expose it at all. Scoping it to Q is not
possible, and setting it process-wide would silently bound porta's own store the day `gate` is
linked in-process rather than run as `gatekeeper`. The other three guards land as designed
(wall-clock deadline through context -> `sqlite3_interrupt`, tested against an unbounded
recursive CTE; a row cap that sets `Truncated` rather than returning a silent prefix; a scratch
byte cap checked after each table). What would earn it back: a measured OOM under a real query.

### One thing added beyond the design

`Result.Sources` carries **rows and elapsed per materialized table**, so §7's "materialize the
whole referenced slice and measure it" needs no new code and pushdown stays unearned until a
number earns it. It also carries each decoded record's `status`, so B2's eight producerless
shapes explain their own empty tables — silence at a path is otherwise indistinguishable from
a dead node.
