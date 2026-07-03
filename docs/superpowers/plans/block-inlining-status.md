# Block Inlining — Track Status

Single source of truth for the block-inlining work. See the design rationale in
[`../specs/2026-06-16-block-inlining-design.md`](../specs/2026-06-16-block-inlining-design.md)
and the per-phase plans alongside this file.

**Last updated:** 2026-06-19
**Conformance:** `47 / 0 / 1` (passed / failed / skipped) on both execution
paths (`OEVM_PATH=AB`: Path A solidified-C image, Path B `.oimg` image-VM).
The lone skip is `scratch_class.ns` (no expectations).

## Phase status

| Phase | Scope | State |
|---|---|---|
| **A** — Foundation | `Scope` class; route resolution through it; label/backpatch helpers; VM `OP_JMP_IF_FALSE` truthiness fix. No inlining (byte-identical no-op). | ✅ Landed |
| **B** — if-family | `ifTrue:` / `ifFalse:` / `ifTrue:ifFalse:` / `ifFalse:ifTrue:` with literal blocks. | ✅ Landed |
| **C** — `and:` / `or:` | Short-circuit inlining with a literal zero-arg block. | ✅ Landed |
| **D** — loops | `whileTrue:` / `whileFalse:` / `to:do:` / `to:by:do:` (literal blocks; literal step for `to:by:do:`). Added `11_loop_inlining.ns`. | ✅ Landed |
| **E** — refactor | Extract the shared cond/branch-emit skeleton duplicated across Phases B–D. Pure no-op cleanup. | ✅ Landed (`3cf2516`) |

**All phases landed — this track is complete.**

## Phase E (landed)

A refactor, not a feature. Spec is in
[`2026-06-16-block-inlining-phase-d.md`](2026-06-16-block-inlining-phase-d.md),
**Task 6** — there is no separate Phase E plan doc. Implemented in commit
`3cf2516` ("compiler: extract `_emit_cond_jmp_false` helper").

- `_emit_cond_jmp_false(cond_expr, cond_reg, label)` in
  `tools/ns_compile_oevm.py` now single-sources the
  "compile condition + `emit_jump(OP_JMP_IF_FALSE, …)`" skeleton.
- The two call sites whose body is exactly `compile_expr` then `emit_jump` —
  `compile_if_inline` and `compile_logic_inline` — use the helper.
- The other two `JMP_IF_FALSE` sites are correctly **not** folded:
  `compile_to_do_inline`'s condition is a synthesized comparison `OP_SEND`, and
  `compile_loop_inline`'s condition is an inlined block, not a plain
  `compile_expr`. No foldable duplication remains.

The goal Phase E was originally scoped around — closing
`04_somns_basic/02_block_inlining.ns` — was independently met: that file passes
via the landed `Array>>do:` library method, not deeper inlining.

## Related future work

- **`oevm#1`** — replace the reflective `home`/`regAt:` outer-variable access
  for *non-inlined* blocks with a direct outer-frame pointer + indexed access,
  on top of the scope/frame model. Tracked in the sibling `oevm` repo; out of
  scope for this track.
