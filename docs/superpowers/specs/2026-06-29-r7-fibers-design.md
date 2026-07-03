# R7 — Fibers: language-level cooperative concurrency (design)

**Date:** 2026-06-29
**Status:** Design approved (brainstorm). Next: implementation plan (writing-plans).
**Roadmap:** R7, "the concurrency pillar" — concurrency for event-driven orchestration
(`docs/superpowers/plans/2026-06-25-ns-zephyr-capability-node-roadmap.md`).

## Framing

The oevm VM already provides the **mechanism** for fibers — verified in the codebase:
`struct Fiber` (its own malloc'd stack + a saved `MethodContext` chain), the universal
primitives `fiberCreate:` and `fiberSwitch:value:` (symmetric coroutine switch), GC tracing
of suspended fibers, and `free()` of the fiber stack on collection. What is missing is the
**language story**: there is no `Fiber`/`Scheduler` class, no `yield`, the lifecycle
(`Dead`/`status`) is never driven, `test_fibers.c` is orphaned (not in the harness), and
concurrency is not a capability.

R7 is therefore primarily a **library/API + scheduler + semantics** task on solid VM
foundations — exactly matching the codebase grain: **mechanism in C, policy/facade in `.ns`**
(as with the `Serial` and `Net` capabilities over their prims).

## Decisions (from brainstorm)

1. **Concurrency model:** asymmetric **scheduler + `yield`** (cooperative). Fibers yield to a
   central scheduler that picks the next runnable. This is what "event-driven orchestration"
   wants and is the prerequisite for fiber-aware blocking I/O.
2. **Capability:** the high-level spawn/scheduler privilege is a **gated capability**
   (handed at boot via an explicit header param, like `platform:`/`serial:`/`net:`). The
   low-level prims (`fiberCreate:`/`fiberSwitch:value:`) stay **universal** (the kernel needs
   them for NLR / `ensure:` / `on:do:`).
3. **Fiber-aware blocking I/O:** **designed now, built later.** The MVP is the cooperative
   core (spawn/yield/run + lifecycle), CPU-only, desktop-first, tested. The park/wake seam is
   built into the scheduler so the I/O tier is a small additive delta.
4. **Channels:** **deferred** — they reuse the same park/wake machinery as the I/O tier.
5. **Approach:** **`.ns` scheduler over the existing prims** (policy in `.ns`), plus **one**
   trivial VM addition (`currentFiber`). No C scheduler.

## Architecture

A pure-`.ns` **`Concurrency`** capability (no C backend — it only drives the universal fiber
prims). Two nested classes:

- **`Concurrency`** — the granted facade *and* the scheduler. Holds `schedulerFiber`
  (the fiber executing `run`, captured via `currentFiber`), `readyQueue` (an
  `OrderedCollection` of `Fiber`), and `current` (the running `Fiber`).
- **`Fiber`** — a handle wrapping the vm-fiber oop plus lifecycle state (`isDone`).

Collaborating classes nest (per the dialect rules: top-level siblings are not bare-name
reachable). Prims are reached via the kernel instance.

### The one VM addition: `currentFiber`

There is a C global `current_fiber` (`dispatch.c:27`) but **no `.ns`-reachable accessor**
(the selector table at `dispatch.c:547` has `fiberCreate:`, `fiberSwitch:value:`,
`registerDebuggerHook:`, `outer`, `resumeWith:` — nothing for the current fiber). Add a
`currentFiber` primitive that returns the global `current_fiber` oop:

- new `PRIM_currentFiber` id + selector-table entry + a `dispatch.c` case (~3 lines);
- the prim itself is selector-table-only and needs no image change — both Path A and Path B
  reach it by sending `currentFiber` to the kernel.

Needed because: a worker must name the scheduler fiber to yield to, and `run` must capture
its own fiber so workers can switch back into it.

> **Correction (vs an earlier draft of this spec):** the *grant* of the `concurrency`
> capability **does** require an image change on Path B. The `.oimg` loader mints and binds
> capabilities at boot from header sel-refs; a new gated capability therefore needs a
> `concurrency_on_sel_ref` header word and a **`format_version` bump to 7** (Net maxed at 6),
> plus the mirror read in `oevm/src/oimg_loader.c`. This is the same machinery Net (#17) used
> at v6. Path A grants in compiler-emitted C and needs no format change. The `currentFiber`
> prim is unaffected — only the capability grant drives the version bump.

## `.ns` API surface

```
concurrency spawn: [ ... ]   "make a runnable Fiber from a 0-arg block; enqueue; return the handle"
concurrency yield            "running fiber yields to the scheduler; resumes here later"
concurrency run              "run the scheduler loop until every fiber is Dead; return when quiescent"
aFiber isDone                "lifecycle query"
```

A spawned block captures `concurrency`, so `concurrency yield` is callable anywhere within
its dynamic extent.

## Scheduler internals + lifecycle

- **`spawn:`** wraps the user block so a fiber always exits via an explicit switch (sidesteps
  undefined "fiber top-closure returns" behavior):
  ```
  spawn: aBlock = (
    | entry f |
    entry:: [ aBlock value. self _complete ].
    f:: Fiber forVmFiber: (kernel fiberCreate: entry).
    readyQueue addLast: f.
    ^ f
  )
  ```
- **`run`** (executes in the caller's fiber = the scheduler fiber):
  ```
  run = (
    | f signal |
    schedulerFiber:: kernel currentFiber.
    [ readyQueue isEmpty ] whileFalse: [
      f:: readyQueue removeFirst.
      current:: f.
      signal:: kernel fiberSwitch: f vmFiber value: nil.   "returns when f yields/completes"
      signal = #yield ifTrue: [ readyQueue addLast: f ].
      signal = #done  ifTrue: [ f markDead ].
    ].
  )
  ```
- **`yield`** = `kernel fiberSwitch: schedulerFiber value: #yield`.
- **`_complete`** = `kernel fiberSwitch: schedulerFiber value: #done` (never returns).

The `#yield`/`#done` value the scheduler receives from its own `fiberSwitch:value:` return
tells it whether to re-enqueue or retire — this is what finally drives the
`Runnable → Suspended → Dead` lifecycle the VM left unused. (`fiberSwitch:value:` already
flips the leaving fiber to `Suspended` and the entered fiber to `Runnable`; `markDead`
records terminal state on the `Fiber` handle. Whether to also set the VM `DeadFiberVtable` /
`status` field is an implementation detail — the `.ns` handle is the source of truth for the
scheduler.)

## Capability-gating & confinement

- **Grant** follows the Serial/Net **param-driven** pattern: the entry class declares a
  `concurrency:` header param; the compiler binds a `Concurrency` instance only when
  declared. Undeclared ⇒ `concurrency` raises a runtime DNU.
- Because `Concurrency` is pure-`.ns`, its grant needs **no C backend/wrapper** (no
  `concurrency_object()` extern, unlike `serial_object()`/`udp_object()`) — the instance is
  minted by sending a no-arg `concurrency` accessor to the kernel. But the *grant binding*
  still flows through the boot machinery on both paths, so Path B needs a
  `concurrency_on_sel_ref` header word + a **`format_version` bump to 7** + the
  `oevm/src/oimg_loader.c` mirror, exactly as `Net` (#17) did at v6. Path A grants in
  compiler-emitted C with no format change.
- **Open point for the plan:** whether the granted `Concurrency` instance is minted in
  `kernel.ns` or threaded through the existing grant plumbing. No image-format impact either
  way; resolved during planning.
- New dialect row **#18** in `docs/NEWSPEAK_DIALECT.md`.

## Testing (desktop-first, both paths)

- **C unit:** integrate the existing `tests/test_fibers.c` into `run_tests_oevm.py` (it
  already exercises create/switch + GC of fibers; today it is never compiled).
- **`.ns` conformance** (Path A + Path B, under the runner):
  1. **Round-robin interleaving** — spawn 3 fibers, each yields N times appending its id to a
     shared log; `run`; assert the interleave order.
  2. **Lifecycle** — `isDone` is false while runnable, true after completion;
     `run` returns only when all fibers are Dead.
  3. **Confinement pin** — a class that does not declare `concurrency:` and sends
     `concurrency` ⇒ runtime DNU (`expect-runtime-fail`), mirroring `43_udp_confined`.
- Suite stays green both paths (currently 72/0/6).

## Designed-but-deferred (built ready, not built)

- **park/wake seam (in the MVP scheduler from day one):** `_park: aFiber` (ready→parked-set)
  and `_wake: aFiber` (parked→ready). `yield`/`run` are unchanged; the parked-set is simply
  empty in the MVP.
- **Fiber-aware blocking I/O (next tier):** a blocking `receive` calls
  `concurrency parkUntilReadable: socket`; when `readyQueue` is empty but the parked-set is
  not, `run` polls the parked sockets (non-blocking, the existing UDP `receive` returning
  `nil`) and `_wake:`s the ready ones. This is the *entire* delta for fiber-aware I/O — the
  MVP scheduler is shaped for it.
- **Channels (next tier):** a `Channel` on the same park/wake machinery (send parks if full,
  receive parks if empty).

## Out of scope (R7 MVP)

- Preemption / fairness beyond cooperative round-robin (no timeslicing — fibers yield
  voluntarily).
- Priorities, timeouts, cancellation.
- Hardware/Zephyr bring-up of fibers (desktop-first; the facade is portable `.ns`, so the
  hardware path follows for free once the I/O tier lands — no backend work, unlike `Net`).
- Multi-core / true parallelism (the VM is single-threaded; fibers are cooperative).

## Risks / things to verify during implementation

- **Fiber top-closure return:** confirm with `test_fibers.c` (line 149 asserts a fiber
  returns `123`) what the VM does if a fiber's entry closure returns. The `spawn:` wrapper
  ends in `_complete` (an explicit switch) so this should never happen in practice; verify
  the defensive case anyway.
- **`currentFiber` at boot:** `run` calls `currentFiber` to capture the scheduler fiber; the
  VM lazily mints the boot fiber on first interpretation (`vm.c:152`), so by the time `.ns`
  `run` executes, a current fiber exists. Confirm the lazy-init has already run.
- **GC of the ready/parked queues:** the `Concurrency` instance holds the `Fiber` handles
  (and thus the vm-fibers) live across switches — confirm reachability so a queued-but-not-
  running fiber is not collected.
