# rete

An embedded fleet system in one repository: **nodus**, the device end — a small VM and a
Smalltalk-family language running on Zephyr boards — and **porta**, the gateway that
orchestrates a fleet of them, with the wire contract they share in `protocol/`.

The two halves are coupled **only over the wire**; they never import each other's code.

## The VM in five lines

- **oevm** is a register VM in C on Piumarta's open object model: three primitive kinds,
  everything else a late-bound message send.
- It runs **nsl**, a Newspeak-derived dialect, compiled on the host into an `.oimg`
  image the flashed VM executes in place.
- **Run-to-completion**: one flat interpreter loop, one fixed stack. No fibers, no
  non-local return, no growable stacks — they were built, measured and deleted.
- Concurrency is a **reactor**: a tuple space of paths, `whenever:do:` reactions, a timer
  pulse, a supervisor that lets a job crash without taking the node down.
- A node's state is a **space** that porta mirrors over UDP + CBOR; operators write
  `goal/**`, nodes write `obs/**`, and convergence is a tree diff.

The one-page version is `docs/vm.md`.

## Tree

    porta/       the gateway (Go) — the northbound half
    nodus/       the device end
      oevm/      the VM (C) + the Zephyr board ports
      nsl/       the dialect, its compiler and the kernel (which is itself nsl)
      tests/     the nsl suite
    protocol/    the wire contract (v1 frozen under protocol/v1/)
    review/      the program ladder and the claims ledger
    docs/        the VM's documents — start at docs/README.md
    tools/       repo-wide scripts

## Gates

    cd nodus/nsl  && python3 tools/run_tests_oevm.py     # Passed: N / Failed: 0, and read the aux line
    cd porta      && go test ./...
    cd nodus/oevm && bash tests/porta_sync_host.sh      # PORTA SYNC OK

The suite count only ever drifts upward; the number that matters is `Failed: 0` plus a
green aux-gate line (C unit tests, footprint, manifest). Board gates are in
`nodus/oevm/tests/target/` and need hardware.

## Where to read next

| you want | read |
|---|---|
| what the VM is and is not | `docs/vm.md`, then `docs/MILESTONE_A.md` |
| the language and the kernel | `nodus/nsl/README.md`, `nodus/nsl/lib/kernel.nsl` |
| the gateway | `porta/README.md`, `porta/docs/ARCHITECTURE.md` |
| the wire | `protocol/PROTOCOL-NSL.md`, `protocol/MANIFEST.cddl` |
| where the program is going | `review/program-sequence.md`, `docs/next-steps-overview.md` |
| every document, indexed | `review/DOCMAP.md` |
| the standing rules for working here | `CLAUDE.md` |

**There is no tutorial yet.** Building the VM, running ten lines of nsl on the host, and
flashing a board are the three walkthroughs #79 owes an outsider; until they exist, the
suite runner (`nodus/nsl/tools/run_tests_oevm.py`) and `nodus/oevm/zephyr/run.sh` are
the executable documentation.

## Lineage

`st/berry` → `ns/berry` → `ns/oevm` → `nsl/tuvm` → **rete**. Four wholesale rewrites to
get here; nothing is built to last, it is built to teach.
