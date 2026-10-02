# Getting started with porta

> **Current as of 2026-08-29 (#279)** — v2 throughout. This page was the one that
> walked an outsider through a command vocabulary deleted weeks earlier, which is why
> the tutorials are executed by the suite and `CLI.md` is gated against the binary.
> This page is neither: it is prose about installing and operating a gateway, checked
> by hand on that date. **If a command here disagrees with
> [`CLI.md`](CLI.md) or with
> [`../../docs/tutorials/03-porta.md`](../../docs/tutorials/03-porta.md), they are
> right and this page is the bug** — both are gated and this one is not.

porta is a self-hosted, language-neutral LAN gateway for a fleet of nodes. Each
node owns a tree of paths and porta holds a mirror of it: operators write intent
under `goal/**`, the node writes what it sees under `obs/**`, `tel/**` and
`sys/**`, and convergence is the diff between them. This guide gets a gateway
running, points a node at it, and does the first operations; the command-by-command
reference is [`CLI.md`](CLI.md) and the big picture is
[`ARCHITECTURE.md`](ARCHITECTURE.md).

`porta serve` is one process with **four** loops:

- **`:6969/udp`** — the node-facing exchange: two node-initiated, idempotent messages
  (`pull`, `push`) carrying CBOR. No TFTP, and no verbs on the wire.
- **`:6970/udp`** — the bulk plane for image bytes, always the exchange port **+ 1**.
- **`:6970/tcp`** — the operator HTTP surface, `GET /health` and `GET /manifest`;
  `--http-port 0` disables it and the gateway still serves nodes.
- **a silence sweep every 30 s**, which forms porta's own liveness verdict per node
  and writes it to `gw/liveness`.

The two `:6970`s are a coincidence of the defaults, not a conflict: one is UDP and one
is TCP, and `--udp` and `--http-port` move independently.

---

## 1. Run a gateway

### Option A — install the `.deb` (recommended for a real box)

For a Debian/Ubuntu host (needs glibc ≥ 2.34, which any current release has):

```bash
# build the package (on a box with Go + a C toolchain; porta uses CGO sqlite)
VERSION=0.5.4 ./deploy/build-deb.sh        # → deploy/dist/porta_0.5.4_amd64.deb

# install on the target
sudo apt install -y ./porta_0.5.4_amd64.deb
```

The package installs a systemd unit that runs `porta serve` as a `DynamicUser`
with its database under `StateDirectory`:

```bash
systemctl status porta        # active (running), enabled
# listens on :6969/udp + :6970/tcp; db at /var/lib/porta/porta.db
```

> **Schema upgrades:** porta uses `CREATE TABLE IF NOT EXISTS` with no migrations
> (pre-1.0, crash-and-fix). A release that *adds* columns needs the db recreated:
> `sudo systemctl stop porta && sudo rm -f /var/lib/porta/porta.db* && sudo systemctl start porta`.
> Pure UI/web releases do not. (On a `DynamicUser` host the real path is
> `/var/lib/private/porta/porta.db`.)

### Option B — run the binary directly (quickest for a look)

porta links SQLite via CGO, so building needs `CGO_ENABLED=1` (the default) and a
C compiler:

```bash
go build ./cmd/porta
./porta serve --db ./porta.db
# porta: serving the space on udp :6969 (db=./porta.db)
# porta: serving HTTP on 0.0.0.0:6970
```

Run the whole test suite while you're here:

```bash
go test ./...
```

---

## 2. Confirm it's up

`GET /health` and `GET /manifest` are the whole HTTP surface:

```bash
curl -s http://localhost:6970/health
# {"nodes":0,"status":"ok","uptime_s":2,"version":"dev"}
```

- `nodes` is the row count in the `nodes` table. **`-1` is not an error state** —
  it means the count was unavailable (no store attached, or a transient DB blip)
  and the response is still `200`, because the endpoint reports that the
  *listener* is healthy, not that the store is. A gateway serving before any
  store exists is a supported state.
- `version` is the build stamp; a plain `go build` with no release ldflags reads
  `dev`.
- The body is deliberately cheap — in-memory stats only, no `PRAGMA` and no
  `stat`, so it is safe as an uptime probe on any interval.

`GET /manifest` returns the CDDL contract this gateway is enforcing, with its digest in
an `X-Manifest-SHA256` header so a fetcher can refuse an endpoint that lies about its own
bytes. Without `--manifest` on this process the route is a `404`.

**There is no third route.** `/`, `/api/nodes` and `/api/status` were v1's and answer
`404`. Reading the fleet is a CLI job against the database (§4); the browsable surface is
`custos` (§5).

The HTTP listener is allow-listed to RFC1918 + loopback + Tailscale CGNAT by
default, so it's reachable from your LAN and over Tailscale, but not the public
internet. Adjust with `serve --http-allow-cidr`.

---

## 3. Point a node at it

A node needs two things: **where the gateway is**, and **who the node is**. Both are
baked into the image today, and neither is provisioned over the network.

**Where the gateway is** — `gwAddr` and `gwPort` are constants in the node's own nsl
source (`nodus/oevm/zephyr/PortaBridgeNodeC6.nsl`, and the loopback
`PortaBridgeNode.nsl` the host tutorial uses). Changing the gateway address is an edit,
a rebuild and a reflash. That is also why `docs/tutorials/03-porta.md` cannot pick a
different port: the node's `gwPort` is a source constant, so the tutorial binds 16969
and 16970 or nothing.

```nsl
gwAddr    = ( ^ 'fdc5:9109:9afe:97ea:9541:d963:f8d9:8567' )   (* a mesh-local EID *)
gwPort    = ( ^ 6969 )
```

The mesh has **no global IPv6 and no DNS64**, so a Thread node's gateway address is a
literal mesh-local EID — not a hostname.

**Who the node is** — `platform nodeId`, the board's factory link address: 12 lowercase
hex characters on ESP32-family and nRF52840 boards, so the porta space of a board is that
id, not its bench name. RP2040/RP2350 have no radio and so no link address; they answer
the flash chip's factory serial instead, 16 hex characters, the same value the board's USB
serial string shows. A host VM has no silicon and answers the `OEVM_NODE_ID` environment value instead
(unset or empty fails). The human-facing name is the role at `goal/name`; the bench name
of a board is published separately, as the optional `board` field of `obs/build`, from the
build-time `OEVM_BOARD_NAME`. History: before rete#186 the id was a string literal in the
bridge's source, so every board flashed with that image wrote into one space; #186 and
#181 made it a per-build define; rete#118 replaced the define with the hardware address.

> **A node id is a name and not a credential.** Any peer can claim any id today. That is
> the G5 authentication question (`PROTOCOL-NSL.md` §7, open question 5) and it is out of
> this rung.

`devsdk/provision` encodes a gateway-address contract as `firmware.config["porta"]`, but
that is the **Toit-era** shape: it is how a v1 node was provisioned at flash time, not
how an nsl node finds the gateway today.

Once a node boots and joins the network it checks in on its own — the exchange is
node-initiated in both directions, so nothing on the gateway has to know a node's address
or reach it first. Watch it arrive with `porta log <node>`.

---

## 4. First operations

The whole operator vocabulary is three verbs over paths. There is no `scan`, no `ping`
and no `device` subcommand; each of those was one of these against a different path, and
[`CLI.md`](CLI.md)'s closing table says which.

```bash
# what porta wants
porta put --db /var/lib/porta/porta.db c6bench goal/name c6bench --reason 'first light'

# what the node reports, and what porta thinks of it
porta get --db /var/lib/porta/porta.db c6bench

# everything that happened, both directions, newest first
porta log --db /var/lib/porta/porta.db c6bench --limit 20
```

A converged `get` puts the two halves side by side — `goal/name` written by you with
`boot=-`, `obs/name` published by the node with the boot that published it, and porta's
own `gw/liveness` verdict beside them. **Real output for exactly this sequence, against a
real node, is in tutorial 3** — this page shows no sample of it because it has no live
node to capture one from, and an invented transcript is how a page starts drifting.

**`goal/` against `obs/` is the whole convergence story.** It is a tree diff, so the
desired-versus-observed view and the self-heal loop are the same query — there is no
reconcile mechanism to enable and no command to re-issue.

Three things worth knowing before you trust an early read:

- **Pass `--db` explicitly.** Every verb opens the sqlite file directly and the default
  is a relative `porta.db`, so a defaulted read on the gateway answers from the wrong
  store rather than failing. `get` and `log` print `# store <abspath>` on stderr for
  exactly this reason.
- **A rebooted node rewrites its whole tree over several seconds.** A read taken inside
  that window mixes two boots; `porta get` marks the stale rows `~` and banners
  `! MIXED READ` on stdout.
- **`gw/liveness` is porta's verdict, not the node's report** — `#live`, `#silent`,
  `#neverSeen` or `#unknown`, formed against the budget the node itself declared at
  `obs/liveness`. `#neverSeen` on a node you just flashed means it has never reached the
  gateway at all.

**For the executed version of all of this, read
[`../../docs/tutorials/03-porta.md`](../../docs/tutorials/03-porta.md)** — a gateway and a
real node on loopback in about a dozen commands, every block of it run by the nsl suite,
so it cannot go stale the way §§3–4 of this page did.

---

## 5. The operator surface

porta's own HTTP surface is deliberately tiny: `GET /health` and `GET /manifest`, both
covered in §2. **The exposed, agent-facing front door is a second binary, `custos`**
(Latin *guardian* — it stands at the *porta*). It is a separate process on purpose:
custos can be restarted and iterated on freely, while restarting porta costs every node
an outage.

```bash
go build ./cmd/custos
./custos --docs /path/to/rete/docs        # :6971 by default
```

**custos is not in the `.deb`** — `deploy/build-deb.sh` packages `cmd/porta` alone, so
custos is built and started by hand today. So is the v2 porta on the bench gateway; both
want a unit, which is rete#282.

| Route | What |
|---|---|
| `/` and `/dev` | redirect to the dev status page |
| `/dev/…` | the repo `docs/` tree, read-only (`.md` served as `text/plain` so a browser shows it) |
| `/health`, `/manifest` | proxied to porta's operator listener |

Flags are Go-`flag` style, single dash: `-bind`, `-port`, `-docs`, `-porta-http`,
`-allow-cidr`. The allowlist has the same semantics as porta's `--http-allow-cidr`, and
the two share one implementation so the surfaces cannot drift apart.

**What is not built yet** is the *query* surface — the schema-aware side that would let an
agent ask a question instead of reading a tree. That is rete#198, designed in
`design/2026-08-22-gatekeeper-first-cut-design.md` (filed under the old name), with
`porta/internal/gate` as its core. Nothing in custos decodes a value today; it serves
documents and proxies two routes.

v1's htmx console, `GET /api/status`, `GET /api/nodes` and the read-only MCP server are
gone. Their fate is recorded as **deferred → operator surface** in the capability audit
(`design/2026-08-01-porta-space-sync-rewrite-design.md` §4.2), not as a silent drop: a
rewrite may delete a mechanism, but it may not delete a capability.

What that audit turned up is worth knowing before anyone rebuilds the page: the status
page's wifi-vs-Thread breakdown was computed from `nodes.source_addr`, a column with no
successor in the new schema until it was enumerated. It came back as `nodes.last_addr`,
stamped by the UDP loop from the datagram's peer address — so the data a future status
page needs is being recorded now, rather than waiting for the page to notice it is
missing.

---

## Building from source / contributing

```bash
git clone https://github.com/davidg238/rete && cd rete/porta
go build ./...        # needs CGO (a C toolchain) for the sqlite driver
go test ./... && gofmt -l .
```

`cmd/porta/` and `cmd/custos/` are the two entry points; `internal/` holds the gateway
(`cval`, `space`, `wire`, `blockserv`, `blob`, `manifest`, `httpsrv`, `portacli`,
`custos`, `gate`); `devsdk/` is the public Go surface node-repo tooling imports. The map
of this directory is [`README.md`](README.md), and the decision record behind all of it is
`design/2026-08-01-porta-space-sync-rewrite-design.md`.

**The wire contract is the one fixed point** —
[`../../protocol/PROTOCOL-NSL.md`](../../protocol/PROTOCOL-NSL.md) §§1–4 with
[`../../protocol/MANIFEST.cddl`](../../protocol/MANIFEST.cddl) as its machine-normative
half. Change it deliberately: both ends of it are reflashed together, so a shape change is
an edit and a rebuild rather than a versioned field.
