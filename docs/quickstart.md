# Quickstart: the first hour

What rete 0.1 promises is a better first hour of embedded development. You change a program on a board in seconds,
without reflashing the firmware, and you debug and profile it there, over one USB cable. This page is that hour, in
order. Each step links the tutorial that teaches it. Steps marked **executed** are run by the nsl suite on every
change, so they work today. The other steps need a board and are checked by hand.

Before you rely on any of it, read [what 0.1 is not](limits.md).

## You need

- Linux, `gcc`, `python3` (3.9+), `node` (18+), and Go (1.26+, as `custos/go.mod` says) for custos.
- For the board half: an ESP32-C6 devkit, one USB cable, and the Zephyr toolchain ([tutorial 2](tutorials/02-board.md) §0).

```bash
git clone <this repository> rete && export RETE=$PWD/rete
cd $RETE/nodus/nsl/grammar && npm install && cd -        # once: the parser's node deps
go build -C $RETE/custos -o $RETE/custos/custos ./cmd/custos
```

## The hour

| # | Step | Do | A pass looks like | Taught in |
|--:|---|---|---|---|
| 1 | Run ten lines of nsl on the desktop | `python3 $RETE/nodus/nsl/tools/run-one.py hello.nsl B` | `Path B: PASS  Output matches expectations.` | [tutorial 1](tutorials/01-host.md), **executed** |
| 2 | Step through them in the debugger, on the desktop | `custos --debug-host "/tmp/oevm_img app.oimg" --debug-map app.oimg.map.json`, then open `http://localhost:6971/debug`, or drive `POST /debug/do` | a stop shown as a file, a line and its source | [tutorial 4](tutorials/04-debugger.md) §1–§6, **executed** |
| 3 | Flash the C6 over the cable | `bash nodus/oevm/zephyr/run.sh --board esp32c6 --node App.nsl --conf-extra "prj_esp32c6_thread.conf;prj_esp32c6_thread_ftd.conf" --pristine --flash --capture 60` (the documented build is the Thread one; a cable-only image is not yet written down, see [limits](limits.md)) | last line `[console] <instance> <port>` | [tutorial 2](tutorials/02-board.md) §5 |
| 4 | Read what the board prints | the same cable; `run.sh --capture` prints it | your program's lines | [tutorial 2](tutorials/02-board.md) |
| 5 | Change the program and load it without reflashing | `custos --load-port <tty>`, then `POST /load?name=App.oimg` with the image as the body, or the page at `/load` | 200, and the `SHA256` the board printed at boot is your file's | [development.md](development.md) §2 |
| 6 | Profile it on the board | `custos --profile-port <tty> --profile-map App.oimg.map.json`, then `POST /profile/capture?ms=3000` or `/profile` | a table of methods ranked by activations | [tutorial 4](tutorials/04-debugger.md) §8 |
| 7 | Debug it on the board | flash a debug image (`OEVM_DEBUG_HOOKS=1 run.sh …`), then `custos --debug-port <tty> --debug-map …` | the same page as step 2, on the board | [tutorial 4](tutorials/04-debugger.md) §7 |
| 8 | Stop, patch a method, restart it, resume | desktop only, in a debug build: `dbg:patch <file>`, `dbg:restart` | the stopped call finishes in the new code | [the spike's result](results/2026-10-06-313-patch-restart-spike.md); rete#313 |
| 9 | See the gateway mirror a node | `porta serve` with a desktop node | the node's paths in `porta get` | [tutorial 3](tutorials/03-porta.md), **executed** |

The desktop half needs no board: steps 1, 2 and 9 run on any Linux machine. The terminal front, `custos tui`, has a tab
each for debug, profile and load, over the same HTTP.

## For an agent

Every step above has an HTTP path that answers JSON: `/debug/do` and `/debug/state`, `/profile/capture`, `/load`.
HTTP is the only agent surface; the command lines are kept but not advertised ([peer design](design/2026-10-04-custos-peer-design.md)
D11–D12). What an agent needs that a person does not is in [development.md](development.md) §4.
