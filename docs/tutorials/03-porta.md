# Tutorial 3 — porta on your host: a gateway, a node, and the diff between them

No board, no radio. By the end you will have run a gateway, written intent for a node
that does not exist yet, brought a real node up against it over UDP, watched the two
trees converge, read the write log in both directions, and watched the wire contract
refuse a write that does not fit.

**This page is executed.** Every block marked `bash gate` below is extracted and run, in
order, in a fresh empty directory, by `docs/tutorials/gate.sh` — which the nsl suite runs
as its `tutorial-porta` aux gate. If a command here stops working, the suite goes red.
That is the only reason to trust a tutorial that is more than a week old.

## 0. What you need

- Everything tutorial 1 needed (`gcc`, `python3`, `node`), plus **Go 1.26**.
- `$RETE` pointing at your clone, as in tutorial 1:

```bash
export RETE=~/workspace/rete
```

- **Loopback ports 16969 and 16970 free.** The node's gateway address is a source
  constant — `gwAddr`/`gwPort` in `nodus/oevm/zephyr/PortaBridgeNode.nsl` — so this page
  cannot pick a different port, and it cannot run at the same time as
  `nodus/oevm/tests/porta_sync_host.sh`, which uses the same pair. (Who decides a node's
  address and identity is genuinely undecided; it is issue #118.)

## 1. The two halves

`porta` is the northbound half: the gateway an operator talks to. The node is the device
end. **They are coupled only over the wire** — neither imports the other's code, and
`protocol/` is the whole of the contract. Everything below runs both halves on one host,
which is exactly what makes the coupling visible: two independently written programs, one
protocol.

Build the gateway:

```bash gate
WORK=$PWD
(cd $RETE/porta && go build -o "$WORK/porta" ./cmd/porta)
./porta --version
```

porta's own suite, which this page does not run for you:

```bash
cd $RETE/porta && go test ./... && gofmt -l .
```

## 2. A space, and a gateway to hold it

The unit of coupling is a **space**: every node owns a tree of paths, and porta holds a
mirror of that tree. Operators write intent under `goal/**`; the node writes what it sees
under `obs/**`, `tel/**` and `sys/**`. **There are no verbs on the wire and none in the
store** — `put` is a path write, `get` is a tree read, `log` is a query over the event
log, and that is the entire operator vocabulary.

```bash gate
./porta serve --db tut.db --udp 127.0.0.1:16969 --http-port 0 \
              --manifest $RETE/protocol/MANIFEST.cddl > serve.log 2>&1 &
PORTA_PID=$!
trap 'kill $PORTA_PID ${NODE_PID:-} 2>/dev/null || true' EXIT
for _ in $(seq 1 100); do grep -q "serving the space" serve.log && break; sleep 0.1; done
grep -q "serving the space" serve.log
cat serve.log
```

```
porta: manifest gate ON (…/protocol/MANIFEST.cddl, sha256 96e29343…) — persisted to the store
porta: serving the space on udp 127.0.0.1:16969 (db=tut.db)
porta: serving blobs on udp 127.0.0.1:16970
porta: sweeping for silent nodes every 30s
```

The flags, in the order they matter:

- `--db` is the store: four sqlite tables (`nodes`, `cells`, `events`, `exchanges`).
  **`serve` creates it; `get`, `put` and `log` refuse to** — a read that silently created
  an empty store would answer "no rows yet" and "you named the wrong database"
  identically.
- `--udp` is the node-facing exchange. The bulk plane binds itself one port up, which is
  why 16970 is spoken for; it is a separate stateless listener and is never multiplexed
  onto the space socket.
- `--http-port 0` turns the operator HTTP surface off for this page. A node keeps
  checking in either way — the two listeners are independent.
- `--manifest` arms the kind+schema gate, which is §7. Without it porta says so loudly:
  a contract that is off quietly is a contract nobody notices is off.

The fourth log line is not about a flag at all: porta forms its own verdict on which
nodes are still talking, on a ticker rather than at read time, because the failure it has
to catch is porta hearing nothing — and a view that evaluates only when read is silent
exactly when everything is. The end of this page says where that verdict lands.

The trap is so that a failed step below does not leave a gateway holding the port.

## 3. Intent for a node that has never spoken

```bash gate
./porta put --db tut.db tut-node goal/name bench --reason 'tutorial 3'
./porta get --db tut.db tut-node
./porta log --db tut.db tut-node
```

```
wrote goal/name seq=1
# store /…/tut.db
  goal/name                        seq=1      boot=-    "bench"
# store /…/tut.db
2026-08-29T12:53:19-07:00 → goal/name          seq=1      boot=-    david        "bench"
```

Nothing has ever checked in as `tut-node`, and the write is still accepted. **It is not
queued** — there is no queue anywhere in this system. It is a value at a path in a tree,
and the node collects it whenever it turns up, which is the same thing that happens if the
node was already listening. Reboots, first contact and steady state are one code path.

Notice:

- `# store /…` goes to **stderr**, on every read. A correct-looking empty answer must
  still say which file it came from.
- `boot=-` on an outbound row: porta's own intent belongs to no node boot, so no node
  reboot can make it stale. Inbound rows carry the boot they were published in, and `get`
  marks the ones that predate the node's current boot rather than mixing two boots
  silently.
- `→` is direction: out to the node. Every event also records the **actor** (your OS user
  here) and a `--reason`. The reason is stored on the row but no surface prints it yet —
  that is issue #280.

## 4. A real node

The node is `nodus/oevm/zephyr/PortaBridgeNode.nsl`: 260 lines of nsl that bind a socket,
hold one bridge, and on each tick check in and publish what the node observes. There is no
protocol handler in it, and nothing else is on the node on purpose — a second concern would
make a failure ambiguous.

It runs on the host VM from tutorial 1. Running any test on Path B is what builds that VM
and captures the object layout its images must match, so this line both checks your
toolchain and leaves `/tmp/oevm_img` and `/tmp/oevm_layout.json` where the next block
needs them:

```bash gate
python3 $RETE/nodus/nsl/tools/run-one.py $RETE/nodus/tests/01_expressions/01_addition.nsl B
```

```
Path B: PASS  Output matches expectations.
```

Compile the node to an image and start it:

```bash gate
python3 $RETE/nodus/nsl/tools/ns_compile_oevm.py \
        $RETE/nodus/oevm/zephyr/PortaBridgeNode.nsl node.oimg \
        --format=oimg --layout=/tmp/oevm_layout.json
OEVM_TIMER_WALLCLOCK=1 OEVM_BOOTID_FILE=$PWD/node-boot OEVM_NODE_ID=tut-node \
  /tmp/oevm_img node.oimg > node.log 2>&1 &
NODE_PID=$!
for _ in $(seq 1 100); do grep -q "PORTA NODE READY" node.log && break; sleep 0.1; done
grep -q "PORTA NODE READY" node.log
```

```
OIMG written to node.oimg (223080 bytes, entry=PortaBridgeNode)
```

The three environment knobs are all seams a board fills differently:

- `OEVM_TIMER_WALLCLOCK=1` — the host VM's clock is **virtual** by default, so the suite
  can advance time by an exact amount. A check-in cadence is real time, so this node needs
  the real clock.
- `OEVM_BOOTID_FILE` — the persistent boot counter a board keeps in NVS. Without it porta
  prints `boot=?`, which means *this node has no persistent identity*, not *boot zero*.
- `OEVM_NODE_ID` — overrides `platform nodeId`, the same explicit door `zephyr/run.sh`
  uses on a board.

## 5. What the node says about itself

A helper, because the gate cannot guess how long a check-in takes. Typing along, just wait
a couple of seconds and run `porta get` yourself:

```bash gate
await() {
  for _ in $(seq 1 100); do
    ./porta get --db tut.db tut-node | grep "$1" >/dev/null && return 0
    sleep 0.2
  done
  return 1
}
await obs/health
./porta get --db tut.db tut-node obs
```

```
  obs/build      seq=2   boot=1  #Build{#chip: #host, #build: "host"}
  obs/health     seq=9   boot=1  #Health{#up: 2002, #bid: 1, #pto: 0, #ref: 0, #rej: 0, #sdr: 0, #wak: 1}
  obs/liveness   seq=6   boot=1  #Liveness{#bud: 16000, #cad: 2000}
  obs/name       seq=12  boot=1  "bench"
```

Nobody asked for any of this. **An idle check-in is the heartbeat**, so a node that has
nothing to report still says so, and `obs/health` is that report. Every field in it is
liveness or delivery and nothing else: `up` is uptime in ms, `bid` the boot id, `wak` how
many times the node has woken, `pto` pushes that timed out, `ref` exchanges the gateway
answered `#err` to, `rej` inbound entries **this node** dropped as malformed or out of its
authority, and `sdr` entries the bridge dropped rather than send. `ref` and `pto` are
deliberately not one counter: a gateway refusing everything still replies to every
exchange, so `pto` reads 0 for a node whose data is not landing at all.

`obs/liveness` is the node's own promise about its silence: a 2 s cadence and a 16 s
budget, which is eight times the cadence. **The node picks both numbers**, because the
node is the only party that knows its own loss rate — and porta forms its verdict from
what the node declared rather than from a constant of its own.

Those `#Health{…}` values are records, not strings. They rode as deterministic CBOR built
by the node's own codec and decoded here by a hand-rolled one written from the record
catalogue — two independent implementations of `protocol/PROTOCOL-NSL-RECORDS.md`. A shape
either side got wrong shows up as an undecodable row, here, and not on a bench six weeks
later.

## 6. Convergence is a diff, not a mechanism

The `goal/name` you wrote in §3, before this node existed, has arrived:

```bash gate
await obs/name
./porta get --db tut.db tut-node goal
./porta get --db tut.db tut-node obs/name
```

```
  goal/name      seq=1   boot=-  "bench"
  obs/name       seq=12  boot=1  "bench"
```

That is the whole argument for the design. There is no reconcile loop, no convergence
engine, and no `converged` flag: `goal/X` against `obs/X` is a tree diff, so the
desired-versus-observed view an operator reads and the self-heal query a controller runs
are **the same query**. The node's whole side of it is two lines of nsl, and neither is a
protocol handler — one is an ordinary channel reaction, the other an ordinary write:

```
(space channel: 'goal/name') whenever: '' do: [ :v | self nameChanged: v ].

nameChanged: v = ( space put: 'obs/name' value: v. ^ self )
```

(The reaction body does nothing but send to `self`, which is not style: a cross-class call
inside a reified block cannot resolve its outer chain — issue #133.)

The log now runs both ways:

```bash gate
./porta log --db tut.db tut-node --limit 20
```

```
… ← tel/log       seq=13  boot=1  tut-node  #Log{#lvl: #info, #txt: "tick 1"}
… ← obs/name      seq=12  boot=1  tut-node  "bench"
… ← obs/health    seq=9   boot=1  tut-node  #Health{…}
… ← sys/metrics   seq=7   boot=1  tut-node  #Metrics{…}
… ← obs/build     seq=2   boot=1  tut-node  #Build{…}
… → goal/name     seq=1   boot=-  david     "bench"
```

`←` is the node writing, `→` is you. One append-only table holds both, so "what did this
node do" and "what did we ask of it" are one query — and superseded intent survives in it,
which a drained command queue could never manage.

`tel/log` is where the other half of the model shows: a leaf is a **cell** or a
**stream**, decided by its first write. `obs/health` is a cell — latest value wins, and a
replay from cursor 0 after a reboot is harmless because writing the latest value twice is
the same as writing it once. `tel/log` is a stream — every entry is delivered exactly
once, and porta will not re-serve a row the node has already acked, which is what stops
`goal/do/*` re-executing across a reboot.

## 7. The wire contract says no

`--manifest` armed `protocol/MANIFEST.cddl`, which pins every path to a kind, a class, a
direction and a shape. Write `goal/name` as a stream instead of a cell and the write is
refused at the door — by name, with the rule that refused it:

```bash gate
if ./porta put --db tut.db tut-node goal/name bench --kind stream 2> refused.log; then
  echo "expected the manifest gate to refuse this"; exit 1
fi
grep -q 'manifest:kind' refused.log
grep -m1 -o 'manifest:kind.*' refused.log
```

```
porta: manifest:kind path=goal/name got=1 want=2 (rule name-cell)
```

The manifest is loaded once and **persisted into the store**, so every later `porta put`
against this database gates too, whether or not the flag is repeated — the CLI doors are
separate processes over the same file, and that is where mis-kinded writes have historically
got in.

## 8. Stop

```bash gate
kill $NODE_PID $PORTA_PID
```

## Where next

- `protocol/PROTOCOL-NSL.md` — the space model, path namespaces, cells versus streams,
  the exchange. **§§1–4 are the conformance target** for a second implementation.
  `protocol/PROTOCOL-NSL-RECORDS.md` is the record catalogue and `protocol/MANIFEST.cddl`
  its machine-normative half.
- `nodus/oevm/tests/porta_sync_host.sh` — this page's claims as a gate, and then some:
  exactly-once delivery with no gaps, a profile window opened over the wire and closed by
  the node itself, a reboot note arriving as `sys/panic`. Run it standalone; it uses these
  same ports.
- **What silence looks like** is the one thing this page skips. porta sweeps for quiet
  nodes every 30 s and writes its own verdict to `gw/liveness` — `#live`, `#silent`,
  `#neverSeen` or `#unknown`. The verdict is formed from the budget the node declared in
  `obs/liveness` and from no constant of porta's own: `now - last_seen > bud → #silent`,
  one line with no numbers in it. Kill the node, wait out its 16 s budget and the next
  sweep, and `porta get --db tut.db tut-node gw` turns over:

  ```
  porta: tut-node is #live   —   947 ms since its last exchange, declared budget 16000 ms
  porta: tut-node is #silent — 16946 ms since its last exchange, declared budget 16000 ms
  ```

  Worth doing once by hand. It is not a gate block here because the waiting, not the
  gateway, would be most of this page's runtime.
- `porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md` — why the store has
  four tables where v1 had nine, and the audit that gave every deleted v1 verb a fate.
- Tutorial 1 is the VM on your host. `docs/tutorials/02-board.md` is tutorial 2 — the same
  bridge node, but on a real ESP32-C6 over a real 802.15.4 radio: the board's two adapters,
  the Thread overlay whose absence is silent, and the eleven target gates that cover the
  timer seam neither this page nor tutorial 1 can reach. It is **prose, not executed**, and
  says so at the top; `nodus/oevm/tests/target/` is the executable half.
