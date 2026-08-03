# Pre-fiber baseline commits in oevm / ns-oevm

*2026-07-04 — reference note for the milestone-a fiber teardown. Answers: "was there a
commit before fibers were introduced that we can compare a stripped tuvm against?"*

## Short answer

There is **no fully fiber-free commit** in either repo — but there **is** a clean
boundary before the R7 language-level-concurrency effort and the derecursify cascade
that followed it, which is the machinery milestone-a is tearing out.

| Repo | Last pre-R7 commit | Date | First commit of the balloon |
|------|--------------------|------|-----------------------------|
| oevm | `cee55d4` perf(string): identity fast path in String= | 2026-06-29 | `d139e84` feat(fiber): currentFiber primitive, then `b083fee` ucontext/swapcontext switching |
| ns-oevm | `a327d60` docs(spec): measured min-RAM floor | 2026-06-27 | `cab5414` (R7 roadmap), then `f07bf25` R7 fibers spec; `.ns` Concurrency/Fiber facade lands in `88fdf6e` |

## Why "no fully fiber-free commit"

oevm's very first commit (`4465e36`, 2026-06-15) is titled *"Phase 4 completed — Fiber
Scheduling Architecture"* and already contains `struct Fiber`, the
`fiberCreate:` / `fiberSwitch:value:` primitives, per-fiber oop stacks, and
`tests/test_fibers.c`. NLR arrived in the second commit (Phase 5). The repo begins at
Phase 4; earlier phases were never committed. A previous "nothing available" answer was
technically right on that narrow reading, but it missed the useful boundary below.

## What sits on each side of the boundary

**At `cee55d4` (pre-R7):**
- Fibers are simple heap objects: `struct Fiber { oop *stack; capacity;
  current_context; stack_ptr; status }` — no ucontext, no per-fiber C stacks,
  no frame ABI. `git grep -c ucontext cee55d4 -- src/` → zero hits.
- Scheduling is the old "run_interpreter hijack" (re-enter the interpreter per fiber).
- The interpreter is still **recursive** (C-stack recursion per send).
- NLR is the original Phase-5 bytecode-driven unwinding.
- Fiber footprint is small: ~38 mentions in `vm.c`, no `frame_abi.h`, no
  `oimg_loader` concurrency grant.

**Everything after `d139e84`/`b083fee` (the balloon milestone-a removes or inherits):**
- R7: ucontext/swapcontext switching, per-fiber C stacks, GC scanning of suspended
  fibers' saved register files, format_version-7 concurrency capability grant.
- Derecursify (forced by R7's problems): flat non-recursive interpreter, universal
  single-stack fiber switch, temp-vectors for captured locals, self-migration
  (`OP_LD_SELF`), growable per-fiber stacks + hard-cap policy, reify-on-escape (E1).

## Caveats when comparing a stripped tuvm against `cee55d4`

1. **`cee55d4` is pre-balloon, not the milestone-a target.** It still has the simple
   Phase-4 fibers and Phase-5 NLR; nothing in history matches WHY.md's
   "no fibers, no NLR, no growable stacks" end-state.
2. **The interpreter architecture differs.** tuvm was seeded from oevm tip
   (`aea0af1`), which includes the flat non-recursive interpreter — and milestone-a
   keeps it (it revived the de-recursify-era C-unit tests). `cee55d4` predates the
   flip, so a raw diff mixes the fiber removal with the interpreter rewrite.

So treat `cee55d4` as a **sanity reference for scale and shape** — "how small was the
VM before the balloon" — not as a diff base to revert to.

## Suggested comparison method

```sh
# side-by-side worktree; does not disturb oevm's checkout or any agent
git -C ~/workspace/oevm worktree add /tmp/oevm-pre-r7 cee55d4

# scale check: balloon size in oevm terms
git -C ~/workspace/oevm diff --stat cee55d4 aea0af1 -- src/

# shape check: fiber surface then vs stripped tuvm now
git -C ~/workspace/oevm grep -c -i fiber cee55d4 -- src/
grep -rci fiber ~/workspace/rete/nodus/tuvm/src/ | grep -v ':0'
```

Useful questions to ask of the stripped tuvm against this baseline:
- Does anything R7-era survive that `cee55d4` never needed (frame-ABI remnants,
  concurrency grants in the loader, GC hooks for suspended stacks)?
- Is the stripped VM's fiber-adjacent surface *smaller* than `cee55d4`'s ~38
  `vm.c` mentions? It should be near zero — `cee55d4` still carried real fibers.
