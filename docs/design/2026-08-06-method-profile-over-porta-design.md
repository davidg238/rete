# Per-method profiling, surfaced over porta

**Status:** design IN PROGRESS — three decisions taken, the rest unbrainstormed · **Date:** 2026-08-06
**Issues:** #164 (the counter), #163 / #161 (what it was built to answer), #159 (made moot by D2),
#74 (promoted to a hard dependency by D3), #28 (named as the fix for #163)

> **This document is incomplete on purpose.** The session stopped for context at the point
> below. D1–D3 are settled and were chosen by David; everything under "Still open" has not
> been brainstormed and must not be inferred from D1–D3.

## Why

#164 landed an exact per-method call counter and it immediately answered #163 (see
`docs/results/2026-08-06-method-call-counter-spike.md` and the #163 thread): the codec's
cost is one interpreted method activation *per byte*, and byte-level accessors are 97.9%
of all codec activations. That answer was read out of a console log by hand. The point of
this work is that David can **see** it — the same way the #56 console made a dead node
observable.

## Decisions

### D1 — Investigation tool, not continuous fleet telemetry

A node counts and publishes only during a **bounded capture window**, the way
`SpanCapture` already works (RECORDS §4.6). Idle cost is zero; the measured 0.90%
(C6, `docs/results/2026-08-06-method-call-counter-spike.md`) is paid only inside a window.

Rejected: always-on counting. It would pay 0.90% forever and, worse, force an answer to
224 methods against a 512 B batch on every check-in — the exact contention that already
bit as #150, where 14 span cells starved `sys/metrics`.

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
on it; automatic build matching removes the chance to get it wrong.

### Consequence: naming happens in the VIEWER, not in porta

RECORDS §4.5: "Porta stays a **stateless relay**; the debug session lives in the node."
That plus D4.1 (values are raw CBOR; decode only for display) puts offset→name resolution
in whatever renders the table. porta stores and relays `dbg/req` / `dbg/resp` and does not
interpret them.

## Still open — NOT brainstormed

1. **The response shape.** `dbg/resp` is a line stream the node paginates (§4.5). A full
   table is ~224 rows; at a 256 B value and 512 B batch that is ~112 check-ins. Bounded
   top-N is the obvious answer and needs a no-silent-caps rule (report the dropped count
   and the total, per the standing discipline).
2. **The node-side reader.** The counter's readback prims are on the **mirror**, which is
   granted to the image's Supervisor (`oimg_loader.c:238`), not the app. A node app cannot
   currently read its own counters. This is the largest unresolved piece and it is a
   capability-posture question, not a plumbing one.
3. **What "see" is** — a `porta` CLI subcommand, an htmx view on the existing
   `httpsrv` mux (which `cidr.go` already describes as the extension point for exactly
   this), or both.
4. **Whether the console stays the bulk path.** CLAUDE.md keeps profiler blobs
   deliberately off the space. The full table may belong on the console (#56) with only a
   summary on the wire.
5. Capture lifecycle: window bounds, reset semantics, what happens if a window is still
   running when another is requested (`SpanCapture` treats a re-start as a restart).

## What is already built and green

- The counter, off by default behind `OEVM_METHOD_PROFILE` — `nodus/tuvm/src/vm.c`.
- Symbol resolution: the `<image>.oimg.methods.json` sidecar from
  `ns_compile_oevm.py --format=oimg`, and `nodus/tuvm/tools/mprof_resolve.py`.
- Measured: 0.90% on a C6, exact counts, 246/0 suite.
