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

/* #151: handle_idx >= 0 -> heap payload behind an M6 handle (a GPIO message object).
   handle_idx < 0 -> `value` IS the payload, an immediate that cannot move and needs
   no pinning. The two are compared together when coalescing, so posts that differ
   only in payload — two distinct timer tokens — no longer look identical. */
#define PULSE_BY_VALUE (-1)
struct PulseItem { int chan_id; int handle_idx; oop value; };
static struct PulseItem mailbox[OEVM_PULSE_MAILBOX_DEPTH];
static int mb_head = 0, mb_count = 0;
static uint32_t mb_drops = 0, mb_coalesced = 0;

static void pulse_enqueue(int chan_id, int handle_idx, oop value) {
    /* tuvm#34: shed, never abort. A full mailbox is a load condition (the reactor
       is not pumping — e.g. a blocking render, tuvm#54), not a structural invariant
       violation, and G2 already settled that a load condition must not kill the
       node. Drop the newest post and count it; the queued ones keep causal order,
       matching the space ring's STREAM rule one seam later. */
    /* tuvm#34/#55: coalesce per source. A source with a post already pending gains
       nothing from a second one — the payload is a doorbell token, not a value, so
       the two entries are byte-identical and merging loses no information. This is
       what stops one repeating timer from consuming every slot and starving a GPIO
       edge. The queue IS the pending set, so there is no side table to keep in sync
       (the zephyr backend cannot scan its k_msgq and does keep one). O(depth) with
       depth 16, all comparisons, ISR-cheap. */
    for (int k = 0; k < mb_count; k++) {
        int idx = (mb_head + k) % OEVM_PULSE_MAILBOX_DEPTH;
        if (mailbox[idx].chan_id == chan_id && mailbox[idx].handle_idx == handle_idx
            && mailbox[idx].value == value) {
            mb_coalesced++;
            return;
        }
    }
    if (mb_count >= OEVM_PULSE_MAILBOX_DEPTH) { mb_drops++; return; }
    int tail = (mb_head + mb_count) % OEVM_PULSE_MAILBOX_DEPTH;
    mailbox[tail].chan_id = chan_id;
    mailbox[tail].handle_idx = handle_idx;
    mailbox[tail].value = value;
    mb_count++;
}

void pulse_post_isr(int chan_id, int handle_idx) {
    pulse_enqueue(chan_id, handle_idx, NULL);
}

void pulse_post_isr_value(int chan_id, oop immediate) {
    pulse_enqueue(chan_id, PULSE_BY_VALUE, immediate);
}

int pulse_pump(void) {
    int drained = 0;
    while (mb_count > 0) {
        struct PulseItem it = mailbox[mb_head];
        mb_head = (mb_head + 1) % OEVM_PULSE_MAILBOX_DEPTH;
        mb_count--;
        /* An immediate needs no lookup and can never stale. tuvm#13: for a HANDLE,
           NULL means the pinned slot was freed and reused between post and pump (a
           stale generation) — drop it rather than post the new occupant. */
        oop o = (it.handle_idx == PULSE_BY_VALUE) ? it.value : handle_get(it.handle_idx);
        if (o) { space_post_id(it.chan_id, o); drained++; }
    }
    return drained;
}

/* Posix pulse_pump is already non-blocking, so the nb variant is identical. */
int pulse_pump_nb(void) { return pulse_pump(); }

uint32_t pulse_drops(void) { return mb_drops; }

uint32_t pulse_coalesced(void) { return mb_coalesced; }
