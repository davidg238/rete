/* Target pulse backend: an ISR-safe k_msgq mailbox (pattern from st-zephyr's
   test-dongle-05-jast/src/hal/zephyr/jast_event.c). A GPIO/timer ISR calls
   pulse_post_isr -> k_msgq_put(K_NO_WAIT) with a (chan_id, handle_idx) token; the
   run-to-completion main loop calls pulse_pump, which BLOCKS for the first item
   (K_FOREVER), then drains the rest (K_NO_WAIT) and, in thread context, resolves the
   M6 handle to an oop and calls space_post_id. space.c stays irq-free. */
#include "../../pulse_backend.h"
#include "../../space.h"
#include "../../handles.h"
#include <zephyr/kernel.h>

struct PulseItem { int chan_id; int handle_idx; };
K_MSGQ_DEFINE(pulse_q, sizeof(struct PulseItem), OEVM_PULSE_MAILBOX_DEPTH, 4);

void pulse_post_isr(int chan_id, int handle_idx) {
    struct PulseItem it = { chan_id, handle_idx };
    if (k_msgq_put(&pulse_q, &it, K_NO_WAIT) != 0) {
        printk("pulse mailbox full\n");
        k_oops();                     /* Tier-2 crash, target equivalent of abort() */
    }
}

int pulse_pump(void) {
    struct PulseItem it;
    int drained = 0;
    /* block for the first event (idle safe point), then drain all currently queued */
    /* tuvm#13: NULL means the pinned slot was freed and reused between post and pump
       (a stale generation) — drop it rather than post the new occupant. */
    if (k_msgq_get(&pulse_q, &it, K_FOREVER) == 0) {
        oop o = handle_get(it.handle_idx);
        if (o) { space_post_id(it.chan_id, o); drained++; }
        while (k_msgq_get(&pulse_q, &it, K_NO_WAIT) == 0) {
            o = handle_get(it.handle_idx);
            if (o) { space_post_id(it.chan_id, o); drained++; }
        }
    }
    return drained;
}

/* Non-blocking drain (K_NO_WAIT throughout): used by pumpEvents when a second
   event source (an HTTP listener, #46) shares the run-to-completion loop and the
   reactor must NOT sleep in pulse_pump's K_FOREVER, or http_poll would never run
   again after the first tick. A pure pulse/timer node still uses the blocking
   pulse_pump() above and sleeps efficiently until its ISR fires. */
int pulse_pump_nb(void) {
    struct PulseItem it;
    int drained = 0;
    while (k_msgq_get(&pulse_q, &it, K_NO_WAIT) == 0) {
        oop o = handle_get(it.handle_idx);
        if (o) { space_post_id(it.chan_id, o); drained++; }
    }
    return drained;
}
