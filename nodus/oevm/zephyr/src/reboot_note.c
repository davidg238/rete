/* rete#215 — the retained panic note on target, plus the fatal-error seam that fills it.
   2026-08-10 §5.1 chose `.rtc_noinit` over CONFIG_RETAINED_MEM and that choice is adopted
   verbatim; g_rtc in main.c:285 is the second in-tree user, so the section is already
   proven here.

   THE FATAL SEAM IS ONLY HALF THE PROBLEM, and it is the half that already worked. A
   wedge raises no fatal error, so this handler never runs for #192/#213 — that is the
   whole reason rete#215 exists. What lands here is the OTHER reasons: a CPU exception, a
   kernel oops, a stack-check failure. The wedge producer is observer.c.

   k_sys_fatal_error_handler is __weak in kernel/fatal.c:37 and oevm overrides nothing
   today, so this replaces nothing. It MUST NOT RETURN: Zephyr's default halts, and
   returning from a fatal handler is undefined. */
#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <zephyr/sys/reboot.h>
#include <string.h>
#include "reboot_note.h"

#include "observer.h"   /* observer_halt (invarlint#48) */

#ifdef CONFIG_RISCV
#include <zephyr/arch/riscv/csr.h>
#endif

#ifdef CONFIG_SOC_FAMILY_ESPRESSIF_ESP32
#include <esp_attr.h>   /* RTC_NOINIT_ATTR — .rtc_noinit is NOLOAD, so it survives a reset */
static RTC_NOINIT_ATTR struct RebootNote g_note;
#else
/* Every other board: plain .noinit. It does not survive a power cycle and on some parts
   does not survive a reset either — which costs those boards the report, not the node.
   The guard is on the STORAGE ATTRIBUTE, never on the definitions: platform.c externs
   these five symbols and they must link on every Zephyr board. */
static __noinit struct RebootNote g_note;
#endif

void reboot_note_init(void) {
    if (!reboot_note_is_valid(&g_note)) memset(&g_note, 0, sizeof g_note);
}

int reboot_note_read(struct RebootNote *out) {
    if (!reboot_note_is_valid(&g_note)) return 0;
    *out = g_note;
    return 1;
}

void reboot_note_write(uint32_t reason, uint32_t mcause,
                       uint32_t mepc, uint32_t ra, uint32_t mono_ms) {
    uint32_t prev = reboot_note_is_valid(&g_note) ? g_note.consecutive : 0;
    /* boot_id is read UNGUARDED, and that is deliberate — see the host twin in
       src/platform.c. reboot_note_ack() clears the magic while KEEPING the id, so the
       block is legitimately "no report, known identity"; a validity-guarded read would
       take 0 there and every post-ack death would report an unknown boot.
       reboot_note_init() zeroing the whole block on garbage is what makes it safe. */
    uint32_t boot = g_note.boot_id;
    g_note.magic = RN_MAGIC;
    g_note.reason = reason; g_note.mcause = mcause;
    g_note.mepc = mepc;     g_note.ra = ra;
    g_note.boot_id = boot;  g_note.mono_ms = mono_ms;
    g_note.consecutive = prev + 1;
    g_note.sum = reboot_note_checksum(&g_note);
}

void reboot_note_ack(uint32_t boot_id) {
    memset(&g_note, 0, sizeof g_note);
    g_note.boot_id = boot_id;
}

uint32_t reboot_note_consecutive(void) {
    return reboot_note_is_valid(&g_note) ? g_note.consecutive : 0;
}

/* Zephyr's K_ERR_* to Panic.reason. Anything unrecognised becomes #hardFault rather than
   a sixth symbol: RECORDS §2.4 pins the enumeration, and a reason the gateway cannot
   name is worth less than an imprecise one it can. */
static uint32_t rn_reason_for(unsigned int k_err) {
    switch (k_err) {
    case K_ERR_STACK_CHK_FAIL: return RN_REASON_STACK_OVERFLOW;
    case K_ERR_KERNEL_OOPS:
    case K_ERR_KERNEL_PANIC:   return RN_REASON_ASSERT;
    default:                   return RN_REASON_HARD_FAULT;
    }
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf) {
    /* mcause from the CSR, NOT from the esf: struct arch_esf carries mcause only under
       CONFIG_CLIC_SUPPORT_INTERRUPT_LEVEL (include/zephyr/arch/riscv/exception.h:81),
       which is unset on the C6. The CSR still holds the trap cause here — nothing has
       re-trapped between the fault and this handler. */
#ifdef CONFIG_RISCV
    uint32_t mcause = (uint32_t)csr_read(mcause);
    uint32_t mepc = esf ? (uint32_t)esf->mepc : 0u;
    uint32_t ra   = esf ? (uint32_t)esf->ra   : 0u;
#else
    /* Non-RISCV Zephyr boards (the nRF52840 DK/dongle, native_sim) reach this file too —
       the note is architecture-neutral and the register triple simply reads 0 there,
       which is honest rather than wrong: 0 means unknown here exactly as it does for
       boot_id. */
    uint32_t mcause = 0u, mepc = 0u, ra = 0u;
    ARG_UNUSED(esf);
#endif

    reboot_note_write(rn_reason_for(reason), mcause, mepc, ra,
                      (uint32_t)k_uptime_get_32());

    /* The dump goes to the console, which the collector already persists — so the crime
       scene survives the reboot in ~/.local/state/rete/console/ even though the node
       recovers. printk, not printf: the fault path gags newlib stdio. */
    printk("oevm: FATAL reason=%u mcause=0x%08x mepc=0x%08x ra=0x%08x up=%ums consec=%u\n",
           reason, (unsigned)mcause, (unsigned)mepc, (unsigned)ra,
           (unsigned)k_uptime_get_32(), (unsigned)reboot_note_consecutive());

    /* §5.2's loop breaker. Safe to ship only because gw/liveness is watching (built and
       verified 2026-08-18, #217) — without it this would trade a noisy zombie for an
       invisible corpse. observer_halt() disables the hardware watchdog first; see the
       comment there for why that is not optional. */
    if (reboot_note_consecutive() >= 3) {
        printk("oevm: 3 consecutive faults with no check-in between — HALT\n");
        observer_halt();   /* does not return */
    }
    sys_reboot(SYS_REBOOT_COLD);
    CODE_UNREACHABLE;
}
