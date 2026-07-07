# Exceptions (`on:do:` / `ensure:`) — why NOT detailing it now is safe, and the
# five constraints to pin anyway

**Status:** analysis 2026-07-03, answering "did we need to design the exception
mechanism?" Answer: **no — and structurally so, not just by preference.** Recorded here
so the reasoning survives until the §8 exception step actually starts.

## Why the deferral is safe (the structural argument)

The fear would be: "exceptions are non-local control flow — are we rebuilding the NLR
balloon later?" No. The two problems are fundamentally different in what they must
identify:

- **NLR** must find the **lexical home frame** of a closure — an identity that must
  survive the closure escaping its activation, being stored, passed across jobs,
  outliving the frame itself. That forced reification-on-escape, `home_ctx` frame
  identity, generation guards — the whole E-series cascade.
- **Exceptions** must find the nearest **dynamically enclosing handler** — a walk up
  the *live* frame chain at raise time. Nothing escapes; nothing outlives anything;
  no frame ever needs an identity that survives its own death. On tuvm's heap-side
  language stack this is: walk `prev_frame` links looking for handler-marked frames,
  run `ensure:` blocks as you pass them, reset `current_frame` at the handler. No
  reification, no cascade.

Add the first-step simplifications already in force — run-to-completion dispatch (one
stack, one known shape at every raise) and the L5 error convention (the stdlib is
exception-free by design, so nothing accumulates a dependency on `on:do:` in the
meantime) — and there is no corner being built into. The spec's "separate unwind
channel" phrase is exactly this: unwinding is a stack walk, not a value-return path.

## The five constraints to pin NOW (cheap today, expensive to retrofit)

1. **Errors are first-class values from day one.** `self error: 'text'` should produce
   (conceptually) an error value — at minimum the text, ideally a frame-chain snapshot —
   and the Milestone-D crash record on `sys/crash` (landed spelling — plain `sys/`,
   no `%` sigil) should carry *it*, not a bare string. Then when `on:do:` lands,
   handlers catch the same thing supervisors already observe; the errors.md string
   table becomes the match vocabulary. One representation, decided once.
2. **Terminate-only semantics — no resumable exceptions.** Smalltalk's `resume:`
   (handler returns a value to the raise point) is continuation-shaped machinery — a
   second balloon. Toit and Erlang live without it; so does every driver script.
   Pin: raise unwinds, always.
3. **The handler runs AFTER unwinding** (frames between raise and handler are dead
   before the handler body executes), not on top of the dying frames. This keeps the
   stack shape simple, matches terminate-only, and means the Milestone-D `setjmp`
   boundary is literally the degenerate case: a handler at the reactor frame.
4. **Milestone D's boundary should be built as "unwind to a marked frame," even though
   only one marked frame (the reactor's) exists.** Same recovery code, one marker.
   `on:do:` later = allowing user frames to carry the marker + running `ensure:` blocks
   during the walk. If D instead hard-codes "crash = reset the whole stack," the §8
   step has to rework the recovery path instead of generalizing it.
5. **Exceptions never cross a dispatch boundary.** A raise unwinds at most to the
   current firing's reactor frame; no handler in job A can catch job B's crash — that
   is the supervisor's channel, by design. Pinning this keeps crash-isolation semantics
   identical before and after `on:do:` exists.

## What was deliberately NOT designed

Class hierarchy of exceptions, selector-based filtering (`on: Error do:` vs string
match), `ensure:`'s exact interaction with `ifCurtailed:`-style variants, signal/retry
protocols: all §8-time decisions, none of them constrain the VM shape given 1–5 above.
