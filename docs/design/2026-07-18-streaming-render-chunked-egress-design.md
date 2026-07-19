# Streaming render — chunked HTTP egress from nsl

**Date:** 2026-07-18
**Status:** design (approved for planning)
**Scope:** `zephyr/HttpServerNode.nsl`, `src/http_core.c`, `src/backend/*/http_*.c`,
`src/platform.c`, `src/prim_ids.h`, `~/workspace/nsl-tuvm/lib/kernel.nsl`
**Related:** `docs/design/2026-07-18-esp32-wifi-http-last-mile-design.md`,
`review/model-capability-findings.md` (#46 on-device close-out)

## Problem

On the WROOM-32 (no PSRAM), `HttpServerNode` runs at the edge of **both** RAM
budgets simultaneously (measured #46, `review/model-capability-findings.md:198`):

- VM-heap high-water **~52.8 KB of a 53.2 KB arena** (~0.5 KB spare)
- **~1 KB of 192 KB DRAM** free at link

The VM arena is capped at 53248 B by DRAM — WiFi's ~40 KB ESP driver heap + the
TCP net-buf pool leave no more (`zephyr/run.sh:37-45`). It cannot grow. With
~0.5 KB of headroom, **any** added demo feature (a color legend, an extra route,
a higher-res grid) OOMs the arena as-is.

Most of that peak is **transient render garbage**. `renderFrame:` /
`statsPageOn:` build each page as one big immutable String via `,` concatenation
— for the 8×8 SVG that is 64 `<rect>` fragments chained, O(n²) copying and
**300+ GC cycles** per render. Reclaiming this peak is the prerequisite for any
further practical-demo work on this node.

**Goal (chosen):** reclaim the render peak so demo features have room; pages stay
small (a few KB). Explicitly *not* chasing deltablue-class compute — that is a
PSRAM-part question (`review/model-capability-findings.md:204`).

## Why not the obvious buffer fixes (rejected alternatives)

Investigation of the VM's actual types (2026-07-18) overturned the intuitive
"use a mutable buffer instead of concat" fix. On this VM:

- **Every *mutable* accumulator is 4 bytes per character.** `ByteArray`'s backing
  `raw` is a plain indexable (`u8At: i = raw at: i`) — an Array of boxed smallint
  slots, 4 B each on Xtensa, **not byte-packed** (`nsl-tuvm/lib/kernel.nsl:561`).
  Stock `WriteStream` is worse: an `OrderedCollection` of boxed chars (4 B/char),
  then `contents` copies through `asArray` (another 4 B/char) into a packed String
  — triple pass (`kernel.nsl` `class WriteStream`).
- **Only *immutable* String/Bytes are byte-packed** (1 B/char). String `,`
  concat already runs entirely in packed Strings — `dispatch.c:1505-1518`
  `gc_allocate` a fresh `struct String` + memcpy both halves.

Consequence: for a ~4 KB page **held whole in RAM**, swapping concat →
ByteArray/WriteStream trades ~15 KB of *packed garbage* for ~16–32 KB of *live
4-B/char accumulator*. It reliably cuts **GC cycles** (a CPU/latency win) but
**does not shrink peak RAM** — it can worsen it. So:

- **Rejected — stock `WriteStream`:** ~16–32 KB accumulator for a 4 KB page. A
  trap at this size (it is fine only for the tiny `dbg:` lines it serves today).
- **Rejected — mutable `ByteArray` / template-substitution:** same 4-B/char
  reality; no peak-RAM win. The template model also fits `/stats` (fixed rows)
  but not the data-driven repeated-`<rect>` `/thermal`.
- **Rejected — fragment-list + one-pass `join` primitive:** keeps everything
  packed (~9 KB peak vs ~15 KB) and honors "hold the page," but only reclaims
  ~6 KB and still materializes the whole body. A reasonable smaller-scope
  fallback, but a *patch*, not a mechanism.

## Chosen approach — stream chunks to the socket

Never hold the page. Emit each packed fragment straight to the socket as it is
generated, using **HTTP/1.1 chunked transfer-encoding** (no `Content-Length`).
The page size is decoupled from RAM permanently.

This is the *least-kludge* option and the natural completion of an arc #46
already began: the C transport already streams header-then-body separately so a
body needs no scratch buffer (`http_core.c:48`, findings #3 "stream large egress
bodies"). This design pushes the streaming boundary **up into nsl** so the body
itself is produced incrementally.

### Other uses unlocked (why it is a mechanism, not a patch)

- Arbitrarily large bodies: metrics JSON export, log dumps, higher-res grids.
- **Live-updating thermal view** via Server-Sent-Events (grid refreshes in the
  browser with no reload) — a genuinely better demo than reload-to-refresh.
- The emit-sink primitives are reusable by any egress node, not just HTTP.

## Design

### §1 — Three `id`-keyed streaming primitives

The transport already keys everything by request **`id`**; the raw `fd` lives in
the in-flight slot table and is looked up by `http_respond(id, …)`. nsl never
sees the fd. The streaming seam reuses that handle:

| nsl primitive | C action (`http_respond`-style, keyed by id) |
|---|---|
| `platform httpBeginId: id status: st` | look up fd; write header block with `Transfer-Encoding: chunked` (no `Content-Length`); mark slot streaming |
| `platform httpChunkId: id data: s` | write one chunk: `hex(len)\r\n` + `s.bytes` + `\r\n` |
| `platform httpEndId: id` | write terminator `0\r\n\r\n`; **close fd**; free slot |

`s` is a small packed String fragment, emitted then immediately collectable, so
the page never materializes whole and everything stays 1 B/char. These sit
beside the existing `httpRespondId:status:body:` (`prim_ids.h`, `platform.c:781`)
— which is retained for the small fixed routes (`/`, `/hello`, 404) where a
one-shot body is simpler.

### §2 — C transport refactor (share `send_all`, defer close)

`http_emit` currently frames a full response and calls `zsock_close(fd)` at the
end. Split its body-writing into begin/chunk/end that share the existing
`send_all()` (`http_zephyr.c:33`), with **close owned by end**:

- **Backpressure is already solved** — `send_all` sends in 512-B sub-chunks and,
  on `EAGAIN`/`EWOULDBLOCK`/`ENOMEM`, does a bounded cooperative `k_msleep(1)`
  yield-retry ("let TX drain," bounded to ~2 s so a dead peer can't wedge the
  reactor). This *is* the reactor's "yield, don't hog" philosophy applied to
  egress. The streaming writes reuse it verbatim.
- Our chunked frames (~60 B per `<rect>`) are smaller than `send_all`'s 512-B
  sub-chunks, so an individual chunk almost never blocks; aggregate TX-buffer
  fullness is handled by `send_all` identically to today.
- New backend hooks (per target, posix + zephyr): `http_emit_begin(fd, hdr,
  hlen)`, `http_emit_chunk(fd, data, len)`, `http_emit_end(fd)`. The posix sink
  mirrors this over its blocking/nonblocking socket; `fd == -1` (injected) is a
  no-op / stdout as today.

**Why not suspend/resume (true reactor FSM on output):** parking a half-rendered
page and resuming it on the next writable tick would need coroutines. The VM's
fiber primitives (`fiberCreate:`, `fiberSwitch:value:`, `resumeWith:`,
`currentFiber`) were **deliberately removed** (`prim_ids.h:9`, Task 9a). An nsl
render therefore runs to completion; `send_all`'s cooperative yield-retry covers
backpressure without needing to suspend the render. Adequate at demo scale
(browsers do not parallel-connect to one page — findings note).

### §3 — nsl render contract: body-as-block through the reply channel

Today the router reaction builds a `{id,status,body}` Record → `reply` channel →
a separate `whenever:` reaction forwards it to `httpRespondId:status:body:`
(`HttpServerNode.nsl:36-41`). Streaming cannot stage a String on a channel — the
render must run **at emit time with `id` in hand**.

Chosen contract **(b) body-as-block**: the reply Record carries a *renderer
block* instead of a body String. The egress `whenever:` reaction:

```
reply whenever: '' do: [ :resp |
  | id st blk |
  id:: resp at: #id. st:: resp at: #status.
  blk:: resp at: #body.                     "a block: [ :sink | ... ]"
  platform httpBeginId: id status: st.
  blk value: (self sinkFor: id).            "block writes fragments via the sink"
  platform httpEndId: id ].
```

A `sink` is a thin nsl object wrapping `id` whose `<< aString` (or `nextPutAll:`)
calls `platform httpChunkId: id data: aString`. `renderFrame:` becomes:

```
renderFrame: f toSink: sink = (
  sink << '<html><body><svg viewBox=''0 0 8 8'' ...>'.
  1 to: 8 do: [ :row | 1 to: 8 do: [ :col |
    | k | k:: ...
    sink << '<rect x=''' << col printString << ''' y=''' << row printString
         << ''' width=''1'' height=''1'' fill=''rgb(' << k printString << ',0,'
         << (255 - k) printString << ')''/>' ] ].
  sink << '</svg><p>' << (self caption: f) << '</p></body></html>' )
```

Fixed small routes may still return a String body (a block that emits it once, or
keep `httpRespondId:` for them). Uniform "everything egresses through the reply
channel" idiom is preserved — the "body" generalizes from *String* to
*something that writes to a sink*, a reusable streamed-egress pattern.

**(a) inline-stream** (router calls begin/chunk/end directly, bypassing the reply
channel for streamed routes) was considered and rejected: simpler but
special-cases streamed routes and breaks the uniform egress seam.

### §4 — Feasibility gate (first implementation step)

Contract (b) depends on a channel Record field carrying a **block** that is
`value:`'d in a *different* reaction at egress. This must be proven before the
rest is built:

- Blocks are first-class in nsl (reactions are blocks), but confirm a block
  stored in a Record field survives channel `out:`/`whenever:` delivery and is
  invocable at egress with the sink argument, and that the GC keeps the captured
  block's closure roots alive across the safe point.
- If blocks-through-channels does **not** hold, fall back to **(a) inline
  stream** (router streams directly) — same primitives, no channel-carried block.

## Impact / measurement

- **Expected:** render peak drops from ~52.8 KB toward the node's non-render
  live set (target ≥ ~15 KB arena headroom); GC cycles per render 300+ → ~0
  transient-string allocations.
- **Verify on-device** via the existing `/stats` page (`heap high-water`,
  `gc cycles`): serve `/thermal` repeatedly, read high-water before/after. No new
  instrumentation needed — the metrics already exist (`HttpServerNode.nsl`
  `statsPageOn:`).
- **Correctness:** `curl -v http://<ip>:18080/thermal` shows
  `Transfer-Encoding: chunked` and a well-formed body; browser renders the grid
  identically to today.

## Out of scope

- Content compression, keep-alive, HTTP/1.1 pipelining.
- Concurrent slow-client fairness (would need the removed fibers; not demo-scale).
- deltablue-class compute headroom (PSRAM-part question, separate track).

## Promotion (close-out)

On completion, promote the durable knowledge to:
- `review/model-capability-findings.md` — replace the "streaming would reclaim
  ~15 KB (theory)" note with the measured post-streaming high-water.
- The owning transport doc under `review/lib/` — the begin/chunk/end egress seam
  + `send_all` backpressure reuse.
- `review/program-sequence.md` — the rung entry, if this closes one.
This dated design doc stays as the historical "why."
