# Node profiler — the porta-lite slice

**Status:** design, approved to plan · **Date:** 2026-07-22 · **Issues:** tuvm#72 (facility this
consumes), tuvm#73 (the bug this makes structurally impossible), tuvm#69 (witness store this seeds)

## 1. What this is, in one line

A node emits raw CBOR over UDP; a gateway program (porta-lite) mirrors it into a store and
renders the page. This slice proves the mechanism on the smallest real payload — the #72 span
table — and nothing more.

## 2. Why — the problem this closes

A browser opening the node's own `/stats` page pins the node's connection budget
(`CONFIG_NET_MAX_CONTEXTS=5`, four usable after the listener) with parallel and speculative
sockets, and the node then answers nothing at all (tuvm#73). tuvm#65 was the same accept path
under an unpaced burst. **Both are diseases of the inbound-TCP-accept path.** A profiler that
reaches the node over a bound UDP socket never uses that path, so it cannot catch either
disease — the fix is architectural, not a better reaper.

A second motive: the span figures quoted in `review/` and in the performance deck are typed by
hand from a rendered page today. A store of pushed runs, each attributable to a node and (later)
a build, is the witness store tuvm#69 asks for.

## 3. The shape

```
  browser ──HTTP──▶ profweb.py :8086        (renders from the store; NEVER touches a node)
                        │
                     store (SQLite, porta schema)
                        │
                     mirror.py               (binds collector UDP port; CBOR⇄dict; sends ctl)
                        │ UDP/CBOR
        ctl ──────────▶ │ ◀────────── result
                     node :diagPort          (one NetBridge; the #72 facility, unchanged)
```

Three units, and the boundary is the point:

| unit | where | job | must NOT |
|---|---|---|---|
| `NetBridge` wiring | node, nsl | bind one UDP socket to two channels | render, format, serve HTTP |
| `mirror.py` | gw | collector socket; decode CBOR; write store; send ctl | know what a "span" is |
| `profweb.py` | gw | `:8086`; render span table from the store | open a socket to any node |

**`profweb` never speaks to a node.** That is what makes tuvm#73 *structurally* impossible on
this path rather than merely unlikely: no browser-driven code path reaches the node at all.

## 4. Node side — the minimum, one declaration

The #72 capture facility already reacts to writes on `diag/spans/ctl` and writes its report to
`diag/spans/result`. It needs **zero changes**. We add one bridge:

```
NetBridge socket: diagSock out: (space channel: 'diag/spans/result')
          peer: gwAddr port: diagPort in: (space channel: 'diag/spans/ctl')
```

- A gw datagram carrying a `#SpanCtl` record decodes onto `diag/spans/ctl` → starts/stops the
  window. No node code special-cases the gw.
- When the window closes, the facility writes `diag/spans/result`; the bridge's `out whenever: ''`
  encodes it and **pushes** it to the gw. No poll, no `.txt` route, no HTML, no scraping.

The bridge's **inbound** side is pump-based, not push: `pumpInbound` (`kernel.nsl:1435`) calls
`socket receive` in a loop, so the node's event loop must call `bridge pumpInbound` each tick
alongside its existing `pumpEvents` / `drain`, and must call `bridge start` once to arm the
outbound subscription. This is a node-side *pump*, not a listener — a bound UDP socket has no
accept queue — so tuvm#73 immunity is unaffected; but if the pump is not wired, ctl datagrams
are silently never read. The plan must wire it.

This is also the first real proof of the §7 claim in the #72 design — "this page is a CLIENT of
the facility and not the facility itself." A second client drops in over a different transport
without touching the facility. If that claim were wrong, this is where it breaks.

### 4a. RISK — verify first: does `out whenever: ''` fire on a cell `put`?

`NetBridge` (`nsl-tuvm/lib/kernel.nsl:1431`) was built against `echo/out`. The outbound
subscription firing on a **cell write** (`put`) is the one unproven assumption in this slice.
If it does not fire, the fix is small: make `diag/spans/result` a `post` (stream) at the write
site, or have the bridge peek-on-write. Test this before anything else.

## 5. Gateway side — porta-lite, one slice

The store is porta's schema (`porta/docs/PROTOCOL-NSL.md:349-352`), in SQLite (stdlib — still a
python hack, no new dependency):

```
space_events(node_id, path, seq, value_cbor, ts, direction)   -- append-only; the run log
space_cells (node_id, path, seq, value_cbor, ts)              -- latest value per path
```

- `mirror.py` is porta-lite's kernel: it mirrors **any** path a node pushes and knows nothing
  about spans. The next thing mirrored (thermal, soak) is a new *view*, not new transport — that
  is the entire value of taking the slice this way.
- `profweb.py` is one view: it reads the latest `diag/spans/result` cell for a node and renders
  the span table (the columns #72 already defines — span, lang, n, fails, min/max µs).
- `node_id` is the source address until nodes can name themselves (§7). Recorded as a known gap,
  not disguised.

### 5.1 Driving a run

For the thermal node the job ticks on its own timer, so a run is: send `ctl:running` → wait →
send `ctl:stopped` → receive the pushed result. **This slice generates no synthetic load** — it
brackets the node's real work. That is exactly the always-on-node story, so the simplest version
is also the honest one. Synthetic HTTP load is a later, HTTP-node-only special case, not the
mechanism.

## 6. Measurement integrity — rules the code enforces

- **Never poll a node inside a capture window.** Every poll is a request the spans record; a
  fast-polling profiler perturbs the thing it measures. The node speaks when it has something to
  say.
- **Durations only; never subtract timestamps from two clocks** (`latency_harness.py:8-19`). The
  network column = client RTT − node service time, each half on one clock. When synthetic load
  lands, this lives in exactly one function with that comment on it.
- **Figures come from the capture table, never the cumulative one** (the cumulative table partly
  measures its own rendering). Standing rule from the #72 addendum.

## 7. Out of this slice — named so nobody assumes coverage

- **Build identity.** No version string or build id exists in the tree today (checked: no
  `GIT_DESCRIBE`/`VERSION` in CMake, none in `kernel.nsl`). A stored run is attributable only by
  address until this lands. Filed as its own issue; honours "skip if expensive."
- **Run diffing / history UI** — the store makes it possible; this slice does not build it.
- **Thermal and stats as CBOR views** — the direction of travel (§9), explicitly not now.
- **Auth, DTLS, multi-node** — porta's job, not porta-lite's.

## 8. `/hello` — the one HTTP path a wifi node keeps

The endpoint of the direction of travel is a production node with **no HTTP accept path at all**.
The single deliberate exception is `/hello`, because it is the one thing CBOR-to-gw cannot give:
liveness with zero infrastructure — one `curl`, no gw, no mirror, no decoder.

- **Single path, no links.** A browser has nothing to prefetch, so #73's fan-out has nothing to
  grab. It defends the accept path by *shape*, not by a reaper.
- **Answers what is cheap:** `hello`, uptime (`millis`, already used by SpanCapture `t0`), and a
  hand-maintained nsl version string. Real build/date waits on §7's issue.
- It is a probe, not a view. Everything that is a view moves to the gw; the one thing that is a
  probe stays on the node.

## 9. Direction of travel — NOT part of this slice

Recorded so the slice is understood as a first step, not a whole.

- **Thermal → CBOR.** Measured 2026-07-22: `/thermal` is 4,067 bytes / 64 `<rect>` / 147 ms off
  the gw. The 64 readings as CBOR ints are ~210 bytes — ~19× less wire — and shipping them raw
  deletes 64 × `printString` + row concat, which is what tuvm#50 is open about. The gw renders a
  better grid (real colour scale, history) than string-concatenated rects.
- **Stats → CBOR.** Even more so: that page is data plus *static prose* (the legend). Reference
  material about the system belongs on the gw, not re-emitted from the node's image per view.
- **HTTP listener → opt-in, default off.** For production nodus nodes the listener is the common
  ancestor of #65 and #73; a build-time knob defaulting off means the default node carries no
  accept path. **Gate on measurement** (how much of the 3,568 B spare DRAM returns is a number,
  §5.1 discipline), **do not delete before the CBOR path is proven end-to-end**, and re-base the
  #61 latency story on the job spans (`tickQueue`/`acquire`/`vmCompute`/`publish`/`jobThermal`)
  since `httpQueue`/`render`/`egress` stop measuring anything real.

The node-renders-its-own-SVG demo is a genuine loss as an *exhibit* even as it is a win as
engineering. The opt-in knob is what keeps both: demo build renders, production build sends.
Parked deliberately — the porta-lite story stands without it.
