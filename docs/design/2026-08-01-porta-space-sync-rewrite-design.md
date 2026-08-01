# porta → channels: the space-sync rewrite (v1 deleted)

**Status: design record, approved by David 2026-08-01.** This is the decision record for
porta#25 — the total do-over that scraps the command model. It supersedes the *porta half*
of `../PROTOCOL-NSL.md` §5 and amends its §3/§5.1 where the two contradict each other; the
node-facing conformance semantics in §§1–4 of that document stand.

**No backward compatibility.** The v1 command protocol, the TFTP resource surface and the
`toit`/`st` node kinds are deleted, not deprecated. The last v1 tree is tagged
`porta-v1-final` and stays inspectable in git history.

**Companions:** `../PROTOCOL-NSL.md` (the ratified model), `../PROTOCOL-NSL-RECORDS.md`
(per-prefix schemas + the size law), `~/workspace/tuvm/review/program-sequence.md` (the
ladder), porta#23 / porta#24 / porta#25.

---

## 1. Why now, and what the reversal means

porta#25 set the working order: **lay out what is best on the node, then fix porta to
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

### D6 — Kind is a property of the leaf, set by the first write.

Dropping `PROTOCOL-NSL.md` §3's "fixed by its path prefix" rule, per porta#25 item 3. The
node has enforced leaf-kind since rung 9 (`put` makes a cell, `post` makes a stream, the
wrong verb raises), and the prefix rule is already contradicted by the node's own `dev/**`
family — `dev/thermal0/sample` is a cell and `dev/thermal0/tick` is a stream under one
prefix. porta stores the kind per path as reported by the writer and does not infer it.

### D7 — Demolition precedes construction.

v1 is deleted in one commit before the new store is written, so no design decision is made
with v1 still in the room. The tree is briefly near-inert; `porta-v1-final` is tagged first.

---

## 3. Architecture

Three live packages replace nine.

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

Four tables replace seven. All timestamps are **milliseconds** (D4.2); all paths are
**node-relative** (D4.5); all values are **raw deterministic CBOR** (D4.1).

```sql
CREATE TABLE nodes (
  id            TEXT PRIMARY KEY,
  name          TEXT,
  first_seen_ms INTEGER,
  last_seen_ms  INTEGER,
  cadence_ms    INTEGER,   -- liveness floor; offline = k × cadence, k = 3 (v1's constant, kept)
  boot_id       INTEGER,   -- current session (RECORDS §2.4)
  cursor_acked  INTEGER,   -- how far the node has consumed porta's outbound seq
  node_seq_hw   INTEGER    -- high-water of the node's push sequence, for dedup
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

## 8. Known hazards

- **The `goal/do/*` replay hazard.** "Cold boot = cursor 0 = full replay" is safe for cells
  and dangerous for streams: replaying a cell is idempotent by construction, replaying
  `goal/do/reboot` reboots the node again, and a node that cold-boots into a reboot loop is
  bricked in the field. Replay-from-0 must mean *latest value per cell path*, and for `do/`
  streams only entries written after the node's current `bootId`. Adjacent to porta#24 and
  not previously written down. **To be filed as an issue, not built in this slice.**
- **porta#24 — cursor vs retention.** A node whose cursor falls behind porta's retention
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
`~/workspace/tuvm/review/program-sequence.md` and `~/workspace/porta/docs/PROTOCOL-NSL.md`,
which this record amends at D3, D5 and D6.
