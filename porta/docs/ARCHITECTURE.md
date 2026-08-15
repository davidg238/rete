# Porta system architecture

This is the **canonical system-architecture document** for porta and the fleet it
controls. porta owns the *system architecture* the same way it owns the *wire
protocol*: `docs/PROTOCOL.md` is the southbound wire contract, `docs/DEVSDK.md` is
the northbound tooling contract, and **this** document is the picture they sit
inside — what the pieces are, why the boundaries fall where they do, and which
properties hold across the whole system.

Where this document and the code/contracts disagree, the contracts
(`PROTOCOL.md`, `DEVSDK.md`) and then the code win; this document is the bug.

---

## 1. The system at a glance

```
                       DEV MACHINE  (has source + jag + SDK + USB)
              ┌──────────────────────────────────────────────────────┐
              │   nodus tool          nodus-st tool                    │
              │   run / flash /        run / flash / …                 │
              │   decode               (Smalltalk)                     │
              │        └──────────┬───────────┘                        │
              │        imports github.com/davidg238/porta/devsdk (Go)  │
              └───────────────────┼────────────────────────────────────┘
                                  │  control-plane HTTP API  (:6970)
                                  ▼
  operator ── HTTP / htmx ──►  ┌──────────────────────────────────────┐
  (browser · porta CLI ·       │             PORTA GATEWAY              │
   read-only MCP)              │  `porta serve`  — the single writer    │
                               │                                        │
                               │  • HTTP: control API + htmx UI + /mcp  │  :6970
                               │  • UDP/TFTP listener                   │  :6969
                               │  • sqlite store: nodes · command_queue │
                               │    · payloads · reports · data_log     │
                               │  • command audit + config self-heal    │
                               └──────────────────┬─────────────────────┘
                                                  │  TFTP RRQ/WRQ over UDP
                                                  │  (node = client, gateway = server)
            ┌─────────────────────────┬───────────┴───────────┐ · · · · · · · · · · · · ·┐
            ▼                          ▼                        ▼                          ▼
      ┌───────────┐            ┌───────────┐            ┌───────────┐            ┌───────────┐
      │   nodus   │            │   nodus   │            │   nodus    │   ...      │ nodus-st  │
      │   Toit    │            │   Toit    │            │   Toit     │            │ Smalltalk │
      │  ESP32    │            │ ESP32-S3  │            │  ESP32-C6  │            │ nRF52840  │
      │ deep-sleep│            │ always-on │            │ deep-sleep │            │  Zephyr   │
      └───────────┘            └───────────┘            └───────────┘            └───────────┘
       └──── Toit nodes · WiFi · IPv4 (UDP) ───────────────────┘       Thread · IPv6 (proven) ┘
            └────────── heterogeneous fleet — coupled to porta ONLY over the wire ───────────┘
                  (every node conforms to docs/PROTOCOL.md — same RRQ/WRQ surface, any link)
```

Three planes, three contracts:

| Plane | Who talks | Contract | Transport |
|-------|-----------|----------|-----------|
| **Southbound** (gateway ↔ node) | porta ↔ any node | `docs/PROTOCOL.md` | TFTP/UDP |
| **Northbound** (dev tool → gateway) | node-repo dev tools → porta | `docs/DEVSDK.md` + the HTTP API | HTTP |
| **Operator** (human/agent → gateway) | browser, CLI, MCP → porta | the HTTP API | HTTP |

---

## 2. A heterogeneous fleet with one fixed point

porta commands a fleet of **heterogeneous** nodes: different languages (Toit, and
Smalltalk via `nodus-st`), different chips (classic ESP32, -S3, -C6 on WiFi; nRF52840
on Thread), different power profiles, different jobs. They share **nothing** but the
wire.

The one fixed point is `docs/PROTOCOL.md`. A node is "conforming" if it speaks that
protocol — command vocabulary, report schema, TFTP resource surface, CRC32-IEEE
image verification — and **any node MUST be implementable from that document alone,
without reading another node's source.** This is why the gateway, not any node,
owns the protocol: the wire contract outlives any single node codebase.

Consequences of "coupled only over the wire":

- porta **never imports node code**; nodes **never import porta code** at the
  firmware level. (The *dev tools* import porta's `devsdk` — that is the northbound
  plane, not the firmware.)
- Adding a new node kind (a new chip, a new language) is a wire-conformance
  exercise, not a gateway change. The gateway shows a node's `kind` as a label; it
  branches on no language.
- The fleet can be genuinely mixed: a deep-sleep Toit air-quality sensor and an
  always-on Toit keyboard and a Smalltalk `nodus-st` node all check in against the
  same gateway, queue, and audit trail.

---

## 3. The role of porta — a neutral, always-on LAN node manager

porta has exactly two jobs: **(1) be always on, (2) manage a LAN fleet.** It
implements **zero** language- or hardware-specific function. Everything that needs
source, a compiler, a chip toolchain, or a USB cable lives off the gateway, on the
dev machine, in node-repo tooling (see §6).

What the gateway *is*:

- **The single writer.** All fleet state lives in one sqlite store, mutated only by
  `porta serve`. Operators and dev tools do not open the store; they go through the
  HTTP control-plane API, which serializes every write and records it. This is what
  makes the audit trail (§5) trustworthy and lets multiple clients (CLI, browser,
  MCP, remote dev tools) act without stepping on each other.
- **The command authority.** It owns the command vocabulary and queues commands per
  node; nodes pull and apply them.
- **The image courier.** It stores container images and hands them out over TFTP,
  verified by length + CRC32 (§7).
- **The telemetry sink.** It ingests typed telemetry and panics into `data_log`.
- **The operator surface.** `porta serve` exposes an HTTP control API, an htmx
  operator console, and a read-only MCP endpoint — all on the same listener.

What the gateway is **not**: a compiler, a flasher, a relocator, a panic decoder, a
fleet-wide pod builder. Those are node-repo concerns (§6).

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
  `TUVM_BOOTID_FILE`, a board without `CONFIG_SETTINGS`) genuinely reports 0, as do rows
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

## 4. Transports

> **v1 — SUPERSEDED (marked 2026-08-08). Everything below describes the deleted TFTP
> surface and no longer names a live transport.** porta speaks two today, both plain UDP:
> the **exchange** (node-initiated pull/push carrying CBOR, `:6969` by default,
> `internal/wire`), and the **block plane** for bulk image bytes on exchange port + 1
> (§3.3, `internal/blockserv`). There is no TFTP listener left in the binary — `--port`
> went with it, and a test asserts the flag stays gone. The current contract is
> `protocol/PROTOCOL-NSL.md` §§1–4 at the rete root, not `PROTOCOL.md`. Kept below
> because the Toit v1 nodes still speak it, and because §4's link-agnosticism argument
> (the resource surface, not the link, is the contract) is the reasoning the space model
> inherited.

Today the only transport is **TFTP over UDP** (`:6969`): the node is the TFTP
**client**, the gateway is the **server**, and the node identifies itself with a
`?id=<hex-id>` suffix (opaque lowercase hex, 12–16 digits) on every resource it
touches. WiFi is the only physical link in use. Full framing, resource names, and
the drain-to-empty-body semantics are in `PROTOCOL.md §1`.

Why TFTP/UDP: it is tiny enough to implement on a constrained node, connectionless
(no session state to keep across a node's deep-sleep/reboot cycle), and the
RRQ/WRQ resource surface is a clean abstraction boundary.

**The resource surface — not the link — is the contract.** A conforming node MAY
implement any transport that presents the same RRQ/WRQ resources (`commands`,
`payload`, `report`, `data`, each keyed by `?id=`). The Toit nodes in use today run
over **WiFi (IPv4)**.

This link-agnosticism is **not just theoretical: the RRQ/WRQ interface has been
proven over Thread (6LoWPAN/IPv6) to Zephyr devices running `nodus-st`** — Smalltalk
nodes on an **nRF52840**. Those nodes reach the gateway over **IPv6** rather than the
Toit nodes' IPv4, but still as plain TFTP clients touching the same `?id=`-keyed
resources, with no change to the command model, the store, or the audit trail. The
gateway does not care how the bytes arrive, nor over which IP version. **ESP-NOW** and
**BT-mesh** remain planned behind the same interface, so a future low-power or meshed
node can join the same fleet on the same terms.

---

## 5. Node power modes and container lifecycle

A node has **two independent dimensions**, both node-owned and echoed back in the
report's `node_config` block (`PROTOCOL.md §2.3, §2.7, §3.2`):

**Power mode** (`set-mode`, atomic, per node):

- **`deep-sleep`** (default): the node wakes, polls the gateway (drains commands,
  reconciles, reports, ships telemetry), then deep-sleeps for its poll interval,
  waking via a **full reboot**. This is the battery/duty-cycle profile.
- **`always-on`**: the node never sleeps; it keeps long-lived daemons alive between
  reports. This is the mains-powered profile.

**Container lifecycle** (`lifecycle` on each `run`, per app):

- **`run-once`** (default): the container is expected to **return**. The supervisor
  may `wait` on it (with a cap) before sleeping. The natural fit for a deep-sleep
  node — wake, take a reading, return, sleep.
- **`run-loop`**: the container **never returns** (a daemon). The supervisor starts
  it and must not block on it.

The dimensions interact: a `run-loop` daemon on a `deep-sleep` node is **killed by
each sleep**, so a long-lived daemon requires `always-on`. The halting behaviour of
a container cannot be inferred, which is why `lifecycle` is *declared* at install
rather than guessed. Together these cover the real fleet: deep-sleep + run-once for
a sensor that samples every few minutes; always-on + run-loop for a daemon that must
stay live.

Wake/run conditions are expressed as **triggers** (`boot`, `interval`,
`gpio-high/low/touch:<pin>`, `install`) attached to each app — see `PROTOCOL.md §4`.

---

## 6. Command protocol and audit trail

**Declarative and absolute.** Every command is a single JSON object with a `verb`
and flattened args. Applying one is idempotent, and a later command for the same
target wins — so redelivery is always safe. The verbs (`run`, `stop`, `set-mode`,
`set-name`, `set-forward`, `set`, `reboot`) and their schemas are in `PROTOCOL.md §2`;
`set-mode` is atomic (whole-or-reject).

**The queue + delivery accounting.** Commands sit in `command_queue` per node. A
node drains by repeated `commands?id=` RRQ until it gets a zero-byte body. A real
command is marked **delivered only on the TFTP transfer-complete event with
`ok=true`** — a failed or drain transfer marks nothing. So "delivered" means the
bytes actually reached the node, not merely that they were dequeued.

**The audit trail.** Because the gateway is the single writer, the command log is a
complete, ordered record of intent: who asked for what, when it was queued, when it
was delivered. `porta log` (and the operator UI's command timeline) replays it. The
derived lifecycle of a command — queued → delivered → (for `set`) converged /
pending / drift, or expired — is computed from this log plus the node's reports.

**Two state planes, reconciled.** The **goal plane** (which apps run, at what
triggers/runlevel/lifecycle) and the **config plane** (`set`: per-app scalar config)
are separate. The node echoes both its observed apps and its applied `config` in
every report (`PROTOCOL.md §3`). After each report the gateway runs **config
self-heal**: it diffs desired config (projected from delivered `set` commands)
against the reported `config` and re-enqueues any delivered-but-divergent `set`
(tagged `gateway-reconcile`). Desired-vs-observed convergence is a first-class,
visible property — the node does nothing special beyond echoing what it applied.

---

## 7. Compatibility with the existing Toit toolchain

porta is deliberately *not* a reinvention of the Toit container format or the chip
toolchain — it rides on them, so that today's Toit nodes (and the `jag` workflow
operators already know) keep working.

**Image format = relocated Toit container, jag-aligned.** What porta stores and
delivers as a `payload` is the **raw bytes of a relocated container image** — the
same artifact `jag` produces and deploys. There is **no porta-specific framing**:
size and CRC ride in the `run` command, and the `payload` resource is raw image
bytes (`PROTOCOL.md §5`). The integrity check is **CRC32-IEEE with the exact same
parameters jag uses** for its `X-Jaguar-CRC32` (`PROTOCOL.md §5`), so an image built
through the jag toolchain verifies bit-for-bit on the node.

**Firmware is jag/standard-Toit firmware.** Nodes are flashed with the normal Toit
toolchain (jag, or the nodus flasher wrapping `jag flash`). porta delivers and runs
*containers* onto already-flashed firmware; it does not replace the firmware flow.
The SDK/chip coupling that matters — an image must be relocated for the node's chip
and match the firmware's SDK version — is enforced by the node-repo dev tool
(`nodus run` checks the node's reported `chip`/`sdk` before deploying), not by the
gateway. Use the *same* jag/SDK to flash a device and to build the image it runs.

**Relationship to Artemis.** porta is an **independent control plane** that operates
at the container-image level, coexisting with the existing Toit tooling rather than
consuming Artemis fleet artifacts. The shared substrate is the relocated image +
jag CRC, so images flow from the same build toolchain.

To work toward future compatibility with Artemis, porta's command vocabulary
deliberately mirrors the **container triggers Artemis supports**, so a container's
run conditions mean the same thing on either control plane (`PROTOCOL.md §4`):

| Trigger | porta value | Meaning |
|---------|-------------|---------|
| `boot` | `1` | Run on (cold) boot. |
| `install` | int | Run on the Nth install generation. |
| `interval` | int (seconds) | Periodic wake. |
| `gpio-high:<pin>` | `<pin>` (int) | Wake/run on GPIO `<pin>` high (ext1). |
| `gpio-low:<pin>` | `<pin>` (int) | Wake/run on GPIO `<pin>` low. |
| `gpio-touch:<pin>` | `<pin>` (int) | Wake/run on touch pin `<pin>`. |

Where the two diverge today is the **unit of deployment**. Artemis synchronises a
fleet against a `.pod` — a self-contained, versioned firmware *archive* that bundles
the Toit SDK envelope, every container image, and the per-container trigger/runlevel
config into one artifact flashed/OTA'd as a whole. porta does not consume `.pod`
bundles: it delivers and runs **individual relocated container images** (`run`/`stop`,
`PROTOCOL.md §2`) onto already-flashed firmware, with triggers/runlevel/lifecycle
declared per-command rather than baked into a pod. Ingesting `.pod` bundles — reading
their container set and trigger config to drive porta's command queue — is **not
implemented** and is a candidate direction, not a current capability.

**Where the toolchain physically lives.** Compile, relocate, flash, and panic-decode
need source + jag + SDK + a USB cable, which a headless gateway does not have. They
live on the dev machine in node-repo tools built on porta's `devsdk` (Go): `devsdk/
apiclient` (talk to the control API), `devsdk/exec` (narrating runner for `toit`/
`jag`/`esptool`), `devsdk/provision` (the `firmware.config["porta"]` gateway-address
contract). See `docs/DEVSDK.md`. porta itself ships none of this — `porta` the
binary hosts zero language/hardware toolchain.

---

## 8. Repo topology

The system spans several repos, each owning one end of one boundary; dependencies
point **one way** (node-side → porta), and porta imports nothing from any of them.

| Repo | Role | Couples to porta via |
|------|------|----------------------|
| **porta** (`github.com/davidg238/porta`) | the neutral Go gateway + the two contracts (`PROTOCOL.md`, `DEVSDK.md`) + the public `devsdk/` | — (owns the contracts) |
| **nodus** (`github.com/davidg238/nodus`) | the Toit node — firmware (southbound, conforms to `PROTOCOL.md`) **and** its dev tool (northbound, imports `devsdk`) | wire + `devsdk` |
| **nodus-st** (`github.com/davidg238/nodus-st`) | the Smalltalk node + tooling — Zephyr on nRF52840 over Thread (same pattern) | wire + `devsdk` |
| **gateway** (`github.com/davidg238/gateway`) | a full **Toit** implementation of the gateway — an alternative to porta itself, conforming to the same `PROTOCOL.md` | implements the wire |

Note `gateway` is a *gateway* (another implementation of porta's role), not a node;
`nodus`/`nodus-st` are *nodes*. Both kinds conform to the same wire protocol — which
is exactly the point of letting the gateway own it.

---

## 9. See also

- `docs/PROTOCOL.md` — the southbound wire contract (commands, report, TFTP, CRC32).
- `docs/DEVSDK.md` — the northbound dev-SDK contract (`apiclient`/`exec`/`provision`,
  the `firmware.config["porta"]` provisioning shape).
- `CLAUDE.md` — repo layout and current build/sub-project state.
- `docs/specs/` — the dated design records behind the decisions above.
