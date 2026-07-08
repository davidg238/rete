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
    if (k_msgq_get(&pulse_q, &it, K_FOREVER) == 0) {
        space_post_id(it.chan_id, handle_get(it.handle_idx));
        drained++;
        while (k_msgq_get(&pulse_q, &it, K_NO_WAIT) == 0) {
            space_post_id(it.chan_id, handle_get(it.handle_idx));
            drained++;
        }
    }
    return drained;
}
