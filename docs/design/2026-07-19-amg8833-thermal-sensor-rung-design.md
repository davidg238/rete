# AMG8833 Grid-EYE thermal sensor rung — design

*Written 2026-07-19. Arc 1 (demonstrable node / driver ladder) of the 2026-07-17 four-arc
sequence (`review/program-sequence.md`). Supersedes the BME280-first ordering in
`review/zephyr-driver-exposure.md`: BME280 is dropped (no module on hand), so **AMG8833
becomes the first device** and writes the Tier-1 `sensor`-API seam fresh. The TFT
character-grid (Tier 2) is the **next** rung, its own spec.*

Owner doc this promotes: `review/zephyr-driver-exposure.md` (the pre-bring-up sketch).
North star this shrinks the gap to: `review/demo-node.ns` `TempNode`.

---

## 1. Goal

Replace the **faked** 64-pixel frame in `zephyr/HttpServerNode.nsl` (lines 20–23, every
pixel hard-coded to 2200 centi-°C except pixel 30 = 3120) with a **real Panasonic AMG8833
Grid-EYE** read through **Zephyr's uniform `sensor` API**, sampled on a `ctl`-requested
cadence (default 2 fps), publishing onto the *same* `dev/thermal0/sample` cell the
`/thermal` SVG render already consumes — **live over WiFi on the ESP32 WROOM-32**.

The bar: open `http://<esp32>/thermal` in a browser and see a live, changing 8×8 heat-grid;
wave a hand across the sensor and watch the hot pixels move.

### What is reused unchanged

The `dev/thermal0/sample` channel, the SVG heat-grid render (`renderFrame:`/`caption:`),
the centi-°C integer contract, the chunked HTTP egress (streaming-render rung), the
timer→pulse→reactor path (rung 8), the pulse/G2 ISR seam.

### What is new

A thin Tier-1 sensor-frame prim over the uniform `sensor` API; a self-contained
`ThermalNode` sensor service; a generic route-accepting `HttpServer` service; a stateless
`ThermalView` presenter; a thin composition-root node; a `dev/thermal0/ctl` channel; an
ESP32 devicetree overlay; and (phase 2) the threshold `event`.

## 2. Decisions locked in brainstorming

1. **Target: ESP32 WROOM-32, feeding the live `/thermal` HTTP demo.** The convergence
   point — channel, consumer, and render already exist; we make the data real.
2. **Least-work, driver-first (David):** go through Zephyr's `sensor` API
   (`sensor_sample_fetch` + `sensor_channel_get`), *not* raw I2C. Consequence: the amg88xx
   driver hands us cooked per-pixel `struct sensor_value`s, so the mixed-width binary
   decode that **tuvm#28** (binary-data library) was meant to solve is handled *inside the
   Zephyr driver*. **AMG8833 via the sensor API does not force tuvm#28.**
3. **Frame shape: a 64-element `List` of centidegree SMIs** (`val1*100 + val2/10000`).
   Matches the existing consumer byte-for-byte, keeps the datapath in C (WHY.md), and dodges
   the Float-arena boxing that OOM'd perfstex `floatMath` on this exact no-PSRAM chip. No
   Float, no LargeInteger on the pixel path.
4. **Sampling model: `ctl`-cadence sampler job** (approach A). A `k_timer` (the existing
   `timer_zephyr.c` seam) fires at the `ctl` rate → pulse token → at the reactor safe point
   an nsl reaction calls the frame prim and `put`s onto the cell. No new thread; reuses the
   rung-8 timer→pulse→reactor path end to end. G9-honest: the cadence exists only because a
   `ctl` record asked for it; CPU idles in WFI between ticks.
5. **Phase 2 (threshold event) stays in this spec** — cheap, both halves already exist.
6. **Composition: three decoupled units wired by a thin composition root** (David's
   Option 2+, refined). `ThermalNode` (produce), `HttpServer` (transport), `ThermalView`
   (present) never reference each other; producer and consumer meet only at the channel name
   `dev/thermal0/sample`. See §4.
7. **Precursor (separate micro-change, landed first): nil-defaulting cell reads.** Add
   `Channel>>peek` (unary — this channel's own cell) and `Channel>>peek: rel` (a sub-cell),
   each `= ( ^ self peek: rel ifAbsent: [ nil ] )`, leaving `peek:ifAbsent:` untouched for the
   never-written-vs-nil case. Distinct selectors (arity is part of the name — no overload
   conflict). Lands as its own kernel commit + both-paths gate *before* this rung, so cell
   reads here use the clean bare `peek`. `take`/`out` have the same latent asymmetry — noted,
   not widened now.

## 3. Components

| Unit | Lives in | Purpose |
|---|---|---|
| **`sensorFrame` prim** | `src/sensor_backend.h` + `src/backend/{posix,zephyr}/sensor_*.c` | Fetch 64 pixels via the sensor API, reduce each to a centi-°C SMI, return a fresh 64-element `List` — or `nil` if the device is not ready. Datapath in C. |
| **`sensorArmEvent:` prim** (phase 2) | same seam | Register the driver's threshold trigger; its handler `pulse_post_isr`s onto the given channel. |
| **`ThermalNode`** | nested class in `zephyr/ThermalHttpNode.nsl` (see §4a) | The sensor service. Owns `dev/thermal0/{sample,ctl,event}`; arms the cadence timer; publishes frames. Knows nothing about HTTP. Modeled on demo-node `TempNode`, but Tier-1-simpler (no register code). |
| **`HttpServer`** | nested class in `zephyr/ThermalHttpNode.nsl` (lifted from `HttpServerNode.nsl`) | Generic transport: a `path → block` route table, `listen:`, request dispatch over the existing chunked prims. Knows nothing about sensors or presentation. |
| **`ThermalView`** | nested class in `zephyr/ThermalHttpNode.nsl` (stateless) | Presenter: `renderFrame: f toId: id on: platform` walks the 64-int frame into per-row SVG `<rect>` chunks; plus `caption:`/`centi:`. Lifted from today's `HttpServerNode.nsl`. The seam the TFT rung reuses (a second view of the same frame). |
| **composition root** | `zephyr/ThermalHttpNode.nsl` (new, the baked boot node — the module declaration) | Top-level class nesting the three above. `main:` installs them, wires each route (peek+view for `/thermal`; `self`-send helpers for `/stats`,`/`), runs the one reactor loop. Holds the `/stats`+`/` presentation helpers. |
| **`dev/thermal0/sample`** | channel (cell) | Latest frame out. *Consumer unchanged.* |
| **`dev/thermal0/ctl`** | channel (cell) | Config in: `{rate: N}` fps, `{on: false}` to stop sampling. |
| **`dev/thermal0/event`** | channel (stream, phase 2) | Threshold alerts via the driver trigger → pulse seam. |

## 4. Composition & decoupling

Three decoupled units — **produce** (`ThermalNode`), **transport** (`HttpServer`),
**present** (`ThermalView`) — wired by a thin composition root. Only one `main:` runs per image:

```
public main: platform args: args = (
  | space thermal tview http |
  space::   platform space.
  thermal:: ThermalNode onSpace: space seam: platform.   thermal start.   "publishes dev/thermal0/sample @2fps"
  tview::   ThermalView new.
  http::    HttpServer  onSpace: space seam: platform.
  http route: '/thermal' do: [ :id :plat |
      tview renderFrame: ((space channel: 'dev/thermal0/sample') peek) toId: id on: plat ].
  http route: '/stats' do: [ :id :plat | self showStatsTo: id on: plat ].
  http route: '/'      do: [ :id :plat | self showIndexTo: id on: plat ].
  http listen: 18080.
  [ true ] whileTrue: [ platform pumpEvents. space drain ].
)
```

- **No service references another.** `HttpServer` holds opaque route blocks; `ThermalNode`
  publishes to a channel; `ThermalView` is a stateless presenter. The only coupling — the
  channel name `dev/thermal0/sample` — lives in the composition root, where coupling belongs.
  All three become reusable, and `ThermalView` is the seam the **next rung** reuses: the TFT
  character-grid is just a second view of the same frame (`ThermalGridView renderFrame:…`).
- **The reified-block rule (precise).** A `whenever:`/route block is a genuine reified
  `BlockClosure` stored by the reactor and fired later. Inside it, **naming a module-level
  class directly** (`ThermalView`, `Cbor new`, `OrderedCollection new`) aborts — the class
  reference doesn't resolve its outer/module context (`LD_SLOT` on a non-object receiver;
  `kernel.nsl:1155` NetBridge note). What is **fine**: sending to captured objects/temps/ivars
  (`tview renderFrame:…`, `space channel:…`, `thermal peek:…` — the last is today's shipping
  `HttpServerNode` block). So the pattern is: **construct the view once in `main:`
  (`tview:: ThermalView new`, class resolved in method-body context), capture it, and the block
  just sends to the captured instance.** No `self`-forwarder needed for `/thermal`. Routes that
  *must* name a class in their body (e.g. `/stats` building the metrics table) use a `self`-send
  helper instead (`self showStatsTo:on:`), where class names resolve normally.
- **`ThermalView` (stateless presenter).** `renderFrame: f toId: id on: platform` — walks the
  64-int frame into per-row SVG `<rect>` chunks via `platform httpChunkId:`; plus `caption:`/
  `centi:`. Lifted verbatim from today's `HttpServerNode.nsl` render methods. The `/stats` and
  `/` presentation stays on the composition root as `self`-send helpers (they name classes /
  read `platform metrics`).
## 4a. Module layout — why the three classes are NESTED (spike finding, 2026-07-19)

The three services are **nested classes inside the one boot module**, not separate `.nsl`
files. This corrects an earlier assumption in this spec and is grounded in a spike:

- **The bake is single-app-file.** `ns_compile_oevm.py` compiles ONE app file, then appends
  `lib/kernel.nsl` and `lib/supervisor.nsl` as top-level siblings (`ast["body"].extend(...)`).
  There is no multi-app-file composition.
- **Appending a library file makes it compiled-in but UNREACHABLE.** Spike: a throwaway
  `lib/spike_hello.nsl` (top-level `HelloService`) was injected as a third sibling; a node
  calling `HelloService usingPlatform: platform` died on **both** paths with
  `Message 'HelloService' not understood by receiver <platform>`. The compiler's
  `resolve_name` routes an unbound capitalised name to `platform Name` (step 4), and
  `Kernel` reaches its own classes only because they are **nested inside it** with an
  explicit accessor each (`public NetBridge = ( ^ NetBridge )`). A sibling has no accessor.
- **That routing is deliberate, not a gap.** The compiler comments pin it: route through
  "the handed `platform` capability … **never a VM global (capability-pure)**", and Kernel
  classes are siblings precisely "so `resolve_class_ref` never resolves them in app scope".
  Making separate-file modules directly nameable would reintroduce **ambient globals** —
  what the capability model exists to prevent.
- **Nesting is the Newspeak idiom** for a module's own classes (`DemoNode`/`TempNode`,
  `DeltaBlueNS`/its helpers). Cross-module reuse is by *injection*, not naming.
- **Promotion path (costs nothing later).** Because composition is already by injection,
  extracting a nested class into an injected module later changes only where it lives and
  how it is obtained (`ThermalNode` → `manifest ThermalNode`); the `main:` wiring is
  unchanged. Filed as its own rung — see §11.

## 4b. Standalone boot

`ThermalNode` is reusable as-is for a sensor-only node (the nRF52840 fleet-node story:
publish over Thread, no HTTP) by nesting it in a different boot module with a different
`main:`. One class, two deployments (demo-node principle) — but note that today that means
the class is *copied or promoted*, not shared, until the manifest rung lands.

## 5. Data flow & freshness

```
boot → composition root installs ThermalNode + HttpServer
ThermalNode start → ctl whenever: reconfigure; arm every:500ms #tick; (phase2) arm event trigger
timer ISR → pulse token → safe point → sample whenever 'tick':
    frame ← seam sensorFrame          (C: sensor_sample_fetch + 64× channel_get → centi-int List, or nil)
    frame == nil ifFalse: [ dev/thermal0/sample put: frame ]   (keep last good frame on failure)
browser GET /thermal → HttpServer dispatch → route block → peek → renderFrame: → chunked SVG
```

**Freshness of a peek** (single-threaded run-to-completion; tick and request never preempt):
worst-case age = **sampling interval (500 ms) + reactor jitter + sensor internal latency**.
The 500 ms term dominates and is a `ctl` knob (`{rate:10}` → ≤100 ms). Reactor jitter is a
few ms on ESP32 (a render never yields mid-flight); the AMG8833 internal frame rate (1/10 fps)
adds ≤100 ms. Not a hard real-time bound; typically well under a second.

**Cell payoff:** `peek` is non-destructive and the HTTP path never samples, so N concurrent
browsers share one frame at a fixed 2 fps sensor cost — producer cadence fully decoupled from
consumer rate (rung-9 cell vs stream).

## 6. The C binding (Tier-1 sensor seam)

`sensorFrame`:
1. `dev = DEVICE_DT_GET(DT_NODELABEL(amg88xx))` (resolved once; `device_is_ready` guard → `nil`).
2. `sensor_sample_fetch(dev)` (latches a frame; non-zero → `nil`).
3. Loop 64× `sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &v)` — the amg88xx driver
   yields all 64 pixels on this channel; reduce each to `v.val1*100 + v.val2/10000` centi-°C.
4. Return a fresh 64-element `List` of SMIs.

- **Thin and already generic** (the API is uniform); we do **not** build the
  auto-enumerate-all-devicetree-sensors catalog harness — that is the documented
  generalization path (deferred; §9).
- **GC:** the in-progress `List` is temp-rooted across the 64-pixel fill (the UDP-rung
  "GC temp-rooting confirmed" discipline).
- **posix backend** (`sensor_posix.c`): no hardware → returns a deterministic synthetic
  frame (a uniform field + one hot pixel — the pattern the faked loop used), so host
  round-trip tests stay real and the node code is byte-identical host↔device.
- **zephyr backend** (`sensor_zephyr.c`): the real sensor-API calls above.

Obeys the fflush lesson: **core never reaches around `backend/<target>/`** — the frame prim
is a seam (`sensor_backend.h`), not a direct sensor-API call from `dispatch.c`.

## 7. Board wiring (ESP32 WROOM-32)

- `zephyr/boards/esp32_devkitc.overlay` (or equivalent): `panasonic,amg88xx` on `i2c0`,
  `reg = <0x69>` (AMG8833 default; 0x68 if AD_SELECT low), plus the **required** `int-gpios`
  phandle (the DT binding marks `int-gpios` `required: true` even for the no-trigger build).
- `CONFIG_AMG88XX=y` (auto-selects `CONFIG_I2C`). Phase 1: `CONFIG_AMG88XX_TRIGGER_NONE`
  (INT pin declared but unused). Phase 2: `CONFIG_AMG88XX_TRIGGER_GLOBAL_THREAD` (no extra
  stack).
- **I2C pins:** WROOM-32 default `i2c0` SDA/SCL per the board devicetree; documented in the
  plan for the physical wiring.
- **RAM:** WROOM-32 already runs tight (VM heap + WiFi + net-bufs). The amg88xx driver adds an
  I2C driver and **no framebuffer** (64 px handled per-fetch), so cost is small. This rung
  re-measures `/stats` heap high-water on device to confirm headroom.
- **Config split:** `prj_esp32.conf` is WiFi-laden for all esp32 nodes; the thermal+HTTP node
  needs both WiFi and I2C. The pre-filed "fold WiFi out of the shared conf" follow-up is
  adjacent but out of scope here.

## 8. Phase 2 — threshold event

After phase 1 is green on device:
- `CONFIG_AMG88XX_TRIGGER_GLOBAL_THREAD`; set an upper threshold via `sensor_attr_set`
  (`SENSOR_ATTR_UPPER_THRESH`), register the driver's threshold trigger.
- The Zephyr-side handler calls `pulse_post_isr` onto `dev/thermal0/event` — the exact
  GPIO/timer ISR path `pulse_backend.h` already sanctions.
- An nsl `event whenever: '' do: [...]` reaction logs / serves a hot-spot alert.
- **G2 residue acknowledged:** `pulse_post_isr` still `k_oops()`es on a full mailbox
  (tuvm#34); a sustained event burst could kill the node. Not fixed here (that is Arc-3 P0
  hardening); the demo's threshold fires rarely.

## 9. Error handling

- Device not ready / `sensor_sample_fetch` failure → `sensorFrame` returns `nil`; the sampler
  skips the `put` (keeps the last good frame); `/thermal` already renders "no frame" for a
  `nil` cell. No crash, no partial frame ever on the cell.
- Malformed `ctl` (missing/zero/negative rate) → clamp to a sane floor / ignore; current
  cadence stands (untrusted-input-at-boundary discipline).
- `{on: false}` → `cancel:` the held timer token (rung-8 discipline — a returning node must
  not leak a live repeater); `{on: true}`/`{rate:N}` re-arms.

## 10. Testing & gates

- **Host (posix synthetic):** existing `tests/http_roundtrip_host.sh` stays green — `/thermal`
  renders the synthetic frame *through the real prim* (proves the seam without hardware).
  Suite floor unmoved (this is node/backend code, not VM-core; no `CLAIMS.md` row moves).
- **native_sim:** build-links the zephyr sensor backend (link/compile gate for the sensor-API
  path; emulated bus if available).
- **On-device (ESP32 + real AMG8833):** `/thermal` shows a live, changing heat-grid in a
  browser; hand-wave moves the hot pixels. Bank the green log + a `/stats` heap-high-water
  line to `zephyr/docs/results.md` (the on-device ledger, E-series shape). No `CLAIMS.md` row
  (manual device gate) — matching the Model-B / #46 precedent.
- **Phase 2 on-device:** cross the threshold (warm object) → an `event` alert observed.

## 11. Scope / non-goals

**In:** phase-1 sampler + live `/thermal` on ESP32; phase-2 threshold event; the thin
sensor-API prim + posix synthetic twin; the `ThermalNode`/`HttpServer` service split + thin
composition root; the ESP32 overlay.

**Out (own rungs / deferred):**
- **Manifest-based module injection** — the capability-pure way to get reusable service
  modules in separate files on disk (`manifest HttpServer`), instead of nesting or
  kernel-bloat. The `packageUsing: manifest` parameter every node already declares is
  **currently vestigial** (no node body references it), which is exactly the hook this
  would build out: boot constructs a manifest exposing the app's library modules. Sized by
  the §4a spike as a genuine feature (boot-path + toolchain + manifest construction), not a
  micro-change. The driver ladder (BME280 → AMG8833 → TFT → …) is the pressure that
  justifies it. **Filed as its own rung.**
- TFT character-grid display (Tier 2) — the **next** rung, its own spec.
- Auto-enumeration catalog machinery (`dev/<node>/` per devicetree sensor) — generalize when
  a second sensor lands.
- tuvm#28 binary-data library — unforced via the sensor API.
- tuvm#34 (ISR-side mailbox `k_oops`) — Arc-3 P0 hardening.
- Fold WiFi out of shared `prj_esp32.conf` — pre-filed follow-up.

## 12. Close-out (promotion targets)

Per CLAUDE.md's promotion rule, on close: fold the durable knowledge into
`review/zephyr-driver-exposure.md` (the Tier-1 seam is now built, not sketched — record the
sensor-API-means-no-tuvm#28 finding and the service-composition pattern) and add the E-series
device entry to `zephyr/docs/results.md`. Note in the ladder that the demo-node gap shrank
(TempNode realized on real hardware, Tier-1 variant).
