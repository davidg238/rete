/* Host pulse backend: a plain in-process ring (no irq, no threads). The host GPIO
   mock's simulated-edge prim (platform.c PRIM_pf_gpioFire) calls pulse_post_isr in
   thread context; pulse_pump is non-blocking so the host AB test rig can drive
   [gpioFire; pumpEvents; drain] without hanging. Same code path as the target
   (handle_get -> space_post_id); only the queue mechanism differs. */
#include "../../pulse_backend.h"
#include "../../space.h"
#include "../../handles.h"
#include <stdio.h>
#include <stdlib.h>

struct PulseItem { int chan_id; int handle_idx; };
static struct PulseItem mailbox[OEVM_PULSE_MAILBOX_DEPTH];
static int mb_head = 0, mb_count = 0;

void pulse_post_isr(int chan_id, int handle_idx) {
    if (mb_count >= OEVM_PULSE_MAILBOX_DEPTH) {
        fprintf(stderr, "pulse mailbox full\n");
        abort();
    }
    int tail = (mb_head + mb_count) % OEVM_PULSE_MAILBOX_DEPTH;
    mailbox[tail].chan_id = chan_id;
    mailbox[tail].handle_idx = handle_idx;
    mb_count++;
}

int pulse_pump(void) {
    int drained = 0;
    while (mb_count > 0) {
        struct PulseItem it = mailbox[mb_head];
        mb_head = (mb_head + 1) % OEVM_PULSE_MAILBOX_DEPTH;
        mb_count--;
        /* tuvm#13: NULL means the pinned slot was freed and reused between post and
           pump (a stale generation) — drop it rather than post the new occupant. */
        oop o = handle_get(it.handle_idx);
        if (o) { space_post_id(it.chan_id, o); drained++; }
    }
    return drained;
}
