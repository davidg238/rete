# docs/ — the VM's own documents

Start with `vm.md`. Then the three milestone notes. Everything dated is a record of a
decision or a measurement and stays as written; the living documents are named as such.

## Living (edit, don't append)

- `next-steps-overview.md` — what gates a public release and what is on the bench.
  Re-read at the start of a session and edit it.
- `status/` — the orientation deck (`dev-status.html`, live at gw85224-01:6971) and its
  generator. What is top of mind right now, by issue band.

## Current reference

- `vm.md` — **what oevm is**: register VM, run-to-completion, no fibers / no NLR, precise
  GC, `.oimg` execute-in-place. One page.
- `deployment-model.md` — the Model B decision record (resident VM + pushed image), with
  a 2026-08-29 status note; the decisions stand.
- `op_send_implicit.md` — Newspeak implicit-receiver sends, the opcode that makes nested
  classes work.
- `MILESTONE_A.md` — provenance note: the fiber/NLR/growable-stack deletion and what was
  kept. **Read this before any 2026-06 document.**
- `MILESTONE_D.md` — supervisor, let-it-crash, the host job.
- `MILESTONE_E.md` — `on:do:` / `ensure:` above the crash boundary.
- `optimizations.md` — interpreter performance log with measurements per commit.
- `topics.md` — the durable rationale the log links to by anchor.

## Records

- `design/` — **decision records**, dated. The "why" behind every rung; superseded ones
  say so at the top. A design is closed only when its knowledge has been promoted to a
  mainline document (`review/program-sequence.md`, `review/tests/CLAIMS.md`,
  `protocol/`, `porta/docs/ARCHITECTURE.md`) — see `CLAUDE.md` §Docs convention.
- `results/` — dated board and host measurements, one file per experiment.
- `spikes/` — throwaway code and images that earned a number; kept for the number.
- `superpowers/plans/` — execution scaffolding for the designs; historical once the work
  closes, never a document of record.

## Archive — read the banner first

`archive/` holds documents that were true once and are now superseded: the genesis spec
and concept notes, the five fiber/de-recursify designs, the phase walkthroughs, the
oevm-era roadmap and the four-repo ecosystem map. **Every file there opens with a
`HISTORICAL` banner naming what replaced it and what was deleted since.** They are
archived rather than deleted so that the `#NN` issues and commits that cite them by path
still resolve — and the banner is what makes that safe.

The rule (CLAUDE.md §Docs convention): a document leaves the mainline by **move + banner**,
never by deletion and never by a bare move.

## Elsewhere

- `../README.md` — start here for the whole repo.
- `../review/DOCMAP.md` — the generated inventory of every document across the program,
  plus the curated documents-of-record list.
- `../protocol/` — the wire contract. `../porta/docs/` — the gateway's documents.
- `../nodus/oevm/zephyr/docs/` — board ports and Zephyr lessons.
