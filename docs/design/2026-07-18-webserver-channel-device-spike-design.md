# Spike #1 — the webserver as a channel-native driver

*Design / decision record. Written 2026-07-18 with David. Spike #1 of the
coordination-model spike series (`2026-07-18-coordination-model-spike-series-design.md`).
Probes request/reply (RPC) over the run-to-completion channel model, and ships a usable
thermal-display node as its artifact. Inherits the driver-exposure model from
`review/zephyr-driver-exposure.md` and the reliability rules G2/G3/G9.*

---

## What this spike is

Two things at once, deliberately:

1. **A probe of the channel model.** An HTTP request/reply server is the canonical test
   of the exact capability fibers used to provide: *await → suspend → resume-with-reply*.
   Modelling it purely through the channel substrate answers whether the model expresses
   RPC cleanly — and specifically whether the **reply half** forces us to reify channels.
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
| **http** | **TCP + HTTP envelope framing** | **`dev/http/`** | `request` | `reply/<id>` |

Same law throughout: **C owns the datapath, records cross the seam, nsl owns
coordination.** The http driver frames the HTTP envelope the way the sensor driver
drives I2C and the display driver drives SPI. nsl never sees a socket or a CRLF — only
`{status, body}` records. That the model generalizes across **input device / output
device / service device** with no new concept is itself the sharpest "not cornered"
evidence, and the headline entry for `review/model-capability-findings.md`.

## The round-trip (where the model is under test)

```
  C network thread                 reactor (single thread, run-to-completion)          nsl handler
  ────────────────                 ──────────────────────────────────────────          ───────────
  accept() a TCP conn
  read request line
  mint id, stash {method,path}
    in in-flight[id]  (C arena, G3)
  ─ request mailbox ──▶  pump at safe point:
                         build record {id,method,path} from C buffer
                         space_post 'dev/http/request'  ──────────────▶  drain, route on path
                                                                          render {status, body}
                         ◀───────────  spacePut 'dev/http/reply/<id>' = {status,body}
  ◀─ reply egress ─────  http driver's OUTPUT side sees a write under
  frame HTTP envelope,     'dev/http/reply/'; copies bytes → in-flight[id],
  send, close, free id     signals the waiting network thread
```

### Request half — clean, reuses the pulse seam

C never holds an oop. The network thread enqueues **C data** (id + copied request
bytes) to a mailbox; the reactor **pump** — exactly like `pulse_pump` — builds the nsl
request record at the safe point and posts it to `dev/http/request`. An inbound request
is just an ISR-like external event. Expected verdict: **clean** (proven machinery).

### Reply half — the crux, and it is the *output direction* of the driver model

nsl computes an answer and writes it as a record to a channel it treats as ordinary:

```
dev/http/reply/<id>  =  { status: 200, body: '<html>…</html>' }
```

For that to reach the socket, the **http driver's output side** consumes writes under
`dev/http/reply/` and hands the bytes to the blocked network thread (the reactor stays
run-to-completion: it posts and returns). This is **not** a webserver-specific hook — it
is the general capability:

> **a C driver consuming an nsl-written output channel** — the *output* direction of the
> driver model.

The port today has only the *input* direction (pulse seam: C → channel → nsl). Output
(nsl → channel → C) is genuinely new, and it is **shared infrastructure with the Tier-2
display driver** (which consumes `dev/display/lines` → SPI the same way). So the
spike's sharpest question is not "is this hook clean" but "does the driver model's
**output direction** come out clean" — and answering it pays off the display rung too.

Three ways the reply half can land, and the spike reports which:

- **clean** — the output-driver consumer is a tidy bridge and nsl's `spacePut` reply
  reads naturally → the channel model expresses RPC fine, **no reification needed**;
- **needs-a-helper** — it works but the id-plumbing wants a small nsl "reply-to" helper
  (a promise-ish object wrapping id + channel) → mild extension, not a substrate change;
- **corner** — you genuinely cannot express "route this reply back out" without C
  reaching around the channel model → *that* is the evidence channels should be
  first-class / reified, and it becomes its own substrate spike.

We deliberately build the **channel-native** version (nsl `spacePut`s a reply record),
**not** a bespoke `httpRespond:id:` prim — a bespoke prim would sidestep the very thing
we are probing.

### Correlation & concurrency

- **Correlation** = the string-named convention `dev/http/reply/<id>`; id minted by C,
  carried in the request record. (Channels here are named by string path, not reified
  as first-class values — every `space*` prim keys off a `String` path. Correlation by
  namespace convention is the honest expression of that model.)
- **Concurrency** = a small **bounded in-flight table** (e.g. 4 slots) so multiple ids
  are genuinely outstanding at once. That is what makes it a *real* correlation +
  back-pressure test. The (N+1)th concurrent request → **503**, the G2 shed. The
  in-flight table (request bytes + reply slot) lives in a **C arena** (G3), never the VM
  heap.

## Responsibility split (unambiguous)

| step | who |
|---|---|
| accept socket, read request line, mint id | **C** (network thread) |
| turn request → record, post `dev/http/request` | **C reactor** (pump; pulse-analog) |
| route on path, read `dev/thermal0/sample` cell | **nsl handler** |
| build HTML + SVG heat-grid (color map, labels) | **nsl handler** |
| set `status`, `spacePut 'dev/http/reply/<id>'` the record | **nsl handler** |
| consume reply channel, frame HTTP envelope, send | **C** (http driver output side) |

The reactor does **just** "record in, bytes out to the right socket" — zero
HTTP/HTML/thermal knowledge. nsl owns everything from `<html>` inward *and* the response
semantics (`status`). C owns the HTTP transport envelope (status line, `Content-Type`,
`Content-Length` from body size, `Connection: close`, CRLFs) — transport, the same layer
as TCP, so it belongs with the byte-pump.

## Frame contract — `dev/thermal0/sample`

The durable schema (this is the `lib/dev-records.md` contract the sketch flagged — pin
it here):

- **A cell** (retained latest value; rung-9 cell semantics) at `dev/thermal0/sample`.
- Value = **an nsl `Array` of 64 SMIs**, row-major (`index = row*8 + col`), each a
  temperature in **centi-°C** (0.01 °C units; 21.4 °C → `2140`). Centi-°C in an SMI
  covers ±327 °C, so the contract generalizes beyond Grid-EYE.

Why an `Array` of SMIs and **not** a packed 128-byte `RawBytes`: extracting int16s from
bytes in nsl is exactly the L2 binary-data library (tuvm#28), which is *orthogonal* to
this spike's question. An `Array` reads with plain `at:` — **zero new binary-data
machinery** — keeping the probe focused on coordination. When tuvm#28 lands (the sensor
seam / Spec 2) we can switch to a packed form if memory ever matters (it will not at 64
elements — ~256 B). The **shape** stays: 64 values, centi-°C, row-major — so the sensor
seam is a drop-in that posts the same contract the dummy does.

For spike #1 the frame is **nsl-seeded**: a small nsl routine writes a synthetic
gradient/hot-spot `Array` and `spacePut`s it to the cell. "Dummy → real" later is purely
*who last wrote the cell* (nsl seed vs C sensor pump) — renderer unchanged.

## Render — nsl folds the frame into a page

All nsl, all integer arithmetic + `printString` (both present in the port). Two passes
over the 64-element `Array`:

1. **Reduce** — fold for `max`, `min`, hot-spot `(row,col)`. Feeds a caption; shows
   on-node coordinator judgment cheaply, with no extra machinery.
2. **Color-map + emit SVG** — fixed display window `[Tlo,Thi]` (spike: 15–35 °C =
   `1500..3500`; a `ctl` knob later). Linear ramp blue→red, emitted as **decimal
   `rgb()`** so there is **no hex formatting** to write:

```
k    = clamp255( (T - Tlo) * 255 / (Thi - Tlo) )        "one linear map per pixel"
fill = 'rgb(' , k printString , ',0,' , (255 - k) printString , ')'
rect = '<rect x=' , col , ' y=' , row , " width=1 height=1 fill='" , fill , "'/>"
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
"page containing an image" into a single text reply the nsl handler builds alone: one
route, one response, no `<img>`, no second request, no C-side encoder.

*Boundary noted:* nsl building the whole ~2 KB response string in the VM heap is fine
here (small, short-lived, replaced per request — **not** a G3 "big long-lived buffer").
That approach stops scaling only for genuinely large / streamed responses — which is
what the streaming spike (#6) is for, not this one.

## HTTP scope — deliberately minimal

- **HTTP/1.0, `Connection: close`** — one request per connection. No keep-alive, TLS,
  chunked encoding, or compression.
- C parses **only the request line** (`GET /path HTTP/1.0`); reads-and-discards headers
  to the blank line; **GET only** (method passed in the record, so nsl can answer 405 if
  it chooses).
- Routes are **nsl's** decision: `/hello` (milestone 1a), `/` & `/thermal` → the page,
  anything else → 404.
- Enough for a browser to render the page — nothing more.

## Internal staging

- **1a — `GET /hello`** → nsl handler returns a string. Proves the round-trip
  (request half + reply half + correlation) with a trivial payload.
- **1b — `GET /thermal`** → nsl peeks the dummy `dev/thermal0/sample` cell, renders SVG.
  Ships the node.

## New surface this spike introduces

- **Seam / prims.** A `platform httpListen: port` prim that starts the accept thread and
  wires it to post `dev/http/request`; the reply path reuses existing `spacePut:`. The
  **output-driver consumer** of `dev/http/reply/*` is the one genuinely new mechanism
  (see the reply half) — its exact registration form (a general "C reaction" vs a prefix
  hook) is an implementation-plan choice; the *model concept* is "an output driver
  consumes a channel."
- **Backends.** `backend/posix/http_posix.c` (host, real localhost sockets — mirrors
  `udp_posix.c`) and later `backend/zephyr/http_zephyr.c` (`zsock` listener). One impl
  linked per `OEVM_TARGET`, following the existing seam pattern.

## Testing — the model verdict needs zero hardware

- `native_sim` + `http_posix.c` listening on `localhost:8080`, running the reactor + an
  nsl node that seeds the dummy cell and registers the request handler.
- Host `curl` tests:
  - `/hello` → 200 body (round-trip, 1a);
  - `/thermal` → 200 SVG heat-grid (ships the node, 1b);
  - `/nope` → 404 (nsl routing);
  - **N+1 concurrent requests → the extra gets 503** (bounded in-flight / G2
    back-pressure — the correlation + shed test).
- **`review/model-capability-findings.md` spike-1 entry written from this host run** —
  the model verdict does not depend on hardware.
- **ESP32 + WiFi last mile** (after the verdict): `backend/zephyr/http_zephyr.c`, an
  esp32 board overlay, SSID/PSK via Kconfig (`prj_esp32` WiFi overlay). Point a browser
  at the node's IP. WiFi association risk is orthogonal to the model and deliberately
  isolated here. (ESP32-WROOM-32 has no 802.15.4 — its network is WiFi, so this does
  **not** use the nRF OpenThread path.)

## Reliability rules honored

- **G2** — bounded in-flight table; (N+1)th request shed as 503; request mailbox is
  ISR/thread-safe like the pulse mailbox.
- **G3** — in-flight request buffers live in a C arena, never the VM heap; the frame is
  a small `Array`; the ~2 KB response string is short-lived (see boundary note).
- **G9** — no unrequested periodic work: the server acts only on inbound requests; the
  reactor idles in WFI between them.

## Status

- Design: **approved 2026-07-18 (David).**
- Next: implementation plan (writing-plans), then build 1a → 1b on the host, write the
  findings entry, then the ESP32/WiFi last mile.
