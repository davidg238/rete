# AMG8833 Grid-EYE Thermal Sensor Rung — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the faked 8×8 frame in the `/thermal` HTTP demo with a real Panasonic AMG8833 Grid-EYE read through Zephyr's uniform `sensor` API, sampled at a `ctl`-requested cadence, served live over WiFi from an ESP32 WROOM-32.

**Architecture:** A thin C seam (`sensor_backend.h`) with posix-synthetic and zephyr-real backends, surfaced as one platform prim `sensorFrame` answering a 64-element Array of centi-°C SMIs. In nsl, three decoupled classes nested in one boot module — `ThermalNode` (samples on a timer, publishes to the `dev/thermal0/sample` cell), `HttpServer` (generic route table), `ThermalView` (frame → SVG chunks) — wired by a thin `main:`. Producer and consumer meet only at the channel name.

**Tech Stack:** C99 (tuvm VM), nsl (Newspeak-derived), Zephyr `sensor` API + devicetree, ESP32 WROOM-32 (`esp32_devkitc/esp32/procpu`), Python/Node bake toolchain.

**Spec:** `~/workspace/tuvm/docs/design/2026-07-19-amg8833-thermal-sensor-rung-design.md`

## Global Constraints

- **Repos:** `~/workspace/tuvm` (C + zephyr nodes), `~/workspace/nsl-tuvm` (kernel + toolchain), `~/workspace/nsl-tests` (suite). Work on branch **`feat/amg8833-thermal`** in each (already created; the `peek` precursor is its first commit).
- **Source extension is `.nsl`** for all live sources. A bare `.ns` means frozen oracle / `review/` / archives — never edit those.
- **Suite gate is Path B only** (`.oimg`) by standing decision: `cd ~/workspace/nsl-tuvm && python3 tools/run_tests_oevm.py` (default = Path B). Current floor: **200 passed / 0 failed / 14 skipped**.
- **Fast inner loop:** `python3 ~/workspace/nsl-tuvm/tools/run-one.py <abs-path-to.nsl>` (runs A+B; `run_tests_oevm.py` ignores argv and globs the whole suite).
- **Pixels are centi-°C integers** (2200 = 22.00 °C). Never box a Float per pixel — the float arena OOMs on the no-PSRAM WROOM-32.
- **Reified-block rule:** inside a `whenever:`/route block, never name a module-level or nested class (`ThermalView`, `Cbor new`, `OrderedCollection new`) — it aborts on outer-scope resolution. Sends to captured objects/temps/ivars and `self`-sends are fine. Construct in a method body, capture the instance, send to the capture.
- **Core must not reach around `backend/<target>/`.** All sensor access goes through `src/sensor_backend.h`.
- **Prim ids are append-only.** Never renumber an existing prim.

---

## File Structure

**Create:**
- `tuvm/src/sensor_backend.h` — the sensor seam interface (frame read + phase-2 trigger arm).
- `tuvm/src/backend/posix/sensor_posix.c` — host synthetic frame (deterministic).
- `tuvm/src/backend/zephyr/sensor_zephyr.c` — real Zephyr `sensor` API + phase-2 trigger.
- `tuvm/zephyr/ThermalHttpNode.nsl` — the boot module; nests `ThermalNode`, `HttpServer`, `ThermalView`; `main:` wires them.
- `tuvm/zephyr/boards/esp32_devkitc.overlay` — `panasonic,amg88xx` on `i2c0`.
- `nsl-tests/18_sensor/01_frame.nsl` — the prim's host gate.
- `nsl-tests/18_sensor/02_thermal_node.nsl` — the sampler's host gate.

**Modify:**
- `tuvm/src/dispatch.c` — prim enum + `PRIM_NAMES` + parity guard.
- `tuvm/src/prim_ids.h` — mirror the new ids for the `.oimg` path.
- `tuvm/src/platform.c` — the two prim cases + `#include "sensor_backend.h"`.
- `tuvm/tools/run_c_tests.sh` (and any sibling host build list) — add `sensor_posix.c`.
- `tuvm/zephyr/CMakeLists.txt` — add `sensor_zephyr.c`.
- `tuvm/zephyr/prj_esp32.conf` — `CONFIG_AMG88XX=y` (+ phase-2 trigger).
- `tuvm/tests/http_roundtrip_host.sh` — bake `ThermalHttpNode.nsl`.

**Retire (Task 6):** `tuvm/zephyr/HttpServerNode.nsl`, `tuvm/zephyr/HttpNode.nsl` — superseded by `ThermalHttpNode.nsl`.

---

## Task 1: The C sensor seam + `sensorFrame` prim (host)

**Files:**
- Create: `tuvm/src/sensor_backend.h`, `tuvm/src/backend/posix/sensor_posix.c`
- Modify: `tuvm/src/dispatch.c`, `tuvm/src/prim_ids.h`, `tuvm/src/platform.c`, `tuvm/tools/run_c_tests.sh`
- Test: `nsl-tests/18_sensor/01_frame.nsl`

**Interfaces:**
- Produces: `int sensor_frame_read(int32_t *out)` fills `out[0..63]` centi-°C, returns 0 on success / non-zero if not ready. `int sensor_arm_event(int chan_id, int handle_idx, int32_t threshold_centi)`. Prim `platform sensorFrame` → 64-slot Array of SMIs, or `nil`. Prim `platform sensorArmEventPost: msg on: pathString threshold: centi` → self. Constant `OEVM_THERMAL_PIXELS` = 64.

- [x] **Step 1: Write the failing test**

Create `nsl-tests/18_sensor/01_frame.nsl`:

```
(*
expect:
size 64
px1 2200
px30 3120
done
*)
(* AMG8833 rung Task 1 — the Tier-1 sensor seam through the uniform Zephyr
   `sensor` API. On the host the posix backend answers a deterministic synthetic
   frame (uniform 22.00 C, one hot pixel at nsl index 30 = 31.20 C), identical to
   the frame the pre-sensor HttpServerNode hard-coded, so the rendered page is
   unchanged by this rung. `platform sensorFrame` answers a RAW indexable Array
   (like heapStats / udp basicReceive), so read it with basicAt:, not the
   Array-class at: — the prim receiver does not carry the Array vtable. *)
class SensorFrame packageUsing: manifest = () (
  public main: platform args: args = (
    | f |
    f:: platform sensorFrame.
    ('size ' , f basicSize printString) out.
    ('px1 '  , (f basicAt: 1)  printString) out.
    ('px30 ' , (f basicAt: 30) printString) out.
    'done' out.
  )
) : (
)
```

- [x] **Step 2: Run the test to verify it fails**

```bash
python3 ~/workspace/nsl-tuvm/tools/run-one.py ~/workspace/nsl-tests/18_sensor/01_frame.nsl
```
Expected: **FAIL** on both paths with `Message 'sensorFrame' not understood` (the prim does not exist yet).

- [x] **Step 3: Create the seam header**

Create `tuvm/src/sensor_backend.h`:

```c
#ifndef OEVM_SENSOR_BACKEND_H
#define OEVM_SENSOR_BACKEND_H
/* The thermal-sensor seam. Exactly one impl is linked per OEVM_TARGET:
   backend/posix/sensor_posix.c (deterministic synthetic frame) for the host gate;
   backend/zephyr/sensor_zephyr.c (Zephyr's uniform `sensor` API against the
   devicetree amg88xx node) on target. Core must never call the sensor API
   directly — that is how a host-only assumption reached the SoC once already
   (the ROM-fflush bug). Values are centi-degrees Celsius (2200 = 22.00 C):
   integers keep every pixel far inside SMI range and avoid boxing a Float per
   pixel, which OOMs the float arena on a no-PSRAM WROOM-32. */
#include <stdint.h>

#define OEVM_THERMAL_PIXELS 64

/* Fill out[0..OEVM_THERMAL_PIXELS-1] with one 8x8 frame in centi-degC.
   Returns 0 on success; non-zero if the device is absent/not ready or the
   fetch failed (the caller then answers nil and keeps the last good frame). */
int sensor_frame_read(int32_t *out);

/* Phase 2: arm the driver's threshold trigger so a crossing posts the pinned
   message on chan_id via the ISR pulse seam — i.e. the handler calls
   pulse_post_isr(chan_id, handle_idx). Both are resolved in thread context by the
   caller (space_intern / handle_new), exactly as gpioPin:edge:post:on: does; an
   ISR must never touch the VM. Returns 0 on success, non-zero if unsupported. */
int sensor_arm_event(int chan_id, int handle_idx, int32_t threshold_centi);

#endif /* OEVM_SENSOR_BACKEND_H */
```

- [x] **Step 4: Create the posix backend**

Create `tuvm/src/backend/posix/sensor_posix.c`:

```c
/* Host sensor backend — no hardware. Answers a deterministic synthetic frame so
   the host gate exercises the REAL prim path (not a nsl-side fake): a uniform
   22.00 C field with one hot pixel at C index 29 (nsl basicAt: 30) = 31.20 C.
   These are exactly the values the pre-sensor HttpServerNode.nsl hard-coded, so
   the rendered /thermal page is byte-identical across this rung. */
#include "../../sensor_backend.h"

int sensor_frame_read(int32_t *out)
{
    for (int i = 0; i < OEVM_THERMAL_PIXELS; i++) out[i] = 2200;
    out[29] = 3120;
    return 0;
}

int sensor_arm_event(int chan_id, int handle_idx, int32_t threshold_centi)
{
    (void)chan_id; (void)handle_idx; (void)threshold_centi;
    return 0;   /* host has no trigger hardware; arming is a no-op */
}
```

- [x] **Step 5: Register the prim ids in `dispatch.c`**

In `tuvm/src/dispatch.c`, in the prim enum immediately after `PRIM_pf_httpEnd,`, add:

```c
    /* AMG8833 thermal rung (2026-07-19): the Tier-1 sensor seam. sensorFrame
       answers a 64-element Array of centi-degC SMIs (or nil when the device is
       not ready); sensorArmEventPost:on:threshold: arms the driver's threshold trigger
       onto a channel via the pulse seam. Appended after httpEnd — no
       pre-existing id shifts. Counted (not computed) to be the 116th/117th
       PRIM_NAMES entries. Platform-dispatched (mirrored in prim_ids.h). */
    PRIM_pf_sensorFrame,
    PRIM_pf_sensorArmEvent,
```

In the `PRIM_NAMES` array, immediately after the line `"httpBeginId:status:", "httpChunkId:data:", "httpEndId:",`, add:

```c
    "sensorFrame", "sensorArmEventPost:on:threshold:",
```

Update the parity guard (it must name the LAST enum member):

```c
typedef char prim_names_enum_parity_check[(N_PRIM_NAMES == PRIM_pf_sensorArmEvent) ? 1 : -1];
```

- [x] **Step 6: Mirror the ids in `prim_ids.h`**

At the end of `tuvm/src/prim_ids.h`, add:

```c
/* AMG8833 thermal rung (2026-07-19): the sensor seam.
     sensorFrame                                 -> 64-slot Array of centi-degC
                                                    SMIs, or nil if not ready.
     sensorArmEventPost: msg on: path threshold: -> arm the driver threshold
                                                    trigger; a crossing posts msg
                                                    on the interned channel path
                                                    via pulse_post_isr. Same
                                                    String-path + handle_new shape
                                                    as gpioPin:edge:post:on:. */
#define PRIM_pf_sensorFrame     116
#define PRIM_pf_sensorArmEvent  117
```

- [x] **Step 7: Implement the prim in `platform.c`**

Add `#include "sensor_backend.h"` with the other seam includes at the top of `tuvm/src/platform.c`. Then add these two cases immediately before the `default:` of the platform prim switch:

```c
    case PRIM_pf_sensorFrame: {
        /* `platform sensorFrame` — one 8x8 thermal frame as a 64-slot Array of
           centi-degC SMIs, or nil when the device is not ready. GC-safety: the
           backend fills a C buffer BEFORE the single allocating call
           (indexable_new); the 64 stores are SMIs (immediates, no allocation), so
           no heap oop is ever held across an allocation and no gc_temp_root pin is
           required — the same shape as heapStats/sysStats above. args are ignored
           (0-arg selector). */
        int32_t px[OEVM_THERMAL_PIXELS];
        if (sensor_frame_read(px) != 0) { *out = NULL; return 1; }   /* not ready -> nil */
        oop r = indexable_new(OEVM_THERMAL_PIXELS);
        for (int i = 0; i < OEVM_THERMAL_PIXELS; i++)
            ((struct ObjectInstance *)r)->slots[i + 1] = TAG_SMI((intptr_t)px[i]);
        *out = r;
        return 1;
    }
    case PRIM_pf_sensorArmEvent: {
        /* sensorArmEventPost: msg on: 'path' threshold: centi — mirrors
           gpioPin:edge:post:on: exactly: the channel arrives as a String PATH
           (interned to a chan_id here, in thread context) and the message oop is
           pinned via an M6 handle, because an ISR must never touch the VM. The
           trigger handler then only calls pulse_post_isr(chan_id, handle_idx). */
        oop path = args[1];
        if (path == NULL || IS_SMI(path) || StringVtable == NULL ||
            obj_vtable(path) != StringVtable) {
            fprintf(stderr, "VM Error: platform sensorArmEventPost:on:threshold: expects a String channel\n");
            abort();
        }
        struct String *p = (struct String *)path;
        int chan_id = space_intern((const char *)p->bytes, p->length);
        int hidx = handle_new(args[0]);                 /* pin the message oop via M6 */
        int32_t thr = IS_SMI(args[2]) ? (int32_t)UNTAG_SMI(args[2]) : 0;
        sensor_arm_event(chan_id, hidx, thr);
        *out = platform_object();
        return 1;
    }
```

- [x] **Step 8: Add the backend to the host build**

Find every host build source list that already names `pulse_posix.c` and add `sensor_posix.c` beside it:

```bash
cd ~/workspace/tuvm && grep -rn 'pulse_posix\.c' tools/ zephyr/ 2>/dev/null
```

In `tools/run_c_tests.sh`, extend the shared SRCS block (the one listing `platform_posix.c serial_posix.c udp_posix.c http_posix.c pulse_posix.c timer_posix.c`) with:

```
  src/backend/posix/sensor_posix.c
```

Add the same path to any other list the grep reports.

- [x] **Step 9: Run the test to verify it passes**

```bash
python3 ~/workspace/nsl-tuvm/tools/run-one.py ~/workspace/nsl-tests/18_sensor/01_frame.nsl
```
Expected: **Path A: PASS / Path B: PASS**.

- [x] **Step 10: Run the full suite**

```bash
cd ~/workspace/nsl-tuvm && python3 tools/run_tests_oevm.py 2>&1 | tail -8
```
Expected: **201 passed, 0 failed, 14 skipped** (floor 200 + this new test), all named gates green.

- [x] **Step 11: Commit**

```bash
cd ~/workspace/tuvm && git add src/sensor_backend.h src/backend/posix/sensor_posix.c \
  src/dispatch.c src/prim_ids.h src/platform.c tools/run_c_tests.sh
git commit -m "feat(sensor): Tier-1 sensor seam + sensorFrame prim (116/117)"
cd ~/workspace/nsl-tests && git add 18_sensor/01_frame.nsl
git commit -m "test(sensor): sensorFrame answers a 64-px centi-degC frame"
```

---

## Task 2: `ThermalView` — the presenter

**Files:**
- Create: `tuvm/zephyr/ThermalHttpNode.nsl` (boot module; this task adds the shell + `ThermalView`)
- Reference: `tuvm/zephyr/HttpServerNode.nsl` (lift `renderFrame:`/`caption:`/`centi:` from it)

**Interfaces:**
- Produces: nested `class ThermalView` with `public renderFrame: f toId: id on: platform`, `caption: f`, `centi: v`. Consumes a **raw indexable** 64-px frame — reads with `basicAt:`, **not** `at:` (the prim receiver carries no Array vtable).

- [ ] **Step 1: Create the boot module with `ThermalView` nested**

Create `tuvm/zephyr/ThermalHttpNode.nsl`:

```
(*
ThermalHttpNode — the boot module for the AMG8833 thermal demo. One module
declaration nesting three decoupled classes (Newspeak idiom; the bake is
single-app-file, see the rung design 4a):
  ThermalNode  — samples the Grid-EYE on a ctl cadence, publishes frames
  HttpServer   — generic path->block route table over the chunked HTTP prims
  ThermalView  — renders a 64-px frame as an SVG heat-grid
They never reference each other; producer and consumer meet only at the channel
name 'dev/thermal0/sample'. main: is the composition root.
*)
class ThermalHttpNode packageUsing: manifest = () (

  (* ---- presentation: a 64-px centi-degC frame as an SVG heat-grid ----
     The frame comes from `platform sensorFrame`, a RAW indexable Array, so slots
     are read with basicAt: (like HeapStats / udp basicReceive) — the Array-class
     at: would DNU because the prim receiver does not carry the Array vtable. *)
  class ThermalView = () (
    public renderFrame: f toId: id on: platform = (
      | row col line t k |
      (f == nil) ifTrue: [
        platform httpChunkId: id data: '<html><body>no frame</body></html>'. ^ self ].
      platform httpChunkId: id data:
        '<html><body><svg viewBox=''0 0 8 8'' width=''320'' height=''320'' shape-rendering=''crispEdges''>'.
      row:: 0. [ row <= 7 ] whileTrue: [
        line:: ''.
        col:: 0. [ col <= 7 ] whileTrue: [
          t:: f basicAt: (row * 8 + col + 1).
          k:: ((t - 1500) * 255) // 2000.
          (k < 0)   ifTrue: [ k:: 0 ].
          (k > 255) ifTrue: [ k:: 255 ].
          line:: line , '<rect x=''' , col printString , ''' y=''' , row printString ,
                ''' width=''1'' height=''1'' fill=''rgb(' , k printString , ',0,' ,
                (255 - k) printString , ')''/>'.
          col:: col + 1 ].
        platform httpChunkId: id data: line.
        row:: row + 1 ].
      platform httpChunkId: id data: '</svg><p>'.
      platform httpChunkId: id data: (self caption: f).
      platform httpChunkId: id data: '</p></body></html>'.
      ^ self )

    caption: f = (
      | row col t maxV minV hotR hotC |
      maxV:: f basicAt: 1. minV:: f basicAt: 1. hotR:: 0. hotC:: 0.
      row:: 0. [ row <= 7 ] whileTrue: [
        col:: 0. [ col <= 7 ] whileTrue: [
          t:: f basicAt: (row * 8 + col + 1).
          (t > maxV) ifTrue: [ maxV:: t. hotR:: row. hotC:: col ].
          (t < minV) ifTrue: [ minV:: t ].
          col:: col + 1 ]. row:: row + 1 ].
      ^ 'max ' , (self centi: maxV) , 'C min ' , (self centi: minV) ,
        'C hot (' , hotR printString , ',' , hotC printString , ')' )

    centi: v = (
      | whole tenth |
      whole:: v // 100.
      tenth:: (v - (whole * 100)) // 10.
      ^ whole printString , '.' , tenth printString )
  ) : (
  )

  public main: platform args: args = (
    | tview |
    tview:: ThermalView new.
    ^ 0 )
) : (
)
```

- [ ] **Step 2: Verify it compiles**

**Do NOT verify by running run-one.py on the file directly** — it reports SKIP
*before* compiling (`run_tests_oevm.py:395`/`:483`), so a syntax error looks
identical to success. Build a scratchpad copy carrying an expect block that
exercises the new class, and run that instead:

```bash
sed -e '1i (*\nexpect:\nok\n*)' \
    -e "s|^    tview:: ThermalView new.|    tview:: ThermalView new.\n    (tview centi: 2200) out.\n    'ok' out.|" \
    ~/workspace/tuvm/zephyr/ThermalHttpNode.nsl > /tmp/ThermalHttpNode.nsl
python3 ~/workspace/nsl-tuvm/tools/run-one.py /tmp/ThermalHttpNode.nsl
```
Expected: output `['22.0', 'ok']` on both paths — proving the module parses and
resolves and that a nested-class method dispatches. (With the expect block above
listing only `ok` the runner reports FAIL on the mismatch; that is fine, the
actual output is the evidence. Add `22.0` to the block for a clean PASS.) A parse
or resolve error means the nesting is wrong — fix before continuing.

- [ ] **Step 3: Commit**

```bash
cd ~/workspace/tuvm && git add zephyr/ThermalHttpNode.nsl
git commit -m "feat(node): ThermalHttpNode boot module with nested ThermalView"
```

---

## Task 3: `ThermalNode` — the ctl-cadence sampler

**Files:**
- Modify: `tuvm/zephyr/ThermalHttpNode.nsl` (add the nested `ThermalNode`)
- Test: `nsl-tests/18_sensor/02_thermal_node.nsl`

**Interfaces:**
- Consumes: `platform sensorFrame` (Task 1); the `Timers` capability (`every:post:on:`, `cancel:`); `Channel>>peek` (the landed precursor).
- Produces: nested `class ThermalNode` with class-side `onSpace: sp seam: s timers: t`, instance `public start`. Publishes the frame to the **cell** `dev/thermal0/sample`; ticks arrive on the **stream** `dev/thermal0/tick`; config on the cell `dev/thermal0/ctl`.

**Critical:** the tick channel MUST be separate from the sample channel. `Timers>>every:post:on:` **posts** at the capability's own root (stream semantics); posting at the root of a cell raises `not a cell`/`not a stream`. Hence `dev/thermal0/tick`.

- [x] **Step 1: Write the failing test**

Create `nsl-tests/18_sensor/02_thermal_node.nsl`:

```
(*
expect:
before nil true
after px30 3120
rate change ok
done
*)
(* AMG8833 rung Task 3 — the ctl-cadence sampler, on the host virtual clock.
   Before any tick the sample cell has never been written, so bare `peek` answers
   nil. Advancing the virtual clock past the 500ms cadence fires the timer onto
   the tick STREAM, whose reaction reads `platform sensorFrame` and puts the frame
   on the sample CELL. The tick stream and the sample cell are DIFFERENT names:
   Timers posts at the capability root, and posting at a cell's root would raise. *)
(* The nested `ThermalNode` this test drives is added in Step 3 — deliberately NOT
   written yet, so this test goes red for the right reason (class missing).
   DUPLICATION, deliberate and temporary: a test is its own module and CANNOT name
   another app module's nested class (the bake is single-app-file — see the rung
   design 4a), so Step 3 pastes the class into BOTH this file and
   ThermalHttpNode.nsl, which stays the source of truth. The manifest rung (filed
   in the ladder) removes the copy; keep them in sync by hand until then. *)
class ThermalNodeTest packageUsing: manifest = () (

  public main: platform args: args = (
    | space timers node sample |
    space::  platform space.
    timers:: platform Timers usingSpace: space seam: platform.
    sample:: space channel: 'dev/thermal0/sample'.

    node:: ThermalNode onSpace: space seam: platform timers: timers.
    node start.
    ('before nil ' , (sample peek == nil) printString) out.

    platform advanceMillis: 501.
    space drain.
    ('after px30 ' , ((sample peek) basicAt: 30) printString) out.

    (space channel: 'dev/thermal0/ctl') put: '' value: (platform Record tag: #ctl with: #rate -> 10).
    space drain.
    'rate change ok' out.
    'done' out.
  )
) : (
)
```

- [x] **Step 2: Run the test to verify it fails**

```bash
python3 ~/workspace/nsl-tuvm/tools/run-one.py ~/workspace/nsl-tests/18_sensor/02_thermal_node.nsl
```
Expected: **FAIL** on both paths — `ThermalNode` is not defined in this module yet (a `Message 'ThermalNode' not understood by receiver <platform>` DNU, since an unbound capitalised name routes to a `platform` send). That is feature-missing, the correct red.

- [x] **Step 3: Add the `ThermalNode` class — to BOTH files**

Add this nested class to `tuvm/zephyr/ThermalHttpNode.nsl` immediately after `ThermalView` (the source of truth), **and paste the identical class** into `nsl-tests/18_sensor/02_thermal_node.nsl` immediately after its opening `class ThermalNodeTest … = () (` line. The duplication is forced by the single-app-file bake and is called out in both files' comments.

```
  (* ---- the sensor service: samples the Grid-EYE, publishes frames ----
     Knows nothing about HTTP. Capabilities arrive as constructor args (the
     Milestone-C rule). The cadence exists only because a ctl record asked for it
     (G9): no unrequested periodic work, and the CPU idles between ticks. *)
  class ThermalNode = ( | space seam timers sample ctl event tick timer rate | ) (
    onSpace: sp seam: s timers: t = (
      space::  sp.  seam:: s.  timers:: t.  rate:: 2.
      sample:: sp channel: 'dev/thermal0/sample'.    (* CELL:   latest frame out *)
      ctl::    sp channel: 'dev/thermal0/ctl'.       (* CELL:   {rate: N} in     *)
      event::  sp channel: 'dev/thermal0/event'.     (* STREAM: phase-2 alerts   *)
      tick::   sp channel: 'dev/thermal0/tick'.      (* STREAM: timer pulses     *)
      ^ self )

    public start = (
      (* Both reactions are self-sends: a reified block must not name a class. *)
      tick whenever: '' do: [ :t | self sampleOnce ].
      ctl  whenever: '' do: [ :c | self reconfigure: c ].
      self arm.
      ^ self )

    (* Hold the token and cancel before re-arming — a live repeater that outlives
       its owner is what killed the ESP32 TimerNode (rung 2). *)
    arm = ( timer:: timers every: (1000 // rate) post: #tick on: tick. ^ self )
    disarm = ( timer == nil ifFalse: [ timers cancel: timer. timer:: nil ]. ^ self )

    (* A failed fetch answers nil: skip the put and KEEP the last good frame.
       Never publish a partial or absent frame onto the cell. *)
    sampleOnce = (
      | f |
      f:: seam sensorFrame.
      (f == nil) ifFalse: [ sample put: '' value: f ].
      ^ self )

    (* Untrusted-input boundary: a bad rate leaves the current cadence standing. *)
    reconfigure: c = (
      | r on |
      on:: c at: #on ifAbsent: [ true ].
      r::  c at: #rate ifAbsent: [ rate ].
      on = false ifTrue: [ ^ self disarm ].
      ((r isNil not) and: [ r > 0 ]) ifTrue: [
        rate:: r. self disarm. self arm ].
      ^ self )
  ) : (
    public onSpace: sp seam: s timers: t = ( ^ self new onSpace: sp seam: s timers: t )
  )
```

- [x] **Step 4: Run the test to verify it passes**

```bash
python3 ~/workspace/nsl-tuvm/tools/run-one.py ~/workspace/nsl-tests/18_sensor/02_thermal_node.nsl
```
Expected: **Path A: PASS / Path B: PASS**. If `at:ifAbsent:` DNUs on the ctl record, check the kernel `Record` accessor name and adjust `reconfigure:` to match it.

- [x] **Step 5: Commit**

```bash
cd ~/workspace/tuvm && git add zephyr/ThermalHttpNode.nsl
git commit -m "feat(node): ThermalNode ctl-cadence sampler (2fps default)"
cd ~/workspace/nsl-tests && git add 18_sensor/02_thermal_node.nsl
git commit -m "test(sensor): ThermalNode publishes frames on the virtual clock"
```

---

## Task 4: `HttpServer` — the generic route table

**Files:**
- Modify: `tuvm/zephyr/ThermalHttpNode.nsl` (add the nested `HttpServer`)
- Reference: `tuvm/zephyr/HttpServerNode.nsl:24-37` (the request-dispatch shape being generalized)

**Interfaces:**
- Produces: nested `class HttpServer`, class-side `onSpace: sp seam: s`; instance `public route: path do: aBlock` and `public listen: port`. Route blocks are invoked `value: id value: platform`.

- [x] **Step 1: Add the `HttpServer` class**

In `tuvm/zephyr/ThermalHttpNode.nsl`, add after `ThermalNode`:

```
  (* ---- generic transport: a path->block route table over the chunked prims ----
     Knows nothing about sensors or presentation. C owns the socket and chunk
     framing; routing is nsl. `id` is an opaque correlation token that the C side
     resolves back to the response socket — nsl never touches a file descriptor. *)
  class HttpServer = ( | space seam paths blocks | ) (
    onSpace: sp seam: s = (
      space:: sp. seam:: s.
      paths::  s OrderedCollection new.
      blocks:: s OrderedCollection new.
      ^ self )

    public route: p do: aBlock = ( paths add: p. blocks add: aBlock. ^ self )

    public listen: port = (
      | req |
      req:: space channel: 'dev/http/request'.
      req whenever: '' do: [ :r | self handle: r ].
      seam httpListen: port.
      ^ self )

    (* Run-to-completion: one request is framed begin->body->end before the next,
       so responses for different ids never interleave. *)
    handle: r = (
      | id path i n found |
      id:: r basicAt: 1.  path:: r basicAt: 3.
      i:: 1. n:: paths size. found:: nil.
      [ (i <= n) and: [ found == nil ] ] whileTrue: [
        ((paths at: i) = path) ifTrue: [ found:: blocks at: i ].
        i:: i + 1 ].
      (found == nil)
        ifTrue: [
          seam httpBeginId: id status: 404.
          seam httpChunkId: id data: 'not found'.
          seam httpEndId: id ]
        ifFalse: [
          seam httpBeginId: id status: 200.
          found value: id value: seam.
          seam httpEndId: id ].
      ^ self )
  ) : (
    public onSpace: sp seam: s = ( ^ self new onSpace: sp seam: s )
  )
```

- [x] **Step 2: Verify it compiles**

**Do NOT verify by running run-one.py on the file directly.** `run_test_file` /
`run_test_file_oimg` return `"SKIP", "No expectations defined."` at
`run_tests_oevm.py:395` / `:483` — *before* they compile anything. A file with a
syntax error reports the same clean SKIP, so a bare run proves nothing.

Instead build a temp copy in the scratchpad that carries an expect block and
actually exercises the new class, then run that:

```bash
python3 - <<'EOF'
p='/home/david/workspace/tuvm/zephyr/ThermalHttpNode.nsl'
s=open(p).read()
s = "(*\nexpect:\nroutes 2\nok\n*)\n" + s
s = s.replace("""  public main: platform args: args = (
    | tview |
    tview:: ThermalView new.
    ^ 0 )""",
"""  public main: platform args: args = (
    | tview http space |
    space:: platform space.
    tview:: ThermalView new.
    http:: HttpServer onSpace: space seam: platform.
    http route: '/a' do: [ :id :plat | plat ].
    http route: '/b' do: [ :id :plat | plat ].
    ('routes ' , (http routeCount) printString) out.
    'ok' out.
    ^ 0 )""")
s = s.replace("    public route: p do: aBlock = ( paths add: p. blocks add: aBlock. ^ self )",
"    public route: p do: aBlock = ( paths add: p. blocks add: aBlock. ^ self )\n    public routeCount = ( ^ paths size )")
open('/tmp/ThermalHttpNode.nsl','w').write(s)
EOF
python3 ~/workspace/nsl-tuvm/tools/run-one.py /tmp/ThermalHttpNode.nsl
```
Expected: **Path A: PASS / Path B: PASS** — proving the module parses, resolves,
and that `HttpServer` constructs and accumulates routes. Discard the temp copy;
`routeCount` is a probe, not part of the class.

- [x] **Step 3: Commit**

```bash
cd ~/workspace/tuvm && git add zephyr/ThermalHttpNode.nsl
git commit -m "feat(node): HttpServer generic path->block route table"
```

---

## Task 5: Compose — wire `main:` and repoint the host gate

**Files:**
- Modify: `tuvm/zephyr/ThermalHttpNode.nsl` (replace the stub `main:`), `tuvm/tests/http_roundtrip_host.sh`

**Interfaces:**
- Consumes: `ThermalNode onSpace:seam:timers:`, `HttpServer onSpace:seam:`, `ThermalView new`, `Channel>>peek` (Tasks 1–4 + the precursor).

- [x] **Step 1: Replace the stub `main:` with the composition root**

In `tuvm/zephyr/ThermalHttpNode.nsl`, replace the placeholder `main:` with:

```
  (* ---- composition root: install the services, wire the routes, run the loop ----
     The /thermal route block sends to the CAPTURED `tview` instance; the class name
     is resolved here in method-body context, never inside the block (a reified block
     that names a class aborts on outer-scope resolution). /stats and / name classes
     and read platform metrics, so they route through self-send helpers. *)
  public main: platform args: args = (
    | space timers thermal tview http |
    space::   platform space.
    timers::  platform Timers usingSpace: space seam: platform.
    (* REQUIRED — the seam posts every timer fire on the reserved 'sys/timer' path
       carrying only a token; Timers>>dispatch: fans it out onto the target
       channel, and that wiring lives in the CALLER by design (kernel.nsl Timers
       header). Omit it and fires reach 'sys/timer' and stop: the sample cell
       stays nil forever and /thermal renders 'no frame' with no error anywhere.
       This is what broke Task 3's test. *)
    space whenever: 'sys/timer' do: [ :e | timers dispatch: e ].
    thermal:: ThermalNode onSpace: space seam: platform timers: timers.
    thermal start.
    tview::   ThermalView new.
    http::    HttpServer onSpace: space seam: platform.

    http route: '/thermal' do: [ :id :plat |
        tview renderFrame: ((space channel: 'dev/thermal0/sample') peek) toId: id on: plat ].
    http route: '/hello' do: [ :id :plat | plat httpChunkId: id data: 'hello' ].
    http route: '/stats' do: [ :id :plat | self showStatsTo: id on: plat ].
    http route: '/'      do: [ :id :plat | self showIndexTo: id on: plat ].

    http listen: 18080.
    [ true ] whileTrue: [ platform pumpEvents. space drain ].
  )
```

- [x] **Step 2: Add the `/stats` and `/` helpers**

Copy `statsPageOn:toId:`, `statRow:value:toId:on:`, and `indexToId:on:` verbatim from `tuvm/zephyr/HttpServerNode.nsl:40-84` into `ThermalHttpNode`'s instance side, renaming the two entry points to match the routes:

```
  showIndexTo: id on: platform = ( ^ self indexToId: id on: platform )
  showStatsTo: id on: platform = ( ^ self statsPageOn: platform toId: id )
```

- [x] **Step 3: Repoint the host round-trip gate**

In `tuvm/tests/http_roundtrip_host.sh`, change the baked node from `HttpServerNode.nsl` to `ThermalHttpNode.nsl`:

```bash
cd ~/workspace/tuvm && grep -n 'HttpServerNode' tests/http_roundtrip_host.sh
```
Replace each occurrence with `ThermalHttpNode`.

- [x] **Step 4: Run the host round-trip gate**

```bash
cd ~/workspace/tuvm && bash tests/http_roundtrip_host.sh
```
Expected: **PASS**, and the `/thermal` body byte-identical to before this rung (the posix synthetic frame reproduces the old hard-coded values: uniform 2200, index 30 = 3120).

- [x] **Step 5: Run the full suite**

```bash
cd ~/workspace/nsl-tuvm && python3 tools/run_tests_oevm.py 2>&1 | tail -8
```
Expected: **202 passed, 0 failed, 14 skipped**, all named gates green.

- [x] **Step 6: Commit**

```bash
cd ~/workspace/tuvm && git add zephyr/ThermalHttpNode.nsl tests/http_roundtrip_host.sh
git commit -m "feat(node): compose ThermalNode+HttpServer+ThermalView; repoint host gate"
```

---

## Task 6: Retire the superseded nodes

**Files:**
- Delete: `tuvm/zephyr/HttpServerNode.nsl`, `tuvm/zephyr/HttpNode.nsl`

- [x] **Step 1: Confirm nothing else references them**

```bash
cd ~/workspace/tuvm && grep -rn 'HttpServerNode\|HttpNode' --include='*.sh' --include='*.py' \
  --include='*.txt' --include='*.md' --include='*.c' . | grep -v '^./docs/' | grep -v build-
```
Expected: no live build/test references remain (docs/design records may still mention them — that is history, leave it).

- [x] **Step 2: Delete and re-gate**

```bash
cd ~/workspace/tuvm && git rm zephyr/HttpServerNode.nsl zephyr/HttpNode.nsl
bash tests/http_roundtrip_host.sh
cd ~/workspace/nsl-tuvm && python3 tools/run_tests_oevm.py 2>&1 | tail -8
```
Expected: round-trip PASS; suite **202 passed, 0 failed**.

- [x] **Step 3: Commit**

```bash
cd ~/workspace/tuvm && git commit -m "chore(node): retire HttpServerNode/HttpNode (superseded by ThermalHttpNode)"
```

---

## Task 7: The Zephyr sensor backend + board wiring

**Files:**
- Create: `tuvm/src/backend/zephyr/sensor_zephyr.c`, `tuvm/zephyr/boards/esp32_devkitc.overlay`
- Modify: `tuvm/zephyr/CMakeLists.txt`, `tuvm/zephyr/prj_esp32.conf`

**Interfaces:**
- Produces: the same `sensor_frame_read`/`sensor_arm_event` contract as `sensor_posix.c`, against the devicetree `amg88xx` node.

- [x] **Step 1: Write the Zephyr backend**

Create `tuvm/src/backend/zephyr/sensor_zephyr.c`:

```c
/* Zephyr sensor backend — the Tier-1 catalog play. The AMG8833 is reached through
   Zephyr's UNIFORM sensor API (sensor_sample_fetch + sensor_channel_get), not raw
   I2C: the in-tree panasonic,amg88xx driver does the register work, so this file
   never sees a calibration register. That is why this rung does NOT need the
   mixed-width binary-data library (tuvm#28).
   SENSOR_CHAN_AMBIENT_TEMP yields all 64 pixels in devicetree order. Each pixel
   arrives as struct sensor_value {val1 = whole degC, val2 = millionths}; we reduce
   to centi-degC so no Float is ever boxed. */
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include "../../sensor_backend.h"

#define THERMAL_NODE DT_NODELABEL(amg88xx)

static const struct device *thermal_dev(void)
{
#if DT_NODE_EXISTS(THERMAL_NODE)
    static const struct device *d;
    if (d == NULL) d = DEVICE_DT_GET(THERMAL_NODE);
    return (d && device_is_ready(d)) ? d : NULL;
#else
    return NULL;
#endif
}

int sensor_frame_read(int32_t *out)
{
    const struct device *dev = thermal_dev();
    struct sensor_value v[OEVM_THERMAL_PIXELS];

    if (dev == NULL) return -1;
    if (sensor_sample_fetch(dev) != 0) return -2;
    if (sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, v) != 0) return -3;

    for (int i = 0; i < OEVM_THERMAL_PIXELS; i++)
        out[i] = (int32_t)(v[i].val1 * 100 + v[i].val2 / 10000);
    return 0;
}

int sensor_arm_event(int chan_id, int handle_idx, int32_t threshold_centi)
{
    (void)chan_id; (void)handle_idx; (void)threshold_centi;
    return -1;   /* phase 2 (Task 9) replaces this */
}
```

- [x] **Step 2: Add it to the Zephyr build**

In `tuvm/zephyr/CMakeLists.txt`, in the source list that already names `timer_zephyr.c`/`http_zephyr.c`, add:

```cmake
  ${TUVM_SRC}/src/backend/zephyr/sensor_zephyr.c
```

- [x] **Step 3: Write the devicetree overlay**

Create `tuvm/zephyr/boards/esp32_devkitc_esp32_procpu.overlay`.

**The filename is load-bearing and the name originally given here was wrong.**
Zephyr auto-applies `boards/<board>_<qualifiers>.overlay`, joining the board
target's qualifiers with underscores (`cmake/modules/extensions.cmake:1709-1712`).
For `esp32_devkitc/esp32/procpu` that is `esp32_devkitc_esp32_procpu.overlay`.
A plain `esp32_devkitc.overlay` is not a name the build ever looks for — it would
be silently ignored, leaving no `amg88xx` node, compiling the backend's
`#else` branch, and returning nil frames with no error anywhere. Confirm the
build log prints `Found devicetree overlay: .../boards/esp32_devkitc_esp32_procpu.overlay`.

```dts
/* AMG8833 Grid-EYE on the WROOM-32's i2c0. `int-gpios` is REQUIRED by the
   panasonic,amg88xx binding even for the no-trigger build (phase 1); wire the
   sensor's open-drain, active-low INT pin to this GPIO and configure it pull-up.
   Default I2C address is 0x69 (0x68 when AD_SELECT is tied low). */
&i2c0 {
    status = "okay";
    clock-frequency = <I2C_BITRATE_STANDARD>;

    amg88xx: amg88xx@69 {
        compatible = "panasonic,amg88xx";
        reg = <0x69>;
        int-gpios = <&gpio0 4 GPIO_ACTIVE_LOW>;
        status = "okay";
    };
};
```

> **Pins confirmed** against `~/zephyr-esp32/zephyr/boards/espressif/esp32_devkitc/esp32_devkitc_procpu.dts:83-90`:
> `sda-gpios = <&gpio0 21 ...>`, `scl-gpios = <&gpio0 22 ...>`, and `i2c0` is already
> `status = "okay"` with `clock-frequency` set there — so the overlay only needs to add the
> sensor node, not restate the bus. GPIO4 verified free (no `gpio0 4` claim in the board dts).
>
> Wiring: VIN -> 3V3, GND -> GND, SDA -> GPIO21, SCL -> GPIO22, INT -> GPIO4.
> The binding (`dts/bindings/sensor/panasonic,amg88xx.yaml:11-18`) marks `int-gpios`
> `required: true` even for the no-trigger build, and documents the pin as open-drain
> active-low needing a pull-up — hence `(GPIO_ACTIVE_LOW | GPIO_PULL_UP)`.

- [x] **Step 4: Enable the driver**

In `tuvm/zephyr/prj_esp32.conf`, add:

```
# AMG8833 Grid-EYE thermal array (AMG88XX selects I2C). Phase 1 is poll-only: the
# cadence lives in nsl (ThermalNode's timer), so no driver trigger thread is needed.
CONFIG_SENSOR=y
CONFIG_AMG88XX=y
CONFIG_AMG88XX_TRIGGER_NONE=y
```

- [x] **Step 5: Build for the device (compile gate, no hardware needed)**

```bash
cd ~/workspace/tuvm/zephyr && ./run.sh --model B --node ThermalHttpNode.nsl
```
Expected: a clean build. This is a **compile/link gate only** — it proves the sensor backend and overlay are wired, without needing the sensor attached.

> `run.sh` resolves `--node` relative to `zephyr/` only (a known papercut); `ThermalHttpNode.nsl` lives there, so this works.

- [ ] **Step 6: Commit**

```bash
cd ~/workspace/tuvm && git add src/backend/zephyr/sensor_zephyr.c zephyr/CMakeLists.txt \
  zephyr/boards/esp32_devkitc.overlay zephyr/prj_esp32.conf
git commit -m "feat(sensor): Zephyr amg88xx backend + ESP32 overlay"
```

---

## Task 8: On-device bring-up (bench gate — needs hardware)

**Files:** none changed unless a defect is found.

**Hardware:** ESP32 WROOM-32 + AMG8833 on I2C (SDA/SCL per the board dts, INT to the overlay's GPIO, 3V3 + GND). WiFi credentials via the git-ignored `prj_esp32_wifi.local.conf` overlay.

- [x] **Step 1: Flash and capture**

```bash
cd ~/workspace/tuvm/zephyr && ./run.sh --model B --node ThermalHttpNode.nsl --flash --capture 20
```
Expected: boots, joins WiFi, prints its IP, no fault/oops/coredump.

- [x] **Step 2: Verify the live grid**

```bash
curl -s http://<esp32-ip>/thermal | head -c 400
```
Expected: an SVG heat-grid whose `caption` max/min/hot values are **realistic room temperatures** (roughly 18–30 °C), not the synthetic 22.00/31.20 pair. Re-run while waving a hand across the sensor — the hot cell coordinates must move.

- [x] **Step 3: Bank the footprint line**

```bash
curl -s http://<esp32-ip>/stats
```
Record `heap high-water`, `live bytes`, and `gc cycles` — compare against the pre-rung `/stats` numbers to confirm the sensor path did not eat the WROOM-32's remaining headroom.

- [ ] **Step 4: If the sensor is not detected**

`sensorFrame` answers nil → `/thermal` renders "no frame". Diagnose in this order: (a) `i2cdetect`-style scan or a boot log line for the amg88xx device; (b) confirm the I2C address (0x69 vs 0x68 / AD_SELECT); (c) `nm build-*/zephyr/zephyr.elf | grep -i amg` to confirm the driver linked; (d) check `int-gpios` names a real, free pin. Do **not** paper over it by faking the frame.

- [x] **Step 5: Bank the result**

Append an **E-series entry** to `tuvm/zephyr/docs/results.md` in the shape of the existing E1–E4 rows: node, model, what was proven, the `/stats` numbers, and the green log path. Note explicitly that this is a **manual bench gate**, so it earns no `CLAIMS.md` row (matching the Model-B and #46 precedent).

- [ ] **Step 6: Commit**

```bash
cd ~/workspace/tuvm && git add zephyr/docs/results.md
git commit -m "docs(results): E5 — AMG8833 live /thermal on ESP32 WROOM-32"
```

---

## Task 9: Phase 2 — the threshold event

**Files:**
- Modify: `tuvm/src/backend/zephyr/sensor_zephyr.c`, `tuvm/zephyr/prj_esp32.conf`, `tuvm/zephyr/ThermalHttpNode.nsl`

**Interfaces:**
- Consumes: `sensor_arm_event` (Task 1's contract), the pulse seam `pulse_post_isr(chan_id, handle_idx)`.

- [ ] **Step 1: Switch the driver to trigger mode**

In `tuvm/zephyr/prj_esp32.conf`, replace `CONFIG_AMG88XX_TRIGGER_NONE=y` with:

```
CONFIG_AMG88XX_TRIGGER_GLOBAL_THREAD=y
```
(`GLOBAL_THREAD` avoids the extra stack an `OWN_THREAD` build would cost.)

- [ ] **Step 2: Implement `sensor_arm_event`**

Replace the stub in `sensor_zephyr.c`:

```c
#include "../../pulse_backend.h"

static int   armed_chan;      /* interned in thread context by platform.c */
static int   armed_handle;    /* M6 handle pinning the message oop        */

/* Runs in the driver's global trigger thread, NOT the VM thread: it may only
   enqueue (chan_id, handle_idx) — pulse_pump resolves the handle to an oop and
   posts at the run-to-completion safe point. C never holds a raw oop here. */
static void amg_threshold_handler(const struct device *dev,
                                  const struct sensor_trigger *trig)
{
    (void)dev; (void)trig;
    pulse_post_isr(armed_chan, armed_handle);
}

int sensor_arm_event(int chan_id, int handle_idx, int32_t threshold_centi)
{
    const struct device *dev = thermal_dev();
    struct sensor_value thr;
    struct sensor_trigger trig = {
        .type = SENSOR_TRIG_THRESHOLD,
        .chan = SENSOR_CHAN_AMBIENT_TEMP,
    };

    if (dev == NULL) return -1;

    armed_chan   = chan_id;
    armed_handle = handle_idx;

    thr.val1 = threshold_centi / 100;
    thr.val2 = (threshold_centi % 100) * 10000;
    if (sensor_attr_set(dev, SENSOR_CHAN_AMBIENT_TEMP,
                        SENSOR_ATTR_UPPER_THRESH, &thr) != 0) return -2;

    return sensor_trigger_set(dev, &trig, amg_threshold_handler);
}
```

- [ ] **Step 3: Wire an nsl reaction**

In `ThermalNode>>start`, after the existing reactions, add:

```
      event whenever: '' do: [ :e | self onAlert: e ].
      seam sensorArmEventPost: #hot on: 'dev/thermal0/event' threshold: 3000.
```

The channel is passed as its **String path** (the prim interns it), matching
`gpioPin:edge:post:on:` — not as a `Channel` object.

and the handler:

```
    onAlert: e = ( 'thermal alert' out. ^ self )
```

- [ ] **Step 4: Bench-verify**

Rebuild, flash, and hold a warm object (hand, mug) in view until the threshold is crossed. Expected: `thermal alert` appears in the capture, and the node keeps serving `/thermal` (the alert must not disturb the reactor loop).

> **Known residue, out of scope:** `pulse_post_isr` still `k_oops()`es on a full mailbox (**tuvm#34**) — a sustained event burst can kill the node. The G2 "never kills the node" rule is not yet extended to the ISR seam; that is Arc-3 P0 hardening. The demo's threshold fires rarely.

- [ ] **Step 5: Commit**

```bash
cd ~/workspace/tuvm && git add src/backend/zephyr/sensor_zephyr.c zephyr/prj_esp32.conf \
  zephyr/ThermalHttpNode.nsl
git commit -m "feat(sensor): phase-2 AMG8833 threshold event via the pulse seam"
```

---

## Task 10: Close-out — promote the durable knowledge

Per the project's promotion rule, a rung is not closed until its knowledge reaches a mainline document of record.

- [ ] **Step 1: Promote to `review/zephyr-driver-exposure.md`**

Rewrite its status from *sketch* to *built for Tier 1*, recording: the Tier-1 seam now exists (`sensor_backend.h` + one prim); **BME280 was dropped and AMG8833 became the first device**; the finding that **going through the sensor API means the driver does the register work, so tuvm#28 was never forced**; and the three-way service split (produce / transport / present) as the node-composition pattern.

- [ ] **Step 2: Update the ladder**

In `review/program-sequence.md`, mark the Arc-1 driver-ladder rung as landed for AMG8833, link this plan and the design doc, and note that the **demo-node gap shrank** (`TempNode` realized on real hardware, Tier-1 variant). Leave the filed manifest rung in place.

- [ ] **Step 3: Confirm no `CLAIMS.md` row moves**

The device gates are manual bench runs, so no new named test claim is earned. The two new host tests (`18_sensor/01`, `02`) raise the suite floor to **202/0/14** — state the new floor wherever the old one is quoted.

- [ ] **Step 4: Final full gate**

```bash
cd ~/workspace/nsl-tuvm && python3 tools/run_tests_oevm.py 2>&1 | tail -8
cd ~/workspace/tuvm && bash tests/http_roundtrip_host.sh
```
Expected: **202 passed, 0 failed, 14 skipped**; round-trip PASS.

- [ ] **Step 5: Commit**

```bash
cd ~/workspace/tuvm && git add review/zephyr-driver-exposure.md review/program-sequence.md
git commit -m "docs(close-out): AMG8833 rung — promote Tier-1 seam knowledge to the ladder"
```

---

## Deferred (explicitly NOT in this plan)

- **TFT character-grid display (Tier 2)** — the next rung, its own spec. It reuses `ThermalView`'s shape as a second view of the same frame.
- **Manifest-based module injection** — filed in the ladder; until it lands, a module's own classes nest in its boot module and `ThermalNode` is duplicated into its test.
- **Auto-enumerated `dev/<node>/` sensor catalog** — generalize when a second sensor lands.
- **tuvm#34** (ISR mailbox `k_oops`) and **tuvm#28** (binary-data library, unforced here) — Arc-3 / their own rungs.
- **Folding WiFi out of the shared `prj_esp32.conf`** — pre-filed follow-up; a non-WiFi node (TimerNode, perfstex) should not carry the WiFi stack.
