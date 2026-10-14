# Quickstart: the first hour

What rete 0.1 promises is a better first hour of embedded development: one node, every verb — load, debug, patch,
profile — first over one USB cable, then through porta, over the radio, while the node sleeps.
You change a program on a board in seconds, without reflashing the firmware, and you debug, patch and profile it
there. This page is that hour, in order. Each step links the page that teaches it. Steps marked **executed** are run
by the nsl suite on every change, so they work today. The other steps need a board and are checked by hand.

Before you rely on any of it, read [what 0.1 is not](limits.md).

## You need

- Linux, `gcc`, `python3` (3.9+), `node` (18+), and Go (1.26+, as `custos/go.mod` says) for custos.
- For the board half: an ESP32-C6 devkit, one USB cable, and the Zephyr toolchain ([tutorial 2](tutorials/02-board.md) §0).

```bash
git clone <this repository> rete && export RETE=$PWD/rete
cd $RETE/nodus/nsl/grammar && npm install && cd -        # once: the parser's node deps
go build -C $RETE/custos -o $RETE/custos/custos ./cmd/custos
```

`custos` with no verb serves HTTP on port 6971. Each board is one flag: `--board name=tty` for a board on its cable,
`--host name=binary` for a host VM, and `--map name=App.oimg.map.json` for its program's map (without a map a board
serves load only). Every verb of a board is at `/board/<name>/…`; with one board the paths without it are its
shorthand, and `GET /boards` lists them ([peer design](design/2026-10-04-custos-peer-design.md) D15;
`custos/cmd/custos/main.go`).

## The hour, over the cable

| # | Step | Do | A pass looks like | Taught in |
|--:|---|---|---|---|
| 1 | Run ten lines of nsl on the desktop | `python3 $RETE/nodus/nsl/tools/run-one.py hello.nsl B` | `Path B: PASS  Output matches expectations.` | [tutorial 1](tutorials/01-host.md), **executed** |
| 2 | Step through them in the debugger, on the desktop | `custos --host dev=/tmp/oevm_img --map dev=app.oimg.map.json`, then open `http://localhost:6971/debug`, or drive `POST /debug/do` | a stop shown as a file, a line and its source | [tutorial 4](tutorials/04-debugger.md) §1–§6, **executed** |
| 3 | Flash the C6 over the cable | `bash nodus/oevm/zephyr/run.sh --board esp32c6 --node App.nsl --conf-extra "prj_esp32c6_thread.conf;prj_esp32c6_thread_ftd.conf" --pristine --flash --capture 60` (the documented build is the Thread one; a cable-only image is not yet written down, see [limits](limits.md)) | last line `[console] <instance> <port>` | [tutorial 2](tutorials/02-board.md) §5 |
| 4 | Read what the board prints | `custos --board c6=<tty>`, then `GET /log?since=-180s` | your program's lines | [tutorial 2](tutorials/02-board.md), [console.md](../custos/docs/console.md) |
| 5 | Change the program and load it without reflashing | `POST /build?for=c6&name=App.nsl` with the source, then `POST /load?name=App.oimg` with the image (`GET /build/last.oimg`), or the pages at `/build` and `/load` | 200, and the `SHA256` the board printed at boot is your file's | [development.md](development.md) §2; peer design D14, D18 |
| 6 | Profile it on the board | `custos --board c6=<tty> --map c6=App.oimg.map.json`, then `POST /profile/capture?ms=3000` or `/profile` | a table of methods ranked by activations | [tutorial 4](tutorials/04-debugger.md) §8 |
| 7 | Debug it on the board | flash a debug image (`OEVM_DEBUG_HOOKS=1 run.sh …`), then `custos --board c6=<tty> --map c6=…` | the same page as step 2, on the board | [tutorial 4](tutorials/04-debugger.md) §7 |
| 8 | Stop, patch a method, restart it, resume | in a debug build, stopped: `POST /debug/patch?method=Class>>sel` with the whole edited `.nsl` as the body, or the page at `/debug` | the stopped call finishes in the new code | [the spike's result](results/2026-10-06-313-patch-restart-spike.md) (desktop, then c6spare); peer design D17, rete#313 |
| 9 | See the gateway mirror a node | `porta serve` with a desktop node | the node's paths in `porta get` | [tutorial 3](tutorials/03-porta.md), **executed** |

The desktop half needs no board: steps 1, 2 and 9 run on any Linux machine. The terminal front, `custos tui`, has a
tab for each verb, over the same HTTP.

## The same node, over the radio

The node runs firmware built with porta's public key (`OEVM_OTA_PUBKEY`) and checks in with a `porta serve` over
Thread or WiFi. No cable.

| Verb | Do | A pass looks like | State on master | Record |
|---|---|---|---|---|
| Load | `custos --porta-node bell=<node-id> --porta-http http://<gateway>:6970`, then `POST /board/bell/load?name=App.oimg` with the image; poll `GET /board/bell/load/last` | `Converged: true`; the board prints `ota confirmed` | Built. An agent pushed to the wall-bell C6 this way (rete#398); the node side is proven on the C6 over Thread and the MagTag over WiFi (rete#233) | [design](design/2026-10-12-398-load-through-porta-design.md) |
| Debug | on the gateway: `porta put --raw <node> goal/debug/main <Debug{sha, breaks}>`, then each command as a `dbg/req` row; the empty set releases the node | the stop in `obs/debug`, the answers in `dbg/resp` | Built, driven by porta writes: custos serves only `load` for a porta node | [result](results/2026-10-10-380-debug-through-porta-on-boards.md) (rete#379, rete#380); peer design D19 |
| Patch | — | — | Not tried through porta: over the cable only (step 8) | rete#313 |
| Profile | — | — | **Not on master.** The rows leave over the console, so a profile needs the cable. Through porta is rete#415, in flight | rete#415 |

## For an agent

Every step above has an HTTP path that answers JSON. HTTP is the only agent surface; the command lines are kept but
not advertised ([peer design](design/2026-10-04-custos-peer-design.md) D11–D12). Per board, each also at
`/board/<name>/…` (D15):

| Verb | Paths |
|---|---|
| debug | `GET /debug` (the page), `/debug/source`, `/debug/state?after=N`; `POST /debug/do`, `/debug/patch?method=…` |
| profile | `GET /profile` (the page), `/profile/last`; `POST /profile/capture?ms=N` |
| load | `GET /load` (the page), `/load/last`; `POST /load?name=F.oimg` |
| console | `GET /log`, `/runs`, `/panics`, `/panics/{n}/bt`, `/health`; `POST /lend`, `/return` |

Across boards: `POST /build?for=<board>&name=F.nsl`, `GET /build/last.oimg` and `/build/last.oimg.map.json`,
`POST /run?for=<host board>&name=F.nsl` (D18); `GET /boards`; and with `--store`, the fleet pages `/fleet` and
`/node/{id}`. What an agent needs that a person does not is in [development.md](development.md) §4.
