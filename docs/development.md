# Development: the loop for a human and for an agent

**Status: written 2026-10-02 from what exists in the tree. Prose, not executed: no command on this page was run while writing it, and no gate checks it yet. Where it disagrees with a tutorial or a CLAUDE.md, those are right.**

## 0. What this page is

One loop, the same for David and for a coding agent: write code, run it, put it on a board, watch it, debug it, measure it, update it from afar. Each step needs a command and a way to tell it worked. This page lists, for each step, what exists today, what the repo's own documents warn about, and what is missing. It invents no command: every one is copied from a file, linked beside it, and where none exists the cell says "none found". For the whole system on one page see the [overview](overview.md).

## 1. Terms

| Term | Meaning |
|---|---|
| node | A small device running the VM and one nsl program. |
| gateway | `porta`: keeps a copy of each node's data and holds what the operator wants each node to do. `custos` is a separate program in front of it, for browsers and agents. |
| image / `.oimg` | A compiled nsl program as one file, which the VM loads. |
| flash | Write firmware onto a board over USB. |
| console | The text a board prints on its serial line. A collector service logs it all day. |
| gate | A check that passes or fails by itself. |
| aux gate | A gate listed on the line under the suite's pass count. It can be red while the count reads zero failures. |
| over-the-air update | Replacing a program or firmware over the radio, with no cable. |
| bootloader | The small program that starts first and chooses which stored image to run; with two slots it can roll back. |
| MCP | Model Context Protocol: a way for an AI agent to call a program's functions as typed tools. |

## 2. The loop on one table

Paths are from the repo root. `$RETE` is the clone, as in [tutorial 1](tutorials/01-host.md).

| Step | Human runs | Agent runs | A pass looks like | Documented in |
|---|---|---|---|---|
| Code and run on a desktop | `python3 $RETE/nodus/nsl/tools/run-one.py hello.nsl B`; whole suite: `cd $RETE/nodus/nsl && python3 tools/run_tests_oevm.py` | same | `Path B: PASS  Output matches expectations.`; suite: `Failed:  0` plus an aux line with no `FAIL`; exit 1 if any gate fails | [tutorial 1](tutorials/01-host.md), [nodus/nsl/CLAUDE.md](../nodus/nsl/CLAUDE.md) |
| Flash a board over USB | `bash nodus/oevm/zephyr/run.sh --board esp32c6 --node PortaBridgeNodeC6.nsl --conf-extra "prj_esp32c6_thread.conf;prj_esp32c6_thread_ftd.conf" --pristine --flash --capture 60` (with `ESPTOOL_PORT` and `CONSOLE_PORT` set) | same | Last line `[console] <instance> <port>` naming the board flashed; exit 2 or 3 on a refusal | [tutorial 2](tutorials/02-board.md) (prose), [run.sh](../nodus/oevm/zephyr/run.sh) |
| Observe: console | `python3 nodus/oevm/zephyr/tools/console/logread.py since -- -180s` | same | no fixed verdict line; you read the log | [tutorial 2](tutorials/02-board.md), [console README](../nodus/oevm/zephyr/tools/console/README.md) |
| Observe: gateway | `porta get --db ./porta.db c6bench` | same | a tree of paths; a quiet node shows as `#silent` | [CLI.md](../porta/docs/CLI.md), [tutorial 3](tutorials/03-porta.md) |
| Observe: board scenarios | `./run.sh stale-handle` in `nodus/oevm/tests/target/` | same | `TARGET 1/1 PASS`; otherwise `TARGET N/M PASS — K FAILED` and exit 1 | [target CLAUDE.md](../nodus/oevm/tests/target/CLAUDE.md), [run.sh](../nodus/oevm/tests/target/run.sh) |
| Debug | none found | none found | none found; the host test exits non-zero on failure | [in-image debugger design](../nodus/nsl/docs/design/2026-06-23-ns-oevm-in-image-debugger-design.md) |
| Profile | `porta put --db "$DB" "$NODE" dbg/req "profile 3000" --kind stream`, then `python3 nodus/oevm/tools/mprof_resolve.py <image.oimg.map.json> [--top N] [dump.log ...]` | same | `mprof[window]` appears in the node log; the resolver prints a table | [profile design](design/2026-08-06-method-profile-over-porta-design.md), [porta_sync_host.sh](../nodus/oevm/tests/porta_sync_host.sh) |
| Update the nsl program over the air | `porta key gen`, `porta blob put <file.oimg>` (it prints the `porta put` to paste); host proof: `bash tests/host_ota/run.sh` in `nodus/oevm` | same | `HOST OTA OK`; on a board: none found | [CLI.md](../porta/docs/CLI.md), [code-push design](design/2026-08-07-code-push-host-slice-design.md), rete#233 |
| Update the firmware over the air | none found | none found | none found | rete#233 |

## 3. One subsection per row

### Code and run on a desktop
- The suite builds the VM from source and runs every test as an `.oimg`. About four minutes, per [tutorial 1](tutorials/01-host.md).
- Traps the repo names ([root CLAUDE.md](../CLAUDE.md), Gates; [nodus/nsl/CLAUDE.md](../nodus/nsl/CLAUDE.md)):
  - Read the aux line, not only `Passed: N / Failed: 0`.
  - A SKIP on the aux line is a finding, not a fixture.
  - Never run two suites at once: they share `/tmp` paths.
  - Never run `bash tests/porta_sync_host.sh` (in `nodus/oevm`; success is `PORTA SYNC OK`) beside a suite: both bind udp 16969/16970.
  - `run-one.py` defaults to both paths and the Path A bake overwrites a tracked file, so pass `B` ([compiler.md](../nodus/nsl/docs/compiler.md)).
- Gap: the suite's exit status is the machine-readable verdict, but a SKIP does not change it (see section 4).

### Flash a board over USB
- `run.sh` owns the wrong-by-default steps (working directory, environment, per-board profile, flash port). It builds by default and flashes only with `--flash` ([tutorial 2](tutorials/02-board.md)).
- It refuses with exit 2 on an unknown argument ([run.sh:24](../nodus/oevm/zephyr/run.sh)), a bad `--model` or `--board` ([27-28](../nodus/oevm/zephyr/run.sh)), more than one matching device ([145-152](../nodus/oevm/zephyr/run.sh)), and a missing Thread overlay, which would ship the stock demo credentials ([584-589](../nodus/oevm/zephyr/run.sh); rete#288). Exit 3 means the image does not contain what was asked ([683](../nodus/oevm/zephyr/run.sh)).
- Traps: leave off the Thread overlays and a node that binds a socket gets no interface and prints a bare `bind` ([tutorial 2](tutorials/02-board.md), section 4). One board drives a physical bell and the scripts refuse to flash it; schedule an actuator node for daylight.
- Disagreement found: [tutorial 2](tutorials/02-board.md) says a failed build still exits 0. `run.sh` records `BUILD_RC` and exits with it ([662, 755](../nodus/oevm/zephyr/run.sh)). The script is likely right; the tutorial is prose and unexecuted.

### Observe
- The console is logged all day by a user service, because attaching late misses the boot banner and, on a C6, an attach reads zero bytes. Hold one handle per port; filter by the timestamp field, not by grepping a time ([tutorial 2](tutorials/02-board.md), section 3).
- `logread.py` also has `tail -n 50`, `runs`, `panics` and `symbolize --run 3` ([README](../nodus/oevm/zephyr/tools/console/README.md)). Every `porta` read prints `# store <path>` on stderr so a wrong database is visible ([CLI.md](../porta/docs/CLI.md)).
- The board scenarios are not run by the suite, take about 70-100 minutes in all, and `CLASS=disruptive` means a disposable board only ([target CLAUDE.md](../nodus/oevm/tests/target/CLAUDE.md)). Boards resolve through `~/.config/rete/console-<board>.env`, never a table. The `porta-*` scenarios need a hand-started injector, so a full sweep is not unattended.
- A scenario's checks passing is not the verdict: each one declares how long the board is watched afterwards, and the runner keeps watching that long ([run.sh](../nodus/oevm/tests/target/run.sh), `OBSERVE_S`).

### Debug
- Two things exist. An in-image debugger driven by `dbg:*` lines, switched on at boot by `platform_debug_enabled` — the `OEVM_DEBUG` environment variable on a desktop, the hooks build on a board ([design](../nodus/nsl/docs/design/2026-06-23-ns-oevm-in-image-debugger-design.md); [platform_backend.h](../nodus/oevm/src/platform_backend.h)); the suite's `debugger-integration` aux gate drives it ([header](../nodus/nsl/tools/test_debugger_integration.py)). And a remote path: porta may write `goal/debug/<app>` (`#attach` / `#detach`) and `dbg/req`, the node answers on `dbg/resp`, and porta only relays the lines ([records](../protocol/PROTOCOL-NSL-RECORDS.md), section 4.5; [CLI.md](../porta/docs/CLI.md)).
- What the debugger can do, on a desktop and on a C6 over its console cable (rete#311, [record](results/2026-10-02-311-line-in-spike.md) part 2): list methods, set and clear breakpoints, continue, step in, step over, step out, inspect, and — since 2026-10-02 (part 3) — **pause a running program** (`dbg:methods`, `dbg:break`, `dbg:clear`, `dbg:continue`, `dbg:step`, `dbg:over`, `dbg:out`, `dbg:inspect`, `dbg:pause`), as lines on standard input and output. The pause rule is one sentence: a line arriving while the program runs stops it at its next bytecode, and that line is then the first command of the paused loop, so any verb sent while running works. A sleeping program with no timer never reaches a bytecode and cannot be paused. There is ONE command loop: the boot wait is the first pause (it prints `dbg:ready`), so every verb works at boot too. It lives in [supervisor.nsl](../nodus/nsl/lib/supervisor.nsl) and the gate drives it on both build paths.
- **For a person: [tutorial 4](tutorials/04-debugger.md)** and [dbg.py](../nodus/nsl/tools/dbg.py) (rete#311). `break 9` / `break app.nsl:9`, and every stop is shown as a file, a line and its source, through the image map beside it (`.oimg.map.json`, `protocol/IMAGE-ARTIFACTS.md`) and the VM's `dbg:methods` list. `step`/`next` move a line at a time. The same tool attaches to a host VM it starts or to a board's cable (`--port`). The page's desktop half is the `tutorial-debugger` aux gate; the tool's translations are `test_dbg.py` and a `debugger-integration` case.
- What is missing today:
  - **A breakpoint goes only in a method of the entry class.** The VM's registry walks the entry's vtable chain, so a line in another class of the file, or in a block the compiler did not inline, is refused by name ([tutorial 4](tutorials/04-debugger.md) §4).
  - **`inspect` shows registers, not names**: the source map has no local names.
  - **On a board it is a separate image.** The per-bytecode checks stay compiled out of the default Zephyr build (10–16% per bytecode on the host, 11.6% and 494 B of flash measured on the C6); `OEVM_DEBUG_HOOKS=1` builds them in, and that image IS the debug image: it prints `dbg:ready` at boot and waits for its cable ([CMakeLists.txt](../nodus/oevm/zephyr/CMakeLists.txt); [dbg_session.py](../nodus/oevm/zephyr/tools/dbg_session.py) drives it). The C6's conf carries the interrupt-driven UART every image needs (the polled console reader hangs the board; the UART interrupt is also what notices a line while the program runs).
  - **The remote path carries one verb.** A `dbg/req` line is parsed only as `profile <ms>` (`parseProfileMs:` in [kernel.nsl](../nodus/nsl/lib/kernel.nsl)); `goal/debug/<app>` is in the catalogue with no user found.
  - **A held board is not rebooted** (rete#311, verified on the C6 2026-10-02): a VM blocked in the debug read tells the observer it is held, and the §3.2 feed clock restarts from the release ([observer_core.h](../nodus/oevm/src/observer_core.h)). Held 21 s against an 8 s declared deadline, no reset. A repeating timer whose ISR has stopped (§3.1) is still judged.

### Profile
- A node counts method calls inside a bounded window opened by a `dbg/req` line. The window closes itself; porta sends no stop ([porta_sync_host.sh](../nodus/oevm/tests/porta_sync_host.sh), the "window closed itself" leg). Rows leave over the console, not over `dbg/resp` ([records](../protocol/PROTOCOL-NSL-RECORDS.md)).
- Counts rank fixes but over-state speedups; only a board sizes them (the design and `profile_cost.py`'s own header).
- What is missing today:
  - **No command or document for a person.** The trigger line is found only inside a test script.
  - **The results need a cable.** The rows leave over the console, not through the gateway (design D4), so a node that is not on USB cannot be profiled.
  - **The table's capacity and the cost figure are unmeasured for a real program** (design D9).
  - `goal/profile/<app>` is in the record catalogue but no command that uses it was found; `gwprofiler` is a separate spike, not this path.

### Update the nsl program over the air
- Proven on a desktop only. `bash tests/host_ota/run.sh` (from `nodus/oevm`) runs three VMs and a real `porta serve` through signed push, kill mid-transfer, bad signature and no check-in, and prints `HOST OTA OK` ([run.sh](../nodus/oevm/tests/host_ota/run.sh)). It uses its own port pair, so it can run beside the porta sync gate. It is standalone, not in the suite. rete#233's body says `nodus/tuvm`; the directory is `nodus/oevm`, which exists.
- On Zephyr boards the backend is present and deliberately inert: it reports that it does not know what it is running ([ota_zephyr.c](../nodus/oevm/src/backend/zephyr/ota_zephyr.c)).

### Update the firmware over the air
- Nothing exists: no MCUboot configuration and no A/B slot layout (rete#233).
- **PROPOSED — not decided.** rete#233 says the program push waits on a real flash layout. A bootloader with two slots is what provides one, so program push and firmware update likely share one piece of work. The code-signature check on the board belongs with it ([security draft](design/2026-10-02-security-levels-design.md), decision 9).

## 4. What an agent needs that a human does not

David: "agents can't smell". An agent has no feeling that something is off, so each step needs:

| Need | Meaning |
|---|---|
| (a) a verdict a machine can read | an exit status or a fixed line |
| (b) a bounded run time | the step ends by itself |
| (c) no interactive prompt | nothing waits for a keypress |
| (d) no silent skip | a step that did not run says so, and fails |

### Conventions that should become programmatic (PROPOSED — not decided)

| Convention today | Written in | What the tool should do instead |
|---|---|---|
| Read the aux line, not just the pass count | [root CLAUDE.md](../CLAUDE.md), [nsl CLAUDE.md](../nodus/nsl/CLAUDE.md) | Already exits 1 on any aux `FAIL` ([run_tests_oevm.py:1332](../nodus/nsl/tools/run_tests_oevm.py)); a SKIP does not. Make an unexpected SKIP fail too. |
| Never run two suites at once | [root CLAUDE.md](../CLAUDE.md) | Take a lock and refuse a second run with a clear message. No lock found in the runner; only the board harness locks ([target.sh:122](../nodus/oevm/tests/target/lib/target.sh)). |
| Never run `porta_sync_host.sh` beside the suite | [root CLAUDE.md](../CLAUDE.md), [nsl CLAUDE.md](../nodus/nsl/CLAUDE.md) | Same lock, or per-run ports (the OTA gate already uses its own pair). |
| Pass `B` to `run-one.py` | [compiler.md](../nodus/nsl/docs/compiler.md), [tutorial 1](tutorials/01-host.md) | Make `B` the default so the safe path needs no argument. |
| `CLASS=disruptive` only on a disposable board | [target CLAUDE.md](../nodus/oevm/tests/target/CLAUDE.md) | Already enforced: the harness dies unless the board is the disposable one ([target.sh:102-106](../nodus/oevm/tests/target/lib/target.sh)). Keep. |
| Resolve boards through the collector env files | [target CLAUDE.md](../nodus/oevm/tests/target/CLAUDE.md), [tutorial 2](tutorials/02-board.md) | Already programmatic: no table exists in code. Keep. |

## 5. CLI, MCP, or both: PROPOSED — not decided

### What exists

| Surface | State |
|---|---|
| `porta` command line | Operator tool. Every command opens the SQLite store directly on the gateway host. [CLI.md](../porta/docs/CLI.md) is checked against the binary's `--help` by the suite's `porta-cli` aux gate ([porta/docs/README.md](../porta/docs/README.md)). |
| `custos` | Its own Go module, [`custos/`](../custos/) (#338). Over HTTP: the docs tree and status page, the fleet pages (`/fleet`, `/node/{id}`) and the named queries (`/q/{name}`) over porta's store read-only, and `/health` and `/manifest` passed through to porta ([GETTING-STARTED.md](../porta/docs/GETTING-STARTED.md), section 5). On the command line: `custos sql`, `tables`, `describe` over a live store or a snapshot. Not in the `.deb`; a systemd user unit on the bench gateway. The development verbs (debug, profile, load over USB) are the [custos peer design](design/2026-10-04-custos-peer-design.md)'s first slice, not built yet. |
| Build, flash, test scripts | Section 2. |
| MCP | **Not built** (deferred: the #198 plan's Task 6). The only Go mentions of MCP are comments ([custos main.go](../custos/cmd/custos/main.go) lines 6-8: the query surface is "stdio-MCP-first" and "lands here, never in porta"); no MCP code, no `custos mcp` command; the v1 MCP server is gone. The design's sketch is `gate sql`, `gate describe`, `gate tables`, `gate mcp`, with three tools `space_sql`, `space_tables`, `space_describe`, stdio only ([first-cut design](../porta/docs/design/2026-08-22-gatekeeper-first-cut-design.md), section 9). |

### The record's own position
- The query surface is commands first, with MCP over stdio as one more front ([first-cut design](../porta/docs/design/2026-08-22-gatekeeper-first-cut-design.md), section 9).
- An MCP server "should be a *client* of the path surface", not the other way round ([space-sync design](../porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md), Not-in-this-rung list).

### Comparison

| | Command line | MCP |
|---|---|---|
| Who can use it | Humans and any agent with a shell | Agents, including ones with no shell |
| How it is tested | Already gated against `--help` (`porta-cli`) | Needs its own tests |
| Discovery | `--help` | A typed tool list |
| State between calls | None | Can hold a session, for example a console stream |
| Cost | Exists | A second surface that drifts unless generated from the first |

**Conclusion: the command line is the contract, because one mechanism serves humans and agents and is already gated. MCP is a thin front generated over the same commands, added when a user whose agent has no shell appears, over stdio first so it adds no network surface before authentication exists.**

What makes a command agent-grade:
- A machine-readable output mode. None found on the commands above; `porta` prints text.
- Exit statuses that mean something. Today: `zephyr/run.sh` exits 2 (refused: bad argument, ambiguous device, missing overlay) and 3 (image does not match the request) and passes the build's own status on failure; `tests/target/run.sh` exits 1 on any failed scenario or `target::die`; the suite exits 1 on a red gate. Citations in section 3.
- Built-in time bounds: scenarios carry `TIMEOUT_S` and `OBSERVE_S` in their `.env` files; `run.sh --timeout` overrides. The suite has none found.
- No prompts: none found in the scripts read.

## 6. What 0.1 needs from this page

Decided 2026-10-02: the first release is a 0.x, explicitly unstable ([overview](overview.md)).

| Row | For 0.1 |
|---|---|
| Code and run, flash, observe | Needed. A newcomer must be able to do these three from the documents alone. |
| Debug, profile | **Needed.** David, 2026-10-02: debug and profiling are key to making this a step forward in embedded development. On a desktop both mostly exist and need a command and a page; on a board, see the gaps in section 3. |
| Both over-the-air layers | After 0.1. |

## 7. Gaps

| Gap | Today |
|---|---|
| No gate executes this page | Only the tutorials are executed (`tutorial-host`, `tutorial-porta`) |
| Tutorial 2 is prose | The `tutorial-board` gate checks only its scenario names |
| No newcomer document for debug or for profile | Designs only; no start-here page found |
| Firmware update not designed | rete#233 |
| Conventions not yet programmatic | Section 4 |
| No machine-readable output mode on `porta` | Section 5 |
| Tutorial 2 and `run.sh` disagree on a failed build's exit status | Section 3, Flash |
| The suite's run time is not pinned | Tutorial 1 says about four minutes; bench notes from earlier sessions say nearer twenty, long enough that a 15-minute timeout killed a run. Not re-measured for this page, and the suite has no time bound of its own. |

## 8. Sources

| Document | Used for |
|---|---|
| [tutorial 1](tutorials/01-host.md), [2](tutorials/02-board.md), [3](tutorials/03-porta.md) | Run, flash, observe commands |
| [CLAUDE.md](../CLAUDE.md), [nodus/nsl/CLAUDE.md](../nodus/nsl/CLAUDE.md), [target CLAUDE.md](../nodus/oevm/tests/target/CLAUDE.md) | Gates and traps |
| [zephyr/run.sh](../nodus/oevm/zephyr/run.sh), [target/run.sh](../nodus/oevm/tests/target/run.sh) | Flags, exit statuses |
| [console README](../nodus/oevm/zephyr/tools/console/README.md) | `logread.py` |
| [CLI.md](../porta/docs/CLI.md), [GETTING-STARTED.md](../porta/docs/GETTING-STARTED.md) | `porta`, `custos` |
| [PROTOCOL-NSL-RECORDS.md](../protocol/PROTOCOL-NSL-RECORDS.md) | `dbg/*`, `goal/debug`, `goal/profile` |
| [profile design](design/2026-08-06-method-profile-over-porta-design.md), [code-push design](design/2026-08-07-code-push-host-slice-design.md) | Profile, over the air |
| [first-cut design](../porta/docs/design/2026-08-22-gatekeeper-first-cut-design.md), [space-sync design](../porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md) | MCP |
| [security draft](design/2026-10-02-security-levels-design.md), [WHY-AGENTS.md](../review/WHY-AGENTS.md) | Signing; the observe, hypothesize, change, verify, bank-evidence loop |
