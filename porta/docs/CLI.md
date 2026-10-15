# porta CLI reference

The `porta` CLI is one binary. **Every command opens the SQLite store directly** (`--db`,
default `porta.db`); there is no client mode, no server to point a command at, and no HTTP
API to talk to. `serve` is the only command that additionally binds listeners.

**Verbs live only at the CLI.** The store has none and the wire has none (design D2), so
the whole operator vocabulary below is sugar over two operations: a write to a path and a
read of a tree. Adding a feature adds a path — not a verb, not a table, not an endpoint.

> **This page is gated.** `porta/docs/cli-gate.sh` builds the binary, walks its own
> `--help` tree, and fails if a command or flag named here does not exist or if one that
> exists is not named here. The nsl suite runs it as the `porta-cli` aux gate.
> **Names only** — the gate does not score prose, and it cannot: a stale sentence is a
> judgement call, a phantom flag is a bug. The transcripts on this page were captured by
> hand on **2026-08-29** against `a74d3f2` and are dated evidence, not executed blocks.
> If you want an *executed* introduction, read
> [`../../docs/tutorials/03-porta.md`](../../docs/tutorials/03-porta.md), whose every
> command block the suite runs. The one edit made to the transcripts: the absolute store
> path in each `# store` line is shortened to `/srv/porta/porta.db`.

## The command tree

```
$ porta --help
porta — northbound gateway for nodus-style nodes

Usage:
  porta [command]

Available Commands:
  blob        Bulk-plane image blobs
  completion  Generate the autocompletion script for the specified shell
  get         Dump a node's cell tree
  help        Help about any command
  key         Signing keypair for image push
  log         Show the write log for a node, newest first
  put         Write a value to a node's goal tree
  serve       Run the porta gateway: UDP space listener + operator HTTP surface
  snapshot    Write a consistent, immutable-openable copy of the store (VACUUM INTO)

Flags:
      --db string   SQLite database path (default "porta.db")
  -h, --help        help for porta
  -v, --version     version for porta
```

Seven commands do the work — `serve`, `put`, `get`, `log`, `key gen`, `blob put`,
`snapshot` — plus cobra's own `help` and `completion`.

## Global flags

| Flag | Default | Notes |
|------|---------|-------|
| `--db <path>` | `porta.db` | SQLite store. Persistent across the whole tree, so `porta --db X get n` and `porta get --db X n` are the same command. **Every** command opens it, including the read-only ones. |
| `--version` (`-v`) | — | `porta version dev` for a plain `go build`; a release build appends the short commit, injected by `deploy/build-deb.sh`. |

## Two rules that apply to every command

**A read never manufactures a store.** `serve` creates the database; everything else
refuses to. An empty answer from a store that does not exist and an empty answer from a
quiet fleet used to print identically:

```
$ porta get --db ./nope.db c6bench
Error: no store at ./nope.db (porta serve creates one; pass --db naming the live store)
```

**Every read names the store it answered from**, on stderr so it cannot pollute output a
gate parses (rete#221):

```
$ porta get --db ./porta.db c6bench
# store /srv/porta/porta.db
  goal/name                        seq=1      boot=-    "c6bench"
```

"No rows yet" and "wrong database" are the same three characters on stdout, which is why
the line exists. `get`, `log` and `snapshot` print it. **`put` does not** — the write door
is silent about which store it wrote to, which is #283.

---

## serve

Run the gateway. Four listeners come up, and they are independent: a node keeps checking
in with `--http-port 0`, and `/health` stays up while the mesh is silent.

```
porta serve [--udp :6969] [--http-port 6970] [--http-bind 0.0.0.0]
            [--http-allow-cidr …] [--manifest path/to/MANIFEST.cddl]
```

| Flag | Default | Notes |
|------|---------|-------|
| `--udp` | `:6969` | Node-facing space listener — a **listen address**, not a bare port. |
| `--http-port` | `6970` | Operator HTTP port; `0` disables the listener and the gateway still serves nodes. |
| `--http-bind` | `0.0.0.0` | HTTP bind address. |
| `--http-allow-cidr` | RFC1918 + loopback + Tailscale `100.64.0.0/10` | Repeatable allow-list for the HTTP listener; empty serves any peer. |
| `--manifest` | (none) | Path to `protocol/MANIFEST.cddl`. Arms the kind+schema gate, **persists it into the store**, and serves it at `GET /manifest`. |

```
$ porta serve --db ./porta.db --udp 127.0.0.1:36969 --http-port 36980 \
              --manifest ~/workspace/rete/protocol/MANIFEST.cddl
porta: manifest gate ON (…/protocol/MANIFEST.cddl, sha256 96e29343…) — persisted to the store
porta: serving the space on udp 127.0.0.1:36969 (db=./porta.db)
porta: serving blobs on udp 127.0.0.1:36970
porta: sweeping for silent nodes every 30s
porta: serving HTTP on 0.0.0.0:36980
```

Read those five lines as the listener inventory:

- **the space** on `--udp`: the pull/push exchange, CBOR over plain UDP.
- **the bulk plane** on `--udp` **+ 1**, always — the block server for image blobs is a
  separate stateless listener, never multiplexed onto the space socket. A *bind* failure
  here degrades rather than kills: the exchange is the fleet's lifeline, so porta logs
  `BLOCKS PLANE DOWN`, records the reason in `/health`, and serves nodes without it
  (rete#176).
- **the liveness sweep**, every 30 s: porta's own verdict about which nodes have gone
  quiet, written to `gw/liveness`. A ticker and not a read-time view, because the failure
  it must catch is porta hearing nothing — and a view that evaluates only when read is
  silent exactly when everything else is.
- **the operator HTTP surface** (below).

**The manifest gate is persisted, not merely armed.** `put` is its own process against the
same database, so arming the gate only inside `serve` would leave the CLI door open — the
shape of both #180 and #206. Once a `serve` has stored it, every later `Open` of that
store gates, including a flagless one, and a flagless `serve` says so:

```
porta: MANIFEST GATE OFF for this flagless serve — no --manifest given; a manifest already
stored in the db still gates (rete#243)
```

### The HTTP surface

Two routes, and there is no third. `GET /health`:

```
$ curl -s http://127.0.0.1:36980/health
{"nodes":0,"status":"ok","uptime_s":2,"version":"dev"}
```

`nodes` is the row count in the `nodes` table. **`-1` is not an error state** — it means
the count was unavailable, and the response is still `200`, because the endpoint reports
that the *listener* is healthy, not the store. The body is in-memory stats only, no
`PRAGMA` and no `stat`, so it is safe to poll on any interval.

`GET /manifest` returns the CDDL porta is enforcing, with its digest in an
`X-Manifest-SHA256` header so a fetcher can refuse an endpoint that lies about its own
bytes (rete#243 D5/D11). Without `--manifest` on this process the route is a `404`.

v1's dashboard, `GET /api/*` JSON and read-only MCP server are gone. Their fate is
recorded as **deferred → operator surface** in the capability audit
(`design/2026-08-01-porta-space-sync-rewrite-design.md` §4.2), not as a silent drop.

### Test-only flags

These exist so a gate can reproduce a gateway failure on demand. They are named on the
scenario manifests in `nodus/oevm/tests/target/` that use them, and nowhere else.

| Flag | What it does |
|------|--------------|
| `--test-refuse-all` | Answer every datagram `#err` at the door, ingesting nothing — rete#232's refusing gateway. Gated by `porta-refuse`: a refused node must survive past the 157 s death record. |
| `--test-drop-all` | **Drop** every datagram at the door: no reply at all — rete#269's silent gateway. Gated by `porta-silent`: a node whose uplink is gone must survive it. |
| `--test-drop-ack` | Swallow every Nth outbound `#ack` *after* the push is committed, which is the ack-loss case the node must be idempotent under. |
| `--sweep-tick` | Re-form liveness verdicts on a shorter period. The host gate asserts a verdict, and the real 30 s tick outlasts the whole gate. |

---

## put

The general path write. There is no `device set`, no `device reboot` and no
`container install`; each of them was this command against a different path.

```
porta put <node> <path> <value> [--kind cell|stream] [--raw] [--reason …] [--actor …]
```

| Flag | Default | Notes |
|------|---------|-------|
| `--kind` | `cell` | `cell` (latest value wins) or `stream` (every write is an event). Kind is a property of the leaf, set by its first write — the manifest pins which is which. |
| `--raw` | off | The value is hex-encoded raw deterministic CBOR, validated as a **closed** cval encoding before it is stored. This is how records get written; a command line cannot type one. |
| `--reason` | (empty) | Why. Recorded in the event log. |
| `--actor` | the OS user | Who. Recorded in the event log. |

Without `--raw` the value is a scalar, and its type is inferred: `true`/`false` are
booleans, `nil` is nil, a leading `#` makes a Symbol, then integer, then float, then — the
honest fallback — a string.

```
$ porta put --db ./porta.db c6bench goal/name c6bench --reason 'cli reference'
wrote goal/name seq=1
$ porta put --db ./porta.db c6bench goal/config/sampler/interval 60 --reason 'cli reference'
wrote goal/config/sampler/interval seq=2
$ porta put --db ./porta.db c6bench goal/do/reboot --kind stream \
            --raw d81b82d827665265626f6f74a0 --reason 'cli reference'
wrote goal/do/reboot seq=3
```

That third write is v1's `porta device reboot`: `Reboot{}` at `goal/do/reboot`, a stream
because a reboot is an event and not a state.

**The manifest refuses the write, at this door as much as on the wire.** All three
refusals below are real, from the same session:

```
$ porta put --db ./porta.db c6bench goal/name bench --kind stream
Error: manifest:kind path=goal/name got=1 want=2 (rule name-cell)

$ porta put --db ./porta.db c6bench goal/cad/ms 4000
Error: manifest:path "goal/cad/ms" matches no manifest entry

$ porta put --db ./porta.db c6bench goal/do/reboot --kind stream '#now'
Error: manifest:head path=goal/do/reboot value does not start with reboot's record head (rule reboot)
```

Kind, path and record head. The first of those is the one that used to brick a node.

---

## get

Dump a node's **cell** tree — the latest value at each path. `goal/X` against `obs/X` is
the desired-versus-observed view *and* the self-heal query, because they are the same
diff. That is why there is no `device get`.

```
porta get <node> [prefix]
```

An optional path prefix narrows it; there are no flags.

```
$ porta get --db ./porta.db c6bench
# store /srv/porta/porta.db
  goal/config/sampler/interval     seq=2      boot=-    60
  goal/name                        seq=1      boot=-    "c6bench"
  gw/liveness                      seq=0      boot=?    #neverSeen

$ porta get --db ./porta.db c6bench goal/config
# store /srv/porta/porta.db
  goal/config/sampler/interval     seq=2      boot=-    60
```

Three things that transcript shows:

- **`goal/do/reboot` is absent.** It is a stream, and this is the cell tree. Streams are
  read with `log`.
- **`gw/liveness` is porta's own verdict** about the node, not the node's report and not
  the operator's intent. `#neverSeen` is what the sweep writes for a node that has been
  named but has never checked in.
- **`boot=` is on every row.** `boot=-` means porta wrote it, so no node boot can
  invalidate it; `boot=?` means the boot is unknown (a node with no persistent identity,
  or a row written before the column existed); a number is the node boot that published it.

That last column is a guard. A rebooted node rewrites its whole tree over several seconds,
and a read taken inside that window mixes this boot's values with the last one's. Rows
that predate the node's current boot are marked `~` and `STALE`, under a banner that goes
to **stdout**:

```
! MIXED READ: 3 of 14 rows predate boot 12 — see porta log --boot 12
```

(That one is the line's **format**, taken from `get.go` — not a capture. The session on
this page ran against a node that never checked in, so it has exactly one boot and could
not produce a mixed read.)

stdout rather than stderr because a warning a pipe eats is not a guard (rete#210 — it
nearly inverted a hardware finding). A value that will not decode prints
`<undecodable: …>` rather than nothing: a stored value that cannot be read is a fault, not
a display problem.

---

## log

The write log for a node, newest first, **in both directions and with provenance**. This
is v1's command audit widened: a drained command queue lost superseded intent, and an
append-only log keeps it.

```
porta log <node> [--limit 50] [--boot N]
```

| Flag | Default | Notes |
|------|---------|-------|
| `--limit` | `50` | Maximum rows. |
| `--boot` | (all) | Only rows the node published during boot **N**. |

```
$ porta log --db ./porta.db c6bench
# store /srv/porta/porta.db
2026-08-29T16:05:18-07:00 → goal/do/reboot               seq=3      boot=-    david        #Reboot{}
2026-08-29T16:05:18-07:00 → goal/config/sampler/interval seq=2      boot=-    david        60
2026-08-29T16:05:15-07:00 ◆ gw/liveness                  seq=0      boot=?    porta        #neverSeen
2026-08-29T16:04:47-07:00 → goal/name                    seq=1      boot=-    david        "c6bench"
```

The arrow is an authority claim:

| Mark | Direction |
|---|---|
| `→` | porta's intent, headed down to the node |
| `←` | the node's report, arrived from it |
| `◆` | a **gateway** verdict — written *about* the node, crossing no boundary, so it gets neither arrow |

`--boot` is answerable here and nowhere else. The log is append-only and every row records
its own boot; the cell tree keeps only the latest value per path and could never
reconstruct a past boot, which is why `get` has no such flag (rete#210).

---

## key gen

Generate the signing keypair for image push, **beside the database** —
`porta.key` (mode `0600`) and `porta.pub` in the same directory as `--db`. Run once.

```
porta key gen
```

```
$ porta key gen --db ./porta.db
pub bc705653bbef4c47c40ad7cfcf4cc015cec47f2182baf93d37ae0283f318adc3 (porta.pub)
```

The public half is what a node is provisioned with; the bridge refuses an `App` record
without a signature it can check (rete#25).

---

## blob put

Store an image on the bulk plane, sign its hash, and print the write that deploys it.

```
porta blob put <file.oimg>
```

The blob goes to `blobs/` beside the database, content-addressed by SHA-256, and the
signature is over the hash — signed at push time, by the key above. Without that key the
command refuses rather than storing an unsigned image:

```
$ porta blob put --db ./porta.db demo.oimg
Error: signing key: open porta.key: no such file or directory (run `porta key gen`)
```

With it:

```
$ porta blob put --db ./porta.db demo.oimg
hash c8cd6251c3f31ac8edd0f6780ca552b836b72c1c0d379868865bff65c4ec048f
sig 03652a5b68162606054007f88ff499c0490f0c10905004d6b489d9e05b635abc04515886adf37805…
porta put <node> goal/apps/main --raw d81b82d82763417070a2d82763736967584003652a5b…
```

The third line is the deploy: an `App{image: BlobRef{hash, size}, sig}` record at
`goal/apps/<name>`, ready to paste. **`blob put` writes nothing to any node's tree** — it
prints a `put` and leaves the decision to you. That is what keeps the bulk plane off the
space: the image travels on its own listener, and only a
hash-sized reference rides the exchange.

---

## snapshot

Write a consistent, self-contained copy of the store.

```
porta snapshot --out <file>
```

| Flag | Default | Notes |
|------|---------|-------|
| `--out` | (required) | Destination file. Must not already exist. |

```
$ porta snapshot --db ./porta.db --out snap.db
# store /srv/porta/porta.db
snapshot written: snap.db
```

This is the porta↔custos interface. `VACUUM INTO` yields a compacted single file off a
live WAL database **without blocking the writer**, and the result is genuinely immutable:
custos opens it with `immutable=1`, which is true of a snapshot and false of a live
store, so the boundary is enforced by SQLite rather than promised by convention. porta
owns the store, so producing the snapshot is porta's job.

---

## What is not here

v1's verb set — `scan`, `ping`, `monitor`, `device show|get|set|set-forward|reboot`,
`container list|install|uninstall`, `debug`, `profile` — was deleted on 2026-08-01 with
the command queue it wrote to. **A rewrite may delete a mechanism, but it may not silently
delete a capability** (design D8), so every one of those verbs has a recorded fate in the
§4.2 capability audit. The short version:

| You want | Now |
|---|---|
| `device get <app>` | `porta get <node> goal/config/<app>` against `porta get <node> obs/config/<app>` — the diff *is* the answer |
| `device set <app> <key> <v>` | `porta put <node> goal/config/<app>/<key> <v>` |
| `device reboot` | `porta put <node> goal/do/reboot --kind stream --raw …` |
| `device set-forward` | one write to `goal/forward`, still absolute: the whole policy, never a patch |
| `container list` | `porta get <node> obs/apps` |
| `container install` | `porta blob put`, then the `goal/apps/<name>` write it prints |
| `container uninstall` | `goal/apps/<name>` with its lifecycle written to stopped — there is no tombstone |
| `log -d` | `porta log <node>`, with `actor`/`reason` v1 could not record |
| `debug` / `profile` | writes to `goal/debug/<app>` and `goal/profile`; the sessions are `dbg/req` / `dbg/resp` lines |
| `scan`, `ping`, `monitor` and its tail | **deferred → operator surface.** The data is recorded (`nodes.last_seen_ms`, `exchanges`, `gw/liveness`, `tel/**`); the view that presents it is not built |

## The other program

custos is the exposed, agent-facing front door and a separate program — since rete#338
not part of porta's tree at all, but `../../custos/`, its own Go module. It serves the dev
status page and the repo docs tree at `/dev`, the fleet pages and named queries over
porta's store read-only, and proxies `/health` and `/manifest` to porta's sheltered
operator listener; `custos sql|tables|describe` are the same query core on the command
line. Default port `6971`; [GETTING-STARTED.md](GETTING-STARTED.md) §5 has the flags and
routes. The query surface lands there, never in porta.

## See also

- [`../../docs/tutorials/03-porta.md`](../../docs/tutorials/03-porta.md) — the executed
  introduction: a gateway and a real node on loopback, run by the nsl suite.
- [`GETTING-STARTED.md`](GETTING-STARTED.md) — installing the `.deb`, the systemd unit,
  and the operator-surface story. §3–§4 are still v1 (#279).
- [`../../protocol/PROTOCOL-NSL.md`](../../protocol/PROTOCOL-NSL.md) and
  [`../../protocol/MANIFEST.cddl`](../../protocol/MANIFEST.cddl) — the wire contract the
  gate above enforces. §§1–4 are the conformance target.
- [`design/2026-08-01-porta-space-sync-rewrite-design.md`](design/2026-08-01-porta-space-sync-rewrite-design.md)
  — D1–D8, the store schema, and §4.2's audit in full.
- [`README.md`](README.md) — the map of this directory.
