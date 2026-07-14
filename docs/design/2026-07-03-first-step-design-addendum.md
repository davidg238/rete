# nsl-tuvm first-step design — addendum 1 (review amendments)

**Status:** three amendments accepted by David 2026-07-03 following external design review.
Landed in the specs tree 2026-07-04 once Milestone A merged (it was parked in
`tuvm/review/` during the Milestone-A run because an agent sweeping untracked files there
deleted an earlier copy; that copy is now a `SUPERSEDED` tombstone). Each amendment states
which section of the main spec
(`nsl-tuvm/docs/design/2026-07-03-nsl-tuvm-first-step-design.md`) it extends.
Nothing here changed Milestone A scope; A1 and A2 land in Milestones C and A/§7
respectively, A3 is a non-goal clarification.

---

## A1 — Channel-prefix confinement must be path-segment aligned (extends §4 Milestone C, §5)

Prefix-capability matching is a security boundary, not string matching. Requirements:

- A capability rooted at prefix `P` addresses channel `C` iff `C == P` or `C` starts with
  `P` **followed by a path separator**. A capability rooted at `sensor` must **not**
  address `sensors/temp`; one rooted at `a/b` addresses `a/b/c` but not `a/bc`.
- Channel paths are **canonicalized at grant time**: no empty segments (`a//b`), no
  relative segments (`.` / `..`), no trailing separator. A grant request containing any
  of these is rejected, not normalized silently.
- Attenuation is **monotonic**: a capability derived by extending a prefix can never
  address a channel its parent could not.

The Milestone C confinement proof (§4C "a job holding a rooted-prefix capability cannot
address outside its prefix") **must include** the segment-collision cases above
(`sensor` vs `sensors/…`, `a/b` vs `a/bc`) and the canonicalization rejections, not just
the obvious disjoint-prefix case. This is the one first-step surface where a small
matching bug is a capability escape rather than an ordinary defect.

## A2 — Footprint banking must include a target-config RAM budget line (extends §4 Milestone A, §7)

The host baseline (`tuvm/tools/footprint-baseline.txt`, text = 39,148 bytes) carries
host-sized static arenas (multi-MB gc bss, ~192 KB dispatch bss), so **text is currently
the only meaningful column**. But the number that decides nRF52840 feasibility is RAM:
Zephyr kernel + OpenThread typically claim ~60–100 KB of the 256 KB before nsl-tuvm
arrives, leaving roughly 100–150 KB for engine statics + heap (image, tuple space,
message values, language stack).

Requirement: from Milestone A onward, `footprint.sh` (or a sibling report) additionally
records **data + bss under an MCU-shaped configuration** (heap arena, dispatch tables,
and stack cap set to intended-target sizes, even while building for host). The banked
delta in §7 covers both flash (text) and RAM (data+bss+configured heap). A RAM regression
is a gate failure the same as a text regression.

## A3 — Future async ergonomics are compiler/library work, never VM suspension (extends §8 Non-goals)

Dropping fibers means jobs are run-to-completion handlers; multi-step device protocols
("start conversion, wait 10 ms, read result") become explicit state machines or chained
`whenever:` handlers. Ergonomic pressure to reintroduce suspension **will** arrive with
the first real driver scripts. Decision recorded now so it is not relitigated under
pressure:

- The VM stays a run-to-completion engine. No fibers, no coroutines, no VM-level
  suspension — that is the balloon Milestone A deletes, and the `setjmp` crash boundary,
  the watchdog safe-point model, and the deep-sleep story (reactor state rehydrates from
  explicit objects/channels; a suspended stack cannot survive power-down) all depend on it.
- If sequential-looking async is later justified, the sanctioned mechanisms are, in order
  of preference: (1) a small **state-machine / protocol-object stdlib idiom**, (2) a
  **compiler-level CPS/async-await rewrite** into chained handlers. Cost lands in the
  compiler and library; the VM shape is untouchable.

This section is a §8 non-goal clarification only; no first-step milestone changes.
