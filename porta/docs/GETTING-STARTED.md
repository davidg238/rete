# Getting started with porta

> **The listeners and endpoints in this guide were corrected against the code on
> 2026-08-05. The operator verbs in §3–§4 and the repo layout at the end are
> still v1 and no longer exist.** porta v1 — the command vocabulary, the command
> queue, TFTP image delivery and the report schema — was deleted in one commit;
> the last v1 tree is tagged `porta-v1-final`. The v2 vocabulary is
> `porta serve | get | put | log`, all of it sugar over path writes and tree
> reads.
>
> **Start at [`../../docs/tutorials/03-porta.md`](../../docs/tutorials/03-porta.md)
> instead** — the v2 walkthrough, and every command block on it is executed by the
> nsl suite (`tutorial-porta`), so it cannot go stale the way this page did.
> [`CLI.md`](CLI.md) is the v2 reference and is current — rewritten 2026-08-29 and
> checked against the binary's own `--help` by the `porta-cli` gate (#279). The wire
> itself is [`../../protocol/PROTOCOL-NSL.md`](../../protocol/PROTOCOL-NSL.md).

porta is a self-hosted, language-neutral LAN gateway for a fleet of nodes. Each
node owns a tree of paths and porta holds a mirror of it: operators write intent
under `goal/**`, the node writes what it sees under `obs/**`, `tel/**` and
`sys/**`, and convergence is the diff between them. This guide gets a gateway
running, then points at the [CLI reference](CLI.md) — current, and gated. For the big
picture first, read [`ARCHITECTURE.md`](ARCHITECTURE.md) — itself stale in parts.

porta is one process with two listeners:

- **`:6969/udp`** — the node-facing space sync: two node-initiated, idempotent
  messages (`pull`, `push`) carrying CBOR. No TFTP, and no verbs on the wire.
- **`:6970/tcp`** — the operator HTTP surface. Today that is **exactly one
  route, `GET /health`**; `--http-port 0` disables the listener entirely.

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

`GET /health` is the whole HTTP surface:

```bash
curl -s http://localhost:6970/health
# {"nodes":2,"reports_rejected":0,"status":"ok","uptime_s":51966,"version":"dev"}
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

**There is no other route.** `/`, `/api/nodes` and `/api/status` were v1's and
answer `404`. Reading the fleet is a CLI job against the database: `porta get`,
`porta log`.

The HTTP listener is allow-listed to RFC1918 + loopback + Tailscale CGNAT by
default, so it's reachable from your LAN and over Tailscale, but not the public
internet. Adjust with `serve --http-allow-cidr`.

---

## 3. Point a node at it

A node is provisioned with the gateway's address at flash time (the same touch
that sets WiFi). For the reference Toit node, that's the
[`nodus`](https://github.com/davidg238/nodus) repo — flash a board and give it
`<gateway-host>:6969`. Once it boots and joins WiFi it polls the gateway, shows up
in `porta scan` / the dashboard, and starts reporting its chip and SDK.

porta itself is language-neutral: any node that conforms to
[`PROTOCOL.md`](PROTOCOL.md) works the same way.

---

## 4. First operations

Once a node is online (here, named `vin`):

```bash
porta scan                                   # see it online
porta device show -d vin                      # identity, observed state, counts

# turn on telemetry/log/print forwarding (absolute — all three required)
porta device set-forward -d vin --print on --log on --telemetry on --log-level info

# watch telemetry stream
porta monitor -d vin -f

# push a config value and watch it converge
porta device set -d vin sampler interval 60
porta device get -d vin sampler               # desired vs observed

# deploy a prebuilt image
porta container install -d vin sampler ./sampler.bin --trigger boot --lifecycle run-loop
porta container list -d vin
```

The v2 forms of every command above are in the [CLI reference](CLI.md), which is current;
the v1 spellings in this section are not.

---

## 5. The operator surface

**There is no web dashboard.** v1's htmx console — fleet page, node detail,
telemetry, command log, status page — was deleted with the rest of v1, and so
were `GET /api/status`, `GET /api/nodes` and the read-only MCP server. Their
fate is recorded as **deferred → operator surface** in the capability audit
(`docs/design/2026-08-01-porta-space-sync-rewrite-design.md` §4.2), not as a
silent drop: a rewrite may delete a mechanism, but it may not delete a
capability.

What that audit turned up is worth knowing before anyone rebuilds it: the status
page's wifi-vs-Thread breakdown was computed from `nodes.source_addr`, a column
with no successor in the new schema until it was enumerated. It came back as
`nodes.last_addr`, stamped by the UDP loop from the datagram's peer address — so
the data a future status page needs is being recorded now, rather than waiting
for the page to notice it is missing.

Until that page exists, the operator surface is `GET /health` (§2) plus the CLI
reading the database directly.

---

## Building from source / contributing

```bash
git clone https://github.com/davidg238/porta && cd porta
go build ./...        # needs CGO (C toolchain) for the sqlite driver
go test ./...
```

Repo layout: `cmd/porta/` is the entrypoint; `internal/` holds the gateway
(`store`, `command`, `handler`, `tftp`, `apisrv`, `web`, `mcpsrv`, `portacli`);
`devsdk/` is the public Go surface for node-repo tooling. Design docs live in
`docs/` (`ARCHITECTURE`, `PROTOCOL`, `DEVSDK`) with approved designs in
`docs/archive/specs/` and implementation plans in `docs/archive/plans/`.

The wire protocol ([`PROTOCOL.md`](PROTOCOL.md)) is the one fixed point every
node conforms to — change it deliberately.
