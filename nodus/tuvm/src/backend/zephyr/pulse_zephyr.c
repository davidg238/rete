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

/* tuvm#34: k_msgq offers no way to inspect what it already holds (k_msgq_peek sees
   only the head), so per-source coalescing needs a shadow of the queued tokens. It
   mirrors pulse_q exactly — same FIFO order, same length — so pump pops both in
   lockstep. The posix backend needs no equivalent: its mailbox IS a plain array and
   is scanned directly. Mutated from both ISR and thread context, so every access is
   under irq_lock. */
static struct PulseItem shadow[OEVM_PULSE_MAILBOX_DEPTH];
static int sh_head = 0, sh_count = 0;
static uint32_t mb_drops = 0, mb_coalesced = 0;

void pulse_post_isr(int chan_id, int handle_idx) {
    struct PulseItem it = { chan_id, handle_idx };
    unsigned int key = irq_lock();

    /* Coalesce: a source with a post already pending gains nothing from a second.
       The payload is a doorbell token, not a value, so the entries are identical and
       merging loses nothing — this is what stops a repeating timer from consuming
       every slot and starving a GPIO edge (tuvm#54/#55). */
    for (int k = 0; k < sh_count; k++) {
        int idx = (sh_head + k) % OEVM_PULSE_MAILBOX_DEPTH;
        if (shadow[idx].chan_id == chan_id && shadow[idx].handle_idx == handle_idx) {
            mb_coalesced++;
            irq_unlock(key);
            return;
        }
    }

    /* Shed, never k_oops. A full mailbox is a load condition, not a broken
       invariant, and G2 (tuvm#20) already settled that a load condition must not
       kill the node — this seam was the residue that still did. */
    if (k_msgq_put(&pulse_q, &it, K_NO_WAIT) != 0) {
        mb_drops++;
        irq_unlock(key);
        return;
    }
    shadow[(sh_head + sh_count) % OEVM_PULSE_MAILBOX_DEPTH] = it;
    sh_count++;
    irq_unlock(key);
}

/* Pop the shadow head, keeping it in lockstep with a successful k_msgq_get. */
static void shadow_pop(void) {
    unsigned int key = irq_lock();
    if (sh_count > 0) {
        sh_head = (sh_head + 1) % OEVM_PULSE_MAILBOX_DEPTH;
        sh_count--;
    }
    irq_unlock(key);
}

uint32_t pulse_drops(void)     { return mb_drops; }
uint32_t pulse_coalesced(void) { return mb_coalesced; }

int pulse_pump(void) {
    struct PulseItem it;
    int drained = 0;
    /* block for the first event (idle safe point), then drain all currently queued */
    /* tuvm#13: NULL means the pinned slot was freed and reused between post and pump
       (a stale generation) — drop it rather than post the new occupant. */
    if (k_msgq_get(&pulse_q, &it, K_FOREVER) == 0) {
        shadow_pop();
        oop o = handle_get(it.handle_idx);
        if (o) { space_post_id(it.chan_id, o); drained++; }
        while (k_msgq_get(&pulse_q, &it, K_NO_WAIT) == 0) {
            shadow_pop();
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
        shadow_pop();
        oop o = handle_get(it.handle_idx);
        if (o) { space_post_id(it.chan_id, o); drained++; }
    }
    return drained;
}
