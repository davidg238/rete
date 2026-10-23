# The sensor seam, generalised — accelerometer, wake on motion, out-of-tree boards

2026-10-23 · #436 (seam), #437 (wake on motion), #438 (out-of-tree board) · requested by ttgm, a private
sit/stand sensor on a proprietary nRF52840 + LSM303AGR board. All three P0 (David, 2026-10-23).

## Why

The sensor seam was cut for one device: `sensor_backend.h` fixes `OEVM_THERMAL_PIXELS` and centi-degC,
`sensor_zephyr.c` reaches only `DT_NODELABEL(amg88xx)`, and `sensor_arm_event` is a stub. ttgm needs
`SENSOR_CHAN_ACCEL_XYZ` from Zephyr's in-tree `st,lis2dh` driver (it covers the LSM303AGR accel) and a
motion interrupt that wakes the node.

## Decided by David (2026-10-23)

- **The board is proprietary and cannot live in rete; the driver mechanism can.** rete carries nothing
  ACC01-specific: no overlay, no pin, no board name. The board lives in the ttgm repo and builds rete's VM
  from outside (#438).
- **Thread sleepy end device**, not System OFF (#437): the node stays attached, and the motion interrupt
  posts through the existing ISR pulse seam with no reboot. Measuring both currents is welcome evidence.
  Reminders are delivered gateway-side, so the node sends sit/stand events and has no timer wake to do.

## The seam (my calls — one rule each)

1. **A sensor is named by its kind, a String, and found by a devicetree alias `<kind>0`.** Kinds today:
   `'thermal'` (alias `thermal0`, `SENSOR_CHAN_AMBIENT_TEMP`, 64 values) and `'accel'` (alias `accel0`,
   `SENSOR_CHAN_ACCEL_XYZ`, 3 values). The C table of kinds is the whole catalogue; a new kind is one row.
   The board's overlay (in-tree or out) supplies the alias — that is where the mechanism/board line falls.
2. **Every reading is an integer in milli-units of Zephyr's SI unit for the channel** (milli-degC,
   milli-m/s²). One unit rule, no Float boxed. Thermal moves from centi to milli — call-site churn accepted.
3. **Prims, replacing the thermal pair in place (ids 116/117, so no id shifts):**
   - `platform sensorRead: kind` → raw Array of SMIs (read with `basicAt:`), or nil when the kind is
     unknown, the alias absent, or the device not ready.
   - `platform sensorArm: kind post: msg on: path threshold: milli` → the platform when armed, nil when the
     backend cannot (the rete#184 rule). Same String-path + `handle_new` shape as `gpioPin:edge:post:on:`;
     one pin per kind. For `'accel'` it sets `SENSOR_ATTR_SLOPE_TH` and arms `SENSOR_TRIG_DELTA`
     (any-motion); for `'thermal'` it answers nil until #58.
4. **Host backend:** a synthetic accel at rest, flat (0, 0, 9807 milli-m/s²); arming succeeds and never fires.

## Left open, to be earned by data

- Slope duration, output data rate, range, 6D-position vs any-motion mode: devicetree properties the board
  overlay sets (`anym-mode`, `anym-on-int1`), not prims — until an app needs to change them at runtime.
- SED poll period: start at the OpenThread default; the measured current and the event latency set it.

## Measured (2026-10-23, branch `sensor-seam-436`)

| Build | Result |
|---|---|
| nRF52840 DK, `TimerNode`, 112 KB arena, no accel | RAM overflowed by 6,500 B — **already broken on master's code**, #427 |
| same + lis2dh overlay + `prj_accel_lis2dh.conf` | overflowed by 6,884 B: the accel kind costs **384 B** RAM |
| same, `OEVM_HEAP_SIZE=98304` (#427's measured workaround) | links: RAM 96.18 %, FLASH 44.80 %; `lis2dh_*`, `sensor_arm`, `kinds` in the ELF |
| WROOM-32 `ThermalHttpNode`, master vs branch | dram0 overflowed by 9,280 vs 9,392 B — pre-existing (#53, #431); the seam costs 112 B |

**#427 blocks every nRF52840 target, ttgm's board included.** Its two named levers (registry 64 → the
board's counted paths; no OTA stack where no program slot exists) restore ~10 KB; the decision is David's.

## Order

#436 (seam + host gate + nRF compile with a lis2dh overlay) → #438 (external board root / overlay, so ttgm
builds) → #437 (arm + SED config + current measured on ttgm's board).
