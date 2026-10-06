# The custos debug page (rete#340)

> **DRAFT 2026-10-06, awaiting David's review.** Sections 1–2 were approved in
> conversation (2026-10-06); section 3 and the fate table were written after. An initial
> implementation followed at David's request the same night (branch `340-debug-page`); the
> corrections it forced are marked *as built*.

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
  shows its text. *As built, not handled:* a cable that fails mid-session. `OpenPort`'s
  reader ignores the end of a cable (the CLI's too), so an unplugged board reads as a
  silent one, and a restart of custos reattaches. Wiring `onEnd` through `OpenPort`
  touches the profile and load callers; it waits until a dropped cable has actually cost
  a session.

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
  showed `sum` 0 then 15. The board session is still owed.

## 4. Fate of what it touches

| Item | Fate |
|---|---|
| `custos/internal/debug/session.go`, `frames.go` | `State` added; verbs update it; text unchanged |
| `custos/internal/custos/custos.go`, `cmd/custos/main.go` | `DebugPort/Map/Source` config and flags |
| `custos/internal/custos/debug.go`, `debug.html` | new |
| `custos/internal/custos/profile.html` | nav gains `debug` |
| `docs/tutorials/04-debugger.md` | a short "the same session as a page" section (promotion target) |
| `docs/development.md` §5 | the custos row: it still said the USB verbs were "not built yet" (stale since #339); now names them and both pages |

## Not done here

- Values beyond `printString` (#359); send-by-send stepping (#322).
- The TUI (#341). The `State` struct is what it will read.
- Over the air (A′ through porta's `dbg/req`): the remote stage.
