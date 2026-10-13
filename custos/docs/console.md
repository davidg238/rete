# The console collector (custos, peer design D16)

custos reads every bench board's console all the time and writes each line to
`~/.local/state/rete/console/console-<board>-YYYY-MM-DD.log` (local date), as
`<isoformat ms ±HH:MM> <text>`. The collector parses nothing on the way in; the
verbs below parse on the way out. It replaced `nodus/oevm/zephyr/tools/console/`
(rete#314); that tool's README is archived at
[`nodus/oevm/docs/archive/console-tools-README.md`](../../nodus/oevm/docs/archive/console-tools-README.md).

## Running it

One unit serves every board: [`deploy/systemd/rete-custos.service`](../deploy/systemd/rete-custos.service),
its install steps in its header. The boards it serves are named in
`~/.config/rete/custos.env` (`RETE_CUSTOS_ARGS=--board c6 --board c6spare`), and a
bare `--board <name>` reads its cable from `~/.config/rete/console-<name>.env`
(`RETE_CONSOLE_PORT=/dev/serial/by-id/…`). The cable lives in a bench file, never
in a tracked one, because a by-id hash names one physical adapter on one bench.
A board that logs over the mesh (`log_udp.c`) is `--udp-log <name>=<udp port>`.

A node porta knows (`--porta-node <name>=<node-id>`) gets its console here once custos is
given `--advertise <ipv6>`, this host's address as the nodes reach it (rete#415). custos
binds the node a sink on a port of its own choosing and writes `goal/console =
Console{addr, port}` through porta, so the node tees its console to this custos from its
next check-in. A sleeping node does so on every wake, because the cell is replayed. The
node's console verbs below are then served, and with a `--map`, so is `profile`: its rows
come back on this console, never on the space. Whichever custos wrote the cell last is the
one that hears the node. A custos that has stopped leaves its address in the cell, and the
node keeps sending to a port nobody binds until another custos writes it.

## The verbs, under `/board/<name>/`

| verb | answers |
|---|---|
| `GET log?since=-180s` / `?tail=50` | the stamped lines, as JSON |
| `GET runs` | one row per boot (`*** Booting Zephyr OS`), with its build id |
| `GET panics` | one row per `ZEPHYR FATAL ERROR` and its `#CD:` coredump lines |
| `GET panics/<n>/bt` | panic n resolved against its build's archived ELF, or refused |
| `GET health` | the cable, from memory: `up`, `down` (and why) or `lent` |
| `POST lend?s=<N>` / `POST return` | close the cable for a flash; back after N s or on return |

The shell callers (`run.sh --capture`, `flash-bell.sh`, the target harness) reach
these through `nodus/oevm/tools/bench-identity.sh` (`bench::collector_state`,
`bench::lend`, `bench::return`), at `$RETE_CUSTOS` (default `http://localhost:6971`).

## Why the cable is held all the time

On the C6, flashing goes over the native USB JTAG port (`ttyACM*`) and the
console is a separate CP2102N bridge (`ttyUSB*`) that stays enumerated across
target resets (design §3, #57). Holding it means a panic that fires before you
thought to look is still captured, the failure that lost the first AMG8833 panic.

**Reopening the CP2102N does not reset the C6** (witnessed 2026-08-04). On the
plain esp32 and the MagTag any open reboots the board. It is a property of this
adapter wiring: do not carry it to another board without witnessing it there.

A flash that needs the cable borrows it with `lend` rather than stopping custos,
which would stop every board's collection. esptool and a reader on the same tty
split the ROM handshake (`device reports readiness to read but returned no data`),
a race, not a hard failure. The lend's ceiling gives the cable back if the
flashing script dies.

## Backtraces refuse rather than guess

`bt` resolves the ELF from the run's own `oevm: build-id` line in
`$RETE_ELF_ROOT/<build-id>/` (default `~/.local/state/rete/elf/`, written at every
flash by `nodus/oevm/zephyr/archive.sh`) and **refuses** (422) when the archive has
no match (design D4). It never falls back to `build-*/zephyr/zephyr.elf`: a
confident wrong source location is worse than no answer, because it looks like one.
`?assume=<build-id>` is the one way past, for a run that faulted before its banner
printed; it must be typed, and the answer carries the assumed id.

The same build-id directory holds the image map (`.oimg.map.json`), so a profile
taken today stays readable after the board is reflashed twice: point
`custos profile -map` at the archived map, not at whatever map sits in
`nodus/oevm/zephyr/` (the LAST build's; a wrong map resolves nothing while
looking like a quiet program).

Two things are easy to get wrong, each with an override:

- **The Zephyr venv's Python** (`RETE_ZEPHYR_PYTHON`, default
  `~/zephyr-esp32/.venv/bin/python`): `scripts/coredump/` needs `pyelftools`.
- **`riscv64-zephyr-elf-gdb`** from the SDK (`RETE_GDB`). Never the host `gdb`,
  which gives plausible and wrong output for a RISC-V core.

The coredump stub is driven over `--pipe`, not a TCP port: it accepts exactly
once, so a probe connection would consume the one gdb needs.

## Health must be able to fail both ways

A heartbeat is the collector writing about *itself*: it proves the process and
says nothing about the pipeline. On 2026-08-03 a C6 swap changed the by-id hash,
the port went away, and a heartbeat-only check said `ok` for seven minutes (#147).
So `health` answers the cable's state, and an unreachable custos is its own answer
(`bench::collector_state` says `unserved`). Falsify both: stop the unit
(`unserved`), then with it running unplug the adapter (`down`).

## Reading the markers

Lines beginning `@@rete-console@@` are the collector's, not the device's:

| marker | means |
|---|---|
| `collector started pid=… port=…` | custos took the cable |
| `heartbeat port=up` / `port=down` | every 60 s: alive, and whether it has input, board silent or not |
| `port lost: …` / `port reacquired` | once per transition, the OS reason verbatim |
| `cable lent` / `cable back` | a flash borrowed the cable |
| `pruned …` | a day deleted to stay under the 2 GiB budget (never the newest day) |
| `udpsink started …` / `peer <addr>` | a mesh board's socket, and each new sender |
| `collector stopped` / `udpsink stopped` | custos let go |

The heartbeat is what carries the port state across midnight: a loss recorded in
yesterday's file is invisible to anything reading today's. Every health check
asserts a recent heartbeat is present, never the absence of an error.
