# nsl-tuvm — project instructions

## Docs convention (2026-07-14)

- **Design specs are decision records** and live in `docs/design/` (moved from
  `docs/superpowers/specs/` on 2026-07-14). When a superpowers skill (e.g. brainstorming)
  says to save a design to `docs/superpowers/specs/`, save it to `docs/design/` instead —
  this instruction overrides the skill's default path.
- **Implementation plans** stay in `docs/superpowers/plans/` — they are execution
  scaffolding, historical once the work closes.
- **Promotion rule — part of every close-out.** An issue / rung / milestone is not closed
  until its durable knowledge is promoted to a mainline document of record: the ladder
  (`~/workspace/tuvm/review/program-sequence.md`), `tuvm/review/tests/CLAIMS.md`, or the
  owning doc under `tuvm/review/lib/`. The dated design doc stays behind as the
  historical "why". Every plan's close-out task must name which mainline doc absorbed
  the knowledge.
- **Cite absolute paths** when referencing documents in conversation
  (e.g. `~/workspace/tuvm/review/program-sequence.md`, never a bare `review/`) — the
  program spans five repos across two workspace roots.
- Cross-repo document map: `~/workspace/tuvm/review/DOCMAP.md`
  (regenerate with `~/workspace/tuvm/tools/docmap.sh`).
