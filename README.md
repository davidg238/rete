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

The one-page version is `nodus/oevm/docs/vm.md`.

## Tree

    porta/       the gateway (Go) — the northbound half
    custos/      the nsl developer's peer (Go): today the query engine and fleet pages
    nodus/       the device end
      oevm/      the VM (C) + the Zephyr board ports
      nsl/       the dialect, its compiler and the kernel (which is itself nsl)
      tests/     the nsl suite
    protocol/    the contract, and the Go module porta and custos share (the Toit-era protocol is archived under protocol/archive/)
    review/      the program ladder and the claims ledger
    docs/        the program's documents: tutorials, status, the dated records — docs/README.md
                 (each subsystem keeps its own reference docs: nodus/oevm/docs, nodus/nsl/docs, porta/docs)
    tools/       repo-wide scripts

## Gates

    cd nodus/nsl  && python3 tools/run_tests_oevm.py     # Passed: N / Failed: 0, and read the aux line
    for m in protocol porta custos; do (cd $m && go test ./...); done
    cd nodus/oevm && bash tests/porta_sync_host.sh      # PORTA SYNC OK

The suite count only ever drifts upward; the number that matters is `Failed: 0` plus a
green aux-gate line (C unit tests, footprint, manifest). Board gates are in
`nodus/oevm/tests/target/` and need hardware.

## Where to read next

| you want | read |
|---|---|
| the first hour, step by step | `docs/quickstart.md` |
| what 0.1 does not protect against, and what will change | `docs/limits.md` |
| the whole on one page, and the road to release | `docs/overview.md` |
| what the VM is and is not | `nodus/oevm/docs/vm.md`, then `nodus/oevm/docs/MILESTONE_A.md` |
| the language and the kernel | `nodus/nsl/README.md`, `nodus/nsl/lib/kernel.nsl` |
| the gateway | `porta/README.md`, `porta/docs/ARCHITECTURE.md` |
| the wire | `protocol/PROTOCOL-NSL.md`, `protocol/MANIFEST.cddl` |
| where the program is going | `review/program-sequence.md`, `docs/next-steps-overview.md` |
| every document, indexed | `review/DOCMAP.md` |
| the standing rules for working here | `CLAUDE.md` |

**Start with `docs/tutorials/01-host.md`** — build the VM, run the suite, write ten lines
of nsl, compile one by hand, and watch the compiler refuse a non-local return. Then
`docs/tutorials/03-porta.md`, which runs both halves of this repository against each other
on loopback: a gateway, a real node, and the tree diff that is the whole convergence
model. The command blocks on both pages are executed by the suite (`tutorial-host` and
`tutorial-porta` on the aux line), so they cannot rot silently. `docs/tutorials/02-board.md`
is the board walkthrough — a real ESP32-C6, the two cables, the radio split, and the eleven
target gates. It is **prose, not executed**, and says so at the top: a board tutorial cannot
be gated on a host, so every transcript on it is copied from a dated bench log instead.

## Lineage

`st/berry` → `ns/berry` → `ns/oevm` → `nsl/tuvm` → **rete**. Four wholesale rewrites to
get here; nothing is built to last, it is built to teach.

## License

Apache License 2.0 — see [`LICENSE`](LICENSE). One licence for the whole tree, porta and
`porta/devsdk/` included. Code copied in from elsewhere keeps its own header: the Zephyr
board files (Apache-2.0) and the vendored ed25519 in `nodus/oevm/zephyr/src/ed25519/`
(MIT and BSD-3, listed in its `README.md`); the spike copy of pyperformance's Richards
(`docs/spikes/2026-10-10-296-image-in-sram/mpy/richards.py`, MIT, notice in the file).
