# The custos debug page (rete#340)

> **Approved by David, 2026-10-06** (sections 1–2 in conversation, then the written spec
> with its *as built* corrections). Built on branch `340-debug-page`.

First slice step 6 (`docs/design/2026-10-04-custos-peer-design.md` §6a; D5: CLI first, web
second, TUI third). The profile page went to #312; this page is the debugger's. It renders
what #323 built (`docs/design/2026-10-05-debugger-stack-and-names-design.md`): the stop in
source terms, the stack, and named locals.

## Decisions (David, 2026-10-06)

- **D1. The cable is held from start-up.** custos opens `--debug-port` once and keeps it
  until exit (not attach-on-demand, not host-VM-first).
- **D2. The session's state is data (approach A).** The page and the CLI read one
  `debug.Session`; the page never parses the CLI's prose (B) and there is no second,
  web-only session (C).

## 1. The session as data (`custos/internal/debug`)

- **`State`** — one snapshot:

  | Field | Meaning |
  |---|---|
  | `Seq` | rises on every change |
  | `Run` | `unknown` · `boot` · `running` · `stopped` · `ended` |
  | `Stop` | reason (`break`, `step`, …), file, line, method name, `off`, `pc` |
  | `Frames` | the last `bt` at this stop: where, file, line — innermost first |
  | `Locals` | frame N: `self` first, then name/value |
  | `Breaks` | the set breakpoints, as file:line |
  | `Transcript` | every line the CLI would have printed, the last `TranscriptCap` |

  `unknown` is the state before the first `dbg:` line: a board attached late may already
  have printed its `dbg:ready` (§2).
- **Every verb updates `State` and still `say`s exactly what it says today.** The CLI's
  output does not change by a byte; its tests and the `tutorial-debugger` gate are the
  regression guard for this refactor.
- **`Session.State(after int, wait time.Duration) State`** answers at once when
  `Seq > after`, else waits on the existing `changed` channel up to `wait`. That is the
  long-poll; no new mechanism.
- *As built:* `Output` became `Transcript`. `say` records each line it prints, so the
  page's Session pane is the CLI's screen (the program's output, the stops, the answers)
  and the page needs no second "text of this command" channel.
- **A shown stop fetches its `bt` and `locals 0`** so the page receives a complete stop
  without a follow-up command. Only the stop that is *shown*: a line step's hidden
  bytecode stops do not pay the two round trips. *As built:* the fetch is opt-in
  (`Session.AutoFetch`, set by the page only), so the CLI puts nothing extra on the
  wire, not merely nothing extra on the screen. A fetch and a command never share the
  line: both hold `cmdMu`, and a fetch whose stop has been replaced is dropped.
- **Breakpoints become session state** (today only the VM holds them). `break`/`clear`
  update `Breaks` on the VM's `dbg:ok`, never on the send.

## 2. The server (`custos/internal/custos/debug.go`, `debug.html`)

Shaped like `profile.go`: an embedded page and JSON endpoints, mounted only when its flag
is given.

- **Flags:** `--debug-port`, `--debug-map`, `--debug-source` (meanings as `custos debug
  -port/-map/-source`). At start-up: load the map, open the cable, attach the session.
  A map that fails to load or a cable that fails to open stops custos with the error.
  `--debug-port` equal to `--profile-port` is refused at start (one handle per cable).
- **Endpoints:**

  | | |
  |---|---|
  | `GET /debug` | the page |
  | `GET /debug/source` | the app's lines, and which have code (the page reads it once) |
  | `GET /debug/state?after=N` | long-poll: waits up to 25 s, then answers the current state anyway |
  | `POST /debug/do` | body = one command line in **the CLI's grammar, unchanged** (`break 26`, `step`, `locals 2`, any `dbg:` line); one at a time, a second answers 409; answers the state after it (its text is in `Transcript`) |

- **The page:** source on the left (the current line highlighted, breakpoints in the
  gutter; clicking a line sends `break N`, or `clear N` on a set one); stack and locals on
  the right (clicking a frame sends `locals N`); a button row (continue, step, next,
  finish, pause), the program output, and a command box taking any CLI line at the
  bottom. It uses `fleet.css`; the profile page's nav gains `debug`.
- **A late attach.** The page starts `unknown` and says *press pause*; that is the CLI's
  existing rule (`debug.go`: "the first command finds out"), not a new one.
- **Failures.** A VM that does not answer: the command's existing timeout, and the page
  shows its text. *As built (David, 2026-10-06: "we should put up a message"):* a cable that fails
  mid-session. `OpenPort` hands its failed read (go.bug.st/serial answers `PortClosed` on
  unplug) to an `onEnd`; the session's `OnLost` sets `Run` = `lost` and says
  `debug.LostCable`, and the page shows it as a banner. The CLI says the same line and
  ends. A restart of custos reattaches; there is no automatic reopen.

## 3. Tests

- **Go, `internal/debug`:** a fake `Line` replaying scripted VM lines: a `dbg:paused`
  yields `Run=stopped` with file/line, frames and locals filled; `Breaks` changes only on
  `dbg:ok`; a line step's hidden stops do not auto-fetch; `State(after)` blocks until the
  next change and returns on timeout. Existing `session_test.go` cases stay as they are,
  since they guard the CLI text.
- **Go, `internal/custos`:** the mux over the fake line: the long-poll wakes on a stop,
  a concurrent `/debug/do` answers 409, and equal debug and profile ports are refused.
- **Suite:** `test_custos_debug.py` and `tutorial-debugger` unchanged and PASS. They
  are the proof that the CLI's text survived.
- **Done means:** the gates above green, and one page session on `c6spare` (the
  DebugProbeC6 image): a break by click, a step, the stack through a block, and a named
  captured local, with a screenshot on #340.
- *As built (2026-10-06):* a pty cannot stand in for the cable (`OpenPort` sets modem
  bits; a pty refuses them), so the host check mounted the page over `StartHost` in a
  throwaway test (not committed). On `46_debug_target`, through HTTP: a break in the
  kept block arrived with a 4-frame stack (block, `scaled:`, `main:args:`, Supervisor),
  frame 1's locals `k = 5`, `acc = 0`, `add = [a block]`; `break 26`, `continue`, `next`
  showed `sum` 0 then 15.
- *Board (2026-10-06, `c6spare`, DebugProbeC6, collector stopped):* driven by DOM clicks
  in headless Chrome. A click on 55 and `continue` stopped in `countTo:` with a 6-frame
  stack and `n`, `sum`; a click on 63 stopped in the kept block with a 7-frame stack, and
  frame 1 named `k = 33351`, `acc = 0`, `add = [a block]`; clicks cleared both and
  `continue` left it running. Unplugging the board turned the page `lost` with its banner.

## 4. Fate of what it touches

| Item | Fate |
|---|---|
| `custos/internal/debug/session.go`, `frames.go` | `State` added; verbs update it; text unchanged |
| `custos/internal/custos/custos.go`, `cmd/custos/main.go` | `DebugPort/Map/Source` config and flags |
| `custos/internal/custos/debug.go`, `debug.html` | new |
| `custos/internal/custos/profile.html` | nav gains `debug` |
| `docs/tutorials/04-debugger.md` | a short "the same session as a page" section (promotion target) |
| `docs/development.md` §5 | the custos row: it still said the USB verbs were "not built yet" (stale since #339); now names them and both pages |

## 5. Addendum: the agent path (rete#362)

> **Approved by David, 2026-10-06** in conversation. rete is agent-first. Before #362 an
> agent driving `/debug` could be misled in three ways: a failed command answered 200,
> the HTTP path had no real-VM gate, and the wait for a stop was unwritten.

- **D3. A host VM is the second cable: `--debug-host "<VM command>"`.** It is the
  counterpart of `--debug-port`, and exactly one of the two may be given. Map loading,
  `CheckImage` and `StartHost` are reused unchanged, and when the VM exits `Run` becomes
  `ended`. It is also the agent's board-free path. A Go test that ran only when an
  environment variable named a built VM was rejected: a test-only path, and a SKIP in
  plain `go test`.
- **D4. The piped CLI keeps the pause rule, and the race is documented.** In a piped
  `continue` → `bt`, the `bt` stops the program wherever it is. The agent path is HTTP:
  send `continue`, then long-poll `/debug/state?after=<Seq>` until `Run` is `stopped`
  (or `ended`/`lost`). Never sleep. A piped mode that waited after `continue` was
  rejected: it would be a second CLI behaviour, and a piped `pause` could no longer
  interrupt anything.

**A result for every command.** `Session.Do` answers `Result{Leave bool, Err string}`
instead of `bool`. `Err` is set for: a line `Resolve` refuses, an unknown word, a bad
`locals` argument, a `bt`/`locals` error, "not stopped in a line", a VM `dbg:error`
(its line), and **no answer before the timeout**. The timeout is the one case where the
CLI's text changes: it says a line where today it is silent (a `break` with no `dbg:ok`
said nothing). These are not failures: a break moved to the next line with code (its
`dbg:ok` arrived), "already stopped", and the step limit being reached (the program is
held, and the transcript says where). `POST /debug/do` answers 200 with
`{ok, error, state}`; 409 for a busy session is unchanged. The page reads `.state` and
shows `error`.

**Tests.**
- `nodus/nsl/tools/test_custos_debug.py` gains an HTTP case: custos over `--debug-host`
  with `46_debug_target`. It waits for `boot`; checks that `break 26` → `ok` with 26 in
  `Breaks`, and that a header line → `ok: false` with an error; sends `continue`; then
  long-polls on `Seq` until `stopped` at line 26, where `locals` names `sum`. To falsify
  it, break the `ok` mapping once and watch the gate go red.
- Go, `internal/debug`: a fake line that never answers gives `Err` (the timeout), and
  `dbg:error` gives `Err`. Go, `internal/custos`: `/debug/do` answers `{ok, error, state}`,
  and both `--debug-host` and `--debug-port` together are refused.

**Promotion:** `docs/tutorials/04-debugger.md` gains "the agent path" (the long-poll
loop, `ok`/`error`, and the piped-CLI caveat).

## Not done here

- Values beyond `printString` (#359); send-by-send stepping (#322).
- The TUI (#341). The `State` struct is what it will read.
- Over the air (A′ through porta's `dbg/req`): the remote stage.
