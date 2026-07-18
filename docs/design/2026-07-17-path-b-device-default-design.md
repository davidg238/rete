# Path B (.oimg / image-VM) as the device default

**Date:** 2026-07-17
**Status:** Implemented; ESP32 validated on-device; nRF-DK default flipped but
**unvalidated on hardware** (board not connected — pending reconnect).
**Follows:** `docs/design/2026-07-16-zephyr-model-ab-build-knob-design.md` (the
Model A/B knob this builds on).

## Decision

Flip the Zephyr app's default execution model from **A (baked
`solidified_image.c`)** to **B (generic VM + `.oimg` blob `.incbin`'d into flash
`.rodata`)** on **all device targets**, with **TimerNode** as the default image.

## Why

- **Model A cannot build on ESP32/Xtensa:** `cc1` on the ~13k-line
  `src/solidified_image.c` needs ~4.9 GB RSS and ~50 min and has OOM-killed the
  desktop. Model B `.incbin`s the same bytes in ~0 s. On the device that ships
  Xtensa, B is not merely faster — it is the path that builds at all.
- The A/B machinery already existed and was proven (host ENG_A/ENG_B, `-m32`
  portability gate). This change is defaults + a default-blob fallback, not new
  construction.

## What changed

1. `zephyr/CMakeLists.txt`
   - `set(TUVM_MODEL "A" …)` → `"B"`.
   - Model B's `TUVM_BLOB`-required **FATAL_ERROR** replaced with a **default
     blob fallback**: a bare `west build` with no `-DTUVM_BLOB` falls back to the
     committed `zephyr/TimerNode.oimg`. Explicit `-DTUVM_BLOB` and `run.sh`'s
     freshly-baked blob still override.
2. `zephyr/run.sh`
   - Default `MODEL=A` → `MODEL=B`.
   - **Flash port pinned** (`--port ${ESPTOOL_PORT:-/dev/ttyUSB0}`). esptool with
     no `--port` auto-detects the first ESP32 and clobbered the /dev/ttyUSB1
     jag/Toit board on 2026-07-16; the tuvm devkit is /dev/ttyUSB0.
3. No change to the Model A link set or the tracked `solidified_image.c`
   fixture — A stays reachable via `--model A` / `-DTUVM_MODEL=A` where it builds.

## Validation

- **ESP32 (esp32_devkitc/esp32/procpu), on-device, 2026-07-17:**
  `run.sh --model B --node TimerNode.ns --flash --capture 25` →
  bake `TimerNode.oimg` (109208 B) → incbin → Model-B build
  (`TUVM_MODEL=B`, `zephyr.bin` 313 KB) → esptool write to `/dev/ttyUSB0` →
  `*** Booting Zephyr OS build v4.2.0 ***` / `TIMER OK`. **Gate GREEN.**
- **nRF52840 DK/dongle:** default flipped to B; Model B proven on host `-m32`
  but **NOT yet on nRF hardware** (48 KB arena; no board connected). Known,
  deliberate gap — validate when a DK is reconnected before relying on it.

## Cross-VM context (Toit, same silicon)

Benched on the sibling WROOM-32 running Jaguar (`musing-sleep`, /dev/ttyUSB1),
means-of-3, self-verified:

| Benchmark | Toit (ms) | tuvm Model-B / interp (ms, ref) |
|---|---|---|
| richards  | 1318 | ~15,957 (device, merged defaults) |
| deltablue | ~93  | — |

Toit is a compiled VM; tuvm is a bytecode interpreter — the gap is expected and
is the cross-VM column context for the STIC table (`~/workspaceToit/stic/`).

## Follow-ups

- Validate Model B on nRF-DK hardware when reconnected; if the 48 KB arena is
  tight, keep A default for nRF specifically.
- Promote status into the ladder (`review/program-sequence.md`) at close-out.
