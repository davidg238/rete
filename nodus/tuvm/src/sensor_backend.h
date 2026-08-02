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
