# nodus/nsl/docs — the dialect's documents

`nsl` is the language: the Newspeak/SOMns-derived dialect, its compiler and the kernel the
compiler links into every image. The VM that runs the result is `../../oevm/` — start at
`../../oevm/docs/vm.md`.

## Current reference

- `NEWSPEAK_DIALECT.md` — **what the dialect is and is not.** The place to settle a
  question about syntax or semantics before assuming Smalltalk's answer.
- [`nsl-design-paper.md`](nsl-design-paper.md) — **why the dialect is shaped this way** (DRAFT 2026-10-06): the
  measured constraints, the dialect against Newspeak and Smalltalk, what drives each divergence, and the case for an
  object language written by agents.
- `programming_with_nsl.md` — the task-oriented guide: how to write a program here.
- `compiler.md` — **the map to the compiler code**: the pipeline, the two backends
  (solidified C and `.oimg`), the bytecode encoding, the calling-convention invariants,
  initializers, class-side methods, block inlining, the image map, and the tooling. Rewritten
  2026-08-29 (#279); every symbol and path on it was checked against source that day.

The wire contract both halves answer to is `../../../protocol/`, not here. The gates are
`../tools/run_tests_oevm.py` (run from `nodus/nsl`) and `../tools/run-one.py` for a single
test — and the suite's aux line matters as much as its pass count.

## Dated records

`design/` (39 files, 2026-06 to 2026-07) and `superpowers/` (51 files: `plans/` and one
`findings/`) hold this subsystem's decision records and execution scaffolding from the
milestone-A-through-E era, under the names the issues cite.

**They are here rather than at the program level, which the repo's docs convention did not
intend** — `../../oevm/docs/README.md` states the rule these predate (dated records live in
the root `docs/`; reference docs live with their subsystem). Moving them is a citation
rewrite across issues and commits, so it is a decision, not a tidy-up: it is #284. Until
then, read anything in those two directories as **historical**, and check its date against
`../../oevm/docs/MILESTONE_A.md` — the fiber/NLR/growable-stack deletion of 2026-07-04
invalidated a good deal of what the 2026-06 documents assume.
