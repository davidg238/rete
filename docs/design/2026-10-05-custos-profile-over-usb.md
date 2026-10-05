# `custos profile` over USB — what #312 needs decided first

**Status:** Q1 DECIDED, Q2 waits on a measurement, Q3 open (David, 2026-10-05) · **Date:** 2026-10-05 · **Issue:** #312
(first slice step 4 of `2026-10-04-custos-peer-design.md` §6a) · Drafted overnight after
#338 and #339, instead of guessing at the two choices below.

## What step 4 says

> **`custos profile` over USB**: the trigger sent down the cable, the rows ranked by Go;
> `mprof_resolve.py` deleted.

Each half runs into a fact the design did not record.

## Findings

**P1. No line on the cable opens a profile window, except in a debug image.** Today a
window is opened by a `dbg/req` write through porta (`profile <ms>`, parsed by
`Kernel>>parseProfileMs:`, acted on by the app's bridge;
`2026-08-06-method-profile-over-porta-design.md` D2). On a normal image the console is
read only by an app that calls `readLine` itself. The one command loop that reads the
cable is the Supervisor's `dbg:*` loop, and it exists only in a debug image
(`OEVM_DEBUG` on a desktop, `OEVM_DEBUG_HOOKS=1` on a board).

**P2. Profiling is a second build flag.** The counter's table is compiled in only with
`OEVM_METHOD_PROFILE=1` (`nodus/oevm/zephyr/CMakeLists.txt:341`), as the debugger's
checks are only with `OEVM_DEBUG_HOOKS=1`. So a person who wants to debug and then
profile the same program flashes two different special images today.

**P3. `mprof_resolve.py` has a second reader that the design missed.**
`nodus/oevm/zephyr/tools/console/view.py`, the profile page that the console collector
serves (#165 D8), imports it (`HDR_RE`, `DUMP_RE`, `resolve`). If #312 deletes the
resolver, that page breaks. That page is the one #340 replaces with custos's profile page.

## Questions

1. **The trigger.** *Recommended:* add a `dbg:profile <ms>` verb to the Supervisor's one
   command loop, reusing `parseProfileMs:` for the argument. Like every verb, it stops
   the program when it arrives (the pause rule), and the window then opens on
   `continue`. That way a profile is one more line in the session #339 already holds, and
   no second console reader exists. The alternative is a console reader in profile
   images, outside the debug loop: that is a second mechanism doing the same job.
2. **One development image, not two.** *Recommended:* one flag (say `OEVM_DEV=1`) builds
   in both the debugger's checks and the profile counter. Their costs are already
   measured (11.6 % per bytecode and +494 B for the checks; 0.90 % for the counter inside
   a window), and both exist only for the development loop. A cost measured on the
   combined image is the number the page should state anyway. The cost of the choice is
   that a profile taken on the development image includes the debugger's 11.6 %. That is
   proportional across methods, so it does not change the ranking, but it does change the
   absolute time.
3. **When `view.py` goes.** *Recommended:* #312 takes the profile page from #340, so that
   `custos profile` (the command), custos's profile page and the deletion of both
   `mprof_resolve.py` and `view.py`'s profile view land in one change, and #340 keeps the
   debug page. The alternatives are worse: keeping `mprof_resolve.py` until #340 means two
   resolvers in the repo, and making `view.py` call custos means Python shells out to Go
   for a page that is about to be deleted.

## Answers (David, 2026-10-05)

Context: a two-developer sketch. Each dev machine runs custos with a board on USB (A, B);
one porta on the gateway reaches their field copies (A′, B′). A and A′ drift apart while a
bug is chased on both, alternately, because the cable is faster.

1. **Not as recommended.** One command reader on the node, in **every** image, fed by
   the cable line or by `dbg/req` through porta. The `dbg:*` verbs and `profile <ms>`
   share its grammar (`parseProfileMs:`), so the same line reaches A and A′, and the
   debug verbs gain the porta path D9 needs. Profiling must not require the debug image.
   Open for the plan: a profile verb in a standard image must not pause the program the
   way a `dbg:*` verb does in a debug image.
2. **Waits on a measurement.** Either two images, std (with the counter) and debug, or
   std and debug+profile. Decided by the counter's cost on a real workload: the 0.90 %
   was Hanoi at load 0.008 against a real 0.875, and its idle cost is unmeasured
   (`2026-08-06-method-profile-over-porta-design.md`). Measured on `c6spare`.
3. **Open.**

Also decided: porta is the long-term custodian of commissioned images; developer
images are disposable (D10 in `2026-10-04-custos-peer-design.md`).

**Also owed, whatever the answers:** #312's cost measurement needs a C6 build and
`c6spare`. This desktop has no `cmake` and cannot build Zephyr, so that part waits for
the bench session.
