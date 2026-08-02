/* Zephyr sensor backend — the Tier-1 catalog play. The AMG8833 is reached through
   Zephyr's UNIFORM sensor API (sensor_sample_fetch + sensor_channel_get), not raw
   I2C: the in-tree amg88xx driver does the register work, so this file never sees a
   calibration register. That is why this rung does NOT need the mixed-width
   binary-data library (tuvm#28).

   Verified against the in-tree driver (zephyr/drivers/sensor/amg88xx/amg88xx.c):
     - drv_data->sample is int16_t[64], so channel_get writes exactly
       OEVM_THERMAL_PIXELS entries — v[] below must be that long, and is.
     - channel_get answers -ENOTSUP for any chan except SENSOR_CHAN_AMBIENT_TEMP.
     - Raw LSB is 0.25 degC (AMG88XX_TREG_LSB_SCALING 250000 millionths):
       val1 = raw*250000/1000000, val2 = raw*250000%1000000, val2 sign-matched to val1.

   The centi-degC reduction is therefore exact, not approximate: 0.25 degC lands on
   25 centi with nothing to round, and C division truncating toward zero gives the
   right answer for negatives too (raw -5 -> val1 -1, val2 -250000 -> -100 + -25 =
   -125 = -1.25 degC). No Float is ever boxed — the float arena OOMs on a no-PSRAM
   WROOM-32, which is the whole reason pixels are integers. */
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include "../../sensor_backend.h"

#define THERMAL_NODE DT_NODELABEL(amg88xx)

static const struct device *thermal_dev(void)
{
#if DT_NODE_EXISTS(THERMAL_NODE)
    static const struct device *d;
    /* DEVICE_DT_GET is a compile-time pointer, so this caches rather than probes.
       Readiness is re-checked every call on purpose: a sensor that is not ready at
       boot (slow power-up, bus contention) can become ready later, and answering
       NULL until then makes the prim return nil, which ThermalNode already handles
       by keeping the last good frame. */
    if (d == NULL) d = DEVICE_DT_GET(THERMAL_NODE);
    return (d && device_is_ready(d)) ? d : NULL;
#else
    /* No amg88xx node in the devicetree for this board: build cleanly and answer
       "not ready" forever, exactly as the seam contract allows. This is what keeps
       a board without the overlay (e.g. the nRF DK targets) linking. */
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
