/* Zephyr sensor backend — the Tier-1 catalog play (rete#436; design
   docs/design/2026-10-23-sensor-seam-design.md). Sensors are reached through Zephyr's
   UNIFORM sensor API (sensor_sample_fetch + sensor_channel_get), not raw I2C: the
   in-tree driver does the register work, so this file never sees a calibration
   register.

   The KINDS table below is the whole catalogue. A kind's device is the devicetree
   node aliased `<kind>0`, which the board's overlay supplies — rete carries no board
   pin (#438). A board without the alias builds cleanly and answers "not ready"
   forever, exactly as the seam contract allows.

   Every value is milli-units of Zephyr's SI unit: val1*1000 + val2/1000. That is
   exact for both drivers in use, so no Float is ever boxed:
     thermal — panasonic,amg88xx: drv_data->sample is int16_t[64], so channel_get
               writes exactly 64 entries; raw LSB is 0.25 degC, so val2 is a multiple
               of 250000 and lands on a whole milli-degC. channel_get answers -ENOTSUP
               for any chan except SENSOR_CHAN_AMBIENT_TEMP.
     accel   — st,lis2dh (also the LSM303AGR accel): SENSOR_CHAN_ACCEL_XYZ writes 3
               entries in m/s^2; truncation toward zero drops < 1 milli-m/s^2.
     motion  — a PIR or radar presence part (rete#447; in-tree precedent xbr818):
               SENSOR_CHAN_PROX, 1 value, 0 or 1000 (the milli rule on an adimensional 1).
     ir      — a single thermopile (rete#447; precedent tmp007): the OBJECT temperature
               on SENSOR_CHAN_AMBIENT_TEMP, 1 value. Not SENSOR_CHAN_IR, which is lux.
   Neither has an in-tree driver for the parts that asked (PYD1588, TPiS 1T): the
   board's module supplies it (run.sh --extra-module, rete#438).
   C division truncates toward zero and val2 is sign-matched to val1, so negatives
   come out right (-1.25 degC: val1 -1, val2 -250000 -> -1000 + -250 = -1250). */
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include "../../sensor_backend.h"
#include "../../pulse_backend.h"

#define THERMAL_NODE DT_ALIAS(thermal0)
#define ACCEL_NODE   DT_ALIAS(accel0)
#define MOTION_NODE  DT_ALIAS(motion0)
#define IR_NODE      DT_ALIAS(ir0)

struct sensor_kind {
    const char *name;
    const struct device *dev;   /* NULL when this board's devicetree has no alias */
    enum sensor_channel chan;
    int n;                      /* values one reading writes */
    /* The trigger that sensor_arm sets, and the attribute its threshold sets; a kind
       with no trigger has trig == SENSOR_TRIG_COMMON_COUNT and refuses to arm. */
    enum sensor_trigger_type trig;
    enum sensor_attribute thr_attr;
    /* SENSOR_ATTR_CONFIGURATION written before the threshold, or -1 for none. */
    int32_t arm_cfg;
    /* The armed event: one per kind. handle_idx is the caller's pin, -1 until the
       first arm; sensor_arm hands the superseded one back for the caller to free. */
    int chan_id, handle_idx;
    /* The driver keeps this pointer and hands it back to on_trigger, so it lives
       here, not on sensor_arm's stack (lis2dh stores trig_anymotion = trig). */
    struct sensor_trigger armed;
};

static struct sensor_kind kinds[] = {
    {   .name = "thermal",
#if DT_NODE_EXISTS(THERMAL_NODE)
        .dev = DEVICE_DT_GET(THERMAL_NODE),
#endif
        .chan = SENSOR_CHAN_AMBIENT_TEMP, .n = 64,
        /* #58: the AMG8833 threshold trigger is deferred (WROOM-32 DRAM ceiling). */
        .trig = SENSOR_TRIG_COMMON_COUNT, .arm_cfg = -1, .handle_idx = -1 },
    {   .name = "accel",
#if DT_NODE_EXISTS(ACCEL_NODE)
        .dev = DEVICE_DT_GET(ACCEL_NODE),
#endif
        .chan = SENSOR_CHAN_ACCEL_XYZ, .n = 3,
        /* lis2dh any-motion: SLOPE_TH is the INTx_THS register, SENSOR_TRIG_DELTA
           arms INTx_CFG. Which pin (anym-on-int1) and which mode (anym-mode: OR
           any-motion vs 6D position) are devicetree properties of the board.
           The comparator sees raw acceleration, gravity included, so without the
           high-pass filter any threshold under 1 g fires forever and any over it
           misses all but a jolt. arm_cfg is CTRL2's HPIS1|HPIS2: the filter feeds
           the interrupt comparators only, so a sensorRead: still sees gravity. */
        .trig = SENSOR_TRIG_DELTA, .thr_attr = SENSOR_ATTR_SLOPE_TH,
#ifdef CONFIG_LIS2DH_ACCEL_HP_FILTERS
        .arm_cfg = 0x03,
#else
        .arm_cfg = -1,
#endif
        .handle_idx = -1 },
    /* rete#447. The threshold reaches the driver as val1 = threshold_milli / 1000, in
       whatever unit the driver's SENSOR_ATTR_UPPER_THRESH takes. */
    {   .name = "motion",
#if DT_NODE_EXISTS(MOTION_NODE)
        .dev = DEVICE_DT_GET(MOTION_NODE),
#endif
        .chan = SENSOR_CHAN_PROX, .n = 1,
        .trig = SENSOR_TRIG_MOTION, .thr_attr = SENSOR_ATTR_UPPER_THRESH,
        .arm_cfg = -1, .handle_idx = -1 },
    {   .name = "ir",
#if DT_NODE_EXISTS(IR_NODE)
        .dev = DEVICE_DT_GET(IR_NODE),
#endif
        .chan = SENSOR_CHAN_AMBIENT_TEMP, .n = 1,
        .trig = SENSOR_TRIG_THRESHOLD, .thr_attr = SENSOR_ATTR_UPPER_THRESH,
        .arm_cfg = -1, .handle_idx = -1 },
};

static struct sensor_kind *find_kind(const char *kind, int len)
{
    for (size_t i = 0; i < ARRAY_SIZE(kinds); i++)
        if (len == (int)strlen(kinds[i].name) && memcmp(kind, kinds[i].name, (size_t)len) == 0)
            return &kinds[i];
    return NULL;
}

/* DEVICE_DT_GET is a compile-time pointer, so nothing is probed here. A Zephyr init
   that fails is final: device_is_ready never turns true afterwards. A board whose
   sensor cannot answer at boot marks its node `zephyr,deferred-init`, and the first
   use initialises it here (on the ACC01 the LSM303AGR does not answer at 6.8 ms
   after reset, the moment a Bluetooth image probes it, rete#436). device_init is a
   no-op (-EALREADY) on a device that was initialised at boot. Not ready answers nil,
   which a sampler already handles by keeping its last good reading. */
static const struct device *ready_dev(const struct sensor_kind *k)
{
    if (k->dev == NULL) return NULL;
    if (!device_is_ready(k->dev)) (void)device_init(k->dev);
    return device_is_ready(k->dev) ? k->dev : NULL;
}

int sensor_read(const char *kind, int len, int32_t *out)
{
    const struct sensor_kind *k = find_kind(kind, len);
    struct sensor_value v[OEVM_SENSOR_MAX_VALUES];
    const struct device *dev;

    if (k == NULL) return -1;
    if ((dev = ready_dev(k)) == NULL) return -2;
    if (sensor_sample_fetch(dev) != 0) return -3;
    if (sensor_channel_get(dev, k->chan, v) != 0) return -4;

    for (int i = 0; i < k->n; i++)
        out[i] = (int32_t)(v[i].val1 * 1000 + v[i].val2 / 1000);
    return k->n;
}

/* Runs in the driver's trigger context (its own thread or the global work queue,
   per the driver's TRIGGER_* Kconfig), never the VM's: it only posts the pin. */
static void on_trigger(const struct device *dev, const struct sensor_trigger *trig)
{
    for (size_t i = 0; i < ARRAY_SIZE(kinds); i++)
        if (kinds[i].dev == dev && kinds[i].trig == trig->type) {
            pulse_post_isr(kinds[i].chan_id, kinds[i].handle_idx);
            return;
        }
}

int sensor_arm(const char *kind, int len, int chan_id, int handle_idx, int32_t threshold_milli,
               int *superseded)
{
    struct sensor_kind *k = find_kind(kind, len);
    const struct device *dev;

    if (k == NULL || k->trig == SENSOR_TRIG_COMMON_COUNT) return -1;
    if ((dev = ready_dev(k)) == NULL) return -2;

    struct sensor_value thr = {
        .val1 = threshold_milli / 1000,
        .val2 = (threshold_milli % 1000) * 1000,
    };
    if (k->arm_cfg >= 0) {
        struct sensor_value cfg = { .val1 = k->arm_cfg };
        if (sensor_attr_set(dev, k->chan, SENSOR_ATTR_CONFIGURATION, &cfg) != 0) return -3;
    }
    if (sensor_attr_set(dev, k->chan, k->thr_attr, &thr) != 0) return -3;

    /* Publish the destination BEFORE the trigger can fire. A re-arm replaces the
       handler with the same function, so a firing between these stores and
       trigger_set posts on the new pin — which the caller has already made. */
    int prev = k->handle_idx;
    k->chan_id = chan_id;
    k->handle_idx = handle_idx;
    k->armed = (struct sensor_trigger){ .type = k->trig, .chan = k->chan };
    if (sensor_trigger_set(dev, &k->armed, on_trigger) != 0) {
        /* Not armed after all: the caller frees handle_idx, so stop naming it.
           The old pin stays the kind's — if the driver kept the earlier trigger,
           it still posts there; if it dropped it, the stale pin is harmless. */
        k->handle_idx = prev;
        return -4;
    }
    *superseded = prev;
    return 0;
}
