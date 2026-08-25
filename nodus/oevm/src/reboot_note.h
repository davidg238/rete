#ifndef OEVM_REBOOT_NOTE_H
#define OEVM_REBOOT_NOTE_H
/* The retained panic note — rete#215, adopting 2026-08-10 §5.1 verbatim.

   A node that dies must still be able to say what killed it, and the report has to
   survive the reboot that recovers it. On ESP32 that is `.rtc_noinit` (RTC_NOINIT_ATTR),
   a NOLOAD section which survives both deep sleep and a restart, and which the MagTag
   deep-sleep counter (zephyr/src/main.c:285) already proves in-tree. CONFIG_RETAINED_MEM
   was rejected in §5.1: a driver, a Kconfig and a devicetree edit to store twenty-odd
   bytes.

   THE LOGIC LIVES IN THIS HEADER AND THE STORAGE DOES NOT. Retention differs per side
   (RTC_NOINIT_ATTR on target, a plain static on the host) but the validity RULE must
   not — a rule that is written twice is a rule that will diverge, and the divergence
   would be invisible until a real death. Header-only also means neither the nine engine
   source lists (#63) nor the posix backend gain a file for logic nobody links twice.

   VALIDITY IS MAGIC *AND* CHECKSUM. Magic alone is not enough: uninitialised RTC RAM is
   whatever the last program left there, and a block that happens to open with the magic
   word would be reported as a panic that never happened — inventing a death is worse
   than missing one, because it is unfalsifiable from the gateway. */
#include <stdint.h>
#include <string.h>

/* Maps to Panic.reason (PROTOCOL-NSL-RECORDS.md §2.4). Small ints rather than strings:
   the C side must write this from a fatal handler that may not allocate, and the symbol
   lookup belongs in nsl where the record is built. Append-only. */
#define RN_REASON_WATCHDOG       1u   /* #watchdog       — the wedge detector (rete#215) */
#define RN_REASON_HARD_FAULT     2u   /* #hardFault      — K_ERR_CPU_EXCEPTION and friends */
#define RN_REASON_ASSERT         3u   /* #assert         — K_ERR_KERNEL_OOPS / _PANIC */
#define RN_REASON_STACK_OVERFLOW 4u   /* #stackOverflow  — K_ERR_STACK_CHK_FAIL */
#define RN_REASON_OOM            5u   /* #oom            — reserved; no producer yet */

#define RN_MAGIC 0x52424E31u   /* "RBN1" */

struct RebootNote {
    uint32_t magic;        /* else uninitialised RAM reads as a panic report */
    uint32_t sum;          /* checksum — garbage must not become a report */
    uint32_t reason;       /* RN_REASON_* above; maps to Panic.reason */
    uint32_t mcause;       /* csr_read(mcause) — NOT esf->mcause, see the plan's D-P3 */
    uint32_t mepc;
    uint32_t ra;
    uint32_t boot_id;      /* the boot this node was RUNNING under; 0 means unknown and is
                              never invented into a number (the #217 §6.4 convention) */
    uint32_t mono_ms;      /* uptime at death */
    uint32_t consecutive;  /* §5.2's loop breaker — survives the reboot BY DESIGN */
};

/* Sum every word after `sum` itself, plus the magic. Deliberately trivial: this runs in a
   fatal handler, where anything that can fail must not be here. */
static inline uint32_t reboot_note_checksum(const struct RebootNote *n) {
    return n->magic + n->reason + n->mcause + n->mepc + n->ra
         + n->boot_id + n->mono_ms + n->consecutive + 0x9E3779B9u;
}

static inline int reboot_note_is_valid(const struct RebootNote *n) {
    return (n->magic == RN_MAGIC) && (n->sum == reboot_note_checksum(n));
}

/* Called ONCE at boot, before anything can read the note. Validates the retained block
   and zeroes it whole if it is garbage — so every field below, `boot_id` included, is a
   known value from here on rather than whatever the last program left in RTC RAM. */
void reboot_note_init(void);

/* 1 and fills *out when a report is PENDING (a death that has not yet been acked); 0
   otherwise. Reading NEVER clears: a read that clears loses the report to any transient
   that reads it first, and the app may need to try more than once to publish. */
int reboot_note_read(struct RebootNote *out);

/* Fill and stamp a death. Increments `consecutive` (from 0 if the block held no valid
   note) and carries forward whatever `boot_id` the last ack deposited. Must be callable
   from a fatal handler: no allocation, no logging, no locks. */
void reboot_note_write(uint32_t reason, uint32_t mcause,
                       uint32_t mepc, uint32_t ra, uint32_t mono_ms);

/* The app's "I got far enough to report" signal — §5.2's loop-breaker reset and §5.1's
   report-once clear in one call, because they are the same moment (plan D-P6). Clears
   the pending report and `consecutive`, and deposits THIS boot's id so a later death in
   this boot carries the id it died under. */
void reboot_note_ack(uint32_t boot_id);

/* The loop breaker's live count, for the observer's HALT decision and the dump. */
uint32_t reboot_note_consecutive(void);
#endif /* OEVM_REBOOT_NOTE_H */
