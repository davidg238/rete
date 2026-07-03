# Pared conformance subset (Milestone A)
Host-only, non-fiber, non-NLR, non-growable. Excluded from "green":
- 04_somns_basic/45_fibers.ns          — fibers (deleted)
- 04_somns_basic/55_fiber_capture.ns   — fibers (deleted)
- 04_somns_basic/59_grow_under_fibers.ns — fibers + growable stack (deleted)
- 04_somns_basic/58_growable_stack.ns  — growable stack (deleted); depth 2000 needs realloc
- 04_somns_basic/44_nlr_multiframe.ns  — multi-frame NLR (deleted); ^ out of escaping closure
- 04_somns_basic/43_udp_mesh.ns        — networking, skip-oevm (host-deferred, spec §8)
- 04_somns_basic/44_udp_thread.ns      — networking, skip-oevm (host-deferred, spec §8)
Kept, but semantics change:
- 03_blocks/03_nlr_compile_fail.ns     — must now compile-FAIL for methods/modules too
Re-verify after the tuvm delete (VM-side reify removal may affect capture):
- 04_somns_basic/56_upward_closure.ns, 57_loop_var_capture.ns, 54_deep_recursion.ns (fixed cap)
