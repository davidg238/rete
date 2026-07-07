# CLAIMS — first-step spec §7 success criteria → proving tests

A criterion without a green named test is unproven. Update the Status column as suites
land; add rows if the spec grows. (Gate scripts live in `t0-gates/`; behavioral tests
are relative to this directory.)

| # | Spec §7 criterion | Proven by | Status |
|---|---|---|---|
| 1 | Seeded repos exist w/ recorded provenance | seed commits (tuvm fa301d3, nsl-tuvm abd9915, nsl-tests 8ad658b) | DONE |
| 2 | Balloon deleted; pared conformance green | pared manifest run + `nsl-tests/07_no_nlr/` + `tuvm/tools/diff_oracle.sh` | DONE |
| 3 | Footprint delta measured & recorded | `tuvm/tools/footprint.sh` + banked `tuvm/tools/footprint-baseline.txt` / `footprint-after.txt` (text + RAM per addendum A2) | DONE |
| 4a | Algebra rejects mutation | `nsl-tests/08_messages/12_mutation_selectors.txt` + `12_mutation_matrix/01..06` (one DNU-crash test per type × mutating-selector row, superseding staged `02_mutation_rejection.ns`) | DONE (`02b_reflective_mutation.ns` DEFERRED — `perform:`/`perform:withArguments:` don't exist in the landed language, dispatch slot 11 reserved; lands when mirrors/perform land) |
| 4b | Algebra rejects cycles / depth | `nsl-tests/08_messages/10_depth_bound.ns` (+ `03b_list_depth_guard.ns` / `03c_map_depth_guard.ns`) and `15_cycle_impossible.ns` | DONE |
| 4c | Immutable values shared by reference | `nsl-tests/08_messages/13_aliasing.ns` | DONE |
| 5 | Channels enforce prefix confinement | `nsl-tests/09_channels/02_confinement.ns` (segment-aligned collision proof) + the canonicalization-guard family `01_canonicalization.ns`/`01b_leading_slash.ns`/`01c_empty_segment.ns`/`01d_dot_segment.ns`/`01e_dotdot_segment.ns` (reject-at-grant, never normalize) + `03_attenuation.ns` (monotonic narrowing) + `04_no_ambient.ns`/`04b_no_ambient_no_parent.ns` (no ambient authority, no widening selector); enforced by `nsl-tuvm/lib/kernel.ns` `class Channel` (`resolve:`, ~L598-622; `checkPath:` is a Kernel-module-level helper at ~L798-813, reached via `outer`) atop the C substrate `tuvm/src/space.c` (exact-string channel registry, Milestone C Task 1) | DONE |
| 6a | event → reaction → supervisor loop runs | `td-crash/06_event_seam.ns` | pending D |
| 6b | Crashing handler contained; others unaffected | `td-crash/01_containment.ns`, `02_crash_mid_allocation.ns`, `03_crash_at_depth.ns` | pending D |
| — | Reactor semantics pinned (run-to-completion, FIFO, take-once) | `nsl-tests/09_channels/05_take_semantics.ns`, `06_run_to_completion.ns`, `07_registration_lifecycle.ns` (+ landed-only additions `08_fanout_order.ns` fan-out order/no-dedupe, `09_snapshot_at_dequeue.ns` snapshot-at-dequeue, `10_take_vs_dispatch.ns` take-consumes-before-dispatch); enforced by `nsl-tuvm/lib/kernel.ns` `Space>>drain` (~L642-649) atop `tuvm/src/space.c`'s FIFO event queue + owner-tagged per-channel reactions (Milestone C Task 1) | DONE |
| — | No memory-safety regressions | T0.1 sanitizer configs over the whole suite | pending harness |
| — | No leak creep across crash/restart | `td-crash/05_soak_leak.ns` (slow; milestone gates only) | pending D |
| — | Combinator early-exit semantics (no-NLR story) | `nsl-tests/07_no_nlr/03_combinator_short_circuit.ns` | DONE |
| — | Deleted-test functionality re-expressed (T-A.5) | audit column in the pared-subset manifest | pending A |
