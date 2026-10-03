# docs/ — the program's documents

This directory holds what spans the whole system. Each subsystem keeps its own
reference documents beside its code — that is the rule the 2026-08-02 consolidation
wrote down (`design/2026-08-02-rete-repo-consolidation-design.md`, fate table row
"tuvm docs") and 2026-08-29 finished executing:

| subsystem | its documents |
|---|---|
| the VM | `../nodus/oevm/docs/` — start at `vm.md`; the milestone notes; the archive |
| the board ports | `../nodus/oevm/zephyr/docs/` — `BOARD_PORTS.md`, `ZEPHYR_LESSONS.md` |
| the language | `../nodus/nsl/docs/` — `NEWSPEAK_DIALECT.md`, `programming_with_nsl.md` |
| the gateway | `../porta/docs/` — start at its `README.md` |
| the wire | `../protocol/` — `PROTOCOL-NSL.md`, `MANIFEST.cddl` |
| the ladder | `../review/` — `program-sequence.md`, `tests/CLAIMS.md`, `DOCMAP.md` |

## Here: the program level

**Tutorials — executed, not trusted**
- `tutorials/01-host.md` — build the VM, run the suite, ten lines of nsl, the compiler by
  hand, the no-NLR refusal.
- `tutorials/02-board.md` — a real ESP32-C6: the board's two adapters and which one the
  harness uses, the always-on console collector, the Thread overlay whose absence is silent,
  a hand flash, an annotated healthy boot, and the eleven target gates (~60–90 min, not
  unattended). **Prose, not executed** — the only one of the three, and the page says so in
  its own banner: a board tutorial cannot be gated on a host, so every transcript on it is
  copied from a dated bench log rather than written to look plausible.
- `tutorials/03-porta.md` — a `porta serve` and a real node on loopback: operator intent
  for a node that has never spoken, the check-in, `goal/` converging to `obs/` as a tree
  diff, the write log in both directions, and the manifest gate refusing a mis-kinded
  write. Needs a Go toolchain; it binds udp 16969/16970, so it cannot run beside
  `nodus/oevm/tests/porta_sync_host.sh`.
- `tutorials/04-debugger.md` — the debugger for a person: stop at a line, step by line,
  finish, inspect, pause a running program, on the desktop and over a C6's cable
  (`nodus/nsl/tools/dbg.py`, rete#311). The board half is prose.

  **Every `bash gate` block on pages 1, 3 and 4 is run by the nsl suite** (`tutorials/gate.sh`,
  the `tutorial-host`, `tutorial-porta` and `tutorial-debugger` aux gates), so a stale step reads red. A new
  gated tutorial is one more call to `run_tutorial_gate()`. Page 2 has no such blocks and
  cannot: it needs a devkit, a toolchain and a mesh, and a gate that degraded to a SKIP
  without them would report success for a run that never happened — which is exactly what
  `nodus/oevm/tests/target/run.sh` refuses, and why it is not in the suite either.

**Living (edit, don't append)**
- `overview.md` — **the whole on one page**: the road to release (0.1, 1.0.0, later) and
  the pieces by theme, each with its starting document. A hub; it owns no facts.
- `next-steps-overview.md` — what gates a public release and what is on the bench.
- `status/` — the orientation deck (`dev-status.html`, live at gw85224-01:6971) and its
  generator: what is top of mind right now, by issue band.

**Dated records — they stay here, under their date, because that is where the
citations point.** A decision record is program-time evidence, not a subsystem's
property; a liveness design touches the VM, the kernel and porta at once.
- `design/` — the decision records. Superseded ones say so at the top. A design is closed
  only when its knowledge has been promoted to a mainline document — `CLAUDE.md`
  §Docs convention. One page here is **not** a decision record and says so:
  `design/2026-08-31-liveness-timers.html` is a standalone reference (open it directly, no
  build step) mapping every constant in the liveness path — nine on the node, one on the
  gateway, none in custos — with `file:line` for each, and stating **#273's fork without
  answering it**. When #273 is decided, the decision gets its own dated record and this page
  is corrected to match.
- `results/` — board and host measurements, one file per experiment.
- `spikes/` — throwaway code and images that earned a number; kept for the number.
- `superpowers/plans/` — execution scaffolding for the designs; historical once the work
  closes, never a document of record.

## The archive rule

A document leaves the mainline by **move + banner**, never by deletion and never by a
bare move: it goes to the `archive/` of the subsystem that owns it
(`../nodus/oevm/docs/archive/`, `../porta/docs/archive/`; a program-level one would go to
`archive/` here) with a `HISTORICAL — archived DATE` first line naming what replaced it
and what was deleted since. Kept so that `#NN` issues and commits still resolve by path;
the banner is what stops an archived spec being read as current.
