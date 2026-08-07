# Per-method profiling, surfaced over porta

**Status:** design COMPLETE · **Date:** 2026-08-06
**Issues:** #165 (this work), #164 (the counter), #163 / #161 (what it was built to answer),
#159 (made moot by D2), #74 (promoted to a hard dependency by D3), #28 (named as the fix for #163),
#56 (the console path this leans on)

> An earlier revision of this document stopped after D1–D3 and said so at the top. That
> warning is retired: the remaining questions were brainstormed on the same day and are
> settled below. **D1's reasoning was found wrong and is corrected in place** — see the
> note inside it.

## Why

#164 landed an exact per-method call counter and it immediately answered #163 (see
`docs/results/2026-08-06-method-call-counter-spike.md` and the #163 thread): the codec's
cost is one interpreted method activation *per byte*, and byte-level accessors are 97.9%
of all codec activations. That answer was read out of a console log by hand.

Two things follow. David can **see** it — the way the #56 console made a dead node
observable. And it works on a **field** node from a production porta, which is the
requirement that shaped every decision below: profiling that needs a reflash is not
profiling, it is a bench activity.

## Decisions

### D1 — A bounded capture window, not continuous counting

A node counts only inside a window. Idle cost is a flag check; the measured 0.90%
(C6, `docs/results/2026-08-06-method-call-counter-spike.md`) is paid only inside one.

**Correction to this decision's original reasoning.** The first draft rejected always-on
counting because it "would force an answer to 224 methods against a 512 B batch on every
check-in — the exact contention that already bit as #150." That argument is about
**publishing**, not **counting**, and the two are independent: counting costs 0.90% and
zero bytes. The conclusion survives, but on the honest ground — 0.90% is worth paying while
you are asking a question and not worth paying the rest of the time. Under D4 nothing is
published to the space at all, so the #150 contention never arises either way.

The gate already exists. `g_mprof_enabled` (`nodus/tuvm/src/vm.c:420`) is checked at the top
of `mprof_bump`; what is missing is only a trigger. Today it flips on an activation
countdown, and the comment above it says plainly that this is a "SPIKE MEASUREMENT SEAM
ONLY … not a shipping feature." This design replaces the countdown with porta. That is the
whole delta on the VM side.

Two levels of control survive, and both are already built:

- **`OEVM_METHOD_PROFILE` (build)** — compiled out entirely where the table's `.bss` does
  not fit. That is the ESP32, with 816 B of dram0 spare (#53). See D9.
- **`g_mprof_enabled` (runtime)** — built in, off, flipped for a window.

### D2 — The trigger is `dbg/req`, which makes #159 moot rather than resolving it

PROTOCOL-NSL §3 gives porta exactly two inbound paths: `goal/**` and `dbg/req`. A capture
request is a debug request, so it rides `dbg/req` and **the authority model does not
change**.

This is why that matters. RECORDS §4.6 documents `diag/spans/ctl` as porta→node while §3
forbids porta writing it — a genuine contradiction between two documents of record, filed
as #159 and deliberately left open with four options. D2 does not pick one: it removes the
need for any of them, because a capture never needs `diag/*/ctl` written at all. #159 stays
open for `diag/spans/ctl` on its own merits.

It also gives `dbg/*` its **first implementation since v1 was deleted**. RECORDS §4.5 has
been pinned-but-unbuilt since the rewrite ("the node carries no `dbg/` channel today"), and
that hole is the whole of the remote-debug capability the §4.2 audit promised a fate.

D2 is **stronger** under D4 than it was when first taken: the request line is now the *only*
thing this feature puts on the wire. A mechanism carrying one small control message is much
easier to justify than one carrying a profile.

### D3 — Close #74; the node must be able to name its own build

The node reports a method as a **blob-relative offset** (`ptr - oimg_blob_base`) because a
`CompiledMethod` carries no name — see the #164 commit. Names come from the
`<image>.oimg.methods.json` sidecar the compiler now emits. Joining the two requires
knowing **which build** is on the node, and today `obs/build` carries `#chip` and nothing
else (`PortaBridgeNodeC6.nsl:63`, `ShipsBellAlarmNodeC6.nsl:133`) — which is #74.

So #74 is now a hard dependency rather than a background want, and it pays for two things:
`docs/next-steps-overview.md` already records that the target harness's **field** path has
no anchor without it either. A `platform buildId` accessor closes both.

The failure this prevents is specific: a mismatched sidecar resolves nothing and, without
the guard, would look identical to a quiet program. `mprof_resolve.py` already fails loudly
on it; automatic build matching removes the chance to get it wrong. In the field this stops
being hygiene and becomes necessary — nodes will not all be running the same build.

**This resolves a contradiction with #164's own issue body**, which proposed that "readback
needs no new naming machinery … nsl asks `callsOf: (mirror methodOf: m)` and publishes
`diag/calls/<selector>` — C never learns a method name." That was the original intent and it
is superseded. Under D5 nsl never reads the table, so the node emits offsets and never names
anything. The `mirror` route would also have named only vtable-bound methods; **blocks
appear in no vtable**, and the sidecar names them (`Method_Block_…`). Naming on the node was
never going to be complete, and under D5 it is not attempted.

### Consequence: naming happens in the VIEWER, not on the node and not in porta

RECORDS §4.5: "Porta stays a **stateless relay**; the debug session lives in the node."
That plus D4.1 (values are raw CBOR; decode only for display) puts offset→name resolution
in whatever renders the table. The node ships the minimum that cannot be derived elsewhere —
`offset count` pairs — and everything interpretive happens where there is disk and CPU.

### D4 — The rows leave by the console, not the space

`mprof_report` writes `mprof: <offset> <count>` lines to the console. The console is already
teed over Thread to the collector (#56 step 1, merged 5de48f6) and already durable in
day-partitioned files, so a node on wallwart power with no data cable is covered. **This is
a field path, not a bench path** — that is what makes D4 possible at all.

Rejected: the table over `dbg/resp`. At a 256 B value and a 512 B batch, a few hundred rows
is on the order of a hundred check-ins. CLAUDE.md already states the general form of this as
a standing rule — bulk transfer (images, profiler blobs) is **deliberately off the space** —
and a profile table is exactly a profiler blob. D4 keeps that rule intact rather than
arguing with it.

The result is a clean split: the space carries **one control line in**, and the console
carries **the bulk out**. Both are paths that already exist and are already field-proven.

### D5 — C dumps; nsl never reads the table. The mirror question is retired, not answered

The counter's readback prims (`callsOf:` / `callsStats:` / `callsReset:`) live on the
**mirror**, which `oimg_loader.c:238` grants to the image's Supervisor and not to the app.
An earlier draft called this "the largest unresolved piece" and a capability-posture
question.

It was only a problem because the spike assumed **nsl** would read the counts and publish
them. Under D4 the dump is `mprof_report()`, which is C, already written, and already the
thing the destructor and `mprof_phase_advance` call. Emitting `offset count` is not
reflection — it is the same bytes C already produces. So nsl never touches the table, the
grant does not widen, `supervisor.nsl`'s stated rule that it is the **sole** mirror holder
with no ambient access is untouched, and the Supervisor is not involved at all.

The mirror readback prims stay where they are. They are unused by this feature and are not
removed; they remain the route an in-image debugger would take.

Rejected, and worth recording because each looked reasonable:

- **Widen the grant to the app or the bridge job.** Contradicts the confinement design for a
  feature that does not need reflection.
- **A plain non-mirror prim that answers counts to nsl.** Adds a *second* reflection path and
  makes the counter table ambient — against the standing mechanism-minimization preference,
  and it buys nothing once C does the dumping.
- **Route `dbg/req` into the existing `Supervisor` command loop.** Tempting, because
  `supervisor.nsl` already runs a line-in/line-out loop and `dbg/req`/`dbg/resp` are both
  `{line: str}`. It fails on scheduling: `Supervisor>>run:platform:args:debug:` ends in
  `^ entry main: plat args:` and blocks there for the life of the app, so the loop only runs
  at boot or at a breakpoint. Re-entering it mid-run is a control-flow problem this feature
  does not need to solve.

### D6 — The window is self-limiting: the request carries a duration

The start request carries the window length in **milliseconds** (D4.2 — all timestamps are
ms). The node starts, and stops on its own.

Rejected: an explicit stop request. A lost or undelivered stop leaves a field node counting
at 0.90% forever with nobody aware, and it introduces a half-open state that has to be
reasoned about on every path. A self-limiting window means **a node that never hears from
porta again still stops and still reports**, which is the only behaviour that is safe to
deploy into a field where messages are lost.

Also rejected: an activation budget, which is what the spike does today. Aiming it requires
knowing the activation rate, which is the thing being measured — the existing seam comment
already records that a one-shot version cannot be aimed.

Lifecycle, complete: **start = zero the table + enable**; **end = disable + `mprof_report`**.
A start arriving while a window is open restarts it, matching `SpanCapture`'s existing
treatment of a re-start.

### D7 — porta relays the `dbg` path statelessly, and holds developer assets statefully

RECORDS §4.5's "porta stays a stateless relay" governs the **`dbg/req` / `dbg/resp` path**:
porta does not interpret, buffer or own the debug session. It says nothing about porta
holding developer-facing **assets** — console history, images, symbol sidecars — which live
on a different plane entirely, the same one CLAUDE.md already puts profiler blobs on when it
keeps them off the space.

This is recorded as a decision because the two readings are easy to conflate, and conflating
them makes the whole of D4/D8 look like a violation of §4.5 when it is not. **RECORDS §4.5
should be amended to scope its claim to the path**, so a later reader does not have to
re-derive this.

### D8 — The trial view is served by the console collector, not by porta's httpsrv

An htmx page in the Python console service (`nodus/tuvm/zephyr/tools/console/`), sitting
directly on `logstore.py`, `mprof_resolve.py` and `symbolize.py`. It calls porta to start a
window.

The view needs four things, and three are already Python and already colocated: console
history (`logstore.py`, plain day-partitioned text files under
`~/.local/state/rete/console/`), offset→name (`mprof_resolve.py` + sidecar), and panic
symbols (`symbolize.py`). The fourth — starting a capture — is porta's, because porta owns
the `dbg/req` write.

So either choice has exactly one cross-seam hop and they differ only in which. Serving from
porta's Go `httpsrv` moves the **bulk** across: Go would read the Python-side log files and
either shell out to `mprof_resolve.py` or reimplement it, and the log file layout would
become a de-facto contract. Serving from the collector moves the **trigger** across, which
is one small write. The smaller hop wins.

Note this is **not** porta↔node coupling — the console collector is host-side dev tooling,
not a node — so CLAUDE.md's "the two halves never import each other's code" is not at stake
in either direction.

Deferred, not rejected: an htmx view on porta's own mux, which `cidr.go` already names as
the extension point. If porta becomes the single front door later, a link from it costs
nothing.

### D9 — Table capacity is unmeasured; measure before choosing a fix

`OEVM_MPROF_SLOTS` is 256, and the table is first-come: a method already present always
increments correctly, and a *new* method arriving at a completely full table is dropped and
counted in `g_mprof_overflow`. **No record is lost** — `mprof_report` prints every occupied
slot plus the overflow count, and `mprof_resolve.py` prints the full table under its
`INVALID:` banner rather than withholding it.

What degrades is the **percentages**. The header's `activations=` is the sum of counted
slots only, so it excludes overflowed activations, and the resolver divides by it — every
`%` is inflated when overflow is non-zero. **Fix, in scope for this rung:** divide by
`activations + overflow`, which makes each percentage a correct lower bound, and downgrade
`INVALID` to a caveat.

**MEASURED 2026-08-06: 224 distinct methods, overflow=0, load factor 0.875.** See
`docs/results/2026-08-06-mprof-capacity-under-bridge.md`. Taken on the host gate rather than
a device, because the bench had no porta running and a peerless bridge never runs the
reply/apply path — and because a distinct-method count is a property of the program, not the
processor. The "~224 rows" quoted in earlier drafts of this document was an estimate that
happened to be right; it is now a measurement.

This is **branch 2** below: 224 is 25% past the ~180 the ≤0.7 load factor allows, leaving 32
slots of headroom that the `dbg/` handler in this very rung will spend some of.

**AMENDED 2026-08-06, on hardware: 177 distinct methods, overflow=0, load factor 0.69.** See
`docs/results/2026-08-06-mprof-window-hardware.md` — a 10 s window on c6spare, attached to the
porta mesh, driven by the `dbg/req` this rung built. The two numbers do not conflict and
neither supersedes the other; they answer different questions:

- **224** is the union of every path the host gate exercises, accumulated over a long run.
- **177** is a node in steady-state bridging for ten seconds.

So the branch depends on which case the table must hold. 256 slots is comfortable for the
steady state — 0.69 is *under* the ≤0.7 the code warns about — and tight for the general one;
a window opened during start-up or an unusual path lands nearer 224. The honest reading is
that **the table is adequate for what this rung ships and marginal for the general case**, and
since a saturated table now reports `INCOMPLETE` with exact counts and lower-bound
percentages rather than `INVALID`, saturation degrades rather than lies.

**What forces the decision is not this number.** Call-site keying
(`docs/design/2026-08-07-inclusive-attribution-design.md`, D9) multiplies cardinality from
methods to send sites — plausibly 400–700 — and that is where a 256-slot table stops working.
Deciding then means deciding against a measured call-site count instead of either estimate
above.

**And it undermines the cost figure.** 0.90% was measured on Hanoi — two methods, load factor
0.008, where every lookup is one probe. Unweighted linear-probe cost at 0.875 is ~4.5 probes.
The activation-weighted figure is probably much lower, because the hot methods were inserted
while the table was sparse and sit at their home slots — but it is **unmeasured**, and the
"1% is acceptable" judgement was made against a table that was empty. Measuring the ON arm
against a populated table is the first thing this earns.

So the first task of the plan is one bench run: profile `PortaBridgeNodeC6` and read
`methods=` and `overflow=`. It decides which problem is being solved, and no tunable is
invented before it answers. **It does not depend on anything else in this design** — the
existing `OEVM_MPROF_RUNTIME_GATE` / `FLIP_AT` / `DUMP_AT` seam already produces the dump
today, so the measurement can be taken before the first prim is written.

If the count fits, **top-N is a display choice** — `sort -rn | head -64` in the resolver,
exact counts, no change to the counter at all. If it does not fit, the choice is between:

- **A larger table** (512 slots = 4 KB), which keeps counts exact and is the smallest
  possible change, but is still unbounded in principle and moves further from ever fitting
  a constrained target.
- **Space-Saving displacement at k=64** — on a miss, evict the minimum and give the newcomer
  `min+1`. Bounded memory for *any* program. Its usual objection (needing an O(1)
  Stream-Summary) does not apply here: the set of methods a program touches is small and
  **stable**, so the table fills during warmup and then it is all hits. A miss happens
  roughly once per distinct method *ever* — a few hundred times in a run, not per activation
  — so a linear min-scan over 64 slots costs nothing measurable and the hot path is
  unchanged. It also has a prize unrelated to overflow: 64 × 12 B = **768 B** with the error
  field, which fits the ESP32's 816 B of spare dram0 and would put the profiler on a target
  the 2 KB table excludes. Its cost is honesty: Space-Saving **overestimates**, so counts
  become bounded ranges and the per-entry error field must be kept and rendered as such.

### D10 — Localized profiling scopes by METHOD IDENTITY, not by breakpoint (deferred, gated on D9)

David's workflow, 2026-08-06: profile the whole program for a rough estimate, then narrow to
a region and re-profile for a higher-quality measurement. This is the right shape and it
attacks D9's capacity question from a better angle than displacement does — **narrowing the
scope shrinks the working set**, so 64 slots may become generous rather than tight. It also
fixes signal quality: a whole-program profile is dominated by the event loop
(`pumpEvents` / `Space>>drain`), rows a reader is currently told to ignore by hand.

**Rejected: reusing the existing breakpoint machinery.** It is not available where this has
to run. `nodus/tuvm/zephyr/CMakeLists.txt:267` compiles `OEVM_NO_DEBUG_HOOKS=1` into every
Zephyr build, recording that the per-bytecode checks cost **10–16% (ledger E8)** and that no
on-target debugger transport exists. Gating a 0.90% measurement behind a 10–16% tax inverts
the economics. The bp check at `vm.c:765` is also per-bytecode with a linear `bp_hit` scan,
and it requires `debugger_hook`, which is registered through the mirror — returning to
exactly the Supervisor confinement problem D5 retired.

> **Corrected 2026-08-06 by measurement.** The paragraph below argued Space-Saving at k=64
> would be nearly free because "a miss happens roughly once per distinct method *ever*." That
> premise assumed the working set fits the table. It does not: the measured working set is
> **224 distinct methods** (`docs/results/2026-08-06-mprof-capacity-under-bridge.md`). At 224
> methods in 64 slots the table is permanently saturated and displacing continuously, which
> puts the linear min-scan on a **hot** path instead of a cold one and inverts the whole
> argument. **k=64 is viable only in combination with the scoping below**, which shrinks the
> working set beneath the table — never on its own. The same measurement shows the existing
> 256-slot table runs at a **0.875 load factor**, so the 0.90% cost figure (measured on Hanoi
> at load 0.008) does not describe a real workload either.

**Accepted shape: scope by method identity, using #164's own hook.** `push_frame` already
fires on every activation and already holds the `CompiledMethod *`. `OP_RET` (`vm.c:1063`)
is the single symmetric return site. So:

- `push_frame`: `if (m == scope_key) g_mprof_scope_depth++`
- count only while `g_mprof_scope_depth > 0`
- `OP_RET`: `if (method == scope_key) g_mprof_scope_depth--`

The depth counter rather than a flag is what makes recursion correct. Both sites sit inside
`#ifdef OEVM_METHOD_PROFILE`, so a normal build pays nothing, and a profile build pays two
pointer compares.

It composes with every decision above without adding a mechanism: the scope is named by the
**same blob offset** the profiler already reports and the sidecar already resolves, so the
start request grows one optional field and porta needs no new concept. The loop becomes
whole-program → read the table → scope to the dominant method → re-profile.

What is given up versus real breakpoints is **granularity**: a scope is a method, not an
arbitrary bytecode offset. For profiling that is the correct unit — the question is
inclusive cost under a call, not the cost of a bytecode range.

**Deferred, and gated on D9's measurement**, per David: whether this is needed depends on what
the real distinct-method count turns out to be. Recorded now because it changes what the D9
answer *means* — if scoping is this cheap, narrowing is a better answer than a larger table
or displacement, and D9 should be read with that option on the table.

## Implementation notes and traps

- **`platform timers` must be held as a single instance.** It is the one platform accessor
  where re-fetching breaks behaviour: a fresh instance makes `dispatch:` silently drop every
  timer event. D6's window end is a timer.
- **Prim ids are name-resolved and append-only.** Two new Kernel-seam prims — start
  (reset + enable) and stop (disable + report).
- **The bridge is already authorized to read `dbg/req`** (PROTOCOL-NSL §3, and the bridge job
  holds exactly `goal/**` + `dbg/req` inbound). No capability changes.
- **There are two unrelated classes named `Supervisor`** — `nodus/nsl/lib/supervisor.nsl` (the
  mirror holder, minted by the loader) and `Kernel>>Supervisor` (job/restart supervision,
  reached via `platform supervisor`). D5 concerns only the first. Do not conflate them.
- **The idle cost of the runtime gate is unmeasured, and may be unmeasurable here.** The
  0.90% figure is an ON-vs-OFF delta *within one binary*, with the gate scaffolding present
  in both arms so it cancels out. Measuring the gate itself requires a cross-binary
  comparison — the thing the seam comment warns about, having produced an ON build of
  `fib(35)` that came out 4% *faster*. Do not chase it; `OEVM_METHOD_PROFILE` covers the
  case where it would matter.
- **A dump is `stderr`.** It reaches the collector because the console is teed, not because
  anything routes it. A target whose console is not teed gets no rows and no error.

## What is already built and green

- The counter, off by default behind `OEVM_METHOD_PROFILE` — `nodus/tuvm/src/vm.c`.
- The runtime gate `g_mprof_enabled` and `mprof_report()` — both present, wired to an
  activation countdown that this design replaces.
- Symbol resolution: the `<image>.oimg.methods.json` sidecar from
  `ns_compile_oevm.py --format=oimg`, and `nodus/tuvm/tools/mprof_resolve.py`.
- The console path end to end: tee over Thread, `collect.py`, `logstore.py`, `symbolize.py`,
  and the `rete-console@.service` unit (#56 step 1).
- Measured: 0.90% on a C6, exact counts, suite 246/0.

## What this rung does not do

- **Time attribution per method.** Counts only, as #164 scoped it. Counts × the span sums
  that already exist gives µs/call without inventing a sampling rate.
- **Caller attribution or a call graph.**
- **An htmx view on porta's own mux** — deferred by D8, not rejected.
- **`diag/spans/ctl`'s authority contradiction** — #159 stays open on its own merits; D2 only
  removes this feature's dependence on it.
