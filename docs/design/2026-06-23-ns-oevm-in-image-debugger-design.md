# In-image debugger + mirror grant (POSIX, agent-drivable) — design

Date: 2026-06-23
Status: approved (brainstorm), pending implementation plan
Predecessor: `2026-06-23-ns-oevm-mirror-confinement-design.md` (the mirror capability this builds on)
Dialect: NEWSPEAK_DIALECT.md entry #16 (to be added)

## Context

The **mirror-confinement** step (shipped 2026-06-23, suite 69/0/3 both paths) made privileged
reflection a *confined capability*: the 12 reflective prims are reachable only by holding
`mirror_object()`, reached through one receiver-identity hook in `send()`. Crucially, **no boot
path grants a mirror**, so today *no `.ns` code can reflect at all* — confinement is total but the
capability is inert.

The confinement spec named a **Successor**: "the in-image debugger and the `debugger_hook` → jast
MCP wiring … build *on top of* the mirror capability." That successor is really four pieces:
1. the **mirror grant** (who holds the cap),
2. an **HLL `ObjectMirror`/`ClassMirror` facade** returning abstract mirrors, not raw oops (§11.4),
3. the **in-image debugger module** (breakpoint/step state machine on `mirror onBreak:`),
4. the **debugger_hook → jast MCP transport wiring** (the external connection).

This spec covers pieces **1–3 plus a thin POSIX stdin/stdout transport**, deliberately stopping
short of the Zephyr TFTP/SLIP stack and the jast-MCP gateway (piece 4, a later cycle). The driving
goal: a debugger that is **immediately useful on x86-linux and drivable by an LLM agent** today,
while keeping the remote/Zephyr future reachable with no protocol change.

The VM substrate already exists: `debugger_hook` fires on `OP_BREAK` and on single-step, reifying
the stack frame as a `MethodContext` (`oevm/src/vm.c`); the 12 mirror selectors are live in
`oevm/src/mirror.c`; `.stmap` source maps are emitted (`ns-oevm/tools/ns_compile_oevm.py`,
`tools/stmap.py`). What is missing is the grant, the `.ns`-side debugger/facade, the host byte
channel, and a method-resolution table for source-line breakpoints.

## Decisions (locked in brainstorm)

| Decision | Choice |
| --- | --- |
| Scope this cycle | grant + HLL facade + debugger module + thin POSIX stdio transport |
| Grant owner | a **new dedicated `Supervisor`** object (not threaded through `Kernel`) |
| Boot wiring | `Supervisor` is an **unconditional boot citizen**; app never names the mirror; no `.oimg` header change; production drops it via the existing `OEVM_NO_REFLECTION` seam |
| Host channel | **stdin/stdout lines** speaking the existing `dbg:*` verb grammar; agent-drivable via Bash/pexpect |
| Debug enable | gated by an **`OEVM_DEBUG` env var** so normal runs (69/0/3) are byte-for-byte unaffected |

## Architecture

### Boot & launch model

Both boot paths mint exactly one `Supervisor` and route the app launch through it:

- **Path A (solidified):** `ns_compile_oevm.py` emits, into `solidified_image.c`'s entry point,
  code that (1) allocates the `Supervisor` instance, (2) sends it the native `mirror_object()`
  (a new grant send, parallel to the existing `serialOn:` grant at
  `solidified_image.c` ~3424–3433), (3) hands it the app entry instance + its `main:args:`
  arguments, and (4) sends the supervisor a `run`/`launch` message instead of calling the app
  entry directly.
- **Path B (image loader):** `oimg_loader.c` (+ `kernel_boot.c`) does the same unconditionally.
  Because the grant is unconditional (not app-declared), **no new `.oimg` header field** is needed —
  the existing format is untouched.

The `Supervisor`, on `run`:
1. constructs the `Debugger` and registers it via `mirror onBreak: debuggerHook`;
2. **if `OEVM_DEBUG` is set**, enters an *initial command loop* (reads `dbg:*` lines so breakpoints
   can be set before first execution), returning when it reads `dbg:continue`/`dbg:run`;
3. launches the app entry (`anApp main: kernel args: args`).

When `OEVM_DEBUG` is **unset**, step 2 is skipped entirely: the supervisor is transparent, never
touches stdin, and the app runs exactly as today. With no breakpoints set and stepping off,
`OP_BREAK` never appears and the hook never fires — so the registered hook is inert and the full
suite stays 69/0/3.

The app source is unchanged and never names the mirror; only the supervisor (boot infrastructure)
holds the cap.

### The grant

Follows the serial pattern exactly (`oevm/src/serial.c` + the `Serial` wrapper in
`ns-oevm/lib/kernel.ns:210–234`, granted via `serialOn:`):

- C side: a new boot send hands `mirror_object()` to the supervisor instance (emitted by
  `ns_compile_oevm.py` for Path A; written in `oimg_loader.c`/`kernel_boot.c` for Path B).
- `.ns` side: the `Supervisor` stores the native mirror in an instance var and reaches all
  reflection through it (`mirror senderOf: ctx`, etc.). The confinement invariant is preserved:
  the *only* holder of the cap is the supervisor.

### HLL mirror facade (§11.4) — minimal, growable

A small `.ns` facade so `inspect` yields abstract mirrors, not raw oops:

- `ObjectMirror reflecting: anObject` — exposes the object's class/vtable identity and an
  instance-variable view (via `mirror vtableOf:` / `mirror bindingsOf:`).
- `ContextMirror` — wraps a reified `MethodContext`: `sender` (→ `ContextMirror`), `pc`, `method`,
  `registerAt:` / `registerAt:put:` (via `mirror senderOf:`/`pcOf:`/`reg:at:`/`reg:at:put:`).

The `Supervisor`/`Debugger` is the factory. Scope is exactly what `inspect` and stepping need;
deferred: `ClassMirror` metaobjects, reflective method invocation, the fuller Bracha public API.

### In-image Debugger module + mechanics

A `.ns` `Debugger` registered via `mirror onBreak:`, holding:
- a **breakpoint table** of `{methodId, offset, savedInstruction}`,
- **step state** (none / step-into / step-over / step-out, with a target frame depth).

Mechanics reuse only existing mirror prims (the set `test_debugger.c` already exercises):

- **set breakpoint** — resolve `methodId` → `CompiledMethod` (see Resolution below), save the
  instruction at `offset`, patch `OP_BREAK` via `mirror method:instructionAt:put:` (COW from flash).
- **clear breakpoint** — restore the saved instruction.
- **continue past a breakpoint** — restore original instruction, single-step once
  (`mirror fiber:singleStep:`) to execute it, re-patch `OP_BREAK`, then resume.
- **step / step-over / step-out** — drive `fiber:singleStep:`; on each step the hook compares the
  current sender-chain depth (via `ContextMirror sender`) against the step target to decide whether
  to pause or keep running.
- **inspect** — walk the frame via `ContextMirror`, format the `dbg:stack` JSON response.

The hook entry mirrors `debug_hook_c` in `oevm/tests/test_debugger.c`: read `pcOf:` the context,
classify breakpoint vs. step, then either report+await command or continue.

### Host transport (stdio, `dbg:` verbs)

When paused, the supervisor's command loop reads `dbg:*` command lines from **stdin** and writes
responses to **stdout**, using the **same verb grammar** as the Berry/nodus-st device
(`nodus-st/firmware/src/jast_debug.c`) so the later Zephyr/jast cycle is a byte-channel swap, not a
protocol change:

Commands (host → VM): `dbg:break <methodId> <offset>`, `dbg:clear <methodId> <offset>`,
`dbg:continue`, `dbg:step`, `dbg:over`, `dbg:out`, `dbg:inspect [frameIndex]`, `dbg:pause`.

Responses (VM → host): `dbg:paused <reason> <methodId> <pc>`, `dbg:stack <json>`,
`dbg:ok <msg>`, `dbg:error <msg>`.

Implementation note: needs a minimal POSIX **stdin line-read** prim. Reuse the existing POSIX
serial/console backend (`oevm/src/backend/posix/`, `serial.c` `basicReadByte`) if it already reads
stdin; otherwise add a small `debug` channel prim. Output reuses the platform console.

### Breakpoint resolution (debug symbol table)

The in-image debugger works purely in **method + bytecode-offset** terms — the device never sees
source lines (matches the confinement/remote spec). Source-line breakpoints are made
agent-drivable by splitting the work:

- **In-image:** the solidifier/compiler emits a **debug symbol table** assigning each
  `CompiledMethod` a stable `methodId`, keyed by the same `Class>>selector` string the `.stmap`
  already uses. The supervisor holds this as a simple `methodId → CompiledMethod` array.
- **Off-disk (agent side):** the driving agent reads `.stmap` (source line → bytecode offset) and
  the debug symbol table (`Class>>selector` → `methodId`), then sends `dbg:break <methodId> <offset>`.

This keeps the in-image side trivial (an array lookup) and reuses the `.stmap` contract unchanged.
The symbol table is emitted as a sidecar (e.g. `.dsym`) alongside the `.stmap`, by the same
emission path in `ns_compile_oevm.py`.

## Components & files

| Component | Location | Change |
| --- | --- | --- |
| Supervisor + Debugger + facade | `ns-oevm/lib/supervisor.ns` (new) | new `.ns` module compiled into every image |
| Mirror grant + supervisor mint (Path A) | `ns-oevm/tools/ns_compile_oevm.py` (~1880–1920 boot emit) | emit unconditional supervisor mint + `mirror_object()` grant + route launch |
| Mirror grant + supervisor mint (Path B) | `oevm/src/oimg_loader.c`, `oevm/src/kernel_boot.c` | same, unconditional |
| Debug symbol table emit | `ns-oevm/tools/ns_compile_oevm.py`, `tools/stmap.py` (or sibling) | emit `methodId ↔ Class>>selector` sidecar |
| POSIX stdin line-read | `oevm/src/backend/posix/` (+ prim wiring) | reuse or add a minimal `debug` channel prim |
| Build wiring | already includes `src/mirror.c` (`run_tests_oevm.py` lines ~82, ~208) | add any new C source to both Path A/B build lists |
| Negative/positive tests | `ns-tests/` + `oevm/tests/test_debugger.c` | see Verification |

## Verification

1. **Agent-driven integration test (the immediately-useful demo).** A small `.ns` app under
   `ns-tests/` plus a Python/pexpect harness that, with `OEVM_DEBUG=1`: reads `.stmap`+`.dsym` to
   translate a source line, sends `dbg:break <id> <off>`, `dbg:continue`, asserts a `dbg:paused`
   line at the expected method/pc, sends `dbg:inspect`, asserts a known local's value in the
   `dbg:stack` JSON, sends `dbg:step` then `dbg:continue`, and asserts the app's normal output.
2. **Revive `oevm/tests/test_debugger.c`** so it links and passes (closes **oevm#14** — the
   positive mirror-reflection proof, currently unlinked). Confirms the prim-level breakpoint /
   single-step / reg / hot-swap mechanics the `.ns` debugger builds on.
3. **Regression:** run the full suite with the gate **off** —
   `OEVM_PATH=AB python3 tools/run_tests_oevm.py` — and confirm **69/0/3 both paths**, byte-for-byte
   unchanged. Re-run the `check_no_ambient_reflection.sh` structural guard (still passes: the app
   still cannot name the mirror; only the supervisor holds it).
4. Remember the runner **clobbers** `../oevm/src/solidified_image.c` (+`.stmap`) every run —
   `git checkout` those after a suite run, per prior cycles.

## Explicitly deferred (reachable unchanged)

- Zephyr TFTP/SLIP transport and the jast-MCP **gateway** integration (piece 4).
- TCP/remote byte channel; post-mortem / remote second mirror implementation.
- Bracha-full reflection: `ClassMirror` metaobjects, reflective method invocation, public mirror API.
- A production `OEVM_NO_REFLECTION` build that strips `mirror.c` + the supervisor (the seam exists;
  not exercised this cycle).

Because the `dbg:*` protocol and the reflectee-as-argument mirror shape are fixed here, each
deferred item is an additive swap, not a protocol renegotiation.

## Open follow-ups (existing, relevant)

- **oevm#14** — revive `test_debugger.c`/`test_mirrors.c` (this cycle closes #14 via the revived
  debugger test + the granted `.ns` debugger path).
- **oevm#15** — add an `argc >= 1` guard in `mirror_dispatch` before `args[0]` (now load-bearing
  once a grant exists — fold into this cycle).
- **oevm#16** — `check_no_ambient_reflection.sh` false-PASSes from the wrong cwd (fix path
  resolution so the regression guard fails loudly).
