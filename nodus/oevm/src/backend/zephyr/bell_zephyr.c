/* The bell leaves of the board (rete#440 R6a): the cosine-generator strike and its
   bring-up. Moved verbatim from zephyr/src/main.c; the behaviour is unchanged. The
   console strike-on-demand and the solenoid one-shot are App wiring and stay in main.c. */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <stdbool.h>
#include <stdint.h>
#include "target_backend.h"

#if defined(CONFIG_DAC) && DT_NODE_EXISTS(DT_ALIAS(spkren))
#include <zephyr/drivers/dac.h>
/* THE LEGACY IDF HEADER IS NOT AN ACCIDENT. Zephyr's DAC API cannot produce a
   tone at all: drivers/dac/dac_esp32.c exposes exactly channel_setup +
   write_value, and write_value sets ONE 8-bit DC level per call — no buffer, no
   DMA, no continuous mode. The ESP32-S2's hardware cosine generator is reachable
   only through this deprecated header, whose implementation
   (components/driver/deprecated/dac_common_legacy.c) is compiled under
   CONFIG_DAC_ESP32 and whose directory is on the global include path
   (hal/espressif/zephyr/esp32s2/CMakeLists.txt:12,64,209-215). So the PIN is
   enabled through the Zephyr API and the TONE is driven through the legacy one. */
#include "driver/dac.h"
/* THE LOW-LEVEL HEADER IS THE ECHO FIX (tuvm#94 stage 1), not a shortcut. The
   envelope has to change ONE register field — dac_scale1 — and the legacy
   driver's only route to it is dac_cw_generator_config(), which also rewrites the
   frequency divider and RE-STAMPS THE PHASE. Doing that mid-tone is a phase
   discontinuity, i.e. an audible transient, three times per strike; it also
   re-measures the uncalibrated RTC-8M oscillator each call, so the pitch
   micro-shifts too. Both were heard on the bench as an echo and initially assumed
   impossible. dac_ll_cw_set_atten() touches SENS.sar_dac_ctrl2.dac_scale1 and
   nothing else.
   Going under the driver skips its mutex. That is safe HERE and only here: this
   TU is the sole DAC user in the image, and the strike runs on the VM's single
   cooperative thread — there is no second caller to race with. The header is on
   the global include path (hal/espressif/zephyr/esp32s2/CMakeLists.txt:45). */
#include "hal/dac_ll.h"
/* MagTag bell (tuvm#94). The cosine generator on GPIO17 — same pin, same amp as
   the #86 LEDC chime, but reaching what LEDC cannot: 130 Hz-55 kHz (vs LEDC's
   ~1 kHz floor on this part) and four amplitude steps, which is what makes a
   decay envelope possible at all.

   GATE 1 WAS A SWEEP, NOT A NOTE, AND THE SWEEP WAS RIGHT TO BE ONE. The stated
   risk was that a sine would be QUIETER than the square wave #86 played, since a
   square carries far more harmonic energy into a tiny buzzer. Measured
   2026-07-23, the risk was BACKWARDS: 220 and 440 are the good tones and 1760
   sounded bad. A single frequency would have produced a confident wrong
   conclusion either way — the same reason sweeping is what located LEDC's floor
   in #86. */
static const struct gpio_dt_spec bell_en = GPIO_DT_SPEC_GET(DT_ALIAS(spkren), gpios);

/* Set by bell_bringup once the pin is enabled; read by bell_target_available.
   Declared inside the guard, unlike disp_ok — every reference to it is inside the
   same guard, and a file-scope bool on the boards with no speaker would be an
   unused static. */
static bool bell_ok;

/* The bell's fundamental. 440 Hz beat 220 on the bench (gate 2), and it is BELOW
   the ~1 kHz LEDC floor measured in #86 — so this is a pitch the PWM path could
   not have produced at all. */
#define BELL_HZ 440

/* ONE STRIKE. The cosine generator is a pure oscillator with no envelope of its
   own, so what makes this a strike rather than a beep is the four-step amplitude
   ramp — DAC_CW_SCALE_1/2/4/8 is 1, 1/2, 1/4, 1/8 of full scale, i.e. -6 dB a
   step. Four steps is ALL the hardware has; a real bell's continuous exponential
   decay (and its inharmonic partials) needs per-sample synthesis, which is the
   decision this gate exists to inform.

   The steps LENGTHEN as they quieten, which is what an exponential decay does and
   what makes the tail read as ring-out rather than as a staircase. Total 500 ms —
   #86 measured 160 ms as inaudible at rc=0 and 500 ms as loud on this buzzer. */
static void bell_strike(uint32_t hz) {
    /* The attenuation names come from the HAL (dac_cosine_atten_t), not from the
       legacy DAC_CW_SCALE_* aliases, because dac_ll_cw_set_atten takes that type.
       The two enums are numerically identical (0-3) — this is a typing choice, not
       a behaviour change. */
    static const struct { dac_cosine_atten_t atten; int ms; } envelope[] = {
        { DAC_COSINE_ATTEN_DB_0,   50 },   /* attack: full amplitude, short */
        { DAC_COSINE_ATTEN_DB_6,   90 },
        { DAC_COSINE_ATTEN_DB_12, 140 },
        { DAC_COSINE_ATTEN_DB_18, 220 },   /* tail: quietest, longest */
    };

    /* CONFIGURE ONCE, THEN STEP ATTENUATION ONLY. The frequency, phase and offset
       are set here and never touched again for the life of the strike — that is
       the whole of the echo fix (see the hal/dac_ll.h note above). config()
       brackets the RTC-8M clock with enable/disable to read its frequency, but
       that is refcounted and generator_enable holds a reference, so the clock does
       not drop out mid-strike. */
    dac_cw_config_t cw = {
        .en_ch  = DAC_CHAN_0,
        .scale  = DAC_CW_SCALE_1,
        .phase  = DAC_CW_PHASE_0,
        .freq   = hz,
        .offset = 0,
    };
    (void)dac_cw_generator_config(&cw);
    (void)dac_cw_generator_enable();

    for (size_t i = 0; i < ARRAY_SIZE(envelope); i++) {
        dac_ll_cw_set_atten(DAC_CHAN_0, envelope[i].atten);
        k_msleep(envelope[i].ms);
    }

    /* SILENCE BETWEEN STRIKES HAS TO COME FROM DISABLING THE GENERATOR. The
       quietest step is 1/8 scale, not zero, so leaving it running would leave a
       continuous tone under the whole pattern and destroy the phrasing. */
    (void)dac_cw_generator_disable();
}

/* N BELLS, STRUCK IN PAIRS — the ship's-bell convention: two quick strikes, a
   gap, two more, with a LONE FINAL STROKE for an odd count. Five bells is
   ding-ding … ding-ding … ding, and it is the most informative count to demo
   because it exercises both the pairing and the odd remainder.

   The gap after a strike depends on its position: even index = first of a pair,
   so a short gap holds the pair together; odd index = second of a pair, so a long
   gap separates it from the next. No trailing gap after the final stroke. */
static void bell_strike_pattern(uint32_t hz, int n) {
    printk("bell: %u Hz x%d\n", hz, n);
    for (int i = 0; i < n; i++) {
        bell_strike(hz);
        if (i == n - 1) {
            break;
        }
        k_msleep((i % 2 == 0) ? 160 : 600);
    }
}

/* tuvm#94 stage 1: BRING-UP ONLY — it no longer strikes anything. Gates 1 and 2
   were a boot-time demo (sweep, then three times of day); now that nsl drives the
   bell through PRIM_pf_bellStrike, a boot demo would be ~20 s of bells before the
   node gets a word in, and worse, it would make every reset sound like a working
   node whether or not the seam works. What is left here is exactly the state the
   strike cannot re-derive per call: the pin enable and the availability flag.
   The amp is NOT un-muted here — that is per-strike, below. */
void bell_bringup(void) {
    /* THE ZEPHYR DAC DEVICE IS DELIBERATELY NOT REQUIRED, AND ITS INIT FAILS.
       drivers/dac/dac_esp32.c:71 does `if (clock_control_on(...) != 0) return
       -EIO`, but clock_control_esp32.c:506 returns -EALREADY when the module
       clock is ALREADY ON — a success condition. So dac_esp32_init reports -EIO,
       the device never becomes ready, and every failure message is swallowed
       because it goes through LOG_ERR while this build has only CONFIG_PRINTK.
       Note drivers/adc/adc_esp32.c:668 calls the very same clock_control_on for
       the very same module (both DAC and ADC hang off ESP32_PERIPH_SARADC_MODULE)
       and DISCARDS the return value — i.e. the ADC driver already tolerates what
       the DAC driver dies on. Upstream bug; not patched locally because this
       Zephyr checkout carries the C6 radio fix and must not accrete casual
       patches.

       It costs nothing to route around: the tone must come from the legacy API
       regardless (Zephyr's DAC API cannot generate one), so taking the pin enable
       from the same place removes a dependency rather than adding a workaround.
       CONFIG_DAC_ESP32 is still required — it is what compiles
       dac_common_legacy.c — hence the devicetree node stays enabled. */
    if (!gpio_is_ready_dt(&bell_en)) {
        printk("bell: speaker-enable pin not ready\n");
        return;
    }

    /* CHANNEL 0, AND THE NAME IS A TRAP. GPIO17 is DAC_CHAN_0
       (soc/esp32s2/include/soc/dac_channel.h: DAC_GPIO17_CHANNEL == DAC_CHAN_0).
       The legacy alias DAC_CHANNEL_1 is deprecated and its VALUE IS 0 — the name
       says "1" and means the first channel. Writing 1 here would silently target
       GPIO18, which goes nowhere on this board: rc=0 and no sound. */
    int rc = dac_output_enable(DAC_CHAN_0);
    printk("bell: dac_output_enable rc=%d\n", rc);
    if (rc) {
        return;   /* never discard an error code on a bring-up path */
    }

    /* Leave the amp MUTED. It is un-muted per strike (bell_target_strike), so a
       node that never rings leaves no idle hiss on the speaker and, more to the
       point, so the 50 ms settle below is paid where it is actually needed. */
    gpio_pin_configure_dt(&bell_en, GPIO_OUTPUT_INACTIVE);
    bell_ok = true;
    printk("BELL OK %u Hz\n", (unsigned)BELL_HZ);
}
#endif /* CONFIG_DAC && spkren */

/* tuvm#94 stage 1: the device half of the bell seam. The host half is in
 * src/platform.c under #ifndef __ZEPHYR__. Both symbols must exist and link on
 * EVERY Zephyr board, including the ones with no DAC and no speaker (ESP32, C6,
 * nRF52840-DK/dongle) — src/platform.c declares them extern unconditionally under
 * __ZEPHYR__. So the guard goes INSIDE each body, exactly as the display five do,
 * never around the definitions. On a board with no bell, available() answers 0
 * and strike() is unreachable in practice but still reports a fault rather than
 * silently claiming to have rung. */
int bell_target_available(void)
{
#if defined(CONFIG_DAC) && DT_NODE_EXISTS(DT_ALIAS(spkren))
	return bell_ok ? 1 : 0;
#else
	return 0;   /* no DAC or no speaker-enable pin on this board */
#endif
}

int bell_target_strike(int bells)
{
#if defined(CONFIG_DAC) && DT_NODE_EXISTS(DT_ALIAS(spkren))
	if (!bell_ok) return 2;

	/* THE AMP NEEDS TO SETTLE — the single biggest time sink in #86, where
	 * enabling SPEAKER_SHUTDOWN and driving in the same breath was silent
	 * and looked like a routing bug for three builds. Per strike now, not
	 * once at boot: 50 ms against a ~500 ms strike is noise, and holding the
	 * amp live between strikings buys nothing.
	 *
	 * 440 Hz, SETTLED ON THE BENCH 2026-07-23. Gate 1 swept 220/440/880/1760:
	 * all four audible, 1760 bad. Gate 2 struck the pattern at 440 and 220
	 * back to back and 440 was "far superior". 440 is BELOW LEDC's floor on
	 * this part (#86: 500 Hz -> -EINVAL), so the pitch this bell wants is one
	 * PWM could never have produced — the rung's whole justification. */
	gpio_pin_configure_dt(&bell_en, GPIO_OUTPUT_ACTIVE);
	k_msleep(50);
	bell_strike_pattern(BELL_HZ, bells);
	gpio_pin_configure_dt(&bell_en, GPIO_OUTPUT_INACTIVE);   /* re-mute */
	return 0;
#else
	ARG_UNUSED(bells);
	return 2;   /* unreachable in practice: available() already gated this */
#endif
}
