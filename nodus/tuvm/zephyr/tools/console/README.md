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

`symbolize` resolves the ELF from the run's own `tuvm: build-id` line and
**refuses** if the archive has no match (design D4). It never falls back to
`build-*/zephyr/zephyr.elf`: a confident wrong source location is worse than
no answer, because it looks like one. Exit 5 is a refusal.

`--assume-build` is the one way past that, for a run that faulted before its
banner printed. It must be typed; nothing infers it. Every line of the result
is prefixed with the key that was assumed, so the assumption travels with the
output. `~/.local/state/rete/elf/last-flashed-c6` will tell you what was
flashed last — read it yourself, the tool will not.

It needs two things that are easy to get wrong:

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

## Testing

    python3 -m pytest

No board required: the collector takes a *path*, which is a
`/dev/serial/by-id/` symlink in production and a PTY in tests — the same code
path either way, which is the whole reason the daemon is testable.
