# The custos TUI (rete#341)

> Status: design approved by David in conversation, 2026-10-06. Third front of the custos
> peer design (`2026-10-04-custos-peer-design.md` D5, §4), after the agent paths (D11:
> #362 debug, #364 profile) and D12 (HTTP is the only agent surface, #365).

## Decisions

- **D1 — The TUI is an HTTP client of a serving custos.** `custos tui [--url URL]`
  (default `http://127.0.0.1:6971`) talks only to the endpoints the page and an agent
  use: `POST /debug/do`, `GET /debug/state?after=Seq` (long-poll), `GET /debug/source`,
  `POST /profile/capture?ms=N`, `GET /profile/last`. It holds no cable, embeds no
  `debug.Session`, and parses no prose: it renders `debug.State` and `profile.Report`.
  A target is chosen where custos is started (`--debug-host`/`--debug-port`,
  `--profile-host`/`--profile-port`), never in the TUI.
- **D2 — custos owns the cable; every front is its client.** This replaces peer design
  §4's "whichever front is running owns the cable" (written before D12). The shared cable
  daemon §4 deferred is, in effect, the serving custos; the collector is still stopped
  around it by hand, as for the page. Consequence wanted: the TUI can run on one machine
  (an Omarchy desktop) against a custos on another (the tailnet), and beside the page.
- **D3 — Colours are the terminal's 16 ANSI colours only.** Omarchy's theme sets the
  terminal palette, so the TUI follows the current theme without reading any Omarchy file.
  No theme config, no hex colours.
- **D4 — A subcommand of custos, code in `custos/internal/tui`.** One binary; new
  dependencies `charmbracelet/bubbletea` v1.3.10 and `lipgloss` v1.1.0. *Amended in the
  plan:* no `bubbles` — a one-line input and a list are cheaper hand-rolled than a third
  module; v1 rather than v2 because its API is the one the plan's code was written
  against (a port to v2 is mechanical when it earns one).

## Screens

Two tabs, switched with `tab`.

**Debug** — four tiled panes:

| pane | from | interaction |
|---|---|---|
| source | `/debug/source`; current line from `State.Stop`, marks from `State.Breaks` | cursor ↑/↓; `b` toggles a breakpoint on the cursor line (`break N` / `clear N`) |
| stack | `State.Frames` (custos fetches `bt` and `locals 0` on every stop, AutoFetch) | select a frame → `locals N` |
| locals | `State.Locals` | — |
| transcript | the tail of `State.Transcript` (the "log tail") | — |

Keys: `c` continue, `s` step, `n` next, `f` finish, `p` pause, `:` a command line taking
any command of tutorial 4 §4, `q` quit. A status line shows `Run`, the stop, and the last
command's `Error` when `OK` is false.

**Profile** — `r` asks for a window in ms (default 3000) and posts a capture; the ranked
`Report.Rows` fill a table, warnings above it. On start the tab shows `/profile/last` if
there is one.

## Data flow

One goroutine long-polls `/debug/state?after=Seq` and delivers each `State` as a
`tea.Msg`; commands are `tea.Cmd`s posting to `/debug/do`, whose answer's `State` is
applied too (Seq decides which is newer). Nothing sleeps to wait for a stop.

## Errors

custos unreachable, a 404 (no debug or profile target configured), or a 409 (a command or
capture already in flight) shows in the status line; the long-poll retries with a short
back-off. `Run = lost` is shown as such. The TUI never exits on an error except `q`.

## Testing

- **Go unit tests:** the model is fed `State` / `Report` JSON and key messages; `View()`
  is checked for the stop line, breakpoint marks, frames and locals.
- **Real-VM gate:** start `custos --debug-host` on a suite target, drive the TUI's model
  with key messages and no terminal (break, continue, wait for the stop, step, `locals`),
  against the same long-poll; then a profile capture over `--profile-host`. Placed as an
  extension of `test_custos_debug.py` / `test_custos_profile.py` if that is less
  mechanism than a new aux gate; decided in the plan.
- **Falsified:** break one assertion and confirm the gate fails at it.

## Not in this slice

- `custos load` has no HTTP path yet (D11 orders it before a TUI covers it).
- Source with per-line profile counts beside it (peer design §4 page idea) — later.
- Expanding a value's slots (#359).

## What changes on adoption

- Peer design §4 "Who holds the cable" gets an addendum citing D2 here.
- `docs/development.md` custos row and tutorial 4 gain a short "the TUI" note.
