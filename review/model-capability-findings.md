# Model-capability findings — coordination-model spike series

Verdicts from the coordination-model spike series: can the run-to-completion
channel model (nodes + `whenever:` reactions over a channel space, C-owned
datapaths at the `pumpEvents` safe point) express the coordination patterns a
real device program needs — and, for each, is the fit **clean**, does it
**need-a-helper**, or is it a **corner** (expressible only awkwardly)?

- Series design: `~/workspace/tuvm/docs/design/2026-07-18-coordination-model-spike-series-design.md`
- Rubric per spike: (a) expressible? (b) the idiom (the actual reactions/prims),
  (c) verdict per sub-pattern, (d) helper needed, (e) rough cost.

---

## Spike #1 — Webserver channel-device (request/reply / RPC over channels)

- Design: `~/workspace/tuvm/docs/design/2026-07-18-webserver-channel-device-spike-design.md`
- Plan: `~/workspace/tuvm/docs/superpowers/plans/2026-07-18-webserver-channel-device-spike.md`
- Landed: driver (`src/http.c`, `src/backend/posix/http_posix.c`, 3 platform
  prims), node (`~/workspace/tuvm/zephyr/HttpNode.nsl`, `HttpServerNode.nsl`),
  conformance twins (`~/workspace/nsl-tests/14_http/0{0,1,2,3}_*.nsl`), C unit
  test (`tests/test_http.c`), curl integration (`tests/http_roundtrip_host.sh`).

### (a) Expressible? — YES, cleanly.

Request/reply (the RPC shape) decomposed into three pieces the model already
has, with **no channel reification** — no "reply channel" object, no
continuation, no blocking call was introduced:

1. **Ingress = the pulse seam.** C owns TCP + HTTP framing on an accept thread;
   it copies request *bytes* into a bounded ring and, at the `pumpEvents` safe
   point, `http_pump` builds a `[id, method, path]` Array and `space_post`s it to
   `dev/http/request`. Identical shape to the ISR→mailbox→channel pulse driver.
2. **Egress = the NetBridge idiom.** A plain `whenever:` reaction on
   `dev/http/reply` forwards the reply Record to a send-prim
   `platform httpRespondId:status:body:`. No new mechanism.
3. **Correlation = an opaque SMI id.** C mints the id in the in-flight table
   (`{id → socket fd}`), the id rides through the request Array (slot 1), the
   node echoes it into the reply Record (`#id`), and the egress prim hands it back
   to C, which looks up the fd. **The model never touches the socket** — it
   carries an opaque integer. This is what lets request/reply avoid reifying a
   reply channel.

### (b) The idiom (verbatim from the node)

App reaction — route + build the reply Record (id carried through):

```
req whenever: '' do: [ :r |
  | id path status body |
  id::   r basicAt: 1.
  path:: r basicAt: 3.
  (path = '/hello') ifTrue: [ status:: 200. body:: 'hello' ]
                    ifFalse: [ ... /thermal ... else 404 ].
  reply out: '' value: (platform Record tag: #response
                          with: #id -> id with: #status -> status with: #body -> body) ].
```

Egress reaction — forward the reply to the send-prim (the whole reply half):

```
reply whenever: '' do: [ :resp |
  platform httpRespondId: (resp at: #id ifAbsent: [ 0 ])
           status:        (resp at: #status ifAbsent: [ 500 ])
           body:          (resp at: #body ifAbsent: [ '' ]) ].
```

### (c) Verdict per sub-pattern

| Sub-pattern | Verdict |
|---|---|
| Request half (ingress → channel) | **clean** — is the pulse seam, unchanged |
| Reply half (channel → send-prim) | **clean** — is the NetBridge egress idiom |
| Correlation id (reply → socket) | **clean** — opaque SMI in a C in-flight table; no channel reification |
| Back-pressure (G2 shed) | **clean** — bounded in-flight table, (N+1)th → 503 |

No sub-pattern needed a corner or a new VM mechanism.

### (d) Helpers / idiom sharp-edges surfaced

Two are **language-idiom** findings (the `00_idioms` probe + implementation),
one is a **C/model-boundary capacity** lesson. None blocked the pattern.

1. **`\\` modulo is not a binary selector in this dialect** — `3120 \\ 100`
   fails to parse ("Expected … `/` `*` `-` `+`"). Integer division `//` *is*
   present. Workaround used in `centi:`: `tenth := (v - (v // 100) * 100) // 10`.
   → idiom note for the ladder; a `\\`/`rem:` selector would remove the wart.
2. **Channel payloads must be algebra values — a mutable `Array` does not cross.**
   `Array new: 64` populated with `at:put:` is a workbench, not a value; putting
   it in a cell crashes `not a value` (errors.md boundary). The 8×8 frame had to
   be built as an algebra **List** (`platform List fromCollection: oc`), the value
   sequence type. This is *by design* (the value boundary is the model's safety
   guarantee) but is a real authoring gotcha: nodes build values, not collections,
   for anything that crosses the seam. String `=` is value-equality (no `sameAs:`
   needed) and `at:`/`at:put:` on the workbench are fine.
3. **Stream large egress bodies; don't stage them in a bounded C buffer.** The
   naive per-`<rect>` SVG for an 8×8 grid is ~4.1 KB and overflowed a fixed 4 KB
   response scratch buffer (→ 500). Fixed by splitting `emit()` into a small
   framed **header** write + a **direct body write** streamed from the VM String
   — no response-sized C buffer, so node-generated bodies may be arbitrarily
   large. Aligns better with G3 (less C buffering) than the original design's
   `HTTP_RESP_MAX`.
4. **A helper method does not see `main:args:`'s parameters.** `platform` is a
   *parameter* of `main:args:`, not an instance slot — inside a separate method a
   bare `platform` is an implicit-receiver send `self platform`, which DNUs and
   aborts the VM. Reaction blocks close over `main`'s `platform` lexically (so
   inline reactions are fine), but any method they call must take it explicitly
   (`statsPageOn: platform`). Surfaced adding the `/stats` route
   (`platform metrics`); `renderFrame:`/`caption:` never tripped it because they
   touch only their argument and arithmetic.

### (e) Rough cost

- **VM core:** none. No `builtins.def` touched, no `BUILTIN_REGISTRY_VERSION`
  bump, no policy added to `src/space.c`.
- **Prims:** 3 (`httpListen:`, `httpRespondId:status:body:`, `httpInject:path:`)
  + one `http_pump()` hook on `PRIM_pf_pumpEvents`.
- **C:** ~230 lines pure backend (framing + ring + bounded in-flight + posix
  accept thread + mutex), ~40 lines VM glue (`src/http.c`).
- **nsl:** ~90-line node (routing + on-node reduce/SVG render), shared verbatim
  by the conformance test and the live server twin.

### Evidence

- `~/workspace/nsl-tests/14_http/01_hello_roundtrip.nsl` — request/reply round
  trip over channels (framed 200 to stdout).
- `…/02_thermal_render.nsl` — on-node reduce (max/min/hot-pixel) + SVG render of
  the `dev/thermal0/sample` List frame; routing 200 / 404.
- `…/03_backpressure.nsl` + `tests/test_http.c::test_backpressure_shed` — G2 shed
  deterministic.
- `tests/http_roundtrip_host.sh` — real posix sockets: `curl /thermal` → 200 with
  an `rgb(` heat-grid, `/nope` → 404, 8-way concurrent burst → mix of 200 and 503
  (G2 shed under genuine load).

### Bottom line

Request/reply over the run-to-completion channel model is a **clean fit**: both
halves are existing idioms (pulse ingress + NetBridge egress) glued by an opaque
correlation id, with **no channel reification and no VM-core change**. The
friction is entirely in language-idiom ergonomics (no `\\`; build **values** not
mutable Arrays for the seam) and one egress-datapath capacity lesson (stream the
body). ESP32/WiFi bring-up (`src/backend/zephyr/http_zephyr.c`) is the deferred
last mile.
