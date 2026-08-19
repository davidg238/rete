/* rete#215 — the detector that converts "no fault" into a fault worth reporting.

   WHY THIS EXISTS. Two changes on the check-in path — CHeapStats fromList: (#192) and one
   line of valueFits: (#213) — have each killed a C6 within six check-ins. Both passed
   every host gate. Both die the same way: no panic, no coredump, no VM Error, no reboot
   banner, console port=up, obs/health frozen. Neither mechanism is understood and this
   file does not try to explain them. It closes the gap that makes them so expensive to
   chase: the node has no way to say that it stopped.

   WHY THE OBVIOUS DETECTOR IS WRONG. pumpEvents blocks on k_msgq_get(K_FOREVER)
   (pulse_zephyr.c:94). That single call is BOTH the healthy idle state — the node has
   nothing to do and is sleeping efficiently, which is the point of the design — AND the
   wedged state. "Has the VM pumped recently?" cannot separate them. It works today only
   because the bridge happens to hold a 2 s timer, and it would falsely reboot the first
   node whose app legitimately sleeps for ten minutes. So THE DETECTOR CARRIES NO IDEA OF
   HOW OFTEN A NODE SHOULD WAKE. It asks the app, through timer_target_snapshot().

   WHY A THREAD, AND WHY task_wdt SITS BEHIND IT RATHER THAN IN FRONT. task_wdt_feed()
   calls k_sched_lock() (task_wdt.c:240), which asserts !arch_is_in_isr()
   (kernel/sched.c:774) — and CONFIG_ASSERT is OFF in this build, so an ISR-context feed
   would silently decrement the interrupted thread's sched_locked. The feed must therefore
   happen in thread context, and the only thread that could otherwise do it is the reactor,
   which is the thing that wedges. Given a thread, the verdict is better taken in it:
   printk and sys_reboot from thread context are the ordinary paths. That leaves the
   task_wdt channel free to be the backstop for THIS THREAD dying — a genuinely different
   failure — with the C6 hardware WDT behind that for the kernel itself dying.

   COOPERATIVE PRIORITY IS LOAD-BEARING. At a preemptible priority this thread would never
   be scheduled while the VM spins in the interpreter, so the trip would come from a missed
   feed against a FIXED channel period — which is exactly the global "how often should a
   node wake" constant the design forbids. Cooperative priority keeps the app's own
   declared periods the operative deadline. */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <stdbool.h>
#include "reboot_note.h"
#include "pulse_backend.h"

#ifdef CONFIG_TASK_WDT
#include <zephyr/drivers/watchdog.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/task_wdt/task_wdt.h>

/* Declared across translation units the way platform.c declares bell/ntp/rtc — the tree's
   idiom for a target leaf that lives in another file. Defined in
   src/backend/zephyr/timer_zephyr.c. */
int timer_target_snapshot(uint32_t *period_ms, uint32_t *since_ms, int max);

#define OBS_MAX_TIMERS 8    /* == OEVM_MAX_TARGET_TIMERS */

/* The ONE invented constant of the design (§3.3), shipped at 8. Deliberately generous:
   the cost of a false reboot is far higher than the cost of a slow detection, and the
   measured worst-case pushBuild is 132 ms against a 2 s period. It is falsifiable BY
   CONSTRUCTION — a wrong SLACK produces a reboot whose dump says exactly how overdue it
   thought the node was, and a constant that announces its own error when wrong is worth
   more than one argued to be right in advance. */
#define WEDGE_SLACK 8u

/* A sampling resolution, not a deadline. It must sit far below the shortest deadline in
   play; the shortest this bench can produce is the bridge's 8 x 2000 ms, which this
   samples 16 times. */
#define OBSERVER_TICK_MS 1000

/* The backstop for THIS THREAD dying, and nothing else. Deliberately 30x the tick so it
   can never become the operative deadline — if this number ever decides anything, the
   dump will say so, because the wedge path prints a condition and this path prints none. */
#define OBSERVER_WDT_MS 30000

/* A deadline longer than this cannot be judged at all: `since_ms` comes from
   k_uptime_get_32(), which wraps at ~49.7 days, so a comparison against a larger number is
   not merely imprecise, it is unsound — and it would fail toward a FALSE TRIP, the one
   failure mode §8 says would make this instrument worse than nothing. Such a timer is
   skipped and said to be skipped. */
#define OBS_MAX_JUDGEABLE_MS 0x7FFFFFFFull

enum { COND_NONE = 0, COND_TIMER_OVERDUE, COND_PULSE_UNDRAINED };

static const struct device *g_hw_wdt;
static int g_wdt_channel = -1;

/* Kept so the dump can name the margin rather than merely the condition — "which one
   fired, and by how much" is the first fact anyone chasing the next wedge will want. */
static uint32_t g_margin_ms;
static uint32_t g_deadline_ms;

#ifdef CONFIG_TUVM_WEDGE_TRIGGER
/* Latched at observer_start(), never recomputed. A trigger that re-armed on the boot that
   REPORTS the previous wedge would reboot the node again mid-report, and the gate would
   see the second reboot as a death inside its observation window — a red that says
   nothing about the detector. Latching on "was a note already pending when this boot
   started?" makes the synthetic wedge exactly one reboot per gate run, regardless of when
   the app acks. */
static bool g_trigger_armed;
#endif

/* Answers COND_* and fills g_margin_ms / g_deadline_ms. Both conditions derive their
   deadline from state the app already declared; nothing global is assumed.

   They are kept as TWO NAMED CONDITIONS rather than one merged predicate because the dump
   reports which one fired. Neither alone covers both wedge shapes. */
static int observer_verdict(void) {
    uint32_t period[OBS_MAX_TIMERS], since[OBS_MAX_TIMERS];
    int n = timer_target_snapshot(period, since, OBS_MAX_TIMERS);
    if (n == 0) return COND_NONE;   /* the app declared no cadence: nothing to judge it
                                       against, and guessing one is what §2 forbids */

    /* §3.1 — an armed repeating timer is overdue against ITS OWN period. Catches the
       timer subsystem itself stopping: the ISR no longer firing at all. A node with a
       ten-minute timer is judged against ten minutes; one with a 2 s timer against 2 s. */
    uint32_t shortest = 0;
    for (int i = 0; i < n; i++) {
        uint64_t deadline = (uint64_t)period[i] * WEDGE_SLACK;
        if (deadline > OBS_MAX_JUDGEABLE_MS) continue;   /* unjudgeable; see the note above */
        if (shortest == 0 || period[i] < shortest) shortest = period[i];
        if (since[i] > (uint32_t)deadline) {
            g_deadline_ms = (uint32_t)deadline;
            g_margin_ms   = since[i] - (uint32_t)deadline;
            return COND_TIMER_OVERDUE;
        }
    }
    if (shortest == 0) return COND_NONE;   /* every declared timer was unjudgeable */

    /* §3.2 — the ISR is firing and the mailbox is filling, but the VM is not draining it:
       a VM stuck in the interpreter rather than blocked at the mailbox. Judged against the
       SHORTEST declared period, because that is the fastest cadence the app claims to
       service. */
    {
        static uint32_t undrained_since_ms;   /* 0 == not currently undrained */
        uint32_t deadline = shortest * WEDGE_SLACK;
        uint32_t now = k_uptime_get_32();
        if (pulse_pending() == 0) {
            undrained_since_ms = 0;
        } else {
            if (undrained_since_ms == 0) undrained_since_ms = now ? now : 1u;
            uint32_t held = now - undrained_since_ms;
            if (held > deadline) {
                g_deadline_ms = deadline;
                g_margin_ms   = held - deadline;
                return COND_PULSE_UNDRAINED;
            }
        }
    }
    return COND_NONE;
}

/* The crime scene. It goes to the console, which the collector already persists — so the
   evidence survives the reboot in ~/.local/state/rete/console/ AND the node still
   recovers. That was the deciding property: the field-correct behaviour is also the one
   that keeps the evidence.

   The counters come from pulse_backend.h directly — the same single source
   PRIM_pf_sysStats reads (platform.c:1439-1445), so the console dump and sys/metrics can
   never disagree about pdr/pco/pst. */
static void observer_dump(int cond) {
    printk("tuvm: WEDGE cond=%s overdue=%ums deadline=%ums up=%ums "
           "pending=%u pdr=%u pco=%u pst=%u consec=%u\n",
           cond == COND_TIMER_OVERDUE ? "timer-overdue" : "pulse-undrained",
           (unsigned)g_margin_ms, (unsigned)g_deadline_ms, (unsigned)k_uptime_get_32(),
           (unsigned)pulse_pending(), (unsigned)pulse_drops(),
           (unsigned)pulse_coalesced(), (unsigned)pulse_stale(),
           (unsigned)reboot_note_consecutive() + 1u);
}

/* §5.2's HALT arm. THE HARDWARE WATCHDOG MUST BE DISABLED FIRST, and this is not in either
   design document: task_wdt_init() installs a hardware timeout with WDT_FLAG_RESET_SOC
   (task_wdt.c:135-138). A halted node feeds nothing, so the SoC would reset ~11 s later and
   the loop breaker's "clean, findable failure" would silently become the reboot loop it
   exists to stop — with no console line saying why.

   Safe to call from the fatal handler in ISR context: wdt_disable and printk are, and
   nothing after the busy-wait needs a scheduler. */
void observer_halt(void) {
    if (g_hw_wdt) (void)wdt_disable(g_hw_wdt);
    printk("tuvm: HALT — 3 consecutive faults with no check-in between (2026-08-10 §5.2). "
           "The note is kept. gw/liveness is watching (#217).\n");
    k_busy_wait(50000);   /* let the UART FIFO drain before interrupts go away */
    (void)irq_lock();
    for (;;) { /* a clean, findable failure — not a noisy zombie */ }
}

static void observer_thread_fn(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

    g_wdt_channel = task_wdt_add(OBSERVER_WDT_MS, NULL, NULL);
    /* callback NULL == sys_reboot(SYS_REBOOT_COLD) from task_wdt itself
       (task_wdt.c:121-125). That is the right handling for "this thread stopped": there is
       nobody left to write a note, and a reboot with no note is still better than a node
       that is silently gone. The wedge path below never relies on it. */

    for (;;) {
#ifdef CONFIG_TUVM_WEDGE_TRIGGER
        /* The synthetic wedge: after the configured delay, stop answering COND_NONE. It
           forces the VERDICT rather than faking the mailbox — faking the mailbox would
           test the trigger and not the path the verdict takes. */
        if (g_trigger_armed &&
            k_uptime_get_32() > (uint32_t)CONFIG_TUVM_WEDGE_TRIGGER_S * 1000u) {
            g_trigger_armed = false;
            g_deadline_ms = 0; g_margin_ms = 0;
            observer_dump(COND_PULSE_UNDRAINED);
            reboot_note_write(RN_REASON_WATCHDOG, 0, 0, 0, k_uptime_get_32());
            if (reboot_note_consecutive() >= 3) observer_halt();
            sys_reboot(SYS_REBOOT_COLD);
        }
#endif
        int cond = observer_verdict();
        if (cond == COND_NONE) {
            if (g_wdt_channel >= 0) (void)task_wdt_feed(g_wdt_channel);
        } else {
            observer_dump(cond);
            reboot_note_write(RN_REASON_WATCHDOG, 0, 0, 0, k_uptime_get_32());
            if (reboot_note_consecutive() >= 3) observer_halt();   /* does not return */
            sys_reboot(SYS_REBOOT_COLD);                           /* does not return */
        }
        k_sleep(K_MSEC(OBSERVER_TICK_MS));
    }
}

/* K_PRIO_COOP(7): see the header comment. It must run when it wakes regardless of what the
   reactor is doing, and its whole body is a handful of comparisons, so it yields promptly
   and starves nothing.

   2048 rather than the 1024 this was first written with, and the margin is deliberate.
   The frame holds two 8-element uint32 arrays (64 B) plus a printk of nine varargs, and
   Zephyr's cbprintf is not cheap on stack. A stack overflow ANYWHERE is bad; one in the
   watchdog thread would mean the instrument that exists to report a silent death dies
   silently itself, and CONFIG_ASSERT is off in this build so nothing would say so. 1 KB
   of extra SRAM against sram0_0_seg at 72.28% is not a cost worth optimising. */
K_THREAD_STACK_DEFINE(observer_stack, 2048);
static struct k_thread observer_thread;

void observer_start(void) {
#if DT_NODE_HAS_STATUS(DT_ALIAS(watchdog0), okay)
    g_hw_wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
    if (!device_is_ready(g_hw_wdt)) g_hw_wdt = NULL;
#else
    g_hw_wdt = NULL;
#endif
    if (task_wdt_init(g_hw_wdt) != 0) {
        /* Loud, and NOT fatal: a node with no watchdog is a node with no wedge detector,
           which is the state every build was in before this one — but it must not be the
           state a build is in SILENTLY. */
        printk("tuvm: observer — task_wdt_init failed, wedge detection is OFF\n");
        return;
    }
#ifdef CONFIG_TUVM_WEDGE_TRIGGER
    g_trigger_armed = (reboot_note_consecutive() == 0);
    printk("tuvm: observer — SYNTHETIC WEDGE TRIGGER is compiled in, %s\n",
           g_trigger_armed ? "armed" : "disarmed (a note is already pending)");
#endif
    printk("tuvm: observer armed (slack=%u tick=%ums backstop=%ums hw=%s)\n",
           (unsigned)WEDGE_SLACK, (unsigned)OBSERVER_TICK_MS, (unsigned)OBSERVER_WDT_MS,
           g_hw_wdt ? "yes" : "no");
    k_thread_create(&observer_thread, observer_stack,
                    K_THREAD_STACK_SIZEOF(observer_stack),
                    observer_thread_fn, NULL, NULL, NULL,
                    K_PRIO_COOP(7), 0, K_NO_WAIT);
    k_thread_name_set(&observer_thread, "tuvm_observer");
}

#else  /* !CONFIG_TASK_WDT */

/* Boards without the task watchdog subsystem — the nRF52840 DK/dongle and native_sim
   today. The SYMBOLS must exist and link on every Zephyr board (the bell/ntp/rtc rule), so
   the guard is on the BODIES and never on the definitions. observer_start says out loud
   that the node is unwatched rather than leaving it to be inferred from silence. */
void observer_start(void) {
    printk("tuvm: observer — CONFIG_TASK_WDT is not enabled, wedge detection is OFF\n");
}
void observer_halt(void) {
    printk("tuvm: HALT requested with no watchdog subsystem — parking.\n");
    (void)irq_lock();
    for (;;) { }
}

#endif /* CONFIG_TASK_WDT */
