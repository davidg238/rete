/* rete#215 — the detector that converts "no fault" into a fault worth reporting.

   WHY THIS EXISTS. Two changes on the check-in path — CHeapStats fromList: (#192) and one
   line of valueFits: (#213) — have each killed a C6 within six check-ins. Both passed
   every host gate. Both die the same way: no panic, no coredump, no VM Error, no reboot
   banner, console port=up, obs/health frozen. It closes the gap that makes them so
   expensive to chase: the node has no way to say that it stopped.

   BOTH MECHANISMS ARE NOW KNOWN, AND THEY ARE THE SAME ONE (rete#222, 2026-08-20): an
   unbounded probe in dispatch.c's selector→prim-id cache, which spins in C once the table
   is full. This detector found it — by being switched on and then asked WHERE, under
   CONFIG_OEVM_WEDGE_FORENSICS below. The paragraph after this one was written believing
   the reactor was parked in the mailbox, and the measurement refuted that; the reasoning it
   draws about DEADLINES is unaffected and still load-bearing, which is why it stays.

   WHY THE OBVIOUS DETECTOR IS WRONG. pumpEvents blocks on k_msgq_get(K_FOREVER)
   (pulse_zephyr.c:94). That single call is BOTH the healthy idle state — the node has
   nothing to do and is sleeping efficiently, which is the point of the design — AND one
   wedged state. (It was NOT the state #192/#213/#222 were in: a thread waiting in
   k_msgq_get is handed a put directly and the depth stays 0, so the pending>0 those
   wedges report proves nobody was waiting. The argument below still holds — a detector
   that assumes a wake rate would falsely reboot a legitimately slow node — it simply is
   not the case that these three were asleep.) "Has the VM pumped recently?" cannot separate them. It works today only
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
#include "observer_core.h"
#include "pulse_backend.h"
#include "target_backend.h"   /* timer_target_snapshot (invarlint#53) */
#include "platform.h"         /* platform_app_cad_ms (invarlint#48) */

#ifdef CONFIG_TASK_WDT
#include <zephyr/drivers/watchdog.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/task_wdt/task_wdt.h>


/* rete#311: is the VM blocked in the debug read (a breakpoint, or the boot-time
   command loop)? Defined in src/backend/zephyr/platform_zephyr.c beside the read. */
int platform_debug_held(void);

/* rete#269 — OBS_MAX_TIMERS, OBS_WEDGE_SLACK, OBS_MAX_JUDGEABLE_MS, the OBS_COND_*
   enum and the VERDICT ITSELF now live in src/observer_core.h, which is portable C and
   is therefore reachable by tests/test_observer_core.c on the host. This file no longer
   holds a copy of any of them: a decision that exists twice is one that can drift, and
   the drift would be invisible exactly here, since no host gate compiles this TU
   (footprint.sh excludes every source that #includes <zephyr/...>). What stays below is
   everything that ACTS — the thread, the watchdogs, the dump, the reboot. */

/* A sampling resolution, not a deadline. It must sit far below the shortest deadline in
   play; the shortest this bench can produce is the bridge's 8 x 2000 ms, which this
   samples 16 times. */
#define OBSERVER_TICK_MS 1000

/* The backstop for THIS THREAD dying, and nothing else. Deliberately 30x the tick so it
   can never become the operative deadline — if this number ever decides anything, the
   dump will say so, because the wedge path prints a condition and this path prints none. */
#define OBSERVER_WDT_MS 30000

static const struct device *g_hw_wdt;
static int g_wdt_channel = -1;

/* The verdict's state, carried between samples, plus what the dump names afterwards
   (`margin_ms`/`deadline_ms`) — "which one fired, and by how much" is the first fact
   anyone chasing the next wedge will want. Zero at boot, and nothing outside
   observer_core_verdict() writes it. */
static struct ObsCore g_core;

/* rete#269 — a ring of what each observer sample actually saw, printed in the dump
   beside the evidence #222 already preserves. It exists because the reboot destroys the
   only data that can distinguish a wedge from a false trip, and reading the sources could
   not: the depth column and the drained column disagreed, and that disagreement WAS the
   bug. Kept rather than removed — this is the instrument that found the detector was
   rebooting a healthy node, and the next such question will look exactly like this one.
   `fed_at` is recorded BEFORE the verdict runs, so a latch shows on the sample
   AFTER the one that saw an empty mailbox. */
#define OBS_TRACE_N 32
struct ObsTrace { uint32_t up, pend, drn, since; };
static struct ObsTrace g_trace[OBS_TRACE_N];
static uint32_t g_trace_w;                /* monotonic write count; index is w % N */

static void obs_trace_record(void) {
    struct ObsTrace *e = &g_trace[g_trace_w % OBS_TRACE_N];
    e->up    = k_uptime_get_32();
    e->pend  = pulse_pending();
    e->drn   = pulse_drained();
    e->since = g_core.fed_at_ms;
    g_trace_w++;
}

static void obs_trace_dump(void) {
    uint32_t n     = g_trace_w < OBS_TRACE_N ? g_trace_w : OBS_TRACE_N;
    uint32_t start = g_trace_w - n;
    printk("oevm: TRACE %u samples, oldest first: up_ms pending drained fed_at\n",
           (unsigned)n);
    for (uint32_t i = 0; i < n; i++) {
        struct ObsTrace *e = &g_trace[(start + i) % OBS_TRACE_N];
        printk("oevm: T %u %u %u %u\n",
               (unsigned)e->up, (unsigned)e->pend, (unsigned)e->drn, (unsigned)e->since);
    }
    k_busy_wait(30000);   /* the UART FIFO, before anything reboots */
}

#ifdef CONFIG_OEVM_WEDGE_TRIGGER
/* Latched at observer_start(), never recomputed. A trigger that re-armed on the boot that
   REPORTS the previous wedge would reboot the node again mid-report, and the gate would
   see the second reboot as a death inside its observation window — a red that says
   nothing about the detector. Latching on "was a note already pending when this boot
   started?" makes the synthetic wedge exactly one reboot per gate run, regardless of when
   the app acks. */
static bool g_trigger_armed;
#endif

/* Gathers one observation and hands it to the portable decision in
   src/observer_core.h. THE RULE ITSELF IS NOT HERE, deliberately (rete#269) — it is
   pure, so it belongs where tests/test_observer_core.c can sweep it on the host
   instead of where only a board can reach it.

   What remains here is the seam: three target reads — the app's declared timer
   periods, the monotonic drain counter, and the declared app cadence — and the
   clock. */
static int observer_verdict(void) {
    struct ObsSample s;
    s.n = timer_target_snapshot(s.period_ms, s.since_ms, OBS_MAX_TIMERS);
    s.now_ms     = k_uptime_get_32();
    s.drained    = pulse_drained();
    s.app_cad_ms = platform_app_cad_ms();
    s.held       = platform_debug_held();
    return observer_core_verdict(&g_core, &s);
}

#ifdef CONFIG_OEVM_WEDGE_FORENSICS
/* rete#222/#192/#213 — WHERE IS THE REACTOR? Evidence only; this decides nothing and
   reboots nothing.

   The dump above says a wedge happened and the pulse counters say the mailbox is filling.
   What none of it says is what the VM thread is DOING, and the three open wedges have been
   chased for weeks without that fact. Two readings answer it, and both are free:

   `pending=1` ALREADY REFUTES THE STANDING ASSUMPTION. This file's own header, #192 and
   #213 all say the node is parked in `pumpEvents`'s K_FOREVER. It cannot be: a thread
   waiting in k_msgq_get is handed a put directly and the queue depth stays 0, so a depth
   that sits at 1 for sixteen seconds means NOBODY IS WAITING. The reactor is somewhere
   else — inside the tick it took, or blocked on something that is not this mailbox.

   So: sample the VM's own cursors twice, 100 ms apart. `vm_stack_ptr` and `current_frame`
   move on essentially every send, so two identical readings mean the interpreter is not
   advancing (BLOCKED or DEAD), and two different ones mean it is running and will not stop
   (SPINNING). Those are different bugs with different fixes, and nothing in the tree has
   been able to tell them apart. The thread walk then names what it is blocked ON, and
   carries the reactor's Zephyr stack headroom — which is #192's untested VM-stack guess,
   asked at the level that can answer it. */
#include "oevm.h"
#include "oimg.h"
#include "frame_abi.h"
extern oop *vm_stack;
extern size_t vm_stack_ptr;
extern struct MethodContext *current_frame;
extern oop g_active_stack;

/* WHICH THREAD IS BURNING THE CPU. "main is queued, and the VM is not advancing" is
   consistent with two different bugs — the reactor spinning inside a C call that never
   touches the VM cursors, and the reactor being STARVED by something else that never
   yields — and a state string cannot separate them. Cycles can: sample every thread's
   execution_cycles across the same 100 ms window, and exactly one line will have moved. */
#define OBS_MAX_THREADS 16
static const struct k_thread *g_thr[OBS_MAX_THREADS];
static uint64_t g_cyc[OBS_MAX_THREADS];
static int g_thr_n;
static bool g_second_pass;

static void obs_thread_line(const struct k_thread *t, void *ud) {
    ARG_UNUSED(ud);
    k_tid_t tid = (k_tid_t)t;
    k_thread_runtime_stats_t rt;
    uint64_t cyc = (k_thread_runtime_stats_get(tid, &rt) == 0) ? rt.execution_cycles : 0;

    if (!g_second_pass) {
        if (g_thr_n < OBS_MAX_THREADS) { g_thr[g_thr_n] = t; g_cyc[g_thr_n] = cyc; g_thr_n++; }
        return;
    }

    uint64_t before = 0;
    for (int i = 0; i < g_thr_n; i++) if (g_thr[i] == t) { before = g_cyc[i]; break; }

    char st[16] = { 0 };
    size_t unused = 0;
    const char *nm = k_thread_name_get(tid);
    if (k_thread_stack_space_get(t, &unused) != 0) unused = 0;
    printk("oevm:   thr %-14s state=%-10s stack_free=%-6u cycles+=%u\n",
           (nm && nm[0]) ? nm : "?",
           k_thread_state_str(tid, st, sizeof st), (unsigned)unused,
           (unsigned)(cyc - before));
}

/* THE FROZEN CALL STACK, in mprof's own currency. A CompiledMethod carries no name, so
   this prints the BLOB-RELATIVE OFFSET vm.c:530 already established as the portable key —
   `custos profile -map <image>.oimg.map.json` names it, and the pc index joins the
   same map's line table to reach the nsl line. Walked ONLY when the cursors are not advancing: reading a
   live frame chain from another thread would be a race, and a garbage answer here would
   look exactly like a real one. */
static void obs_dump_frames(void) {
    struct MethodContext *f = current_frame;
    for (int depth = 0; f != NULL && depth < 12; depth++) {
        const void *m = f->closure ? (const void *)closure_entry((struct Closure *)f->closure)
                                   : NULL;
        long pc = -1;
        if (m != NULL) {
            const struct CompiledMethod *cm = (const struct CompiledMethod *)m;
            if (f->saved_pc != NULL && cm->instructions != NULL)
                pc = (long)(f->saved_pc - cm->instructions);
        }
        if (m != NULL && is_in_blob(m))
            printk("oevm:   frame %2d method=%u pc=%ld\n", depth,
                   (unsigned)((const uint8_t *)m - oimg_blob_base), pc);
        else
            printk("oevm:   frame %2d method=@%p pc=%ld\n", depth, m, pc);
        f = frame_link_decode(vm_stack, f->prev_frame);
    }
}

/* A SAMPLING PROFILE OF A WEDGED VM, and a look inside the collector.

   READS SCALARS AND DEREFERENCES NOTHING, deliberately. The first version of this resolved
   each sample's method through `f->closure` and it KILLED THE OBSERVER THREAD: between
   samples the reactor is live, so the oop is torn out from under the walk — and losing the
   observer loses the reboot with it, which cost a boot to the 30 s task_wdt backstop. Two
   scalars are enough: a VM that completes even ONE send moves both `vm_stack_ptr` and the
   `current_frame` pointer, so if neither moves across 24 ms at 100% CPU, no send is
   completing at all and the reactor is below the interpreter, in C.

   `gc_forensics` then asks the one C loop that could plausibly be the culprit whether it
   is running. An unterminating mark phase and an interpreter loop are indistinguishable
   from outside — same 100% CPU, same frozen frame, same silence.

   The selector-cache occupancy this also used to print is gone with the cache itself
   (rete#226): a Symbol now carries its own prim id, so there is no table to be full. */
extern void gc_forensics(int *in_progress, unsigned *msp, int *overflowed);
#define OBS_SAMPLES 24
static void obs_sample_top(void) {
    unsigned sp[OBS_SAMPLES];
    void *fr[OBS_SAMPLES];
    int gc_in = 0, gc_ovf = 0;
    unsigned gc_msp = 0;
    for (int i = 0; i < OBS_SAMPLES; i++) {
        sp[i] = (unsigned)vm_stack_ptr;
        fr[i] = (void *)current_frame;
        k_msleep(1);
    }
    unsigned moved_sp = 0, moved_fr = 0;
    for (int i = 1; i < OBS_SAMPLES; i++) {
        if (sp[i] != sp[0]) moved_sp++;
        if (fr[i] != fr[0]) moved_fr++;
    }
    gc_forensics(&gc_in, &gc_msp, &gc_ovf);
    printk("oevm:   sampled %dx1ms sp_differs=%u frame_differs=%u | gc_in_progress=%d "
           "mark_sp=%u mark_overflowed=%d\n",
           OBS_SAMPLES, moved_sp, moved_fr, gc_in, gc_msp, gc_ovf);
}

static void observer_forensics(void) {
    size_t sp1 = vm_stack_ptr;
    void *cf1  = (void *)current_frame;
    g_thr_n = 0; g_second_pass = false;
    k_thread_foreach_unlocked(obs_thread_line, NULL);   /* pass 1: cycles at t0 */
    k_sleep(K_MSEC(100));
    size_t sp2 = vm_stack_ptr;
    void *cf2  = (void *)current_frame;
    size_t cap = g_active_stack ? ((struct Stack *)g_active_stack)->capacity : 0;
    /* NOT proof on its own: a tight loop could return to the same frame at the same depth
       between samples. It is the thread state below that makes it conclusive — a thread
       that is `pending` is not looping, and one that is `ready`/`running` is not blocked. */
    bool frozen = (sp1 == sp2 && cf1 == cf2);
    printk("oevm: FORENSICS reactor=%s vm_sp=%u->%u cap=%u frame=%p->%p\n",
           frozen ? "NOT-ADVANCING" : "ADVANCING",
           (unsigned)sp1, (unsigned)sp2, (unsigned)cap, cf1, cf2);
    g_second_pass = true;
    k_thread_foreach_unlocked(obs_thread_line, NULL);
    if (frozen) obs_dump_frames();
    obs_sample_top();
}
#else
static void observer_forensics(void) { }
#endif

/* The crime scene. It goes to the console, which the collector already persists — so the
   evidence survives the reboot in ~/.local/state/rete/console/ AND the node still
   recovers. That was the deciding property: the field-correct behaviour is also the one
   that keeps the evidence.

   The counters come from pulse_backend.h directly — the same single source
   PRIM_pf_sysStats reads (platform.c:1439-1445), so the console dump and sys/metrics can
   never disagree about pdr/pco/pst. */
static void observer_dump(int cond) {
    printk("oevm: WEDGE cond=%s overdue=%ums deadline=%ums appCad=%ums up=%ums "
           "pending=%u pdr=%u pco=%u pst=%u consec=%u\n",
           cond == OBS_COND_TIMER_OVERDUE ? "timer-overdue" : "app-stale",
           (unsigned)g_core.margin_ms, (unsigned)g_core.deadline_ms,
           (unsigned)platform_app_cad_ms(), (unsigned)k_uptime_get_32(),
           (unsigned)pulse_pending(), (unsigned)pulse_drops(),
           (unsigned)pulse_coalesced(), (unsigned)pulse_stale(),
           (unsigned)reboot_note_consecutive() + 1u);
    obs_trace_dump();   /* rete#269 */
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
    printk("oevm: HALT — 3 consecutive faults with no check-in between (2026-08-10 §5.2). "
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
#ifdef CONFIG_OEVM_WEDGE_TRIGGER
        /* The synthetic wedge: after the configured delay, stop answering OBS_COND_NONE. It
           forces the VERDICT rather than faking the mailbox — faking the mailbox would
           test the trigger and not the path the verdict takes. */
        if (g_trigger_armed &&
            k_uptime_get_32() > (uint32_t)CONFIG_OEVM_WEDGE_TRIGGER_S * 1000u) {
            g_trigger_armed = false;
            g_core.deadline_ms = 0; g_core.margin_ms = 0;
            observer_dump(OBS_COND_APP_STALE);
            reboot_note_write(RN_REASON_WATCHDOG, 0, 0, 0, k_uptime_get_32());
            if (reboot_note_consecutive() >= 3) observer_halt();
            sys_reboot(SYS_REBOOT_COLD);
        }
#endif
        obs_trace_record();   /* rete#269: before the verdict reads the same state */
        int cond = observer_verdict();
        if (cond == OBS_COND_NONE) {
            if (g_wdt_channel >= 0) (void)task_wdt_feed(g_wdt_channel);
        } else {
            observer_dump(cond);
            observer_forensics();   /* rete#222: evidence, before the reboot destroys it */
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
        printk("oevm: observer — task_wdt_init failed, wedge detection is OFF\n");
        return;
    }
#ifdef CONFIG_OEVM_WEDGE_TRIGGER
    g_trigger_armed = (reboot_note_consecutive() == 0);
    printk("oevm: observer — SYNTHETIC WEDGE TRIGGER is compiled in, %s\n",
           g_trigger_armed ? "armed" : "disarmed (a note is already pending)");
#endif
    printk("oevm: observer armed (slack=%u tick=%ums backstop=%ums hw=%s)\n",
           (unsigned)OBS_WEDGE_SLACK, (unsigned)OBSERVER_TICK_MS, (unsigned)OBSERVER_WDT_MS,
           g_hw_wdt ? "yes" : "no");
    k_thread_create(&observer_thread, observer_stack,
                    K_THREAD_STACK_SIZEOF(observer_stack),
                    observer_thread_fn, NULL, NULL, NULL,
                    K_PRIO_COOP(7), 0, K_NO_WAIT);
    k_thread_name_set(&observer_thread, "oevm_observer");
}

#else  /* !CONFIG_TASK_WDT */

/* Boards without the task watchdog subsystem — the nRF52840 DK/dongle and native_sim
   today. The SYMBOLS must exist and link on every Zephyr board (the bell/ntp/rtc rule), so
   the guard is on the BODIES and never on the definitions. observer_start says out loud
   that the node is unwatched rather than leaving it to be inferred from silence. */
void observer_start(void) {
    printk("oevm: observer — CONFIG_TASK_WDT is not enabled, wedge detection is OFF\n");
}
void observer_halt(void) {
    printk("oevm: HALT requested with no watchdog subsystem — parking.\n");
    (void)irq_lock();
    for (;;) { }
}

#endif /* CONFIG_TASK_WDT */
