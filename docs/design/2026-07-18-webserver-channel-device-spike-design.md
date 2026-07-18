# Spike #1 — the webserver as a channel-native driver

*Design / decision record. Written 2026-07-18 with David; reply-half mechanism
corrected 2026-07-18 after mapping the existing seam idioms (NetBridge/udp/pulse).
Spike #1 of the coordination-model spike series
(`2026-07-18-coordination-model-spike-series-design.md`). Probes request/reply (RPC)
over the run-to-completion channel model, and ships a usable thermal-display node as its
artifact. Inherits the driver-exposure model from `review/zephyr-driver-exposure.md` and
the reliability rules G2/G3/G9.*

---

## What this spike is

Two things at once, deliberately:

1. **A probe of the channel model.** An HTTP request/reply server is the canonical test
   of the exact capability fibers used to provide: *await → suspend → resume-with-reply*.
   Modelling it purely through the channel substrate answers whether the model expresses
   RPC cleanly.
2. **A usable node.** A browser hits the node and sees an 8×8 thermal heat-grid (a
   dummy frame now, the real AMG8833 Grid-EYE later). The thermal image is the payload
   that rides on top of the probe.

## Headline finding it is expected to produce

The http server **is a driver** — the same driver model as the sensor and the display,
not a special case:

| device | owns datapath (C) | granted prefix | in | out |
|---|---|---|---|---|
| sensor | I2C + Zephyr `sensor` API | `dev/thermal0/` | `ctl` | `sample` |
| display | SPI + glyph blit | `dev/display/` | `lines` / `at` | — |
| **http** | **TCP + HTTP envelope framing** | **`dev/http/`** | `request` | `reply` |

Same law throughout: **C owns the datapath, records/values cross the seam, nsl owns
coordination.** nsl never sees a socket or a CRLF — only request `[id,method,path]`
values in and `{id,status,body}` records out.

**Both halves reuse proven idioms — which is itself the "not cornered" result.** The
request (ingress) half is the *pulse seam*: an external event enqueued to a mailbox,
drained at the `platform pumpEvents` safe point, posted to a channel. The reply
(egress) half is the *NetBridge idiom*: an nsl `whenever:` reaction that forwards a
channel's values to a C send-prim (exactly how `NetBridgeNode` forwards a channel to
`platform udp basicSend:…`). The channel model **already expresses driver output** — it
was proven for UDP. So request/reply RPC decomposes into:

> **pulse-ingress + NetBridge-egress + a correlation id.**

The *only* genuinely new surface is (a) an ingress mailbox that carries request **bytes**
(method/path) and mints a per-request **id**, and (b) the id round-trip that lets the
reply find its socket. If that comes out clean — and the decomposition above strongly
suggests it will — the verdict is **the model is not cornered by dropping fibers**, no
channel reification needed. *(Reification stays a hypothesis the series can still test
elsewhere; this spike is not expected to force it.)*

## The round-trip (what the model is under test)

```
  C network thread                 reactor (single thread, run-to-completion)          nsl reactions
  ────────────────                 ──────────────────────────────────────────          ─────────────
  accept() a TCP conn
  read request line
  mint id, stash {method,path,fd}
    in in-flight[id]  (C arena, G3)
  ─ request mailbox ──▶  http_pump at the pumpEvents safe point:
                         build Array [id, method, path]  (indexable_new, C)
                         space_post 'dev/http/request'  ──────────────▶  APP reaction: route on path,
                                                                          read dev/thermal0/sample cell,
                                                                          render {id,status,body} Record,
                                                                          out: it to 'dev/http/reply'
                                                          ◀───────────  EGRESS reaction (whenever:
                                                                        'dev/http/reply'): unpack →
                                                                        platform http basicRespondId:
                                                                          status: body:
  ◀─ respond prim ─────  hand (id,status,body) to in-flight[id],
  frame HTTP envelope,     signal the waiting network thread
  send, close, free id
```

### Request half — clean; it *is* the pulse seam

C never holds an oop. The network thread enqueues **C data** (id + copied request bytes)
into a mutex-guarded mailbox; `http_pump()` — called from the existing `platform
pumpEvents` safe point, exactly like `pulse_pump()` — builds an `Array [id, method,
path]` (via `indexable_new`, the same way `space_dequeue` builds its triple) and
`space_post`s it to `dev/http/request`. An inbound request is just an ISR-like external
event carrying a little more data. Expected verdict: **clean** (proven machinery, minus
the bytes-carrying extension).

### Reply half — the NetBridge egress idiom + a correlation id

nsl computes the answer as a `Record` and posts it to a channel it treats as ordinary:

```
dev/http/reply  (stream)  ←  Record tag: #response with: #id -> id
                                                   with: #status -> 200
                                                   with: #body   -> '<html>…</html>'
```

A **driver egress reaction** — `httpReply whenever: '' do: [:r | platform httpRespondId:
(r at:#id ifAbsent:[0]) status: (r at:#status ifAbsent:[500]) body: (r at:#body ifAbsent:[''])]`
— forwards each reply to the C send-prim. This is the established NetBridge shape (reaction → send-prim);
`space.c` stays pure storage, no C hook into the space. C reads only **leaf args** at the
prim boundary (SMI id, SMI status, String body) — it never parses an nsl Record.

Why this is the honest realization (not the earlier "C egress hook" sketch): channels
here are **exact-string, no prefix matching**, so a per-id `dev/http/reply/<id>` path
cannot be wildcard-subscribed, and `space.c` deliberately holds no policy. A single
`dev/http/reply` **stream** carrying id-tagged records, forwarded by one reaction, fits
the model as it actually is.

The spike still reports the verdict on this half — **clean / needs-a-helper / corner** —
but the expectation, given it reuses NetBridge, is **clean**. A `needs-a-helper` outcome
would be a small nsl "reply" helper wrapping the record build + egress; only a genuine
inability to route the reply would be a `corner` (evidence for reifying channels).

### Correlation & concurrency

- **Correlation** = a per-request **id** (SMI), minted by C, carried out in the request
  `Array`, carried back in the reply `Record`'s `#id`, and passed to
  `basicRespondId:status:body:`. C's in-flight table maps `id → socket fd (+ deadline)`.
  (Channels are named by exact string path, not reified — correlation by an id field is
  the honest expression of that.)
- **Concurrency** = a small **bounded in-flight table** (4 slots) so multiple ids are
  genuinely outstanding at once — what makes it a *real* correlation + back-pressure
  test. The (N+1)th concurrent request → **503** (C responds immediately without minting
  an id), the G2 shed. The in-flight table (request bytes + fd + deadline) lives in a **C
  arena** (G3), never the VM heap.

## Responsibility split (unambiguous)

| step | who |
|---|---|
| accept socket, read request line, mint id, stash in in-flight table | **C** (network thread) |
| `http_pump`: build `[id,method,path]` Array, post `dev/http/request` | **C reactor** (at `pumpEvents` safe point) |
| route on path, read `dev/thermal0/sample` cell | **nsl app reaction** |
| build HTML + SVG heat-grid (color map, labels), set `status` | **nsl app reaction** |
| post `{id,status,body}` Record to `dev/http/reply` | **nsl app reaction** |
| unpack reply Record → `platform http basicRespondId:status:body:` | **nsl egress reaction** (NetBridge idiom) |
| frame HTTP envelope, send, close, free id | **C** (send-prim → network thread) |

nsl owns everything from `<html>` inward *and* the response semantics (`status` /
routing). C owns the HTTP transport envelope (status line, `Content-Type`,
`Content-Length` from body size, `Connection: close`, CRLFs) — transport, the same layer
as TCP, so it belongs with the byte-pump.

## Frame contract — `dev/thermal0/sample`

The durable schema (this is the `lib/dev-records.md` contract the sketch flagged — pin
it here):

- **A cell** (retained latest value; rung-9 cell semantics) at `dev/thermal0/sample`.
- Value = **an nsl `Array` of 64 SMIs**, row-major (`index = row*8 + col`; nsl `at:` is
  1-based, so `at: (row*8 + col + 1)`), each a temperature in **centi-°C** (0.01 °C
  units; 21.4 °C → `2140`). Centi-°C in an SMI covers ±327 °C, so the contract
  generalizes beyond Grid-EYE.

Why an `Array` of SMIs and **not** a packed 128-byte `RawBytes`: extracting int16s from
bytes in nsl is exactly the L2 binary-data library (tuvm#28), which is *orthogonal* to
this spike's question. An `Array` reads with plain `at:` — **zero new binary-data
machinery** — keeping the probe focused on coordination. When tuvm#28 lands (the sensor
seam / Spec 2) we can switch to a packed form if memory ever matters (it will not at 64
elements — ~256 B). The **shape** stays: 64 values, centi-°C, row-major — so the sensor
seam is a drop-in that posts the same contract the dummy does.

For spike #1 the frame is **nsl-seeded**: a small nsl routine builds a synthetic
gradient/hot-spot `Array` and `put:`s it to the cell. "Dummy → real" later is purely
*who last wrote the cell* (nsl seed vs C sensor pump) — renderer unchanged.

## Render — nsl folds the frame into a page

All nsl, all integer arithmetic + `printString` (both present in the port; `,` is
**String↔String only**, so every operand is stringified first). Two passes over the
64-element `Array`:

1. **Reduce** — fold for `max`, `min`, hot-spot `(row,col)`. Feeds a caption; shows
   on-node coordinator judgment cheaply, with no extra machinery.
2. **Color-map + emit SVG** — fixed display window `[Tlo,Thi]` (spike: 15–35 °C =
   `1500..3500`; a `ctl` knob later). Linear ramp blue→red, emitted as **decimal
   `rgb()`** so there is **no hex formatting** to write:

```
k    = clamp255( (T - Tlo) * 255 / (Thi - Tlo) )        "one linear map per pixel"
fill = 'rgb(' , k printString , ',0,' , (255 - k) printString , ')'
rect = '<rect x=' , col printString , ' y=' , row printString ,
       " width=1 height=1 fill='" , fill , "'/>"
```

64 rects into one SVG, wrapped in minimal HTML + the caption:

```
<html><body>
  <svg viewBox='0 0 8 8' width='320' height='320' shape-rendering='crispEdges'>
    …64 rects…
  </svg>
  <p>max 31.2C  min 22.1C  hot (3,5)</p>
</body></html>
```

SVG is chosen precisely because it is **text, not a binary image** — it collapses
"page containing an image" into a single text reply the nsl reaction builds alone: one
route, one response, no `<img>`, no second request, no C-side encoder.

*Boundary noted:* nsl building the whole ~2 KB response string in the VM heap is fine
here (small, short-lived, replaced per request — **not** a G3 "big long-lived buffer").
That approach stops scaling only for genuinely large / streamed responses — which is
what the streaming spike (#6) is for, not this one.

## HTTP scope — deliberately minimal

- **HTTP/1.0, `Connection: close`** — one request per connection. No keep-alive, TLS,
  chunked encoding, or compression.
- C parses **only the request line** (`GET /path HTTP/1.0`); reads-and-discards headers
  to the blank line; **GET only** (method carried in the Array, so nsl can answer 405 if
  it chooses).
- Routes are **nsl's** decision: `/hello` (milestone 1a), `/` & `/thermal` → the page,
  anything else → 404.
- Enough for a browser to render the page — nothing more.

## Internal staging

- **1a — `/hello` round-trip.** The full loop (request Array in → app reaction → reply
  Record → egress reaction → `basicRespondId:status:body:`) with a trivial body. Proves
  ingress + correlation + egress. Driven by the **inject test-seam** (below), stdout
  assertions.
- **1b — `/thermal` render.** The app reaction peeks the dummy `dev/thermal0/sample`
  cell, reduces, renders SVG. Ships the node’s content. Still inject-driven, stdout
  assertions.
- **1c — real sockets.** `basicListen:` starts the posix accept thread; a browser / curl
  hits `localhost:8080`. Transport integration test.

## New surface this spike introduces

Mechanism mirrors the **gpio/pulse seam** (device verbs as `platform` prims + a backend +
the pump), **not** the `udp` capability module — `udp`'s object needs invasive boot-time
`#netOn:` injection, whereas the gpio prims (`PRIM_pf_gpioPin/Fire`) are the proven,
minimal-surprise precedent for a device exposed straight off the platform object.

- **Platform prims** (added per the 3-table append procedure — `prim_ids.h` mirror +
  `dispatch.c` enum + `PRIM_NAMES`, id-by-counting; no `builtins.def` / registry bump):
  - `platform httpListen: port` — start the accept thread on `port` (1c);
  - `platform httpRespondId: id status: n body: s` — the egress send-prim (leaf args
    only: SMI id, SMI status, String body);
  - `platform httpInject: method path: p` — **test-seam**: synthesize a request into the
    mailbox without a socket, exactly as `gpio fire:` synthesizes a pulse (1a/1b tests).
- **Backend seam.** `src/http_backend.h` + `src/backend/posix/http_posix.c` (host: the
  request ring + the bounded in-flight table + `http_frame_response` + `http_pump`; the
  `pthread` accept loop + `pthread_mutex_t` guard added in 1c, producer/consumer
  discipline modelled on `pulse_zephyr.c`, not the poll-only `udp_posix.c`) and later
  `src/backend/zephyr/http_zephyr.c` (`zsock` listener). One impl linked per `OEVM_TARGET`.
- **Safe-point hook.** `PRIM_pf_pumpEvents` in `platform.c` also calls `http_pump()`, so
  the standard reactor loop `[true] whileTrue: [platform pumpEvents. space drain]` is
  unchanged in the node.
- **Injected vs socket replies.** The in-flight slot stores an fd; `httpRespondId:…`
  emits the framed response to **stdout** when fd is the inject sentinel (−1, the model
  tests) and over the **socket** when fd ≥ 0 (1c). Same framing path, one branch.
- **Build glue.** Add `src/http.c` + `src/backend/$TARGET/http_$TARGET.c` to every host
  link line (`bench.sh`, `tests/*_host.sh`) and to `OEVM_BACKEND` +the fixed source list
  in the live harness `nsl-tuvm/tools/run_tests_oevm.py`.

## Testing — the model verdict needs zero hardware and zero sockets

Two layers, mirroring how the pulse seam is tested (sim `gpio fire:` **and** the real DK
button):

1. **Model-level (the spike verdict).** Path B (`.oimg`) via the live harness
   `nsl-tuvm/tools/run_tests_oevm.py`. An `HttpNode.nsl` registers the app + egress
   reactions, then uses `platform httpInject:path:` to synthesize requests and runs
   `[…] whileTrue: [platform pumpEvents. space drain]`. `httpRespondId:status:body:` in
   the host backend **prints the framed response to stdout** (no socket, fd = −1), so the test
   asserts routing/status/SVG on stdout:
   - `/hello` → `200`, body `hello` (1a);
   - `/thermal` → `200`, SVG substring + caption (1b);
   - `/nope` → `404` (nsl routing);
   - inject N+1 with the table full → the extra → `503` (G2 back-pressure).
   This is the entry written into `review/model-capability-findings.md`.
2. **Transport integration (1c).** A shell test (`tests/http_roundtrip_host.sh`, modelled
   on `tests/hanoi_ab_host.sh`) bakes `HttpNode.oimg`, runs `/tmp/oevm_img` with
   `httpListen:`, and `curl`s `localhost:PORT/hello|/thermal|/nope` + a concurrent burst,
   asserting status + body over the wire. Proves the real accept-thread/socket path.
- **ESP32 + WiFi last mile** (after the verdict): `src/backend/zephyr/http_zephyr.c`, an
  esp32 board overlay, SSID/PSK via Kconfig (`prj_esp32` WiFi overlay). Point a browser
  at the node's IP. WiFi association risk is orthogonal to the model and deliberately
  isolated here. (ESP32-WROOM-32 has no 802.15.4 — its network is WiFi, so this does
  **not** use the nRF OpenThread path.)

## Reliability rules honored

- **G2** — bounded in-flight table; (N+1)th request shed as 503; request mailbox is
  thread-safe (mutex-guarded ring, like the pulse mailbox).
- **G3** — in-flight request buffers live in a C arena, never the VM heap; the frame is a
  small `Array`; the ~2 KB response string is short-lived (see boundary note).
- **G9** — no unrequested periodic work: the server acts only on inbound requests; the
  reactor idles between them.

## Status

- Design: **approved 2026-07-18 (David)**; reply-half corrected same day post-mapping.
- Next: implementation plan (writing-plans), then build 1a → 1b → 1c on the host, write
  the findings entry, then the ESP32/WiFi last mile.
