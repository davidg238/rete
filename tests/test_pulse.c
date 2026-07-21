/* Unit test for the posix pulse backend (src/backend/posix/pulse_posix.c) + the M6
   handle table + space seam, exercising the SAME path the target ISR uses:
     handle_new(value) -> pulse_post_isr(chan_id, idx) -> pulse_pump() -> space_post_id.
   Proves FIFO delivery, that the value reaches the space queue, and that an over-full
   mailbox aborts "pulse mailbox full". Built with -DOEVM_PULSE_MAILBOX_DEPTH=4. */
#define _POSIX_C_SOURCE 200112L
#include "../src/oevm.h"
#include "../src/space.h"
#include "../src/handles.h"
#include "../src/pulse_backend.h"
#include "../src/prim_ids.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

extern void heap_init(void);
extern void kernel_boot(void);

/* space.c internals we assert against */
extern oop space_take(const char *path, size_t len, int *found);  /* removes+returns oldest live value, or NULL; *found=1 iff pulled (tuvm#21) */

/* Task 3: drive the real prim path (gpioPin:edge:post:on: / gpioFire: /
   pumpEvents) via platform_dispatch, the same route the nsl host gate uses. */
extern int platform_dispatch(int pid, int argc, oop *args, oop *out);
extern oop StringVtable;
extern oop gc_allocate(oop vtable, size_t size);

/* Build a heap String with the given bytes (mirrors test_space.c's make_string
   helper / platform.c's PRIM_pf_string allocation idiom: StringVtable +
   struct String's length-then-inline-bytes layout). */
static oop make_string(const char *s, size_t n) {
    oop str = gc_allocate(StringVtable, sizeof(struct String) + n);
    ((struct String *)str)->length = n;
    if (n) memcpy(((struct String *)str)->bytes, s, n);
    return str;
}

int main(void) {
    heap_init();
    kernel_boot();

    int c = space_intern("sys/gpio", 8);
    int h1 = handle_new(TAG_SMI(11));
    int h2 = handle_new(TAG_SMI(22));

    /* nothing pumped yet: pump on an empty mailbox drains nothing */
    assert(pulse_pump() == 0 && "empty mailbox pumps 0");

    /* post two, pump: both reach the space queue in FIFO order */
    pulse_post_isr(c, h1);
    pulse_post_isr(c, h2);
    assert(pulse_pump() == 2 && "two posted -> two pumped");
    assert(space_take("sys/gpio", 8, NULL) == TAG_SMI(11) && "FIFO: first value first");
    assert(space_take("sys/gpio", 8, NULL) == TAG_SMI(22) && "FIFO: second value second");
    assert(space_take("sys/gpio", 8, NULL) == NULL && "queue drained");

    /* tuvm#34: a full mailbox must NOT kill the node. G2 already retired abort() at
       the space ring for exactly this reason; the seam in FRONT of it kept it, so a
       burst arriving while the reactor was not pumping (a blocking HTTP render —
       tuvm#54) was still a whole-node kill from ISR context. Posts beyond capacity
       are SHED and COUNTED instead, and what the mailbox did accept still delivers.
       Distinct handles so per-source coalescing is not what is being measured. */
    {
        int co = space_intern("sys/shed", 8);
        int hs[OEVM_PULSE_MAILBOX_DEPTH + 3];
        for (int i = 0; i < OEVM_PULSE_MAILBOX_DEPTH + 3; i++) hs[i] = handle_new(TAG_SMI(100 + i));
        uint32_t before = pulse_drops();
        for (int i = 0; i < OEVM_PULSE_MAILBOX_DEPTH + 3; i++) pulse_post_isr(co, hs[i]);
        assert(pulse_drops() - before == 3 && "3 posts beyond depth shed and counted");
        assert(pulse_pump() == OEVM_PULSE_MAILBOX_DEPTH && "mailbox still delivered its full depth");
        printf("test_pulse: mailbox_overflow_sheds_not_aborts PASS\n");
    }

    /* tuvm#34/#55: one source must not be able to starve the others. A repeating
       timer only ever needs ONE pending wakeup — queuing N identical (chan,handle)
       tokens is pure waste, and it is what fills the mailbox (tuvm#54: 2 Hz ticks
       piling up during a blocking render). Posts from a source that already has one
       pending COALESCE into it and are counted separately from sheds: coalescing is
       by design, a shed means the reactor is genuinely behind. Because the mailbox
       payload is a doorbell token and not a value, merging loses nothing.
       Depth here is 4, so 20 uncoalesced posts would shed 16 and starve `other`. */
    {
        int crep = space_intern("sys/rep", 7);
        int hrep = handle_new(TAG_SMI(55));
        int coth = space_intern("sys/other", 9);
        int hoth = handle_new(TAG_SMI(66));
        uint32_t cbefore = pulse_coalesced(), dbefore = pulse_drops();
        for (int i = 0; i < 20; i++) pulse_post_isr(crep, hrep);
        pulse_post_isr(coth, hoth);
        assert(pulse_coalesced() - cbefore == 19 && "19 repeats coalesced into one pending");
        assert(pulse_drops() - dbefore == 0 && "coalescing means nothing had to be shed");
        assert(pulse_pump() == 2 && "one event per source, both sources delivered");
        assert(space_take("sys/other", 9, NULL) == TAG_SMI(66)
               && "the quiet source was NOT starved by the flooding one");
        printf("test_pulse: repeat_source_coalesces_and_does_not_starve PASS\n");
    }

    /* tuvm#34 never-silent law: shedding that nobody can observe is how this class of
       bug hides — the crash at least announced itself. prim 109 `sysStats` carries the
       two counters so Kernel>>metrics can publish them on the sys/metrics cell beside
       the existing space-ring overrun count. Asserted against LIVE non-zero values
       (3 drops, 19 coalesced from the cases above), so this proves real wiring rather
       than two zeros agreeing. */
    {
        oop out = NULL;
        oop stat_args[1] = { NULL };
        assert(platform_dispatch(PRIM_pf_sysStats, 0, stat_args, &out) == 1
               && "sysStats dispatched");
        struct ObjectInstance *a = (struct ObjectInstance *)out;
        assert(a->slots[0] == TAG_SMI(6) && "sysStats widened to 6 fields");
        assert(pulse_drops() > 0 && pulse_coalesced() > 0 && "counters are non-zero here");
        assert(a->slots[5] == TAG_SMI((intptr_t)pulse_drops()) && "field 5 is pulseDrops");
        assert(a->slots[6] == TAG_SMI((intptr_t)pulse_coalesced()) && "field 6 is pulseCoalesced");
        printf("test_pulse: sysStats_surfaces_pulse_counters PASS\n");
    }

    /* drive the real prim path: gpioPin registers, gpioFire simulates the edge,
       pumpEvents drains — the same route the nsl host gate uses. */
    {
        oop out = NULL;
        oop chan = make_string("sensor/alert", 12);
        oop reg_args[4];
        reg_args[0] = TAG_SMI(3);            /* pin */
        reg_args[1] = TAG_SMI(1);            /* edge (#rising, as SMI) */
        reg_args[2] = TAG_SMI(99);           /* msg */
        reg_args[3] = chan;                  /* chanPath */
        assert(platform_dispatch(PRIM_pf_gpioPin, 4, reg_args, &out) == 1
               && "gpioPin:edge:post:on: dispatched");

        oop fire_args[1]; fire_args[0] = TAG_SMI(3);
        assert(platform_dispatch(PRIM_pf_gpioFire, 1, fire_args, &out) == 1
               && "gpioFire: dispatched");

        oop pump_args[1] = { NULL };
        assert(platform_dispatch(PRIM_pf_pumpEvents, 0, pump_args, &out) == 1
               && "pumpEvents dispatched");
        assert(out == TAG_SMI(1) && "pumpEvents delivered exactly one event");

        assert(space_take("sensor/alert", 12, NULL) == TAG_SMI(99)
               && "gpioFire:+pumpEvents landed the registered msg on the channel");
        printf("test_pulse: gpio_prim_roundtrip PASS\n");
    }

    /* Regression: re-registering the SAME pin must not leak an M6 handle-table
       slot per call (Important review finding). host_gpio_register's "replace"
       branch used to overwrite handle_idx without freeing the outgoing one, so
       every re-registration of one physical pin permanently pinned another
       handle slot; with OEVM_HANDLE_TABLE_MAX==32 (src/handles.h default),
       re-registering pin 13 more than 32 times used to abort "handle table
       full". Looping 40 times here (> capacity) proves the leak is gone: if
       host_gpio_register still leaked, this loop would abort before reaching
       the assertions below. */
    {
        oop out = NULL;
        oop chan = make_string("sensor/leak", 11);
        oop reg_args[4];
        reg_args[1] = TAG_SMI(1);            /* edge (#rising, as SMI) */
        reg_args[3] = chan;                  /* chanPath */
        for (int i = 0; i < 40; i++) {
            reg_args[0] = TAG_SMI(13);        /* same physical pin every time */
            reg_args[2] = TAG_SMI(i);         /* fresh message oop each time */
            assert(platform_dispatch(PRIM_pf_gpioPin, 4, reg_args, &out) == 1
                   && "gpioPin: re-register did not leak a handle slot");
        }

        oop fire_args[1]; fire_args[0] = TAG_SMI(13);
        assert(platform_dispatch(PRIM_pf_gpioFire, 1, fire_args, &out) == 1
               && "gpioFire: dispatched after 40 re-registrations");

        oop pump_args[1] = { NULL };
        assert(platform_dispatch(PRIM_pf_pumpEvents, 0, pump_args, &out) == 1
               && "pumpEvents dispatched");
        assert(out == TAG_SMI(1) && "pumpEvents delivered exactly one event");

        assert(space_take("sensor/leak", 11, NULL) == TAG_SMI(39)
               && "last re-registered value (39) round-trips, not a stale one");
        printf("test_pulse: gpio_reregister_no_handle_leak PASS\n");
    }

    printf("test_pulse: OK\n");
    return 0;
}
