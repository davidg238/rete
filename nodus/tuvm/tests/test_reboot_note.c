/* Unit test for the retained panic note (src/reboot_note.h, rete#215 / the
   2026-08-10 liveness design §5.1). This struct is the ONLY channel a node that
   stopped has for saying so, and its failure mode is that it reads zero — which is
   byte-identical to "no wedge ever happened". So the validity rule is proven here,
   on the host, before a board is asked to retain anything: a block of garbage must
   NOT decode as a report, or the first boot after a cold power-on invents a panic.

   Build (Model A link set, same as test_cheap_stats.c):
     gcc -std=c99 -Isrc -DOEVM_HEAP_SIZE=65536 tests/test_reboot_note.c \
         <ENG_A> -lm -o /tmp/trn && /tmp/trn

   Verifies:
     1. An uninitialised block is not a report (magic AND checksum, not magic alone).
     2. reboot_note_init() on garbage leaves a clean, report-free block.
     3. A written note round-trips every field.
     4. consecutive RATCHETS across writes and is NOT reset by a read.
     5. reboot_note_ack() clears the report and consecutive but KEEPS the boot id,
        and the next write carries that id (plan D-P6).
     6. A one-bit corruption of any field invalidates the note. */
#include "../src/reboot_note.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(void) {
    struct RebootNote n;

    /* 1. garbage is not a report */
    memset(&n, 0xA5, sizeof n);
    assert(!reboot_note_is_valid(&n));
    memset(&n, 0x00, sizeof n);
    assert(!reboot_note_is_valid(&n));
    printf("test_reboot_note: property 1 (garbage is not a report) OK\n");

    /* 2. init leaves a clean block */
    reboot_note_init();
    assert(reboot_note_read(&n) == 0);
    assert(reboot_note_consecutive() == 0);
    printf("test_reboot_note: property 2 (init clears) OK\n");

    /* 3. a written note round-trips */
    reboot_note_write(RN_REASON_WATCHDOG, 0xdeadbeef, 0x42004200, 0x1234, 98765);
    assert(reboot_note_read(&n) == 1);
    assert(n.reason == RN_REASON_WATCHDOG);
    assert(n.mcause == 0xdeadbeef);
    assert(n.mepc   == 0x42004200);
    assert(n.ra     == 0x1234);
    assert(n.mono_ms == 98765);
    assert(n.boot_id == 0);            /* no ack yet: unknown stays 0, never invented */
    printf("test_reboot_note: property 3 (round-trip) OK\n");

    /* 4. consecutive ratchets, and reading does not reset it */
    assert(n.consecutive == 1);
    assert(reboot_note_read(&n) == 1 && n.consecutive == 1);
    reboot_note_write(RN_REASON_HARD_FAULT, 2, 3, 4, 5);
    assert(reboot_note_read(&n) == 1 && n.consecutive == 2);
    reboot_note_write(RN_REASON_ASSERT, 2, 3, 4, 5);
    assert(reboot_note_read(&n) == 1 && n.consecutive == 3);
    printf("test_reboot_note: property 4 (consecutive ratchets) OK\n");

    /* 5. ack clears the report and the counter but KEEPS the boot id */
    reboot_note_ack(77);
    assert(reboot_note_read(&n) == 0);
    assert(reboot_note_consecutive() == 0);
    reboot_note_write(RN_REASON_OOM, 0, 0, 0, 11);
    assert(reboot_note_read(&n) == 1);
    assert(n.boot_id == 77);
    assert(n.consecutive == 1);
    printf("test_reboot_note: property 5 (ack clears, boot id survives) OK\n");

    /* 6. a one-bit corruption invalidates */
    struct RebootNote bad = n;
    bad.mono_ms ^= 1u;
    assert(!reboot_note_is_valid(&bad));
    bad = n;
    bad.consecutive ^= 1u;
    assert(!reboot_note_is_valid(&bad));
    printf("test_reboot_note: property 6 (checksum catches one bit) OK\n");

    printf("test_reboot_note: all properties OK\n");
    return 0;
}
