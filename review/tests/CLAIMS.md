# CLAIMS — first-step spec §7 success criteria → proving tests

A criterion without a green named test is unproven. Update the Status column as suites
land; add rows if the spec grows. (Gate scripts live in `t0-gates/`; behavioral tests
are relative to this directory.)

| # | Spec §7 criterion | Proven by | Status |
|---|---|---|---|
| 1 | Seeded repos exist w/ recorded provenance | seed commits (tuvm fa301d3, nsl-tuvm abd9915, nsl-tests 8ad658b) | DONE |
| 2 | Balloon deleted; pared conformance green | pared manifest run + `ta-deletion/01…04` + `t0-gates/diff_oracle.sh` | pending A |
| 3 | Footprint delta measured & recorded | `t0-gates/ratchet.sh` (text + RAM per addendum A2) | pending A |
| 4a | Algebra rejects mutation | `tb-messages/02_mutation_rejection.ns`, `02b_reflective_mutation.ns` | pending B |
| 4b | Algebra rejects cycles / depth | `tb-messages/01_depth_bound.ns`, `04_cycle_impossible.ns` | pending B |
| 4c | Immutable values shared by reference | `tb-messages/03_aliasing.ns` | pending B |
| 5 | Channels enforce prefix confinement | `tc-channels/01_segment_confinement.ns`, `02_canonicalization.ns`, `03_attenuation.ns`, `04_no_ambient.ns` | pending C |
| 6a | event → reaction → supervisor loop runs | `td-crash/06_event_seam.ns` | pending D |
| 6b | Crashing handler contained; others unaffected | `td-crash/01_containment.ns`, `02_crash_mid_allocation.ns`, `03_crash_at_depth.ns` | pending D |
| — | Reactor semantics pinned (run-to-completion, FIFO, take-once) | `tc-channels/05_take_semantics.ns`, `06_run_to_completion.ns`, `07_registration_lifecycle.ns` | pending C |
| — | No memory-safety regressions | T0.1 sanitizer configs over the whole suite | pending harness |
| — | No leak creep across crash/restart | `td-crash/05_soak_leak.ns` (slow; milestone gates only) | pending D |
| — | Combinator early-exit semantics (no-NLR story) | `ta-deletion/03_combinator_short_circuit.ns` | pending A |
| — | Deleted-test functionality re-expressed (T-A.5) | audit column in the pared-subset manifest | pending A |
