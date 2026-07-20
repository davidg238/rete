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
