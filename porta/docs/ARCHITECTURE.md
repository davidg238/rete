# porta system architecture

This is the **canonical system-architecture document** for porta and the fleet it
controls: what the pieces are, why the boundaries fall where they do, and which
properties hold across the whole system. The contracts it sits inside are
`../../protocol/PROTOCOL-NSL.md` (the wire), `../../protocol/MANIFEST.cddl` (the
machine-normative kind+schema catalogue) and `DEVSDK.md` (the northbound tooling
contract).

Where this document and the code or the contracts disagree, **the contracts win, then
the code; this document is the bug.** Rewritten for v2 on 2026-08-29 (#279) — porta v1's
command vocabulary, command queue, TFTP delivery and report schema were deleted in one
commit on 2026-08-01 and the last v1 tree is tagged `porta-v1-final`. Nothing below
describes them; §7 says where they went.

---

## 1. The system at a glance

```
                 DEV MACHINE   (source · nsl compiler · Zephyr SDK · USB)
        ┌──────────────────────────────────────────────────────────────┐
        │  ns_compile_oevm.py --format=oimg  →  app.oimg               │
        │  west build / flash / console                                │
        │  node-repo tooling imports porta/devsdk  (exec, provision)   │
        └────────────────────────────┬─────────────────────────────────┘
                                     │  app.oimg carried to the gateway host
                                     ▼
   agent ·  ── HTTP :6971 ──►  ┌───────────────┐
   browser                     │  CUSTOS   │  /dev docs + status page;
                               │  (own binary) │  proxies /health, /manifest
                               └───────┬───────┘
                                       │ 127.0.0.1
   operator ── porta CLI ────►  ┌──────▼──────────────────────────────────┐
   (opens the sqlite file       │              PORTA GATEWAY              │
    DIRECTLY, on this host —    │   `porta serve` — four loops:           │
    never a network client)     │     • exchange   udp :6969   pull/push  │
                                │     • blocks     udp :6970   image bytes│
                                │     • http       tcp :6970   /health    │
                                │     • sweep      every 30 s  → gw/**    │
                                │   store: nodes · cells · events ·       │
                                │          exchanges   (+ gw_runs · meta) │
                                │   blobs/<sha256> · porta.key · porta.pub│
                                └──────────────────┬──────────────────────┘
                                                   │  plain UDP carrying CBOR
        ┌────────────────────────┬─────────────────┴──────────┐ · · · · · · · ·┐
        ▼                        ▼                            ▼                ▼
  ┌────────────┐          ┌────────────┐              ┌────────────┐    ┌────────────┐
  │  ESP32-C6  │          │  ESP32-C6  │              │  nRF52840  │    │  posix     │
  │ oevm + nsl │          │ oevm + nsl │    ...       │ oevm + nsl │    │ host node  │
  │ WiFi· IPv4 │          │Thread·IPv6 │              │Thread·IPv6 │    │ loopback   │
  └────────────┘          └────────────┘              └────────────┘    └────────────┘
    └────── every node owns a SPACE — a tree of paths — and porta holds a mirror ─────┘
         conformance target: protocol/PROTOCOL-NSL.md §§1–4 + MANIFEST.cddl
```

Three planes, three contracts — and note that only one of them is a network protocol:

| Plane | Who talks | Contract | Transport |
|-------|-----------|----------|-----------|
| **Southbound** (gateway ↔ node) | porta ↔ any conforming node | `protocol/PROTOCOL-NSL.md` §§1–4 + `MANIFEST.cddl` | plain UDP + CBOR (the exchange); a binary block plane for bulk bytes |
| **Northbound** (dev tool → gateway) | node-repo tooling → porta | `porta/devsdk` (`exec`, `provision`) | a Go import. **No network path** — v1's `apiclient` went with v1's HTTP API |
| **Operator** (human/agent → gateway) | CLI → the store; browser/agent → custos | the path namespaces themselves; `GET /health`, `GET /manifest` | a local sqlite open (§3.1); HTTP for the two routes |

---

## 2. A heterogeneous fleet with one fixed point

porta serves a **heterogeneous** fleet: different chips (ESP32-C6 on WiFi and on
Thread, nRF52840 on Thread, a posix host node on loopback), different power profiles,
different jobs. They share **nothing** but the wire.

The one fixed point is `protocol/PROTOCOL-NSL.md`, whose §§1–4 are the conformance
target, with `MANIFEST.cddl` as the machine-readable half. A node is conforming if it
speaks the space model — the two idempotent node-initiated messages, the path
namespaces, the size law — and **any node MUST be implementable from those documents
alone, without reading another node's source.** That is why the gateway rather than any
node owns the contract: it outlives any single node codebase, and it has already
outlived one (v1's Toit nodes, §7).

Consequences of "coupled only over the wire":

- porta **never imports node code**; nodes **never import porta code**. Both halves now
  live in one repository (§8) and that has not changed: a change that makes one side
  compile against the other is a bug, not a convenience.
- **A second implementation must hand-roll its codec.** The algebra's map keys are
  themselves tagged values, a Record's *name* is a Symbol rather than a text string, and
  decoding must be closed. porta's `internal/cval` is that second implementation, and it
  is golden-tested against the node's own output — the node's `Cbor` class in
  `nodus/nsl/lib/kernel.nsl` is the authority, not the RFC.
- Adding a node kind is a wire-conformance exercise, not a gateway change. porta branches
  on no language and no chip; `obs/build` carries `chip`/`sdk` as data.

---

## 3. The role of porta — a neutral, always-on LAN gateway

porta has exactly two jobs: **(1) be always on, (2) hold the fleet's mirror.** It
implements **zero** language- or hardware-specific function. Everything that needs
source, a compiler, a chip toolchain or a USB cable lives off the gateway, on the dev
machine (§7).

What the gateway *is*:

- **The mirror.** Every node owns a tree of paths; porta holds a copy. Operators write
  intent under `goal/**`; the node writes what it sees under `obs/**`, `tel/**` and
  `sys/**`; porta writes its own verdicts under `gw/**`. **Convergence is not a
  mechanism** — `goal/X` against `obs/X` is a tree diff, so the desired-versus-observed
  view and the self-heal loop are the same query.
- **The only *network* writer.** `porta serve` is the single process that ingests from
  the wire, and it serializes every write and records it in an append-only log with
  `actor`/`reason` provenance. It is **not** the only writer at all: the CLI opens the
  same sqlite file directly, by design (§3.1), which is the one place a reader's
  intuition about "single writer" will mislead them.
- **The bulk courier.** Image bytes are content-addressed under `blobs/<sha256>` and
  served by a separate stateless listener (§3.2, §3.3). They are deliberately **off the
  space**: a 512 B batch and a 256 B value are radio constraints, not preferences.
- **The judge of silence.** A ticking sweep forms one liveness verdict per node against
  the budget the node itself declared (§3.5).
- **The operator surface.** Two HTTP routes, `GET /health` and `GET /manifest`, and
  nothing else. v1's dashboard, JSON API and MCP server are deferred → operator surface
  in the capability audit, not silently dropped; the exposed agent-facing front door is
  the separate `custos` binary.

What the gateway is **not**: a compiler, a flasher, a relocator, a panic decoder, a
fleet-wide image builder. Those are node-repo concerns (§7).

### 3.1 The CLI is a local sqlite writer, not a client (recorded 2026-08-07)

**`porta put`, `porta get` and `porta log` do not talk to `porta serve`.** Every verb opens
the sqlite file named by `--db` directly (`internal/portacli/root.go`), and `--db` defaults
to a **relative** `porta.db`. `space.Open` runs `db.Exec(schema)` on whatever path it is
handed, so sqlite *creates* the file: a `put` against the wrong path succeeds, exits 0, and
lands in an empty store nobody serves.

This is not a defect to fix — it is what makes the CLI usable against a captured session or
a copied store, and it follows from D2 (verbs live only at the CLI). But it has two
consequences that anything shelling out to the CLI must respect, and #165's profile-capture
button shipped violating both before it was caught:

1. **It must run on the host holding the served store**, because there is no network path.
2. **It must name the store explicitly.** A defaulted `--db` cannot fail loudly, since the
   default is always openable.

Recorded here because neither is inferable from the CLI's shape, and the failure it
produces is silent success — the hardest kind to find.

### 3.2 The blob store — content-addressed files beside the db (rete#25, recorded 2026-08-08)

Image bytes are **not** a sqlite table. `internal/blob` writes each blob to
`blobs/<hex-sha256>` in the directory holding the `--db` file, streamed to a temp file,
hashed as it copies, and renamed into place — so a blob that already exists is a success
by construction and a crashed upload leaves no half-file under a real name. The space
store's four tables stay four (the unit of extension is a path, not a table), and bulk
bytes stay off the space entirely (`protocol/PROTOCOL-NSL.md` §4.3, at the rete root).

Two files sit beside it: **`porta.key` and `porta.pub`**, one ed25519 keypair written
once by `porta key gen`, refusing to overwrite an existing key rather than rotating
implicitly. **`porta blob put <file.oimg>` stores the blob and signs its SHA-256 at push
time**, printing the hash, the signature, and the ready-made
`porta put <node> goal/apps/main --raw <cbor>` line that carries both. The private key
never leaves porta; the node's runner holds only the public half, and verification
happens there (§3.3). The key story is deliberately dumb — one keypair, two files, a
missing key is an error and never "unsigned".

### 3.3 The block server — exchange port + 1, stateless (rete#25, recorded 2026-08-08)

`porta serve` brings up a second UDP listener on **the exchange port + 1**, a convention
rather than a flag: a node that knows where to check in knows where to fetch. It answers
one fixed binary frame — request `0x01 ‖ hash[32] ‖ blockNo u32 BE`, response
`0x02 ‖ blockNo ‖ more ‖ data` with `data` ≤ 256 B, error `0x7F ‖ text` — as a pure
`Handle([]byte) []byte`, mirroring the `wire` package's UDP loop shape (a copy of the
idiom, not a reuse of the code: the two planes share no type).

**One coincidence worth naming, because it reads like a conflict and is not.** At the
defaults the operator HTTP listener is `:6970` **TCP** and the block plane is `:6970`
**UDP** — the same number, two protocols, two sockets, no collision. `--udp` and
`--http-port` move independently, so the pairing is a default and not a constraint.

It holds **no state per client**, which is the property that matters: resume is the node
re-asking from any block number, so a transfer killed halfway costs only the blocks it
had not got. The shape is a CoAP blockwise GET's, so the ratified TFTP → CoAP direction
costs a reframing and not a redesign. Binary rather than CBOR because the fetching half
is the node's C seam, which cannot reach the nsl `Cbor` class — the full argument, plus
the node-side runner's manifest/marker contract (`manifest.json`, A/B slots,
`stage/ready.json`, the confirm marker, exit 42, and the trial watchdog that reverts a
node which never checks in), is in the rete root's
`docs/design/2026-08-07-code-push-host-slice-design.md` §§2–5 — not `porta/docs/design/`,
because that rung spans both halves.

### 3.4 Every stored row records which boot published it (rete#210, recorded 2026-08-15)

`cells` and `events` each carry a **`boot_id`**, and `porta get` marks any row that predates
the node's current boot.

The reason is a silent wrong answer, not a nicety. `cells` holds the latest value per
`(node, path)`, and a rebooted node rewrites its whole tree **over several seconds** — sixteen
`diag/spans/**` rows took 11 s in the run that exposed this, a window that begins *after* the
target harness's console verdict has returned. A read taken inside it answers with a mixture:
this boot's values for the paths that have arrived, the previous boot's for the rest. Before
`boot_id`, nothing in a row said which, so the mixture was indistinguishable from a coherent
answer, and it nearly inverted a hardware finding on 2026-08-14 (rete#161).

The anchor was **not new information**. `bootId` is a persisted, monotonic, NVS-backed counter
the node already sends as `bid` on every envelope (added for rete#162/#117); porta already
stored it on `nodes` and `exchanges`, and `Ingest` already held it at the moment it wrote both
tables. The fix was to stop discarding it.

Four properties worth knowing before reading output:

- **`boot=?` is unknown, never stale.** A node with no persistent identity (posix without
  `OEVM_BOOTID_FILE`, a board without `CONFIG_SETTINGS`) genuinely reports 0, as do rows
  written before the column existed. Neither is marked.
- **`boot=-` is an outbound row** — your own `goal/**` write. Porta's intent has no node boot
  and no reboot invalidates it.
- **The `! MIXED READ` banner goes to stdout**, so a pipe cannot eat it.
- **`--boot N` exists on `log` and deliberately not on `get`.** The log is append-only, so one
  boot is a filter; the cell tree keeps only the latest value per path and could answer at most
  "paths whose *last* write fell in boot N" — a new way to be misled under a flag name
  promising otherwise.

Existing stores gain the column on open via an idempotent `ALTER TABLE`; rows already present
read unknown and each path acquires a real boot id the next time it is written.

---

### 3.5 A fourth goroutine watches for silence (rete#217, recorded 2026-08-18)

`porta serve` runs **four** loops, not three: the UDP exchange, the block server, the HTTP
listener, and a **silence sweep** that ticks every 30 s. `awaitServeExit` composes all four.

The sweep forms one liveness verdict per node and writes `gw/liveness` **only when the verdict
changes**, so `porta log` is a list of the times the fleet went quiet rather than a row every
30 s. It is a ticker and not a lazy read-time view on purpose: the failure it must catch is
porta hearing *nothing*, and a system that evaluates only when read is silent exactly when
everything is.

**Liveness — porta judges nodes against the number they declared, and infers nothing**
(rete#231, 2026-08-21; shape per rete#273 D2, 2026-09-17). Every node publishes `obs/liveness` =
`Liveness { misses, telCad }`: how often it intends to speak, and how many consecutive misses it
tolerates. The budget is **derived**, `telCad × misses`, and the rule is `now - last_seen >
budget`. The node is the only party that knows its own loss rate — `c6-bell` misses roughly half
its 30 s check-ins to ordinary mesh loss — so a gateway judging it against a cadence would call a
healthy node silent constantly, and a node cannot publish a budget shorter than its cadence
because there is no field to put it in.

Five verdicts at `gw/liveness`, because they are five different operator actions:

| verdict | means | do |
|---|---|---|
| `#live` | a counted pull inside its budget, and the count keeping its declared rate | nothing |
| `#silent` | no counted pull for longer than its budget | go find out why |
| `#neverSeen` | porta has never had an exchange from it | commissioning failure — go plug it in |
| `#unknown` | porta will not answer: the node declared no cadence, its declared budget is below the sweep tick (rete#273 D2.1), porta was not listening for part of the window (see `gw_runs`), no tick count in the node's pulls (an image from before rete#272 — reflash it), or porta holds only one sample | read the row's `reason` |
| `#stalled` | heard from inside its budget, but the count in the node's pull (`#wak`) moved slower than its declared `telCad` between porta's last two samples | find what is blocking its tick — a replyless uplink (#271) doubles the period; a reboot reads as `wak` going backwards |

The budget is `telCad × misses`, both declared by the node at `obs/liveness` (rete#273 D2);
porta contributes no number. Two consequences an operator will meet: **a budget below the
30 s sweep tick reads `#unknown`** with a reason naming all three numbers (D2.1 — porta will
not promise a resolution it cannot deliver; raise `misses` or `telCad`), and **`#silent`'s
reason counts the misses** (`4.5 missed check-ins of 10000 ms, 4 tolerated`, D2.2) — a
ladder in the reason, deliberately not a verdict of its own until an operator is shown to act
differently on the first miss than on the fifth.

`#stalled` is the rate rule (rete#272 D3), judged inside the budget from two samples of the
node's own count of check-ins, which it sends as `#wak` in every pull (D3.3 — not in the push,
whose frame is re-sent unchanged until acked): the **mark** (the sample the previous sweep
judged from) and the latest. A sample is that count and the moment porta heard it; the
verdict reads nothing else. Short by one whole declared period or more is `#stalled`; that one period is the only
slack. A sweep with no new sample since the mark keeps both the verdict and the mark rather
than re-judging. The detection bound is `bud + SweepTick + telCad` (32 + 30 + 2 = 64 s on a
bench node), model-checked in TLC (`docs/spikes/2026-08-27-liveness-model/README.md`, configs
`sound-rate.cfg`, `fresh-clear.cfg`, `fresh-lossy.cfg`). Two rules below are pinned by porta's
tests but not by the model, which has no porta-outage state: a verdict past the budget forgets the
mark, so an unplugged-and-replugged node reads `#silent` → `#unknown` (one sample) → `#live`,
never `#stalled` across the silence; and a window that spans an interval porta was not
listening (every deploy) is re-marked, not judged — porta's absence is not evidence about the
node.

`#unknown` is the one that took an incident to earn. `Store.Sweep` runs inside `porta serve`, so
a gateway that is down writes nothing and every verdict freezes: on 2026-08-19 `gw/liveness` read
`#live` for ten hours and six minutes while the node it described missed every check-in. porta
now records one row per run in `gw_runs`, and a verdict whose window that record does not fully
cover reads `#unknown`. **A powered-off node is still indistinguishable from a wedged one** —
nothing on the gateway side can change that. What the budget buys is knowing when to start
worrying.

Two more properties worth knowing before reading its output:

- **The operator may request a budget, and never set one.** `goal/liveness` is a *request*: the
  node snaps it to its own grid, clamps it to 24 h and publishes what it adopted, so the ordinary
  `goal`/`obs` tree diff answers "did the node accept?". Ask 45 s of a 2 s node and 46 s comes
  back. Nothing has to know the legal values in advance, and porta never writes `obs/**`.
- **`gw` is a third value of `direction`, not a fifth table.** That is what keeps the verdict off
  the wire: `OutboundSince` already selects `direction = 'out'`, so a `gw` row is excluded **by
  construction** rather than by a filter someone must remember. `gw_runs` is the one table this
  rung added, and it is argued as an exception rather than a precedent: it is not per-node data
  and has no path, so the "a feature adds rows, not tables" rule (D2) does not reach it.

What replaced what: cadence used to be the median of a node's last ten pull-leg gaps, written
back to `nodes.cadence_ms`, with silence at `3 × cadence` and no verdict at all under three
exchanges. All of it is deleted — `CadenceMs`, the window, the minimum, `k = 3`,
`nodes.cadence_ms`, and the `exchanges.leg` column that existed only to keep the median honest.
A node dead before the gateway shipped is now `#neverSeen` on its first sweep (rete#219), and a
fast node doing routine blocking work is no longer called silent for exceeding a number it never
chose (rete#220).

The instrument's own remaining lie is written down in `Sweep`'s doc comment, where it is read:
a powered-off node and a wedged-and-halted one are indistinguishable from here, and always will
be.

### 3.6 The manifest gate (rete#243, recorded 2026-08-25)

The kind+schema contract is `protocol/MANIFEST.cddl` (machine-normative; see
`protocol/PROTOCOL-NSL-RECORDS.md` §4's header). porta's half of its enforcement:

- **`porta serve --manifest <path>`** loads the file and **persists its bytes into the
  store** (`meta` table, key `manifest`). Persistence is the point: every later `Open`
  of that db — including `porta put --db`, a separate process — arms the same gate,
  and the CLI door is where both historical node-bricking kind writes (#180, #206)
  came from. A flagless serve over a db that already holds a manifest still gates.
- **All three store write doors check** (`internal/space`, gate seated around
  `internal/manifest`'s ~100-line table): known path (closed set, literal shadows
  wildcard), agreeing kind, agreeing head bytes (record rows: the deterministic
  encoding's constant tag-27/name prefix; scalar rows: a first-byte class), agreeing
  direction (`up`/`down`/`gw`). No value is decoded — the store stays opaque (D4.1).
- **Failure surface**: the operator door (`Write`, `WriteGateway`) refuses loudly and
  immediately with the named check. The wire door (`Ingest`) **refuses-but-keeps**:
  the entry is not applied, its seq IS acked so the node advances instead of wedging,
  and the bytes are retained as an `events` row with `actor=gate` and the failed check
  as `reason` — `porta log` finds them; a refusal count is a query, not a mechanism.
- **`GET /manifest`** on the operator HTTP surface serves the loaded file with its
  SHA-256 in `X-Manifest-SHA256` — the fetch-and-pin source for dev builds
  (`protocol/tools/pin_manifest.py`) and the provision gate's served-hash side
  (`protocol/tools/check_provision.py`).

### 3.7 Store open doors (rete#221, recorded 2026-08-25)

sqlite creates a database file on open, so the store has two doors: `serve` opens
create-on-open (`space.Open`) and is the only initialiser; the operator verbs
`get`/`log`/`put` open existing-only (`space.OpenExisting`) and refuse a missing
path by naming it. Before this split, a wrong `--db` on a read **manufactured a
fresh empty store and answered from it** — output byte-identical to "no rows
yet", plus a decoy file deposited beside the real one (found on the gateway,
2026-08-18). `get` and `log` also print `# store <abspath>` on stderr, so a
correct-looking empty answer still says what it read.

### 3.8 A node's id is its factory address (rete#118/#242, recorded 2026-10-02)

porta keys a space by the id the node declares, and takes it as an opaque string. What
the node declares is decided on the node, once, for every board port:

- **The id is the board's factory address, read from silicon at boot** — lowercase hex of
  the address bytes, most significant first, no separators (`link_addr_hex`,
  `nodus/oevm/src/link_addr.h`). A radio board answers its 48-bit link address, so twelve
  characters: the eFuse base MAC on the ESP32 families, FICR `DEVICEADDR` with the top two
  bits forced on the nRF52840. Both are the value printed on the board's sticker, so a
  space in `porta get` is matched to a board in the hand without a wire read. A board
  with no radio (RP2350, RP2040) answers its flash serial, sixteen characters.
- **Nothing invents an id.** There is no build-time id and no fallback: a board that
  cannot read its address fails `platform nodeId` rather than boot under a made-up name,
  and the host's stand-in for silicon is the `OEVM_NODE_ID` environment variable, where
  unset also fails. Two boards flashed from one image are therefore two spaces.
- **The human name is the role**, `goal/name`, written by the operator and echoed by the
  node at `obs/name`. Nobody reads the id.
- **The bench name is data, not identity**: an optional `board` text field on
  `obs/build`, baked in at build time from `OEVM_BOARD_NAME` and absent when the build
  was told none. The board-name → device mapping is asserted by the node on every boot
  rather than kept in a table.
- **The id is a LAN id**, unique within one porta's store. No WAN id exists; if one is
  ever needed it is the gateway's id prefixed onto the node's.

The bench-named spaces (`c6bench`, `c6spare`, `nrf4ec0`, …) are historical from
2026-10-02, as the slot-named ones are from 2026-08-22 (rete#186); analysis that crosses
either date reads both. porta does not yet check the id's shape at ingest — that is
rete#305, now one rule over two legal lengths. Decision and its addendum:
`docs/design/2026-09-11-node-identity-hardware-rooted-design.md`; the per-family board
proof: `docs/results/2026-10-02-118-node-id-board-proof.md`.

---

## 4. Transports

porta speaks **two** transports, both plain UDP, and neither is a session:

- **The exchange** (`:6969` by default, `internal/wire`). Two node-initiated,
  idempotent messages carrying CBOR: a **`pull`** that fetches the writes porta holds
  for the node, and a **`push`** that delivers the node's entries. Framing is a pure
  `Handle([]byte) []byte` under a thin UDP loop, so the protocol is testable without a
  socket. Both bounded by a **512 B batch and a 256 B value** — 6LoWPAN fragments above
  ~102 B and a datagram survives only if every fragment does, so these are radio
  constraints and not preferences.
- **The block plane** (exchange port **+ 1**, `internal/blockserv`), for bulk image
  bytes. Stateless, resumable from any block number (§3.3).

Node-initiated in both directions is the load-bearing choice: a node that deep-sleeps,
reboots or lives behind a mesh border router has no stable address to be called back on,
and there is no session for a reboot to invalidate.

**The space — not the link — is the contract.** A conforming node MAY reach the gateway
over any link that carries a datagram. This is not theoretical:

- **WiFi / IPv4** and **Thread / IPv6 (6LoWPAN)** are both in service today, against the
  same gateway, the same store and the same log, with no branch anywhere in porta.
- The Thread nodes reach the gateway at a **mesh-local EID**; the mesh has no global IPv6
  and no DNS64, so an address is a literal.
- The UDP backend is `AF_INET6`; a v4 node needs dual-stack at both ends, and a
  v4-mapped destination is accepted and then silently discarded.

**There is no TFTP listener in the binary.** v1's `--port` flag went with it, and a test
asserts the flag stays gone. The v1 wire is archived at `../../protocol/archive/PROTOCOL.md`,
which is what the retired Toit nodes still speak.

---

## 5. Node modes and app lifecycle — declared at a path, not commanded

A node has **two independent dimensions**. In v1 each was a verb; both are now paths,
and each has a `goal/` half porta writes and an `obs/` half the node publishes, so the
diff between them is the answer to "did the node accept?".

**Power mode** — `goal/mode` / `obs/mode`, a `Mode { mode, maxAwakeS, minAwakeS,
loopSleepS, maxAsleepS }`:

- **deep-sleep**: the node wakes, exchanges with the gateway, then sleeps for its
  interval, waking via a **full reboot**. The battery profile — and the reason `seq` is
  not a liveness signal across a reboot, and the reason every stored row records the boot
  that published it (§3.4).
- **always-on**: the node never sleeps and keeps long-lived work alive between
  exchanges. The mains profile.

**App lifecycle** — `goal/apps/<name>` / `obs/apps/<name>`, an
`App { image: BlobRef{hash,size}, sig, runlevel, triggers, lifecycle }`:

- **run-once**: the app is expected to return. The natural fit for a deep-sleep node —
  wake, take a reading, return, sleep.
- **run-loop**: the app never returns. The supervisor starts it and must not block on it.

The dimensions interact exactly as they did: **a `run-loop` daemon on a `deep-sleep`
node is killed by each sleep**, so a long-lived daemon requires `always-on`. Halting
behaviour cannot be inferred, which is why `lifecycle` is *declared* rather than guessed.

**There is no tombstone.** Stopping an app is writing its lifecycle to stopped — a value,
not a deletion. Genuine removal is an action on a `do/` stream, and `goal/do/reboot` is
the only one pinned today.

**Liveness is declared the same way.** `obs/liveness` = `Liveness { cad, bud }` is the
node's own statement of how often it intends to speak and how long its silence may last;
`goal/liveness` is a *request* the node snaps to its grid, clamps to 24 h and publishes
back. porta judges `now - last_seen > bud` and infers nothing (§3.5).

---

## 6. The exchange, the log, and what makes replay safe

**Two messages, both idempotent, both node-initiated.** A check-in is one `pull` and one
`push` — two rows in `exchanges`, and **both** stamp `nodes.last_seen_ms`. There is no
`leg` column: it existed only to feed a cadence inference that #231 deleted.

**One monotonic `seq` per space per side.** Kind is a property of the leaf, set by its
first write and pinned by the manifest; `KindStream = 1`, `KindCell = 2`, shared with the
VM's `space.h` and not renumbered.

**Replay from cursor 0 is designed behaviour, and safe per kind.** A cold-booted node
asks from 0 and porta replays: cells are latest-value and therefore idempotent, while
**streams are guarded** — porta never re-serves a row the node has acked as applied
(`nodes.cursor_hw`, bounded by `served_hw`), so `goal/do/*` cannot re-execute across
reboots.

**The log is the audit trail, and it is strictly more than v1's queue could offer.**
`events` is append-only in both directions, carrying `actor` and `reason` on every row.
A drained command queue lost superseded intent; this keeps it. `porta log` replays it,
`--boot N` filters it to one node boot, and the arrow encodes authority: `→` porta's
intent, `←` the node's report, `◆` a gateway verdict that crossed no boundary.

**A refused entry is kept, not dropped silently.** At the wire door the manifest gate
refuses-but-acks: the entry is not applied, its `seq` IS acked so the node advances
instead of wedging, and the bytes are retained as an `events` row with `actor=gate` and
the failed check as `reason` (§3.6). The node counts the same event as `rej` in
`obs/health` — because the cursor advances past it and the replay guard makes that loss
permanent, so a write the node refused must be visible or `goal` and `obs` diverge
silently forever.

---

## 7. Where the toolchain lives, and the Toit lineage

**Nothing that needs a toolchain runs on the gateway.** Compiling, flashing and reading
a console need source, a compiler, an SDK and a USB cable, which a headless gateway does
not have. They live on the dev machine, in node-repo tooling built on porta's **`devsdk`**
(Go, MIT-licensed so tooling in any language can import it without copyleft):
`devsdk/exec` (a narrating runner for build and flash commands) and `devsdk/provision`
(the gateway-address contract a node is flashed with). **`devsdk/apiclient` was deleted**
with v1's HTTP surface — there is no network path from a dev tool to porta, which is why
`porta blob put` runs on the gateway host.

porta itself hosts **zero** language or hardware toolchain, and that is the property to
preserve: it is what let the node side be replaced wholesale without touching the
gateway's shape.

**The lineage, because it explains what a reader will find in the older documents.** The
fleet's first nodes were **Toit** containers deployed with the `jag` workflow: images
were relocated Toit containers verified with jag's CRC32-IEEE, delivered over TFTP, and
driven by a command vocabulary with a report schema. All of it is gone. Today a node runs
the **oevm** VM executing an **`.oimg`** built by the nsl compiler, fetched over the block
plane, content-addressed by **SHA-256** and **ed25519-signed at push time** — the node's
runner holds only porta's public half. The frozen Toit v1 implementation lives in the
`nodus-v1` repository and still speaks `../../protocol/archive/PROTOCOL.md`.

The **capability audit** in `design/2026-08-01-porta-space-sync-rewrite-design.md` §4.2
gives every v1 verb, route, MCP tool and `nodes` column a fate — `absorbed`, `deferred`
or `regression` — because **a rewrite may delete a mechanism but may not silently delete
a capability** (D8). Three of v1's fourteen `nodes` columns turned out to have no
successor until they were enumerated; all three were closed before demolition. Read that
section rather than guessing where a v1 feature went, and `CLI.md`'s closing table for
the operator-facing short version.

---

## 8. Repo topology

**The system is one repository.** `rete` was consolidated on 2026-08-02 from four —
`tuvm`, `nsl-tuvm`, `nsl-tests` and `porta` — by restructuring in place, which is what
preserved the issue numbers the program navigates by.

| Tree | Role | Couples to the other half via |
|---|---|---|
| `porta/` | the gateway: the store, the exchange, the bulk plane, the CLI, `devsdk/` | the wire, and nothing else |
| `nodus/oevm/` | the VM (C) and its Zephyr board ports | the wire |
| `nodus/nsl/` | the dialect, its compiler, the kernel | — |
| `nodus/tests/` | the nsl conformance suite | — |
| `protocol/` | **the whole of the contract**; the Toit-era v1 archived under `protocol/archive/` | it *is* the coupling |
| `review/`, `docs/`, `tools/` | the program ladder and claims ledger; documents of record; repo-wide scripts | — |

**Being in one repo does not couple the two halves.** porta and the nodes are coupled
**only** over the wire; they never import each other's code, and a change that makes one
side compile against the other is a bug rather than a convenience. The place this is
actively defended is the gates: the nsl suite's two porta-facing gates (`tutorial-porta`,
`porta-cli`) answer a **named SKIP** when there is no Go toolchain rather than failing,
precisely so a missing `go` cannot make the node suite depend on the gateway half.

An implementation of the *gateway* role is as legitimate as an implementation of the
*node* role: both conform to `protocol/`, which is exactly the point of letting the
contract live in neither half.

---

## 9. See also

- `../../protocol/PROTOCOL-NSL.md` — the wire contract; **§§1–4 are the conformance
  target**. `../../protocol/PROTOCOL-NSL-RECORDS.md` is the record catalogue and the size
  law; `../../protocol/MANIFEST.cddl` is its machine-normative half.
- `../../docs/tutorials/03-porta.md` — the executed introduction: a gateway and a real
  node on loopback, every command block run by the nsl suite.
- `CLI.md` — the command reference, gated against the binary's own `--help`.
- `design/2026-08-01-porta-space-sync-rewrite-design.md` — the v2 decision record (D1–D8),
  the store schema, the wire shapes, and the §4.2 capability audit.
- `design/2026-08-26-space-sync-durability-trio-design.md` — the replay guard, and why
  cursor-0 replay is safe per kind.
- `DEVSDK.md` — the northbound tooling contract.
- `../../protocol/archive/PROTOCOL.md` — **v1, historical only.** Read it to understand what
  `porta-v1-final` did; do not implement from it.
- `README.md` — the map of this directory.
