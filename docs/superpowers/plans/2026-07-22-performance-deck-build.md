# Performance deck (#70) — build plan

**Date:** 2026-07-22
**Design (binding):** `~/workspace/tuvm/docs/design/2026-07-21-performance-deck-design.md`
**Artifact:** `~/workspace/tuvm/review/performance_slides.html`
**Issue:** tuvm#70

The design is agreed and is not re-opened here. This plan records only what the build
decides that the design left open, and the drift the design could not have known about.

## Tasks

1. **Slide pool + four playlists + per-element `data-aud`**, one self-contained file,
   inheriting `review/slides.html`'s CSS variables and slide styling.
2. **`DATA` block with provenance.** Every figure once, each carrying a `src`. Scalars in
   `DATA`; the three tables (device spans, STIC, Hanoi, surface) are *rendered from data*,
   not hand-authored as HTML — otherwise the audit in task 4 is theatre and #69 has
   nothing to check.
3. **Status badges as first-class values.** `tbd` (unmeasured), `prov` (measured on a
   build since invalidated), `recalled` (from notes, never recorded), `derived` (a
   subtraction), `indicative` (not yet a benchmark). A figure cannot render without one of
   these or a plain measured value.
4. **Witness appendix**, generated from `DATA` at load: every figure, its value, its
   status, its witness. This is the surface tuvm#69's checker will read.
5. **Close-out** — issue comment, and the promotion below.

## Decisions this build makes (design left them open)

- **§8 open question — normalised STIC column: NO.** Raw only. The deck's ethos is that a
  raw number plus its constraint beats a processed one; a normalised column is a
  rhetorical act, and adding it "just as an option" is how it ends up on the investor
  slide. Recorded as decided, not deferred.
- **Act I carries a `provisional` badge on every HTTP-path figure.** The 2026-07-22
  addendum to the job-latency rung invalidates `egress`, `render` and the client-side
  p50/p95 (measured on a build whose RX packet slab was the bottleneck — #65). The thermal
  half is unaffected. The design could not know this; the deck must not ship the figures
  bare. The paced-run asterisk stays *as well* — it is a separate defect.
- **A second Act I witness is added** (`thermal-witness`): the 2026-07-22 profiler-slice
  capture of the same job, from the porta-lite store rather than transcribed. Two
  independent measurements of the same shape is a stronger Act I than one, and it is the
  #69 story working.
- **Surface figures re-measured today** rather than transcribed from the design.
  `kernel.nsl` has grown 123,308 → 131,423 chars since 2026-07-21; Toit's counts are
  unchanged. The drift in one day is itself the argument for task 4.
- **Act IV's `legibility` slide is rewritten.** The design has #65 as "a silent failure we
  cannot yet explain". It was root-caused and fixed on 2026-07-22. The counterexample gets
  *stronger*, not weaker: the failure was invisible for a day, which is precisely what the
  never-silent design law exists to prevent — so the slide reports the fix and keeps the
  indictment.

## Close-out — which mainline doc absorbs this

`~/workspace/tuvm/review/program-sequence.md` gains the deck rung entry, naming the
artifact, the four playlists, and the badge vocabulary as the mechanism #69 checks against.
`CLAIMS.md` gains no row (the deck asserts nothing new; it cites). The design doc stays as
the historical "why".
