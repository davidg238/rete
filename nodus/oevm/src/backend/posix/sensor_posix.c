/* Host sensor backend — no hardware. Answers deterministic synthetic readings so
   the host gate exercises the REAL prim path (not a nsl-side fake):
     thermal — a uniform 22.000 C field with one hot pixel at C index 29 (nsl
               basicAt: 30) = 31.200 C, the values the pre-sensor
               HttpServerNode.nsl hard-coded, in milli-degC.
     accel   — at rest, flat: (0, 0, 9807) milli-m/s^2, i.e. 1 g on Z.
     motion  — none: 0 (rete#447).
     ir      — an object at room temperature: 22000 milli-degC (rete#447).
   Arming mirrors the target: 'accel', 'motion' and 'ir' arm (and never fire — the
   host has no trigger hardware), 'thermal' refuses until #58. */
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
    if (is_kind(kind, len, "motion")) { out[0] = 0; return 1; }
    if (is_kind(kind, len, "ir")) { out[0] = 22000; return 1; }
    return -1;
}

/* One pin per armed kind, as on target. */
static const char *const armable[] = { "accel", "motion", "ir" };
static int handles[] = { -1, -1, -1 };

int sensor_arm(const char *kind, int len, int chan_id, int handle_idx, int32_t threshold_milli,
               int *superseded)
{
    (void)chan_id; (void)threshold_milli;
    for (unsigned i = 0; i < sizeof armable / sizeof armable[0]; i++)
        if (is_kind(kind, len, armable[i])) {
            *superseded = handles[i];
            handles[i] = handle_idx;
            return 0;
        }
    return -1;
}
