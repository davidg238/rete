/* Host sensor backend — no hardware. Answers deterministic synthetic readings so
   the host gate exercises the REAL prim path (not a nsl-side fake):
     thermal — a uniform 22.000 C field with one hot pixel at C index 29 (nsl
               basicAt: 30) = 31.200 C, the values the pre-sensor
               HttpServerNode.nsl hard-coded, in milli-degC.
     accel   — at rest, flat: (0, 0, 9807) milli-m/s^2, i.e. 1 g on Z.
   Arming mirrors the target: 'accel' arms (and never fires — the host has no
   trigger hardware), 'thermal' refuses until #58. */
#include <string.h>
#include "../../sensor_backend.h"

static int is_kind(const char *kind, int len, const char *name)
{
    return len == (int)strlen(name) && memcmp(kind, name, (size_t)len) == 0;
}

int sensor_read(const char *kind, int len, int32_t *out)
{
    if (is_kind(kind, len, "thermal")) {
        for (int i = 0; i < 64; i++) out[i] = 22000;
        out[29] = 31200;
        return 64;
    }
    if (is_kind(kind, len, "accel")) {
        out[0] = 0; out[1] = 0; out[2] = 9807;
        return 3;
    }
    return -1;
}

static int accel_handle = -1;

int sensor_arm(const char *kind, int len, int chan_id, int handle_idx, int32_t threshold_milli,
               int *superseded)
{
    (void)chan_id; (void)threshold_milli;
    if (!is_kind(kind, len, "accel")) return -1;
    *superseded = accel_handle;
    accel_handle = handle_idx;
    return 0;
}
