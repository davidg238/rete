/* oevm/tests/test_journal_posix.c — host TDD for the outbox journal seam (rete#443;
   design docs/design/2026-11-14-443-outbox-journal-design.md).
   Backend-only link:
     cd nodus/oevm && gcc -std=c99 -Isrc tests/test_journal_posix.c \
       src/backend/posix/journal_posix.c -o /tmp/tj && /tmp/tj

   The posix ring is a file of OEVM_JOURNAL_SECTORS (here 4) x 4096-byte sectors,
   so a wrap takes a few dozen records. Each case starts from a deleted file. */
#define _POSIX_C_SOURCE 200809L
#include "../src/journal_backend.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATH "/tmp/oevm_test_journal.bin"

typedef struct { int n; uint32_t pos[512]; uint8_t first[512]; int len[512]; } seen_t;

static int collect(uint32_t pos, const uint8_t *buf, int len, void *ctx) {
    seen_t *s = ctx;
    s->pos[s->n] = pos; s->first[s->n] = len ? buf[0] : 0; s->len[s->n] = len; s->n++;
    return 0;
}

static void fresh(void) {
    journal_close();
    remove(PATH);
    assert(journal_open() == 1);
}

static seen_t walk_unacked(void) {
    seen_t s; memset(&s, 0, sizeof s);
    assert(journal_walk(journal_last_retired(), collect, &s) == 0);
    return s;
}

int main(void) {
    setenv("OEVM_JOURNAL", PATH, 1);
    setenv("OEVM_JOURNAL_SECTORS", "4", 1);
    uint8_t row[40];
    memset(row, 0, sizeof row);
    uint32_t p1, p2, p3;

    /* append_walk_in_order */
    fresh();
    row[0] = 'a'; assert(journal_append(row, 40, &p1) == 0);
    row[0] = 'b'; assert(journal_append(row, 40, &p2) == 0);
    row[0] = 'c'; assert(journal_append(row, 40, &p3) == 0);
    assert(p1 < p2 && p2 < p3);
    { seen_t s = walk_unacked();
      assert(s.n == 3 && s.first[0] == 'a' && s.first[1] == 'b' && s.first[2] == 'c');
      assert(s.len[0] == 40 && s.pos[2] == p3); }

    /* retire_hides: a marker at p2 leaves only c unacked */
    assert(journal_retire(p2) == 0);
    assert(journal_last_retired() == p2);
    { seen_t s = walk_unacked(); assert(s.n == 1 && s.first[0] == 'c'); }

    /* reopen_survives: the file is the journal, the RAM is only a cache */
    journal_close();
    assert(journal_open() == 1);
    assert(journal_last_retired() == p2);
    { seen_t s = walk_unacked(); assert(s.n == 1 && s.first[0] == 'c' && s.pos[0] == p3); }
    row[0] = 'd'; { uint32_t p4; assert(journal_append(row, 40, &p4) == 0); assert(p4 > p3); }
    assert(journal_lost_at_open() == 0);

    /* wrap_drops_oldest: fill all four sectors, then one more record rotates sector 0
       out; append answers how many UNACKED records it dropped. */
    fresh();
    int appended = 0, dropped = 0;
    uint32_t first_pos = 0;
    while (dropped == 0) {
        uint32_t p;
        row[0] = (uint8_t)appended;
        int r = journal_append(row, 40, &p);
        assert(r >= 0);
        if (appended == 0) first_pos = p;
        dropped = r;
        appended++;
        assert(appended < 1000);
    }
    { seen_t s = walk_unacked();
      assert(s.n == appended - dropped);             /* nothing else was lost */
      assert(s.pos[0] > first_pos);                  /* the oldest are gone */
      assert(s.first[s.n - 1] == (uint8_t)(appended - 1)); } /* the newest survives */

    /* a rotation drops only UNACKED rows: retire every record as it lands (a healthy
       link), append well past one full ring, and no append ever reports a drop. */
    fresh();
    for (int k = 0; k < 600; k++) {
        uint32_t p;
        assert(journal_append(row, 40, &p) == 0);
        assert(journal_retire(p) == 0);
    }

    /* torn_tail_is_not_corruption: garbage after the last record (a write cut by a
       power loss) ends that sector, keeps the earlier records, and the next append
       still lands and walks. */
    fresh();
    row[0] = 'x'; assert(journal_append(row, 40, &p1) == 0);
    journal_close();
    /* A record is {type u8, len u16, pos u32, payload, crc8}: 48 bytes for 40 of
       payload, after sector 0's 16-byte header. Byte 64 is the first erased one. */
    { FILE *f = fopen(PATH, "r+b"); assert(f);
      uint8_t torn[3] = {0, 40, 0}; fseek(f, 16 + 48, SEEK_SET); fwrite(torn, 1, 3, f); fclose(f); }
    assert(journal_open() == 1);
    assert(journal_lost_at_open() == 0);
    row[0] = 'y'; assert(journal_append(row, 40, &p2) == 0);
    { seen_t s = walk_unacked(); assert(s.n == 2 && s.first[0] == 'x' && s.first[1] == 'y'); }

    /* bad_header_erases: a sector header of another format means a reflash changed the
       journal; the whole journal is erased and counted, and it works afterwards. */
    fresh();
    row[0] = 'q'; assert(journal_append(row, 40, &p1) == 0);
    journal_close();
    { FILE *f = fopen(PATH, "r+b"); assert(f); uint8_t v = 99; fseek(f, 4, SEEK_SET); fwrite(&v, 1, 1, f); fclose(f); }
    assert(journal_open() == 1);
    assert(journal_lost_at_open() > 0);
    { seen_t s = walk_unacked(); assert(s.n == 0); }
    assert(journal_append(row, 40, &p1) == 0);

    /* no_journal: with no path configured the board has none */
    journal_close();
    unsetenv("OEVM_JOURNAL");
    assert(journal_open() == 0);
    { uint32_t p; assert(journal_append(row, 40, &p) < 0); }

    remove(PATH);
    printf("test_journal_posix: all cases passed\n");
    return 0;
}
