# porta → channels: the space-sync rewrite (v1 deleted)

**Status: design record, approved by David 2026-08-01.** This is the decision record for
#120 — the total do-over that scraps the command model. It supersedes the *porta half*
of `../PROTOCOL-NSL.md` §5 and amends its §3/§5.1 where the two contradict each other; the
node-facing conformance semantics in §§1–4 of that document stand.

**No backward compatibility.** The v1 command protocol, the TFTP resource surface and the
`toit`/`st` node kinds are deleted, not deprecated. The last v1 tree is tagged
`porta-v1-final` and stays inspectable in git history.

**Companions:** `../PROTOCOL-NSL.md` (the ratified model), `../PROTOCOL-NSL-RECORDS.md`
(per-prefix schemas + the size law), `~/workspace/tuvm/review/program-sequence.md` (the
ladder), #122 / #121 / #120.

---

## 1. Why now, and what the reversal means

#120 set the working order: **lay out what is best on the node, then fix porta to
match — never the reverse.** The node side is already there. `space.c` has carried cells
and streams since tuvm rung 9 (tuvm#32 option A), the CBOR codec has shipped since rung 3,
and the socket seam is proven on a real OpenThread mesh (`ThreadPingNode.nsl` → gw85224-01).
What is missing on the node is replication — a `seq`, a cursor, a `changedSince:` query and
a bridge job — and what is missing on porta is everything downstream of that.

porta today is a TFTP/JSON gateway with a drained command queue. It has been running on
gw85224-01 for 36 days and no node has checked in since 2026-07-12. Nothing is being
carried by keeping it.

The one rule inherited unchanged from the old protocol: **diagnostics stay out of the
`goal/`/`obs/` convergence tree**, so fleet anti-entropy never walks them.

---

## 2. Decisions

### D1 — Transport is plain UDP + CBOR. CoAP is filed against the bulk plane.

TFTP was chosen originally because Thread fragments, and that reasoning was right about
*images* and wrong about the control plane.

6LoWPAN does not cap a UDP datagram at ~102 B; it *fragments* above that, up to the 1280 B
IPv6 MTU. The cost is reliability, not capability, and RECORDS §2.1 already measured ours
on the bench fleet at p = 0.05 on the weakest node (LQI 2):

| payload | ≈ fragments | delivery |
|---|---|---|
| 32 B | 1 | 95% |
| 512 B | ~6 | 74% |
| 1024 B | ~12 | 54% |

**CoAP does not dodge this** — it rides the same UDP, and a 512 B CoAP payload fragments
into the same six 6LoWPAN fragments. Blockwise only helps if the block is set *below* the
fragment threshold, and then a full round trip is paid per 64 bytes.

The control plane does not need it. The ratified caps are 256 B/value and 512 B/batch
(RECORDS §2); anything larger is a `BlobRef`. That fits one datagram.

Nor does the control plane need CoAP's CON/ACK/retransmit, because **the exchange is
idempotent by cursor**: a lost pull response means the next check-in re-pulls the same
range; a lost push is resent, and the gateway dedupes on the `nodeSeq` high-water. Loss is
absorbed by convergence, not retransmission — the etcd model, which is why §5.3 shaped the
exchange this way.

CoAP's genuine earn is **blockwise for the bulk/OTA plane** (the real TFTP replacement) and
**DTLS for G5**, gated on the RAM measurement in `PROTOCOL-NSL.md` §7.7. Neither is in this
rung, and the framing does not move when they land.

### D2 — Verbs live only at the CLI. The store and the wire have none.

A command queue is *drained* — v1 stamped `command_queue.delivered_at` and the row's job
was over. That is the opposite of replayable.

The append-only `events` table is strictly stronger, and notably keeps **superseded
intent**: set `goal/mode` to X then Y before the node checks in, and v1's queue shows two
commands with one delivered, a cells-only store shows just Y, and the event log shows both
writes *and* the fact that the node's cursor never crossed X. Delivery is derivable and
better sourced: "the node's cursor passed seq N" is evidence from the node, where
`delivered_at` was porta's own guess.

The operator vocabulary survives as **client-side sugar**, exactly as `PROTOCOL-NSL.md`
§5.2 has it: `porta app run <node> <name>` compiles to a `goal/apps/<name>` write. That is
the right altitude — it reads well and it cannot leak shape into the store or the radio.

### D3 — Provenance is a column, not a mechanism. (Amends §5.1.)

`PROTOCOL-NSL.md` §5.1 specifies `space_cells(node_id, path, seq, value_cbor, ts)` with
**no actor column**, silently dropping what v1's `command_queue.issued_by` and `porta log`
carried. That is a genuine regression and it needs no verbs to fix — `events` gains `actor`
and `reason`.

`actor` on an outbound row is who wrote it (an operator, an agent, the CLI); on an inbound
row it is the node. `porta log` survives as *the write log for this node* rather than *the
command log*, and it answers a question v1 could not: **what was the goal tree at seq N** —
a fold over the log.

### D4 — Five constraints that keep session replay cheap later.

Replaying a captured session against a node (test / forensics) is **not in this rung**, and
it is not to be foreclosed. It is a reader over the same log plus a driver, provided:

1. **The CBOR bytes are the source of truth**, never a decoded projection. RECORDS §1
   guarantees deterministic encoding, so a captured session replays byte-exact and a golden
   fixture pins by bytes. Decode only for display.
2. **Millisecond timestamps.** v1 is epoch-seconds throughout (`DefaultPollIntervalS = 30`).
   Replay needs inter-arrival timing; second granularity cannot reconstruct a control loop.
   Cheap now, impossible to backfill.
3. **The exchange itself is logged**, not only the writes — one row per check-in. This is
   *also* the heartbeat record, so it is not extra machinery, and without it the batch
   boundary a value arrived on is unrecoverable.
4. **`(bootId, monoMs)` is the session key.** RECORDS §2.4 already defines it and rides
   `bootId` on the batch envelope. It makes "a captured session" one node run, rather than
   a time range someone eyeballs.
5. **Paths are stored node-relative** — `goal/mode` in the path column with `node_id`
   beside it, never `node/<id>/goal/mode` baked in. The `node/<id>/**` prefix in §3's
   picture is a *view*. This is what lets a captured session be re-pointed at another node.

The driver has a home already: the **G10 host fleet-sim** (N host VMs + porta over
localhost UDP) is in the ladder as this protocol's first proving ground, and a replay
driver is that sim with a recorded script instead of live nodes.

### D5 — One monotonic seq per space per side. (Resolves a §3/§5.1 contradiction.)

`PROTOCOL-NSL.md` §3 calls for "a monotonic per-path `seq`"; §5.1 says "monotonic per-node
`seq` assigned on write". These are not the same protocol, and a **scalar cursor forces the
resolution**: per-path seq would make a cursor a vector over every path.

Ratified here: **one monotonic counter per space (per side)**, stamped on every write; each
path additionally records the seq of its last write. This is exactly etcd's
`revision` / `mod_revision` split, and `changedSince: cursor` is a scan for paths whose
last-write seq exceeds the cursor.

> **Amended 2026-08-05, by hardware.** The counter is monotonic *within a boot* and starts
> at 0 on the next one — the node holds it in RAM (NVS is G7 work), which is stated in
> PROTOCOL-NSL but was never carried through to what porta does with it. porta's
> `nodes.node_seq_hw` persists, so a rebooted node pushed seq 1..n against a stored
> high-water of thousands and `Ingest` skipped every row as already-applied. Worse than
> a lost batch: the ack carries that high-water and the node's `applyAck:` adopts it, after
> which the node believes everything it writes is accepted and porta holds observations
> hours old, with no error at either end. The C6 gate's db wipe had been resetting it every
> run, which is why three days of green said nothing about it.
>
> **`node_seq_hw` is a CACHE of the node's own `acked`, and a RESTART invalidates it.**
> The pull frame already carried `hw` — what the node believes porta has accepted — and
> `handlePull` decoded the frame without ever reading that field. porta now rewinds to the
> declared `hw` when the node declares **both** a lower cursor and a lower `hw`
> (`Store.NoteCheckIn`), because both are RAM state that only advances within a boot.
>
> **The first version tested `hw` alone and was wrong.** A missed ack produces the same
> lower `hw`, and rewinding there deletes the `seq <= hw` dedupe — porta had committed
> those rows and the node was legitimately re-offering them, so the rewind manufactures
> exactly the duplicate the dedupe exists to prevent. `porta_sync_c6.sh` caught it inside
> one leg: "the gateway never logged a duplicate-skip", where it had logged 959. The
> cursor separates the two cases structurally rather than by heuristic — it is advanced by
> the batch and the watermark by the ack, so a swallowed ack cannot move it.
>
> Known gap, named: a node porta has never written to has a cursor of 0 on both sides and
> its restart carries no signal here. Closing that needs the per-boot identity `bootId` was
> reserved for (#74). The two mechanisms do not conflict, and this one makes the other
> unnecessary for every node that has ever been given a goal.

### D6 — Kind is a property of the leaf, set by the first write.

Dropping `PROTOCOL-NSL.md` §3's "fixed by its path prefix" rule, per #120 item 3. The
node has enforced leaf-kind since rung 9 (`put` makes a cell, `post` makes a stream, the
wrong verb raises), and the prefix rule is already contradicted by the node's own `dev/**`
family — `dev/thermal0/sample` is a cell and `dev/thermal0/tick` is a stream under one
prefix. porta stores the kind per path as reported by the writer and does not infer it.

### D7 — Demolition precedes construction.

v1 is deleted in one commit before the new store is written, so no design decision is made
with v1 still in the room. The tree is briefly near-inert; `porta-v1-final` is tagged first.

### D8 — `obs/build` is pinned in this rung, not deferred.

v1's `nodes.chip` and `nodes.sdk` columns have **no successor path** — the pinned catalogue
in RECORDS §4.2 stopped at `obs/health`. Deleting them with nothing behind them takes fleet
build provenance from *partial* to *absent*, and that is tuvm#74, which the tracker labels a
**revenue-gate**: an agent cannot reason about a fleet when it cannot tell which code is on
which node.

`obs/build` is therefore pinned now (RECORDS §4.2): `Build { chip, sdk, image, builtAt }`.
The porta side is free — it is another cell. The node answers `chip` and `sdk` today and
omits `image`/`builtAt` until tuvm#74 lands; **absence means "the node cannot say"**, which
is the honest value and not a default.

The general rule this is an instance of: **a rewrite may delete a mechanism, but it may not
silently delete a capability.** Anything v1 could answer that the new model cannot is a
regression wearing the costume of a simplification.

---

## 3. Architecture

Six packages are deleted outright, two are new, five are rewritten in place, and four
survive untouched.

| package | fate |
|---|---|
| `internal/space` | **new** — the store (§4) |
| `internal/wire` | **new** — UDP listener + CBOR framing (§5) |
| `internal/portacli` | rewritten — verbs compile to path writes / tree reads |
| `internal/apisrv` | rewritten — HTTP surface over paths |
| `internal/web` | rewritten — the goal/obs tree diff *is* the convergence view |
| `internal/mcpsrv` | rewritten — the typed path surface (see §7) |
| `internal/httpsrv`, `internal/serverstat`, `cmd/porta`, `deploy/` | unchanged |
| `internal/command` | **deleted** — verb codec, crc32, duration |
| `internal/config` | **deleted** — desired-config reconcile; convergence is a tree diff |
| `internal/control` | **deleted** — lifecycle/profile_status/view |
| `internal/handler` | **deleted** — the TFTP resource surface |
| `internal/tftp` | **deleted** |
| `internal/telemetry` | **deleted** — telemetry is stream entries |
| `devsdk/apiclient` | rewritten against the new HTTP surface |

Node kinds `toit` and `st` are deleted with them.

---

## 4. The store

Four tables replace nine. All timestamps are **milliseconds** (D4.2); all paths are
**node-relative** (D4.5); all values are **raw deterministic CBOR** (D4.1).

### 4.1 Where each v1 table goes — the honest accounting

| v1 table | fate |
|---|---|
| `nodes` (14 cols) | kept, **thinner** — six of its columns become `obs/**` cells |
| `command_queue` | → `events` `direction='out'` |
| `reports` | → `cells` under `obs/**` |
| `data_log` | → `events` `direction='in'` under `tel/**` |
| `debug_request` | → `events` at `dbg/req` |
| `debug_response` | → `events` at `dbg/resp` |
| `profile_session` | → `cells` at `goal/profile/<app>` |
| `profile_result` | **split** — profiler blobs are bulk (§7, RECORDS §2.4); the bounded span results land at `diag/spans/**` (RECORDS §4.6) |
| `payloads` | **deferred, not absorbed** — see below |
| — | `exchanges` is **new** (D4.3) |

**The collapse has a structural cause, and it is worth naming.** Five of those tables —
`command_queue`, `reports`, `data_log`, `debug_request`, `debug_response` — are the *same
two shapes*: a queue with a delivered marker, or an append log keyed `(device, seq)`. That
is `PROTOCOL-NSL.md` §2's multiplicative-extension diagnosis showing up in the schema rather
than in prose. Once the unit of extension is a path, a new feature adds **rows, not tables**.
`debug_request`/`debug_response` are the cleanest proof: two structurally identical tables
whose only difference was direction — now a column.

**Three deductions from the headline number**, so it is not oversold:

1. **`payloads` is deferral, not absorption.** The size law keeps bulk off the space by
   design, so image blobs need a home the moment OTA returns — content-addressed by hash
   rather than v1's CRC. Honestly: 9 → 4 now, 9 → 5 when bulk lands.
2. **`exchanges` is a table v1 never had.** It buys liveness and replay indexing (D4.3). A
   real cost, deliberately paid.
3. **`nodes.chip` / `nodes.sdk` had no successor** — closed by D8, which pins `obs/build`
   rather than letting a revenue-gate regress inside a rewrite. Walking all fourteen
   columns for §4.2's audit found **two more** in the same condition, `source_addr` and
   `last_reset_code`; both are closed there. Three of fourteen columns had no successor
   until someone enumerated them, which is the case for doing the audit at all.

```sql
CREATE TABLE nodes (
  id            TEXT PRIMARY KEY,
  name          TEXT,
  first_seen_ms INTEGER,
  last_seen_ms  INTEGER,
  cadence_ms    INTEGER,   -- liveness floor; offline = k × cadence, k = 3 (v1's constant, kept)
  boot_id       INTEGER,   -- current session (RECORDS §2.4)
  cursor_acked  INTEGER,   -- how far the node has consumed porta's outbound seq
  node_seq_hw   INTEGER,   -- high-water of the node's push sequence, for dedup
  last_addr     TEXT       -- peer address of the last datagram; successor to v1's
                           -- source_addr, which serverstat.TransportOf() reads to
                           -- produce the wifi-vs-Thread breakdown (§4.2)
);

CREATE TABLE cells (          -- latest value per path
  node_id    TEXT, path TEXT, seq INTEGER, value_cbor BLOB, kind INTEGER, ts_ms INTEGER,
  PRIMARY KEY (node_id, path)
);

CREATE TABLE events (         -- append-only, both directions, with provenance (D2/D3)
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  node_id TEXT, path TEXT, seq INTEGER, value_cbor BLOB, kind INTEGER,
  ts_ms INTEGER, direction TEXT,   -- 'in' (node→porta) | 'out' (porta→node)
  actor TEXT, reason TEXT
);

CREATE TABLE exchanges (      -- one row per check-in; the heartbeat AND the replay index (D4.3)
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  node_id TEXT, ts_ms INTEGER, boot_id INTEGER,
  cursor_in INTEGER, cursor_out INTEGER, pushed_lo INTEGER, pushed_hi INTEGER
);
```

There is **no tombstone column** — `PROTOCOL-NSL.md` §3's rule holds: stopped state is
written explicitly as a value, and genuine removal is an action on a `do/` stream.

Convergence is structural: `goal/X` vs `obs/X` is a tree diff, and the operator's
desired-vs-observed view and the self-heal loop are the same query. Self-heal as a separate
mechanism disappears.

### 4.2 Capability audit — every v1 operator surface, and where it lands

§4.1 accounts for the *tables*. This section accounts for the *capabilities*, under D8's
rule: **a rewrite may delete a mechanism, but it may not silently delete a capability.**
Produced mechanically on 2026-08-01 from `porta --help` over every subcommand, the mux
registrations in `apisrv`/`web`/`mcpsrv`/`httpsrv`, and the v1 `nodes` DDL — not from
recall. Fates are `absorbed → <path>`, `deferred → <plan>`, or **regression**.

> A note on method, because it cost a correction: the enumeration grep first written into
> the plan (`grep -rhoE '"/(api|v1)/…"' internal/apisrv internal/web`) returns **test
> fixtures, not routes** — v1 registers `"GET /api/nodes/{sel}"`, so the method prefix puts
> the path off the front of the string literal and the pattern cannot match it. It reported
> twelve paths, of which every real one was absent and several were `blinky`/`ghost` test
> doubles. The routes below come from the `HandleFunc` registrations.

**CLI verbs** (9 root commands, 15 subcommands):

| v1 capability | fate |
|---|---|
| `scan [--include-never-seen]` | deferred → operator surface. Data survives: `nodes.last_seen_ms` + `cadence_ms` (offline = k × cadence, k = 3), and a node staged but never seen still gets a row via `ensureNode` |
| `ping -d` | deferred → operator surface; same liveness source, now corroborated by `exchanges` |
| `device show` | deferred → operator surface; `nodes` + `Tree(id,"obs/")` — **but see the two regressions below** |
| `device get [app] [key]` | **absorbed → structural.** `Tree(id,"goal/")` vs `Tree(id,"obs/")` is the diff; `porta get` in plan 1 |
| `device set` | absorbed → `Write` to `goal/config/<app>/<key>` (RECORDS §4.1); `porta put` in plan 1 |
| `device reboot` | absorbed → stream write `goal/do/reboot`. Carries §8's replay hazard — that is why the hazard is filed |
| `device set-forward` (absolute, all streams) | absorbed → `goal/forward`, pinned as `Forward{…}` — *"the whole policy, never a patch"*, which preserves the absoluteness exactly |
| `container list` | absorbed → `Tree(id,"obs/apps/")`, `AppObs{…}` |
| `container install` | **deferred → bulk/OTA plane (§7).** The image ride is TFTP, deleted here; CoAP blockwise replaces it, content-addressed by hash rather than v1's CRC32. `goal/apps/<name>` already carries the `BlobRef` |
| `container uninstall` | absorbed → `goal/apps/<name>` with `lifecycle` written to stopped (no tombstone, §4.1) |
| `log -d` | **absorbed → `Events`, and widened.** Gains `actor`/`reason` provenance (D3) and superseded intent, which a drained queue could not offer |
| `monitor -d [--kind --since --follow]` | absorbed → `tel/**` events; the three filters are deferred → operator surface. `--kind` is a path prefix (`tel/log`, `tel/m/`, `sys/panic`), `--since` is `ts_ms`, `--follow` is a tail over `events.id` |
| `debug attach/detach/send/poll` | absorbed → `goal/debug/<app>` (`Debug{action}`) + `dbg/req` / `dbg/resp` events; `--after id` maps to `events.id` |
| `profile start/stop` | absorbed → `goal/profile/<app>`, `Profile{action,durationS,continuous}` |
| `profile poll` | absorbed → `diag/spans/<span>` cells + the `diag/spans/result` summary (RECORDS §4.6); the blob half is bulk |
| `profile get <seq>` | **deferred → bulk/OTA plane (§7).** The blob is opaque and unbounded; it cannot ride a 256 B value |
| `serve` | absorbed → Task 1 (HTTP `/health`) + Task 4 (UDP space listener) |
| `completion`, `--db`, `--server`, `-v` | unchanged (cobra/global) |

**HTTP surface** (13 routes registered, plus 9 web routes):

| v1 route | fate |
|---|---|
| `GET /health` | kept — survives behind the narrow `NodeCounter` interface (Task 1 step 4) |
| `GET /api/status` | deferred → operator surface. **Depends on a regression** — see `source_addr` below |
| `GET /api/nodes`, `GET /api/nodes/{sel}` | deferred → operator surface |
| `POST /api/nodes/{sel}/commands` | absorbed → a path write |
| `GET /api/nodes/{sel}/commands` | absorbed → `Events` where `direction='out'` |
| `POST /api/nodes/{sel}/containers` | deferred → bulk plane (§7) |
| `GET /api/nodes/{sel}/config` | absorbed → the `goal/` vs `obs/` tree diff |
| `GET /api/nodes/{sel}/telemetry` | absorbed → `Events` under `tel/**` |
| `POST /api/nodes/{sel}/debug/send`, `GET …/debug/responses` | absorbed → `dbg/req` / `dbg/resp` |
| `GET /api/nodes/{sel}/profile` | absorbed → `Tree(id, "diag/spans/")` (RECORDS §4.6) |
| `GET /api/nodes/{sel}/profile/{seq}` | deferred → bulk plane (§7) |
| web `/`, `/n/`, `/log`, `/status`, `/telemetry`, `/partials/{status,telemetry,nodes,log}`, `/assets/` | deferred → operator surface. §7's warning governs: the web UI must be a *client* of the path surface, not the thing the path surface is shaped around |
| MCP `list_devices`, `device_status`, `device_get_config`, `container_list`, `query_telemetry`, `command_log` | deferred → §7 names this explicitly; `mcpsrv` is rewritten as a client of the same path surface |
| TFTP resource surface (`internal/handler`, port 6969) | deferred → bulk plane (§7) |
| v1 UDP report/poll protocol | **mechanism replaced** by §5's pull/push exchange — a mechanism, not a capability |

#### Two rows land on regression

D8 already caught `nodes.chip`/`nodes.sdk` and pinned `obs/build`. Walking all fourteen
`nodes` columns rather than the six §4.1 counted turns up two more with no successor. Both
are live: traced to their call sites, not assumed.

| v1 column | reached by | fate |
|---|---|---|
| `nodes.source_addr` | `porta device show` (`inspect.go:135`), `GET /api/nodes{,/{sel}}`, web node list, MCP `list_devices` + `device_status`, **and `serverstat.TransportOf(n.SourceAddr)`** which produces the wifi-vs-Thread breakdown on `/api/status` and `/status` | **regression → CLOSED in this plan.** `last_addr TEXT` on `nodes` (§4.1 DDL), stamped by the UDP loop from the datagram's peer address. One column; the wire layer already holds the value |
| `nodes.last_reset_code` | `porta device show` via `control.RenderReset(reason, code)` (`inspect.go:143`), `GET /api/nodes/{sel}` as `reset_code`, web node page | **regression (narrowing) → CLOSED in this plan.** RECORDS §4.4's `Reset` gains `code: int`. On ESP32 the numeric code is the machine-readable form and the reason string is derived from it, so a symbol alone is lossy |

Neither was a revenue-gate on tuvm#74's scale, and neither was expensive — one column and
one record field, both folded in before demolition rather than filed. They are recorded
here because the rule is that a capability gets a *fate*, and "nobody noticed" is not one.

**One conflated path prefix, resolved in opposite directions.** §4.1 originally routed both
remote debug and profiler spans through a single `diag/` prefix: `diag/dbg/req` /
`diag/dbg/resp` and `diag/**`. Those are two different questions and they resolve
differently.

- **Remote debug → `dbg/req` / `dbg/resp`.** RECORDS §4.5 pins the prefix as `dbg/*`, and
  `PROTOCOL-NSL.md` §3's authority model is written in those terms — porta may write
  *"only `goal/**` and `dbg/req`"*. Nesting it under `diag/` was a slip. Nothing implements
  `dbg/` on the node yet (v1's debugger was JSON over TFTP), so the pinned contract stands
  unopposed.
- **Profiler spans → `diag/**` was RIGHT.** The first correction of this section claimed
  `diag/` did not exist, on the evidence that it appears in neither protocol document. That
  was checking the wrong artefact. `diag/` is **live in shipped node code**:
  `nsl-tuvm/lib/kernel.nsl` binds `diag/spans/ctl` and `diag/spans/result` in
  `class SpanCapture` (tuvm#72), and those are two of only three channel paths the node
  uses at all. The namespace was undocumented, not invented — the catalogue was incomplete.
  It is now written down as **RECORDS §4.6**.

The lesson is worth keeping with the record: *the protocol documents are not the system.*
A namespace absent from both of them was nonetheless already shipping, and one grep of the
node source would have said so.

---

## 5. The wire

Two node-initiated CBOR datagrams, both idempotent, 512 B per batch enforced at the bridge.

```
PULL  { id, cursor, bootId, nodeSeqHW }
  →  BATCH { hi, entries: [ [seq, path, kind, value], … ] }        -- empty ⇒ up to date

PUSH  { id, bootId, entries: [ [nodeSeq, path, kind, value], … ] }
  →  ACK { nodeSeqHW, cadenceHint? }
```

`cadenceHint` is the optional back-pressure hint of `PROTOCOL-NSL.md` §4.4 — advisory, the
node's policy may ignore it, and porta stays passive. Emitting it is out of scope here; the
field is reserved so the shape does not move.

Framing is transport-independent by construction (§5.3), which is what makes D1's deferral
of CoAP free.

---

## 6. The node side

Landing in `~/workspace/tuvm` and `~/workspace/nsl-tuvm`, conforming to this document.

- **`space.c`** gains a per-space monotonic counter and a per-channel `last_seq`, stamped
  on every write (D5), plus one prim backing `changedSince:` — a scan for channels whose
  `last_seq` exceeds a cursor. This is additive to the rung-9 cell/stream storage; no
  existing verb changes.
- **`PortaBridge`** — an ordinary supervised nsl job holding the socket capability, write
  access to `goal/**`, and read access to `obs/**` / `tel/**` / `sys/**`. A crash of the
  bridge is a contained fault: porta sees a late check-in and nothing worse.
- **`PortaBridgeNode.nsl`** — a fresh minimal node for the gate, *not* the shipsBells rig,
  which is entangled with the DS3231, the strike driver and #105's boot-energise defect.
- **The cursor lives in RAM for this slice.** Cold boot = cursor 0 = full replay is the
  *designed* behaviour (`PROTOCOL-NSL.md` §3), so NVS persistence is a later optimisation
  and not a prerequisite. The flash-wear question stays open (§7.1 of that document).

---

## 7. Deliberately deferred

Named so they are not silently dropped:

- **Bulk / OTA plane.** Images and profiler blobs stay off the space (§4.3). CoAP blockwise
  replaces TFTP here, gated on §7.7's DTLS RAM measurement.
- **Auth (G5).** The grant machinery cannot ride the thing it guards. Out of this rung.
- **Session replay driver.** D4 pays its bill; the driver rides the G10 fleet-sim.
- **MCP as a typed tool surface.** `docs/next-steps-overview.md` names this as the likeliest
  thing to get built the wrong way round, because the UI is what demos. The rewrite of
  `mcpsrv` should be a *client* of the path surface, and the web UI a client of the same —
  not the other way round.
- **Node identity.** A constant in the node's source for this slice; hardware-derived
  identity is a follow-on (relates tuvm#74, a node cannot name its own build).

### 7.1 Resolved — profiler spans land at `diag/spans/**` (https://github.com/davidg238/porta/issues/28, closed)

Filed as an open question, then answered by reading the node instead of the documents. The
node already ships the whole mechanism (tuvm#72, `class SpanCapture`): a bounded capture
session controlled by the cell `diag/spans/ctl` (`SpanCtl { state: sym }`), reporting to
`diag/spans/result`. Nothing needed designing; it needed cataloguing, and it is now
**RECORDS §4.6**.

The candidate ranking in the original filing was wrong on its leading option. "It probably
merges into `sys/latency`" was a guess from schema shape; the node had already made the
call, and `diag/` is the right prefix for the reason §1 states — diagnostics stay out of the
`goal/`/`obs/` convergence tree, and a span capture has no `goal/` twin to converge against.

**Cataloguing it surfaced a real defect.** `SpanCapture>>reportMs:` nests every slot's
`LatencySpan` in one value: 8 slots (`OEVM_SPAN_SLOTS`) × 140 B measured = **~1120 B**
against §2.2's **256 B** value cap — 4.4× over, and 2.2× over the 512 B batch cap. §2.2
calls that a programming error. It is latent rather than live, because the capture is read
by the node's own `/stats` page and has never been synced. RECORDS §4.6 therefore pins the
per-slot rows as **sibling cells** at `diag/spans/<span>` with `diag/spans/result` carrying
only `{n, ms}`, and the node change is **tuvm#107**. Until that lands,
`diag/spans/result` must not be synced.

The general rule, now stated in RECORDS §4.6: **a collection whose size scales with a count
belongs in the path space, not in a list inside one value.**

## 8. Known hazards

- **The `goal/do/*` replay hazard.** "Cold boot = cursor 0 = full replay" is safe for cells
  and dangerous for streams: replaying a cell is idempotent by construction, replaying
  `goal/do/reboot` reboots the node again, and a node that cold-boots into a reboot loop is
  bricked in the field. Replay-from-0 must mean *latest value per cell path*, and for `do/`
  streams only entries written after the node's current `bootId`. Adjacent to #121 and
  not previously written down. **To be filed as an issue, not built in this slice.**
- **#121 — cursor vs retention.** A node whose cursor falls behind porta's retention
  point is silently stranded (etcd's compaction scar). Still unanswered; the `events` table
  is append-only with no compaction in this rung, which defers rather than solves it.

---

## 9. The gate

Witnessed on the C6 over a real Thread mesh, not simulated:

1. `goal/name` written at porta appears in the node's space.
2. `obs/health` pushed by the node appears in porta's cell tree.
3. `tel/log` entries arrive **exactly once** across a deliberate datagram drop.

Host-side coverage lands with it (store, framing, cursor arithmetic); the hardware run is
what closes the rung. Per the promotion rule, close-out promotes the durable knowledge to
`~/workspace/tuvm/review/program-sequence.md` and `~/workspace/tuvm/protocol/PROTOCOL-NSL.md`,
which this record amends at D3, D5 and D6.
