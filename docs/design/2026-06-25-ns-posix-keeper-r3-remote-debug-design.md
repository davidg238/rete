# R3 — remote-debug ns-posix through porta (design)

> **Status:** design (decided 2026-06-25). Rung **R3** of the Newspeak-node roadmap
> (`docs/superpowers/plans/2026-06-25-ns-zephyr-capability-node-roadmap.md`).
> Predecessors: R1 keeper (`2026-06-25-ns-posix-keeper-r1-design.md`); R3 is **independent
> of the deferred R2** (see §6). Implementation repos: `ns-posix` (keeper) + `porta`
> (gateway). The VM debugger it bridges already ships in `oevm`/`ns-oevm`.

## Decision

Bridge the existing in-image `dbg:*` debugger over porta so an operator can **remote-debug
a deployed ns-posix node from a porta client** — poll-paced, store-and-forward, no live
socket. The debugger, its `.ns` facade, C-assisted breakpoints, and `OEVM_DEBUG=1` entry
are reused **verbatim**; R3 adds only a transport bridge.

## Reused verbatim (no change)

- **`dbg:*` protocol** — line-oriented request/response: `dbg:methods / break / clear /
  continue / step / over / out / inspect`. Read via `platform_debug_readline` (`fgets`
  on stdin); responses via `println`. Sentinels: `dbg:ready`, `dbg:ok <verb>`,
  `dbg:paused <break|step> <id> <off>`, `dbg:stack ...`, `dbg:error <reason>`.
- **`.ns` facade** — `Debugger` nested in `Supervisor` (`ns-oevm/lib/supervisor.ns:59-336`);
  entered when `OEVM_DEBUG=1`, prints `dbg:ready`, blocks in `commandLoop`.
- **C-assisted breakpoints** — `oevm/src/vm.c:225-252` fires the `onBreak:` hook; the node
  runs autonomously to the breakpoint (~1.28×, [[debugger-run-to-breakpoint-cost]]),
  never streaming per-instruction over the wire.
- **stdout flushing** — `platform_console_flush` does `fflush(stdout)` and the println
  path flushes on every newline, so each response line is readable over a **plain pipe**
  immediately. ⇒ **dual pipes, no PTY** (see §3).

## Architecture: session in the keeper, relay in porta

The recon's "porta can't do interactive debug" conclusion holds only if the *session*
lives in porta. It does not. **The session lives in the keeper** — the one process
holding the live, paused VM. **porta stays stateless store-and-forward**, relaying
`dbg:` lines exactly as it already relays `commands` (node-ward) and `report`/`data`
(server-ward). Latency = poll cadence, which the roadmap accepts ("single-step latency
lands where speed is irrelevant").

The poll-paced loop:
1. Client enqueues `dbg:break <id> <off>` (a debug request) at porta.
2. Keeper drains it on its poll, writes it to the live VM's stdin, reads the response
   line(s) up to a sentinel, uploads them.
3. Client enqueues `dbg:continue`; keeper feeds it; VM runs autonomously to the
   breakpoint, prints `dbg:paused …`, blocks on stdin (alive); keeper uploads `dbg:paused`.
4. … repeat turn-by-turn until `detach` or VM exit.

The VM's existing blocking `commandLoop` works **as-is** — the keeper's pipe decouples
"VM blocks on stdin" from "client provides input poll-paced."

## 1. porta protocol extension

### New `debug` verb (commands channel) — declarative

```json
{"verb":"debug","name":"<app>","action":"attach"|"detach"}
```

A **goal-state** ("this node should be debugging app `<app>`"), last-write-wins per the
§2 declarative model — not imperative like `reboot`. `attach` launches the named,
already-installed app under the debugger (long-lived), **superseding its normal run-once
execution while attached**. `detach` tears the session down. The app's image must already
be present from a prior `run` (the keeper caches by crc); absent ⇒ the keeper reports a
debug-session error rather than silently no-op'ing. **(Implemented 2026-06-26, ns-posix#1:**
`do_debug` emits `dbg:error <reason> <app>` over the `debug?id=` channel — `no-app` /
`no-image` / `start-failed` — and starts no session, so `porta debug poll` surfaces the
failure; consistent with the in-image debugger's existing `dbg:error` form.**)**

### New `debug?id=` channel — symmetric, stateless

| Direction | Op | Body | Mirrors |
|-----------|-----|------|---------|
| node-ward (requests) | **RRQ `debug?id=<id>`** | drain queued `dbg:` request lines, newline-delimited text, drain-to-empty (zero-byte body = drained) | `commands?id=` |
| server-ward (responses) | **WRQ `debug?id=<id>`** | append `dbg:` response lines to a per-node debug log | `data?id=` |

**Storage:** a per-node debug-request **queue** (drain semantics, like commands) + a
per-node debug-response **log** (append + read-after-cursor, like data_log). Reuses
existing store patterns; no per-node session state in porta.

### Client

A minimal **`porta debug` CLI** for desktop: `porta debug attach <node> <app>` then a
turn-by-turn prompt that enqueues one `dbg:` line and prints response lines up to the next
sentinel (`dbg:ok`/`dbg:paused`/`dbg:error`). An **MCP bridge** (à la the retired jast
`debug_*` tools) is noted as **future**, not R3.

## 2. ns-posix keeper — the session host

- **`debug attach`** ⇒ keeper spawns the app's cached image under `OEVM_DEBUG=1` as a
  **long-lived child with dual pipes** (keeper writes child stdin, reads child stdout),
  held in keeper session state. Distinct from the run-once `popen("r")` path, which stays
  intact. While a debug session is active for app X, the keeper does **not** run-once X.
- **Per-poll shuttle** (added to `keeper_poll_once`, **after the command drain, before
  `upload_report`** — so the report reflects post-shuttle session state): drain `debug?id=`
  requests → write each to child stdin → read child stdout response lines (short timeout,
  until a sentinel / EOF) → WRQ `debug?id=` with the lines.
- **Run-to-breakpoint**: after feeding `dbg:continue`, the VM executes; the keeper reads
  until `dbg:paused` (hit, VM now blocked alive) or EOF (program completed → session ends).
- **Teardown**: `debug detach`, or VM EOF → reap child, clear session, report.
- **Latency note**: poll cadence sets round-trip latency. R3 reuses the existing
  `loop_sleep_s`; tightening cadence while a session is active is a possible refinement,
  out of scope for R3.
- **Daemon requirement (discovered during build, 2026-06-26):** because the live VM
  child and its pipe fds live in the **keeper process's** memory, a debug session can only
  span polls if the keeper runs as a **persistent daemon** for the session's duration. A
  per-poll `keeperd --once` invocation (a fresh process each poll) closes the stdin
  write-end on exit and kills the VM child, so it cannot host a session. The desktop
  end-to-end gate therefore runs `keeperd` in daemon mode; on hardware (R6) a resident
  daemon is the natural model anyway. (The keeper also ignores `SIGPIPE` so a write to a
  child that exited mid-burst returns `EPIPE` rather than killing the daemon.)

## 3. The debug seam — separate struct, always compiled in

R3's only new **native** code is the keeper-side pipe transport. It is exposed as a
**separate `debug_vm` seam struct** (a clean, audited boundary), and the keeper
**always compiles it in** — making any deployed node OTA-debuggable (see §5).

```c
// keeper/seam.h — debug sub-interface (R3). Always wired in keeperd.c.
struct debug_vm {
    int (*start)(void *ctx, const char *name, const uint8_t *img, size_t len); // spawn alive, OEVM_DEBUG=1
    int (*write)(void *ctx, const char *line);                                 // feed one dbg: request line
    int (*drain)(void *ctx, char *out, size_t cap, int timeout_ms);            // read response lines (to sentinel/EOF)
    int (*stop)(void *ctx);                                                     // reap the child
    void *ctx;
};
```

Posix implementation: **new `posix/oimg_debug_vm.c`** — `posix_spawn`/`fork` + dual pipes
(no PTY; the VM flushes on newline). The device backend implements the same four functions
at R6; the keeper core code is identical on both. The run-once `struct vm` (`oimg_vm.c`) is
untouched.

## 4. Module boundaries

| Module | Change |
|--------|--------|
| `keeper/seam.h` | Add `struct debug_vm` (the four pointers above). |
| `keeper/cmd.{c,h}` | Parse the `debug` verb (`name` + `action`) into `struct command`. |
| `keeper/keeper.{c,h}` | `struct keeper` gains a `debug_vm *` + debug session state (active app, child handle held by the seam ctx). `dispatch(CMD_DEBUG)` attaches/detaches; `keeper_poll_once` runs the per-poll shuttle; debug-active app is excluded from the run-once goal. |
| `posix/oimg_debug_vm.c` *(new)* | Dual-pipe long-lived child implementing `struct debug_vm`. |
| `posix/keeperd.c` | Always wire the `debug_vm` into the keeper. |
| **porta** `internal/command` | Encode/validate the `debug` verb. |
| **porta** `internal/handler/handler.go` | Route RRQ/WRQ `debug?id=` to new `readDebug`/`writeDebug`. |
| **porta** `internal/store` | Debug-request queue + debug-response log (new table/funcs, mirroring commands + data_log). |
| **porta** `internal/portacli` | `porta debug attach/detach` + turn-by-turn driver. |
| **porta** `docs/PROTOCOL.md` | Document the `debug` verb (§2) and `debug?id=` channel (new §). |

## 5. OTA-debug property (why "always compiled in")

Debug splits across **native substrate** (must pre-exist) and **image behavior** (ships
OTA via porta as `.oimg` bytecode):

| Piece | Layer | Pre-exists? |
|-------|-------|-------------|
| Breakpoint hook (`vm.c`), debug mirror prims (`mirror.c`) | native, **already in every oevm** | yes |
| `Debugger` facade + `dbg:*` protocol | **image (`supervisor.ns` → `.oimg`)** | ships OTA |
| Keeper pipe transport (`debug_vm`) | native, in keeper firmware | **yes — always compiled in (R3)** |

Because the debugger's *behavior* is image-delivered and the thin native substrate is
always present, **a deployed node becomes debuggable purely OTA** — ship a debug-capable
image + send the `debug` verb, no reflash. Omitting the keeper transport (the alternative)
would forfeit this; it is small and costs zero when idle (breakpoints gate-off zero,
[[c-assisted-breakpoints-decision]]), so it is always included. Loading *native* code OTA
is explicitly **not** pursued — it cuts against the stack's thesis (behavior ships as
bytecode; the native base stays thin, fixed, audited).

## 6. R2 dependency — resolved

The roadmap lists R3 as depending on R2 because debug responses could otherwise ride
`data?id=`. With the **dedicated `debug?id=` channel**, R3 carries its own traffic and is
**independent of R2** — it ships with R2 still deferred.

## 7. Testing (desktop, two localhost processes)

| Test | Asserts |
|------|---------|
| `tests/test_cmd.c` | `debug` verb parse (`name` + `action`: attach/detach; unknown action rejected). |
| `tests/test_keeper.c` | Session lifecycle via `fake_seam`: attach launches the debug VM and excludes the app from run-once; per-poll drain→feed→read→upload; detach reaps. |
| `tests/test_debug_vm.sh` *(new)* | `oimg_debug_vm` dual-pipe round-trip against a **real** oevm debug image: `dbg:methods` → ids; `dbg:break`/`continue`/`inspect` → `dbg:paused`/`dbg:stack`; newline-flush timing holds over a pipe. |
| porta unit tests | `debug` verb codec; `debug?id=` RRQ drain + WRQ append; request-queue + response-log store round-trip. |
| `tests/porta_integration.sh` (extend R1 gate) | Full loop **over porta**: deploy `46_debug_target`-style app → `porta debug attach` → `dbg:break` → `dbg:continue` → assert `dbg:paused` returns → `dbg:inspect` → assert `dbg:stack` → `detach`. |

## 8. Acceptance

- Keeper unit tests pass (existing + new); 32-/64-bit where the R1 gate runs both.
- `test_debug_vm.sh` drives a real oevm debug image over dual pipes and observes
  `dbg:paused`/`dbg:stack`.
- The porta integration gate completes a remote attach → break → continue → inspect →
  detach **through porta**, with `dbg:paused`/`dbg:stack` returned over `debug?id=`.
- `debug` verb + `debug?id=` channel documented in `porta/docs/PROTOCOL.md`.

## 9. Out of scope (explicit)

- MCP debug bridge (future; a `porta debug` CLI suffices for R3 desktop acceptance).
- **DAP-adapter path (future).** `dbg:*` is a bespoke, line-oriented, transport-friendly
  protocol — *not* a standard (GDB-RSP / DAP / `ptrace`), because it debugs at the
  **bytecode/method/frame** level of the managed VM (Smalltalk-lineage), not native
  memory/registers, and must ride porta's **poll-paced store-and-forward** transport
  (GDB-RSP and DAP both assume a live, chatty connection). Its verbs nonetheless map
  cleanly onto the standards (`break`/`clear`↔`Z0`/`z0`↔`setBreakpoints`;
  `continue`↔`c`↔`continue`; `step`↔`s`↔`stepIn`; `over`↔`n`↔`next`; `out`↔`finish`↔
  `stepOut`; `inspect`↔frame/regs↔`stackTrace`/`variables`). So a future **DAP adapter on
  the porta/client side** can translate DAP ↔ `dbg:*`, letting VS Code / editors drive a
  node without the node itself speaking DAP — keeping the wire minimal while gaining
  editor integration. The node/wire protocol stays `dbg:*`; the adapter is purely
  client-side.
- Tightened poll cadence while a session is active (latency refinement).
- OTA-loading of **native** code (against the thesis; debug behavior already ships as image).
- Multiple concurrent debug sessions per node (one app under debug at a time in R3).
- The device backend's `debug_vm` implementation (arrives with the R6 ns-zephyr port).
