# D16: custos is the console collector — the fate table (rete#314)

**Decision:** peer design D16 (David, 2026-10-06): custos reads every board's console all the time and writes the day
logs; `nodus/oevm/zephyr/tools/console/` is deleted, reader included. This page is the fate table that comes first:
every capability of the old tools and every caller of them gets a fate before anything is removed. Inventory taken
2026-10-06 by reading the tree: 703 lines of tool code in 7 Python files, 577 lines of tests (the `console-py` aux
gate), `archive.sh`, the unit, the fixtures.

**The file format does not change.** `console-<board>-<YYYY-MM-DD>.log` (local date), lines
`<isoformat ms ±HH:MM> <text>`, markers `@@rete-console@@ <msg>`, the lock `<board>.lock` holding `pid=N`. The target
harness, `bench-identity.sh` and tutorial 2 read the files directly, so keeping the format is what lets them stay as
they are. A Go test writes a day file and checks it against `fixtures/c6-boot.log`'s shape before the fixture leaves.

## Capabilities

| # | Capability (old home) | Fate |
|---|---|---|
| 1–3 | `collect.py`: one board's port at 115200, the one-reader lock (exit 3 when held) | **custos**, per `--board`: custos already holds each board's cable (D15); the lock is the same `flock` file |
| 4–7 | Markers: `collector started`, `port lost`/`port reacquired` once per transition, `heartbeat port=up|down` every 60 s, `collector stopped`, `pruned` | **custos**, the same texts, so `check`-style readers keep matching |
| 7 | Pruning to a 2 GiB budget, only at exit | **custos**, at each new day as well as at exit (a 24/7 process rarely exits) |
| 8 | Line assembly: drop NULs, split on `\n`, strip `\r`, UTF-8 with replacement | **custos**: `debug.pump` already does all but the NUL strip, which is added |
| 9–14 | `logstore`: the path, `append` (refuses naive times), `days`, `total_bytes`, `prune` (never the newest day), `acquire_lock` | **custos**, a `console` package beside `load` |
| 15–17 | `udpsink.py`: UDP `[::]:54330`, one datagram one line, `peer <addr>` marker, the same lock | **custos**, `--udp-log <name>=<port>`, repeatable (a board that logs over the mesh, `log_udp.c`) |
| 18–20 | `logread tail`, `since <spec>` | **custos** `GET /board/<name>/log?since=<spec>` (and `?tail=N`), answering the stamped lines as JSON |
| 21 | `logread runs`: boots split on `ESP-ROM:esp32c6` / `*** Booting Zephyr OS`, each run's build id | **custos** `GET /board/<name>/runs`. The C6-only boot marker is widened: a board-neutral rule is `*** Booting Zephyr OS` plus the build-id line, so the nRF52840 and RP2350 split too (0.1 portable by processor) |
| 22 | `logread panics`: the `ZEPHYR FATAL ERROR` banner and `#CD:` blocks | **custos** `GET /board/<name>/panics` |
| 23, 27–29 | `logread symbolize`, `symbolize.py`: the archived ELF by build id, Zephyr's coredump parser and gdbserver under the SDK's gdb, `bt` | **custos** `GET /board/<name>/panics/<n>/bt`, running the same Zephyr tools (the three env knobs kept) |
| 24 | logread's functions, used by tests | **deleted** with their tests; Go tests replace them |
| 25–26 | `check.py`: today's file, a heartbeat or start under 180 s old, the port up | **custos** `GET /board/<name>/health` — and, being the collector, custos answers it from memory rather than the file |
| 30 | `archive.sh`: ELF, binary hash, manifest pin and image map into `$RETE_ELF_ROOT/<build-id>/` | **kept, moved** to `nodus/oevm/zephyr/archive.sh`. It is run.sh's flash archiver, not a console tool, and it is shell, not Python |
| 31 | `rete-console@.service`, one unit per board | **replaced** by one `rete-custos.service` serving every board. Its boards come from the existing `~/.config/rete/console-<name>.env` files (`RETE_CONSOLE_PORT`), so no bench file changes |

## Callers

| Caller | Uses | Fate |
|---|---|---|
| `nodus/oevm/zephyr/run.sh` 893, 956 | `archive.sh` on `--flash` | path change only |
| `run.sh` 900–934 | collector state: active, inactive, no log, port down | `GET /board/<b>/health` |
| `run.sh` 975, 983 | `logread since` for `--capture` | `GET /board/<b>/log?since=` |
| `run.sh`, `flash-bell.sh` 53–54, `tests/target/lib/target.sh` 143–155, 198–206 | stop the collector around a flash, start it after | **open point (below)**: custos lends the board's cable |
| `nodus/oevm/tools/bench-identity.sh` | the store path, the env files, today's log, `systemctl is-active`, `port=down` in the last lines | the files are unchanged; the unit check becomes `GET /board/<b>/health` |
| `tests/target/lib/collect_console.sh` | tails today's file for a build id | unchanged (the format is kept) |
| `tests/target/lib/deploy_flash.sh` 171 | parses archive.sh's `[archive] <id> ->` | unchanged |
| `protocol/tools/check_provision.py` | reads the ELF archive | unchanged |
| `nodus/nsl/tools/run_tests_oevm.py` (`console-py`) | pytest over the tools | **removed** from the aux line; the Go tests are in `go test ./...` |
| `custos/internal/load/board.go:91`, `load/cli.go:18`, `profile/cli.go:19` | "stop rete-console@<board>", an example log path | rewritten |
| `docs/development.md`, `docs/tutorials/02-board.md` | logread and systemctl commands | rewritten to the endpoints |
| Comments: `zephyr/Kconfig`, `src/log_udp.c`, `src/observer.c`, `src/reboot_note.c`, `ShipsBellAlarmNodeC6.nsl`, `CMakeLists.txt`, two board READMEs, `PROTOCOL-NSL-RECORDS.md`, `liveness-fastpoll.env`, `next-steps-overview.md` | name the tools | rewritten |
| `review/` (4 files) | name the tools | **listed for David**, not edited |
| Dated records (`docs/design`, `docs/results`, `docs/superpowers`) | name the tools | left as history |

**On the bench:** `rete-console@c6` and `rete-console@c6spare` are active; env files exist for `c6`, `c6bench`,
`c6spare` and `nrf4ec0`. They move board by board; c6bench's soak last, and only when David says.

## Open point for David

**Flashing firmware needs the cable free.** Today a flash stops the board's collector unit and starts it again. With
one custos serving every board, stopping the unit stops all of them. Proposed: `POST /board/<name>/lend?s=<N>`.
custos closes that board's cable, writes `cable lent` in its log, and takes the cable back after N seconds or on
`POST /board/<name>/return`, whichever comes first. run.sh, `flash-bell.sh` and the target harness call it instead
of systemctl. That adds one verb, the alternative being one custos unit per board, which D15 set out to avoid.
