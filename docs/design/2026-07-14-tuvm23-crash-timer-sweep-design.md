# tuvm#23 — the crash sweep must cancel timers, not just reactions (design)

**Status: implemented 2026-07-14.** Fixes the P0 "crash-looping job kills the node in
≤32 iterations". Companion plan: none (single-seam fix, TDD'd directly); gate test
`nsl-tests/11_supervision/11_crash_timer_sweep.ns`.

## The verified premise — and where the issue text was wrong

tuvm#23 framed the leak as "crash between pin and release": a handler pins an M6
handle around a C-boundary operation, crashes before the matching `handle_free()`,
and the crash boundary unwinds without releasing it. **Verified against the code,
that pattern does not exist.** There are exactly two `handle_new` sites
(`grep`-confirmed):

- `src/platform.c` `PRIM_pf_gpioPin` — a **registration-lifetime** pin, correctly
  freed on re-registration of the same pin (`host_gpio_register`).
- `src/backend/zephyr/timer_zephyr.c` `timer_start` — a **registration-lifetime**
  pin, freed at `timer_backend_cancel` (or lazily reaped for fired one-shots).

No dispatch-scoped pin/release pair exists, so a dispatch-epoch auto-release
mechanism (the issue's proposed owner-field-per-slot) would be **mechanism with no
customer** — the same argument that kept option B unbuilt at rung 9.

## The real root cause

**The supervisor's crash sweep was incomplete.** `Supervisor>>restart:` swept the
job's *reactions* (`spaceDropReactionsOwnedBy:`) but not its *timers*. A crashed
instance's timer token is held only by the dead instance — nobody left can
`cancel:` it. Each restart re-runs the start thunk, which registers a fresh
`every:`:

- **target**: a new `k_timer` + a new M6 handle pin per restart; the old ones live
  forever → `handle table full` abort (or `timer table full` k_oops) at ≤32
  restarts. Let-it-crash inverted into node death — the issue's headline, real.
- **host**: the leaked repeaters keep firing (ghost re-posts into the swept
  channel) and `Timers.pending` grows without bound — silently, which is why the
  fault class survived: nothing said so.

## The fix — nsl policy, zero C changes

Same division as rung 9 ("space.c is storage, not policy"): ownership is **policy**,
so it lives in nsl, and cancellation goes through the *existing* verified path
(`Timers>>cancel:` → `seam timerCancel:` → on target `k_timer_stop` + `handle_free`).

- `Timers>>cancelAddressedBy: rootPath` — cancel every pending timer whose target
  channel is `rootPath` or nested under it. Ownership is **derived from the
  letter's address** (`p chan rootPath`), not stored per registration: under the
  capability discipline a job's timers post into its own subtree, so the address IS
  the owner — the same attribution rule the supervisor already applies to crash
  records (`SupervisedJob>>addresses:`, byte-compare idiom copied verbatim).
- `Supervisor>>sweepJob:` — the full sweep (reactions + timers), applied by both
  `restart:` (before the thunk re-runs, so the fresh instance's registrations
  survive their own job's sweep) and `quarantine:` (or a quarantined job's
  repeaters ghost-post forever and hold their pins).
- `Supervisor>>timers:` — optional wiring (`sup timers: timers` at boot). Nil keeps
  the pre-#23 reaction-only sweep: existing rigs unchanged.
- `Timers>>pendingCount` — observability for the gate test (and the cheap nsl-level
  half of the issue's telemetry ask; the C-side live-handle/high-water counters
  belong to tuvm#24's `sys/metrics` record).

## Gate

`nsl-tests/11_supervision/11_crash_timer_sweep.ns`: 5 crash+restart loops (progress
posts keep the job clear of the K=3 quarantine limit), then ONE `advanceMillis: 10`
must yield exactly **1 tick** (the live instance's repeater — 6 before the fix) and
`pendingCount` **1** (6 before); then 3 consecutive crashes quarantine and sweep to
`pendingCount` **0**. The host symptom (ghost fires) and the target symptom (handle
pins) are the same leaked registration, released by the same `cancel:` path, so the
host gate covers the target mechanism — the rung-8 seam-symmetry argument.

## Deliberately out of scope

- **Dispatch-scoped handle auto-release** (the issue's fix shape): no customer
  today. If a future prim ever pins within a dispatch, build it then — and tuvm#13's
  generation tags should land in the same slot-struct change (this fix adds no slot
  fields, so #13 stays orthogonal and open).
- **GPIO registrations**: not crash-amplified (re-register replaces + frees; leak is
  bounded by pin count, not restart count). A quarantined job's GPIO pin does stay
  registered — noted, not release-blocking.
- **Live-handle telemetry**: tuvm#24 (`sys/metrics`).
