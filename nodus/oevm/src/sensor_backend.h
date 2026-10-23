#ifndef OEVM_SENSOR_BACKEND_H
#define OEVM_SENSOR_BACKEND_H
/* The sensor seam (rete#436; design docs/design/2026-10-23-sensor-seam-design.md).
   Exactly one impl is linked per OEVM_TARGET: backend/posix/sensor_posix.c
   (deterministic synthetic readings) for the host gate; backend/zephyr/sensor_zephyr.c
   (Zephyr's uniform `sensor` API) on target. Core must never call the sensor API
   directly — that is how a host-only assumption reached the SoC once already (the
   ROM-fflush bug).

   A sensor is named by its KIND ("thermal", "accel"); on target the device is the
   devicetree node aliased `<kind>0`, supplied by the board's overlay — in-tree or
   out-of-tree (#438), which is where the mechanism/board line falls. Every value is
   an integer in MILLI-units of Zephyr's SI unit for the channel (milli-degC,
   milli-m/s^2): integers keep every value inside SMI range and box no Float, which
   OOMs the float arena on a no-PSRAM WROOM-32. */
#include <stdint.h>

/* The widest reading any kind answers: the AMG8833's 8x8 frame. */
#define OEVM_SENSOR_MAX_VALUES 64

/* Fill out[0..n-1] with one reading of `kind` (len bytes, not NUL-terminated) and
   answer n (1..OEVM_SENSOR_MAX_VALUES). Answers a negative number when the kind is
   unknown, the device absent or not ready, or the fetch failed — the caller then
   answers nil, and a sampler keeps its last good reading. */
int sensor_read(const char *kind, int len, int32_t *out);

/* Arm `kind`'s event trigger at threshold_milli (the kind's reading unit) so that
   each firing posts the pinned message on chan_id via the ISR pulse seam — i.e. the
   handler calls pulse_post_isr(chan_id, handle_idx). Both are resolved in thread
   context by the caller (space_intern / handle_new), exactly as
   gpioPin:edge:post:on: does; a handler must never touch the VM. Answers 0 when
   armed and sets *superseded to the handle this kind's previous arm held (-1 if
   none), which the caller frees — one pin per kind, kept here rather than in a
   second copy of the kind list (rete#184). Answers non-zero, *superseded
   untouched, when the kind has no trigger or the driver refused. */
int sensor_arm(const char *kind, int len, int chan_id, int handle_idx, int32_t threshold_milli,
               int *superseded);

#endif /* OEVM_SENSOR_BACKEND_H */
