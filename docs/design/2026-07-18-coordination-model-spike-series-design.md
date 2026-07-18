# Coordination-model spike series — does the reactor/channel model hold up post-fibers?

*Design / decision record. Written 2026-07-18 with David. Scopes a series of spikes
that validate the run-to-completion channel model against real-world coordination
patterns. Companion to `review/zephyr-driver-exposure.md` (the driver ladder this
series is a prequel to) and the reactor/channel substrate (`src/space.c`,
`src/pulse_backend.h`, rung-9 cells).*

---

## Why this series exists

We gave up fibers (stackful suspension) for the run-to-completion reactor model — a
deliberate trade for memory (no per-op stack) and safety (atomicity within a handler,
no data races). The open question that trade leaves behind:

> **Did dropping fibers box us into a corner?** For real-world coordination patterns,
> does the channel model yield *clean, reusable idioms* — or does await-shaped logic
> metastasize into unmaintainable state-machine soup?

Every application we build sits on this substrate. So before building more apps on it
(the Arc-1 driver ladder and beyond), we spike the model against a representative
spread of real problems and find out where — if anywhere — it strains or fails. This
is effectively an **Arc-0.5 substrate-validation prequel**: the driver ladder resumes
after, built on validated idioms (or on a model we have deliberately extended).

## The one question each spike answers

The reactor model is **strong exactly where fibers were expensive** — no per-op stack,
atomicity within a handler for free, no data races. It is **awkward exactly where
fibers were ergonomic** — anything that in a fiber is "one function that awaits in the
middle" becomes an explicit state machine split across handlers, with the in-flight
state parked in a record instead of on a stack. So every spike answers, for its
pattern: *does a clean, reusable idiom emerge, or does it turn into state-machine
soup?*

## Output of the series

1. **A go / no-go on the substrate** — is the model sound for the practical problem
   space, or are there corners that force a rethink?
2. **An idiom catalog** — the blessed way to write each pattern, as reusable nsl.
3. **A short list of substrate additions the model genuinely needs** — reified
   channels? a promise object? a workflow combinator? — *each earned by a spike hitting
   a wall, never assumed up front.* (Reifying channels, in particular, is a hypothesis
   a spike tests, not a premise.)

## Two tiers of spike

- **Heavyweight (ships a real artifact).** Spike #1 doubles as a usable node (the
  thermal webserver), so it gets the full brainstorm → spec → plan → build treatment.
- **Lightweight idiom probes.** The rest are a ~50-line nsl sketch on the host plus a
  findings entry, escalating to a full design *only if* they hit a corner. The point of
  a probe is to find the idiom — or the wall — fast, not to build the thing.

## The pattern set & order

All are real node/embedded scenarios, chosen to span what fibers used to buy us — no
arbitrary edge cases. Ordered so each builds on the previous idiom.

| # | Pattern | Weight | Real instance | What it stresses |
|---|---|---|---|---|
| 1 | **Request/reply (RPC)** | heavy — ships the thermal webserver | browser → node | await + correlation — the canonical fiber-replacement test; drags in #4 and a taste of #6 |
| 2 | **Sequential multi-step workflow** | probe | protocol handshake / sample→decide→actuate→confirm | many awaits in a row — does the state machine stay readable? |
| 3 | **Scatter-gather (fan-out / fan-in)** | probe | poll N sensors, aggregate | await-all / await-quorum |
| 4 | **Timeout racing an await** | folded into #1 | "reply within 500 ms or give up" | `select`-shaped: a timer channel vs a reply channel |
| 5 | **Long-lived stateful session** | probe | CoAP-observe / MQTT session / a shell | per-session state with no stack to hold it |
| 6 | **Streaming output** | partly in #1, else probe | chunked HTTP / file transfer | incremental produce without buffering the whole thing (G3) |

Spike #1 (request/reply) is first because it is the sharpest probe of the exact
capability fibers provided — await → suspend → resume-with-reply — and because it
naturally exercises #4 (per-request timeout) and a taste of #6 (streaming the
response).

## Per-spike rubric

Every spike records, for its pattern:

- **(a) expressible?** yes / no;
- **(b) the idiom** — the code sketch of the blessed way to write it;
- **(c) verdict** — **clean** / **needs-a-helper** / **corner**;
- **(d) if needs-a-helper** — *what* helper (a promise object, reified channel, a
  workflow combinator, …);
- **(e) rough cost** — memory / latency.

That five-part entry is the durable finding.

## Roll-up, test discipline, ladder

- **Roll-up.** All findings promote into one mainline doc of record —
  `~/workspace/tuvm/review/model-capability-findings.md` (new; per the CLAUDE.md
  promotion rule). The dated per-spike design docs stay behind as the historical
  "why".
- **Test discipline.** Every spike is **host-only** — posix backends + `native_sim`,
  no hardware. Fast iteration; matches "little hardware hookup." The one exception is
  spike #1's ESP32 + WiFi bring-up, deliberately the last mile.
- **Ladder.** This series precedes the resumption of Arc 1 (the driver ladder). Update
  `~/workspace/tuvm/review/program-sequence.md` at that boundary, per the file's own
  rule.

## Status

- Series scope: **approved 2026-07-18 (David).**
- Spike #1 (webserver-as-channel-device): designed — see
  `2026-07-18-webserver-channel-device-spike-design.md`. Next: implementation plan.
- Spikes #2–#6: not started; each opens its own probe when reached.
