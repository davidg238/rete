# Exposing Zephyr drivers to nsl — a sketch

*Written 2026-07-11. Pre-Zephyr-bring-up sketch, not a spec: feeds the rung-2 (Zephyr
seam) design when it starts. Companion to `WHY.md` ("reuse a Zephyr display driver —
don't write one") and `reliability-gaps.md` G2/G3/G9, whose rules it inherits.*

---

## The shape: a device is a granted channel prefix

The capability model already gives the answer's outline. A Zephyr device (a devicetree
node) surfaces in nsl as **a channel namespace rooted at a prefix** — e.g.
`dev/env0/`, `dev/display/` — granted by the supervisor to the jobs that may use it.
No new concept: drivers are capabilities, attenuable by prefix extension
(`dev/display/` → `dev/display/backlight/`), never ambient globals.

Under every prefix, the same small vocabulary:

| channel | direction | meaning |
|---|---|---|
| `dev/<n>/sample` | driver → nsl | readings, as immutable records |
| `dev/<n>/event`  | driver → nsl | edges/alerts via the ISR pulse seam (G2 ring, coalesce) |
| `dev/<n>/ctl`    | nsl → driver | config as records: `{rate: …}`, `{mode: …}` |

The grain rules are already law elsewhere; this design just obeys them:
- **control plane crosses the seam as immutable records; data plane stays in C**
  (WHY.md: "the language is the coordinator; C is the datapath");
- **big long-lived buffers live in C arenas, never the VM heap** (G3 rule);
- **ISRs never touch the VM** — fixed ring of small tokens, per-source coalesce/drop
  policy (G2);
- **no unrequested periodic work** — sampling cadence is user-requested config via
  `ctl`, so the power budget stays honest (G9).

## Three exposure tiers

**Tier 1 — uniform API families: the catalog play.** Zephyr's whole value is that
hundreds of drivers hide behind a handful of uniform APIs (`sensor`, `gpio`, `pwm`,
`led`, `display`). So we bind the *API family*, not the device: **one C binding for
`sensor_sample_fetch`/`sensor_channel_get` exposes every sensor Zephyr supports** —
enumerated from devicetree at boot, each as its own `dev/<node>/` prefix. This is the
product thesis made mechanical: write one seam module, inherit a catalog. Expected to
cover the long tail of environmental/IMU/light/battery parts with zero per-device
code.

**Tier 2 — subsystems with their own machinery (display: a character-grid UI).**
Decision 2026-07-11: **LVGL is dropped** — its RAM appetite (widget heap + working
buffers) and its own timer/event loop are exactly the kind of foreign machinery the
reactor model exists to avoid, and simple nodes don't need it. What simple nodes *do*
need is an "ascii-art" UI: a character grid on a small panel (128×128 TFT class),
enough for labels, values, and block-glyph bars/tiles.

The C-side driver job owns the datapath (SPI, per-glyph blitting); nsl speaks
characters, never pixels:

- `dev/display/lines` ← a list of strings — repaint the grid (a 16×16 char grid on
  128×128 with an 8×8 font);
- `dev/display/at` ← `{row:, col:, text:}` — the high-frequency path is a tiny record;
- `dev/display/ctl` ← `{contrast: …}`, `{on: false}` (sleep the panel — G9).

Renderer choice is an implementation detail behind that contract: Zephyr's **CFB**
character-framebuffer subsystem (`subsys/fb/cfb`, in-tree, font support) where the
panel's pixel format allows, else a ~1 KB 8×8 glyph blitter in the driver job —
per-glyph blitting also avoids a full framebuffer (128×128×16bpp would be 32 KB;
G3 says no such buffer on the VM heap, and per-glyph needs none at all). Zephyr has
in-tree drivers for the usual 128×128 SPI panels (ST7735R class).

**Tier 3 — device-specific escape hatch.** For what the uniform APIs don't cover: a
thin custom prim + the same channel vocabulary. Should stay rare; every Tier-3 binding
is a small admission the catalog play didn't reach.

## The proposed example ladder

Three examples, one per tier, each earning the next:

1. **BME280/BME680 environmental sensor (Tier 1)** — first, because it proves the
   catalog claim: the binding is to Zephyr's `sensor` API, so the day it works for the
   BME280, it works for hundreds of parts. Records like
   `{t: 21.4, rh: 38, p: 1013, ts: …}` on `dev/env0/sample` at a `ctl`-requested
   cadence. Small, honest, demos the whole loop (grant → sample → channel → job →
   publish over Thread).

2. **Panasonic AMG8833 Grid-EYE thermal array (still Tier 1!)** — the mid-rung, and
   better than first hoped: **Zephyr has a native in-tree driver** — verified in the
   local NCS checkout at `~/ncs/zephyr/drivers/sensor/amg88xx/` (plus the
   `panasonic,amg88xx` devicetree binding, an interrupt/trigger half in
   `amg88xx_trigger.c`, and a sample app). The 8×8 frame comes through the *standard
   sensor API* (`SENSOR_CHAN_AMBIENT_TEMP` yields the 64 pixels), so the Tier-1
   binding written for the BME280 covers it with no custom seam code — the catalog
   play demonstrated on something far richer than a scalar. 64 px ≈ 128 B a frame at
   up to 10 fps: trivially a message. On-node reduction (min/max/hot-spot/zone
   averages as records, frames on request) shows coordinator-vs-datapath judgment,
   and the driver's threshold trigger maps straight onto `dev/thermal0/event` via
   the G2 pulse seam. Presence/occupancy on a coin cell is also simply a good demo.
   *(Pin the driver against the NCS version at bring-up — sdk-nrf **v3.2.4** here,
   which bundles Zephyr **4.2.99**; the amg88xx driver is byte-identical to upstream
   Zephyr v4.2.0, so the ESP32 workspace (`~/zephyr-esp32`) gets the same driver.)*

3. **Character-grid display on a 128×128 TFT (Tier 2)** — the flagship, deliberately
   last: it's the one example that needs a C-side driver *job* (owning SPI + glyph
   blitting) rather than a binding, and it carries the sleep question (panel off =
   `ctl` record; the reactor still idles in WFI between repaints, per G9). Scope is
   deliberately "ascii-art": lines/at/ctl records, block-glyph bars for graphics.
   LVGL is dropped, not deferred — revisit only if a product genuinely demands rich
   UI, and then as its own project.

## Why this ladder

Each rung retires a distinct risk: (1) the seam and the catalog mechanism, (2)
frame-shaped data and trigger pacing through the same Tier-1 binding, (3) the first
C-side driver *job* — an output device with its own datapath under the reactor model.
And each rung is a demo worth showing on its own — no throwaway work. Rungs 1+2
sharing one binding is itself the proof the catalog play works.

## Open questions (for the rung-2 design doc)

- Devicetree → channel-name mapping (node label? alias? stable across image pushes?).
- Enumeration & grant flow: does the supervisor auto-grant `dev/*` to a configured
  job set, or is every device grant explicit in the boot script? (Explicit is more
  capability-honest; a `dev/` manifest record at boot may be the middle path.)
- `ctl` schema versioning — same L6/CBOR discipline as the wire (G7).
- Where Tier-2 driver jobs run: the reactor's thread (a repaint is fast enough to be
  a handler) vs a dedicated Zephyr thread fed by a k_msgq (only if blitting a full
  repaint over SPI proves too long for a run-to-completion budget — measure first).
- CFB vs own blitter for the color-TFT case: CFB targets mono pixel formats; check
  whether the chosen panel/driver combination supports it before writing the ~1 KB
  fallback blitter.
- Per-family record schemas: pin `sensor`-API channel→field naming once, in a
  `lib/dev-records.md`, before the first binding lands (retrofit is churn).
