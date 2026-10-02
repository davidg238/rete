# Console collector and offline reader (#56)

`collect.py` holds the C6 console port and writes everything it sees to
`~/.local/state/rete/console/console-<board>-YYYY-MM-DD.log`. `logread.py`
reads those logs offline. The collector parses nothing; the reader parses
everything (design D3).

Design: `docs/design/2026-08-03-device-console-collector-design.md`.
Plan: `docs/superpowers/plans/2026-08-03-device-console-collector.md`.

## Install

    mkdir -p ~/.config/rete
    cp console-c6.env.example ~/.config/rete/console-c6.env
    ls -l /dev/serial/by-id/          # confirm the hash in that file is yours

    cp rete-console@.service ~/.config/systemd/user/
    systemctl --user daemon-reload
    systemctl --user enable --now rete-console@c6

`Linger=yes` is already set for this user, so it starts at boot and survives
logout. Check it with `systemctl --user status rete-console@c6`.

The port lives in the env file rather than the unit because a
`/dev/serial/by-id/` hash names one physical adapter on one bench. The unit
is tracked; the hash is not. A missing env file fails the unit loudly, which
is deliberate — falling back to a default port would mean collecting from
whatever enumerated first.

## Why the collector holds the port permanently

On the C6 there is no conflict: flashing goes over the native USB JTAG port
(`ttyACM*`) and the console is a separate CP2102N bridge (`ttyUSB*`) that stays
enumerated across target resets (design §3, #57). Holding it costs nothing and
means a panic that fires before you thought to look is still captured — the
exact failure that lost the first AMG8833 panic.

**Restarting the unit does not reset the C6.** Witnessed 2026-08-04 08:08: the
collector dropped and reopened the CP2102N and the target neither rebooted nor
printed a banner. That is the opposite of the flash-port rule on the plain esp32
and the MagTag, where any open — `stty` included — reboots the board, and it is
what makes this daemon restartable while the board is doing something you care
about. It is a property of *this* adapter wiring; do not carry it to another
board without witnessing it there.

This is **C6-only**. The MagTag and the nRF dongle both need design work first
(§10), and the plain esp32's CP210x port is both flash port and console, so a
permanent holder there would fight `--flash`.

## Everyday use

    ./logread.py tail -n 50
    ./logread.py runs
    ./logread.py panics
    ./logread.py symbolize --run 3

`--capture N` on `run.sh --board esp32c6` is a read of this log, not a serial
open. It shows the boot you just triggered even if it happened before you
asked.

## Symbolizing

    ./logread.py symbolize                 # last run with a panic
    ./logread.py symbolize --run 12
    ./logread.py symbolize --assume-build <key>

`symbolize` resolves the ELF from the run's own `oevm: build-id` line and
**refuses** if the archive has no match (design D4). It never falls back to
`build-*/zephyr/zephyr.elf`: a confident wrong source location is worse than
no answer, because it looks like one. Exit 5 is a refusal.

`--assume-build` is the one way past that, for a run that faulted before its
banner printed. It must be typed; nothing infers it. Every line of the result
is prefixed with the key that was assumed, so the assumption travels with the
output. `~/.local/state/rete/elf/last-flashed-c6` will tell you what was
flashed last — read it yourself, the tool will not.

**A profile can be resolved later by build id.** `archive.sh` now copies the
`.oimg.methods.json` offset→name map and the `.stmap` source map into the same
`~/.local/state/rete/elf/<build-id>/` directory it already keeps the ELF in —
one build-id-keyed symbol store, not two. So a capture taken today stays
readable after the board has been reflashed twice: take the `oevm: build-id`
line out of the run and point `mprof_resolve.py` at that directory's sidecar
rather than at whatever `.oimg.methods.json` happens to be sitting in
`nodus/oevm/zephyr/` (that one is the LAST build, not necessarily this run's,
and a map from a different build resolves nothing while looking identical to a
quiet program). If a Model B bake's sidecars are missing, `archive.sh` fails
and names the file rather than archiving less than it claims.

`symbolize` needs two things that are easy to get wrong:

- **The Zephyr venv's Python**, because `scripts/coredump/` imports
  `pyelftools`, which is not in the system interpreter. Located automatically
  at `~/zephyr-esp32/.venv/bin/python`; override with `RETE_ZEPHYR_PYTHON`.
- **`riscv64-zephyr-elf-gdb`** from the SDK, found on `PATH` or under
  `~/zephyr-sdk-*/`; override with `RETE_GDB`. It will not fall back to the
  host `gdb`, which produces plausible and wrong output for a RISC-V core.

The stub is driven over `--pipe`, not a TCP port. It does `listen(1)` +
`accept()` exactly once, so anything that opens a probe connection first
consumes the one gdb needs and the session dies in packet errors.

## Health

    ./check.py ; echo $?        # 0 = alive and attached, 1 = down, wedged or orphaned

Two assertions, and it takes both:

- **a heartbeat newer than 180 s** — three missed beats, so one slow write is
  not a false alarm. A positive assertion; device traffic does not count,
  because a chatty board proves the *board* is alive, not that the collector is
  still writing.
- **the port is attached** — the collector's own most recent statement about
  its port is not `down`.

The second one is #147, and it is the mirror of the first: a heartbeat is the
collector writing about *itself*, so it proves the process and says nothing
about the pipeline. On 2026-08-03 a C6 swap changed the `/dev/serial/by-id/`
hash, the port went away at 21:04, and this check printed `ok: collector alive`
for seven minutes. **A health check that reads only the process proves the
process, not the pipeline.**

To confirm it can actually fail, both ways:

    systemctl --user stop rete-console@c6
    # wait past 180s
    ./check.py ; echo $?        # must be 1 — the heartbeat gate
    systemctl --user start rete-console@c6

    # then, with the collector running, unplug the console adapter
    ./check.py ; echo $?        # must be 1 — the port gate

A check that passes with the daemon stopped is measuring nothing (design §9);
a check that passes with the adapter unplugged is measuring only half of it.

## Reading the markers

Lines beginning `@@rete-console@@` are the collector's own, not the device's:

| marker | means |
|---|---|
| `collector started pid=… port=…` | a fresh collector process took the port |
| `heartbeat port=up` | the collector was alive at this instant AND had the port, device silent or not |
| `heartbeat port=down` | the collector was alive and had **no input** — it is collecting nothing |
| `port lost: …` | the port went away; the reason is verbatim from the OS |
| `port reacquired` | it came back |
| `pruned …` | a day's log was deleted to stay under budget |

The heartbeat exists because a log that stops looks identical to a board that
went quiet. Every health check asserts a *recent heartbeat is present* — never
the absence of an error (design D7, §8).

`port lost` is written once, at the transition; the heartbeat restates the port
state on every beat (#147). That is what carries the state across a midnight
rollover — a loss recorded in yesterday's file is invisible to anything reading
today's.

## The profile view (#165)

    python3 view.py --board c6spare --node <12-hex-id> \
      --sidecar ../../PortaBridgeNodeC6.oimg.methods.json \
      --porta-db <the store porta serve is serving> --port 8087

Open `http://localhost:8087`.

**Run this on the host holding the served store, and point `--porta-db` at
that file.** `porta` is not a network client: every verb — `put`, `get`,
`log` — opens the sqlite file directly, so starting a capture from here is a
local write to that file and nothing else. Without `--porta-db` the CLI
defaults `--db` to a *relative* `porta.db` and porta creates it on open, so
the write succeeds, exits 0, and lands in an empty store nobody serves —
after which the page shows an earlier capture that reads like the one just
requested. That is the defect this flag exists to make impossible, and it is
why the flag is required rather than defaulted: which file is served is a
fact about the deployment, and neither this tool nor its README can guess it.
`porta-gateway-operational-state` records that the running v2 instance serves
one shared store, hand-started; ask that deployment, do not assume a path.

If the collector and the served store are on different hosts, this view
cannot start a capture — that is a deployment fact, not something to engineer
around here. Move the view, or write `dbg/req` by hand on the store's host.

**The board and the node id are different strings, and both flags are
required.** `--board` names the collector's log file (`console-<board>-*.log`)
— the attached devkit is `c6spare`. `--node` is what porta addresses the node
as on the wire — its `platform nodeId`, the board's factory link address (12 lowercase hex
characters; the node prints it in its 'no goal/name arrived' line). It used to be the
literal `'c6-bench'` hard-coded in `PortaBridgeNodeC6.nsl`, and then the bench name (#186). Neither flag defaults:
`c6bench` (a third, similarly-named board) has been off the bench since
2026-08-06 with its by-id gone, and a baked-in default naming an absent board
would make "no capture in this window" indistinguishable from "you read the
wrong sink" — the one failure this view exists to prevent.

Three routes:

- `GET /` — the page: a capture form and the most recent window's table,
  read straight out of this collector's own log via `logstore` and named via
  `mprof_resolve` against the `--sidecar` map. The search scans retained
  days NEWEST FIRST and falls back to an older day when today's has no
  window — a capture doesn't go blind at midnight — and the table always
  states WHEN it was taken, read from the capture's own log timestamp — or
  says `capture time unknown` when it cannot, never omitting the line, since
  showing the age is the condition under which searching backward is safe.
  The table is truncated to `--top` (64 by default) and **says how many rows
  it is not showing**: a profile that silently drops rows reads as complete
  while hiding its largest caller. `htmx.min.js` is vendored
  beside this file and never fetched from a CDN, because the bench network
  may have no route out and a view that blanks when the internet is down is
  useless at exactly the moment it is wanted.
- `POST /capture` — starts a window. This is the only place the trigger
  crosses the seam to porta: it shells out to `porta put <node> dbg/req
  "profile <ms>" --kind stream --db <--porta-db>`, because porta owns the
  `dbg/req` write and the store pins a leaf's kind on its first write with no
  tombstone to undo it. It then **reads the write back** with `porta log`
  — not `porta get`, which is a cell-tree read and can never see a stream —
  and reports "capture NOT started" if the row is not there. A failure to
  start (porta not on `PATH`, the write timing out, a non-zero exit, or a
  write that exits 0 and lands nowhere) renders that message, quoting porta's
  own stderr, rather than the same blank table a healthy-but-empty window
  would show.
- `GET /health` — builds the whole page and throws it away: `200 ok` only if
  `GET /` would have served a real table, `500 not ok` if it raises or
  assembles only an error banner. A probe that does not build the thing it
  certifies can report only that the process is running, which is the one
  fact nobody was in doubt about (#147, and this view's own round 1).

The view moves only the trigger across the seam; the bulk (log history, the
offset→name join, the symbolizer) stays on the console collector's side,
because that is where it already lives.

## Testing

    python3 -m pytest

**This suite is a gate.** `nodus/nsl/tools/run_tests_oevm.py` runs it as
`console-py` on the aux line, alongside `mprof-tools-py` for
`nodus/oevm/tools/`. Until #165 it was run by nothing at all, which meant the
view and `mprof_resolve.py` could break with all three CLAUDE.md gates green —
a test that is never run is worse than no test, because the suite still reads
green and the coverage is believed rather than merely absent.

No board required: the collector takes a *path*, which is a
`/dev/serial/by-id/` symlink in production and a PTY in tests — the same code
path either way, which is the whole reason the daemon is testable.
