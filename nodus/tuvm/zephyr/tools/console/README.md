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

## Reading the markers

Lines beginning `@@rete-console@@` are the collector's own, not the device's:

| marker | means |
|---|---|
| `collector started pid=… port=…` | a fresh collector process took the port |
| `heartbeat` | the collector was alive at this instant, device silent or not |
| `port lost: …` | the port went away; the reason is verbatim from the OS |
| `port reacquired` | it came back |
| `pruned …` | a day's log was deleted to stay under budget |

The heartbeat exists because a log that stops looks identical to a board that
went quiet. Every health check asserts a *recent heartbeat is present* — never
the absence of an error (design D7, §8).

## Testing

    python3 -m pytest

No board required: the collector takes a *path*, which is a
`/dev/serial/by-id/` symlink in production and a PTY in tests — the same code
path either way, which is the whole reason the daemon is testable.
