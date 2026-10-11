# nodus/nsl — the nsl suite and its aux gates

Loaded when working under `nodus/nsl/`. Moved verbatim from the root `CLAUDE.md` (2026-09-30); the root keeps the
gate commands and the rules every session needs.

The nsl suite prints `Passed: N / Failed: 0` **and** a line of aux gates — read the aux
line, because it can be red while the count reads zero failures.
**`c-units` leads that aux line and covers ALL 43 C unit tests** (#275, 2026-08-29):
`nodus/oevm/tools/run_c_tests.sh` is DELETED — it was a second aggregate runner nothing
invoked, so it sat red (four aborts) without anyone learning it, which is #261's shape.
Every C test is now registered individually in `nodus/nsl/tools/run_tests_oevm.py`'s
`C_UNIT_TESTS` table, which also owns the `ENG_A`/`ENG_B` link sets and the
`OEVM_BUILD=asan|ubsan` knob. Adding a C test is one row in one file; a test registered in
two places is one that gets forgotten in one of them. The registry runs every test and
reports every failure rather than stopping at the first.
**`tutorial-host`, `tutorial-porta`, `tutorial-board` and `tutorial-debugger` (rete#311, executed like page 1) close that aux line** (#79, 2026-08-29):
`docs/tutorials/gate.sh` extracts every `bash gate` block from a page and runs them in a
fresh directory, canary first. A tutorial is the one document nobody who knows the system
re-reads, so it is executed instead — edit the page and the gate follows; a new tutorial is
one more call to `run_tutorial_gate(page, label)`. **`tutorial-porta` (03-porta.md, ~12 s)
runs both halves of the repo against each other**: a real `porta serve` and a real
`PortaBridgeNode` image on loopback. It therefore binds **udp 16969/16970** — the node's
`gwPort` is a source constant — so it must never run beside
`nodus/oevm/tests/porta_sync_host.sh`, which claims the same pair (that gate is standalone,
so the suite does not collide; two concurrent suites would). It is the ONE gate here that
needs a Go toolchain, and `needs=("go",)` makes an absent `go` a named SKIP rather than a
hard failure, because a missing Go must not couple the nsl suite to the gateway half.
**`tutorial-board` is the odd one and deliberately so**: `docs/tutorials/02-board.md` is PROSE
— its walkthrough needs a devkit, a toolchain and a mesh, so it carries no `bash gate` blocks
and its banner says why. The gate checks the ONE claim on it that is a fact about this repo
rather than about a board: §7's scenario NAMES against `nodus/oevm/tests/target/*.env`. Names
only — a missing scenario is a bug, a stale description is a judgement call, and a gate that
scores prose is one that gets disabled. An unparseable table yields the empty set and FAILS,
because vacancy must not read as agreement.
**`porta-cli` closes it after them** (#279, 2026-08-29): `porta/docs/cli-gate.sh` builds
porta, walks its `--help` tree, and fails if `porta/docs/CLI.md` names a command or flag the
binary does not have, or omits one it does. The tutorial answer — execute the page — is not
available to a REFERENCE, whose claims nobody runs; cobra's own account of itself is. Names
only, both directions, transcripts included; it falsifies itself on an empty page and on an
invented flag before it clears anything. Needs Go, so `needs`-style SKIP applies for
tutorial-porta's reason.
**`footprint: SKIP` is NO LONGER expected, and it never should have been** (#177, fixed
2026-08-22): arm-zephyr-eabi-gcc ships inside `~/zephyr-sdk-0.17.4` and always did. What was
broken was the gate's own un-SKIP instruction, which named a path the #79 consolidation
deleted — so the SKIP was unfollowable, and this file recorded it as normal. `footprint.sh`
now finds the SDK itself. **A SKIP here fails the suite (#314).** A second suite refuses to
start (one lock, `/tmp/rete-nsl-suite.lock`). The suite takes ~5 min on msi (282 s, 2026-10-11): the compiler caches parsed ASTs under `~/.cache/nsl-parse/`, keyed by a hash of source, grammar, parser and ohm-js (`NSL_PARSE_CACHE=off` to bypass); before that every compile re-parsed the kernel and the suite took ~20 min (#416). **The count above drifts upward as
tests land — treat a mismatch as staleness in this file, never as a missing test.** The
suite also prints `target gates: NOT RUN BY THIS SUITE`: the posix timer backend bypasses
the pulse mailbox, so coalescing, shedding and stale handles need `nodus/oevm/tests/target/run.sh`
and a board.
