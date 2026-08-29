# nodus/oevm/docs — the VM's documents

Start with `vm.md`. The board ports have their own directory, `../zephyr/docs/`. Dated
design records, results and plans are program-level and live under the repo's
`docs/` — see `docs/README.md` for why.

## Current reference

- `vm.md` — **what oevm is**: register VM, run-to-completion, no fibers / no NLR, precise
  GC, `.oimg` execute-in-place. One page; every claim checked against source 2026-08-29.
- `MILESTONE_A.md` — provenance note: the fiber/NLR/growable-stack deletion and what was
  kept. **Read this before any 2026-06 document.**
- `MILESTONE_D.md` — supervisor, let-it-crash, the host job.
- `MILESTONE_E.md` — `on:do:` / `ensure:` above the crash boundary.
- `deployment-model.md` — the Model B decision record (resident VM + pushed image), with a
  2026-08-29 status note; the decisions stand.
- `op_send_implicit.md` — Newspeak implicit-receiver sends, the opcode that makes nested
  classes work.
- `optimizations.md` — interpreter performance log with measurements per commit.
- `topics.md` — the durable rationale the log links to by anchor.

## Archive — read the banner first

`archive/` holds documents that were true once and are now superseded: the genesis spec
and concept notes, the five fiber/de-recursify designs, the six phase walkthroughs, the
oevm-era roadmap and the four-repo ecosystem map. **Every file opens with a
`HISTORICAL — archived 2026-08-29` banner** naming what replaced it and what was deleted
since. Archived rather than deleted so the issues and commits that cite them by path
still resolve. The rule: `CLAUDE.md` §Docs convention.

## The code these describe

`../src/` — `vm.c` (the interpreter), `dispatch.c` (send, the method cache, the
primitives), `gc.c` (the arena and the collector), `space.c` (the tuple space),
`oimg_loader.c` (the image format), `frame_abi.h` (the frame contract). The C unit tests
are in `../tests/` and are run only by the nsl suite's `c-units` gate.
