/* The outbox journal on target (rete#443; design docs/design/2026-11-14-443-outbox-journal-design.md):
   the journal_backend.h contract over Zephyr's Flash Circular Buffer (subsys/fs/fcb) on the
   `outbox` fixed partition. backend/posix/journal_posix.c is the same contract over a file,
   and tests/test_journal_posix.c is its specification; read both before changing this.

   Always linked. A board answers journal_open 0 (RAM `pending`, as before #443) unless it has
   BOTH CONFIG_FCB and an `outbox` partition: a devicetree node labelled `outbox` (the C6), or a
   Partition Manager partition named `outbox` (nRF, where flash_map_pm.h maps FIXED_PARTITION_*
   onto pm_static_dk.yml). One file and an #if, rather than a CMake switch and a second stub.

   What FCB gives and what this file adds:
   - FCB: the sector ring (one sector header {magic, version, id} per sector), a length and a
     CRC-8 per entry, a torn entry skipped (its CRC fails), the oldest sector erased by
     fcb_rotate. fcb_append answers -ENOSPC when the ring is full; it never rotates by itself.
   - Here: an entry is {type u8 (0 data, 1 marker), pos u32 LE, payload}. A marker's pos is the
     retired pos and it has no payload. Before each rotation the oldest sector's unacked,
     never-loaded data entries are counted, which is the drop the caller adds to `sdr`.
   - The format version is FCB's own sector-header `fd_ver` (f_version below). fcb_init checks
     the header magic but not the version, so journal_open reads every sector header itself
     first: one of another magic or version (a reflash that changed the journal) erases the
     partition, and the non-blank sectors erased are journal_lost_at_open (design rule 7).
     The header layout is FCB's private struct fcb_disk_area (subsys/fs/fcb/fcb_priv.h:50),
     restated below as `hdr_t`; a Zephyr that changes it reads here as "foreign" and erases
     once, which is the safe failure.

   Write alignment: FCB pads every field to flash_area_align() (4 on the C6's internal flash,
   1 on the MX25R), so an entry's bytes go out through a small aligned bounce buffer, padded
   with the erased value. */
#include "journal_backend.h"

#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>

#if defined(CONFIG_FCB) && FIXED_PARTITION_EXISTS(outbox)

#include <errno.h>
#include <string.h>
#include <zephyr/fs/fcb.h>
#include <zephyr/sys/printk.h>

#define J_MAGIC    0x4E524A4Fu   /* "OJRN" little-endian, the posix file's sector magic */
#define J_VERSION  1             /* bump with any change to the entry layout below */
#define T_DATA     0
#define T_MARK     1
#define EHDR       5             /* type + pos */
#define MAX_SEC    255           /* struct fcb's f_sector_cnt is a uint8_t (fs/fcb.h:106) */
#define WB         32            /* bounce buffer; f_align must divide it */

typedef struct { uint32_t magic; uint8_t ver; uint8_t pad; uint16_t id; } hdr_t; /* fcb_priv.h:50 */

static struct flash_sector sectors[MAX_SEC];
static struct fcb fcb;
static int      is_open;
static uint8_t  erased;                /* the flash's erase value */
static uint32_t next_pos, retired;
static uint32_t loaded;                /* highest pos journal_next has handed out since open */
static int      lost_at_open;
/* The read cursor: the entry the last journal_next answer came from. A call whose `after`
   is that answer's pos resumes after it; any other call, or a rotation that erased its
   sector, walks from the oldest sector. */
static struct fcb_entry cur;
static int      cur_ok;
static uint32_t cur_pos;
static uint8_t  __aligned(4) wbuf[WB];

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* An entry's type and pos, from its first EHDR bytes. */
static int entry_head(const struct fcb_entry *loc, uint8_t *type, uint32_t *pos) {
    uint8_t h[EHDR];
    if (loc->fe_data_len < EHDR) return -1;
    if (flash_area_read(fcb.fap, FCB_ENTRY_FA_DATA_OFF((*loc)), h, EHDR) != 0) return -1;
    *type = h[0];
    *pos = rd32(h + 1);
    return 0;
}

static int note_open(struct fcb_entry_ctx *ctx, void *arg) {
    uint8_t type; uint32_t pos;
    (void)arg;
    if (entry_head(&ctx->loc, &type, &pos) != 0) return 0;
    if (type == T_DATA && pos >= next_pos) next_pos = pos + 1;
    if (type == T_MARK && pos > retired) retired = pos;
    return 0;
}

static int count_unacked(struct fcb_entry_ctx *ctx, void *arg) {
    uint8_t type; uint32_t pos;
    if (entry_head(&ctx->loc, &type, &pos) != 0) return 0;
    if (type == T_DATA && pos > retired && pos > loaded) ++*(int *)arg;
    return 0;
}

/* Every byte of the sector at the erased value. */
static int sector_blank(const struct flash_area *fa, const struct flash_sector *s) {
    uint8_t b[64];
    for (uint32_t off = 0; off < s->fs_size; off += sizeof b) {
        size_t n = MIN(sizeof b, s->fs_size - off);
        if (flash_area_read(fa, s->fs_off + off, b, n) != 0) return 0;
        for (size_t i = 0; i < n; i++) if (b[i] != erased) return 0;
    }
    return 1;
}

/* Reads every sector header before FCB does. Answers the non-blank sectors erased: 0 for a
   journal of this format (left alone), or every non-blank sector when a header is foreign
   (another magic or version) or when no sector has a header at all (a partition never
   formatted may hold anything, and FCB writes headers without erasing first). */
static int erase_if_foreign(const struct flash_area *fa, int nsec) {
    const uint32_t blank_magic = 0x01010101u * erased;
    const uint32_t want = J_MAGIC ^ ~blank_magic;   /* fcb_flash_magic(), fcb_priv.h:42 */
    int valid = 0, foreign = 0, lost = 0;
    for (int s = 0; s < nsec; s++) {
        hdr_t h;
        if (flash_area_read(fa, sectors[s].fs_off, &h, sizeof h) != 0) { foreign++; continue; }
        if (h.magic == blank_magic) continue;
        if (h.magic == want && h.ver == J_VERSION) valid++; else foreign++;
    }
    if (valid > 0 && foreign == 0) return 0;
    for (int s = 0; s < nsec; s++) {
        if (sector_blank(fa, &sectors[s])) continue;
        flash_area_flatten(fa, sectors[s].fs_off, sectors[s].fs_size);
        lost++;
    }
    /* A partition that held no journal loses no rows, whatever it held. */
    return foreign ? lost : 0;
}

int journal_open(void) {
    const struct flash_area *fa;
    uint32_t cnt = MAX_SEC;
    journal_close();
    if (flash_area_get_sectors(FIXED_PARTITION_ID(outbox), &cnt, sectors) != 0 || cnt < 2) {
        printk("journal: outbox has no usable sector layout (more than %d sectors?)\n", MAX_SEC);
        return 0;
    }
    if (flash_area_open(FIXED_PARTITION_ID(outbox), &fa) != 0) return 0;
    erased = flash_area_erased_val(fa);
    lost_at_open = erase_if_foreign(fa, (int)cnt);
    flash_area_close(fa);

    memset(&fcb, 0, sizeof fcb);
    fcb.f_magic = J_MAGIC;
    fcb.f_version = J_VERSION;
    fcb.f_sector_cnt = (uint8_t)cnt;
    fcb.f_scratch_cnt = 0;
    fcb.f_sectors = sectors;
    if (fcb_init(FIXED_PARTITION_ID(outbox), &fcb) != 0 || fcb.f_align > WB || WB % fcb.f_align) {
        printk("journal: fcb_init failed on outbox\n");
        return 0;
    }
    next_pos = 1; retired = 0; loaded = 0; cur_ok = 0;
    fcb_walk(&fcb, NULL, note_open, NULL);
    if (next_pos <= retired) next_pos = retired + 1;   /* only markers survived a wrap */
    is_open = 1;
    printk("journal: outbox %u x %u B, next %u, retired %u, lost at open %d\n",
           (unsigned)cnt, (unsigned)sectors[0].fs_size, (unsigned)next_pos,
           (unsigned)retired, lost_at_open);
    return 1;
}

void journal_close(void) {
    if (is_open && fcb.fap) flash_area_close(fcb.fap);
    is_open = 0;
    cur_ok = 0;
}

/* Bytes an FCB entry of `len` data bytes occupies, as fcb_append reserves them
   (fcb_append.c: the length field, the data, the CRC, each padded to f_align). */
static uint32_t al(uint32_t n) { return (n + fcb.f_align - 1) & ~(uint32_t)(fcb.f_align - 1); }
static uint32_t entry_bytes(int len) { return al(len < 0x80 ? 1 : 2) + al((uint32_t)len) + al(1); }

/* Writes `n` bytes of the entry's stream at flash offset `at` through the bounce buffer. */
static int stream(off_t *at, int *fill, const uint8_t *p, int n) {
    while (n > 0) {
        int k = MIN(n, WB - *fill);
        memcpy(wbuf + *fill, p, (size_t)k);
        *fill += k; p += k; n -= k;
        if (*fill == WB) {
            if (flash_area_write(fcb.fap, *at, wbuf, WB) != 0) return -1;
            *at += WB; *fill = 0;
        }
    }
    return 0;
}

static int put(uint8_t type, uint32_t pos, const uint8_t *buf, int len) {
    struct fcb_entry loc;
    int dropped = 0, rc;
    int n = EHDR + len;
    if (al(sizeof(hdr_t)) + entry_bytes(n) > sectors[0].fs_size) return -1;
    for (int tries = 0; (rc = fcb_append(&fcb, (uint16_t)n, &loc)) == -ENOSPC; tries++) {
        if (tries >= fcb.f_sector_cnt) return -1;
        struct flash_sector *old = fcb.f_oldest;
        fcb_walk(&fcb, old, count_unacked, &dropped);
        if (cur_ok && cur.fe_sector == old) cur_ok = 0;
        if (fcb_rotate(&fcb) != 0) return -1;
    }
    if (rc != 0) return -1;
    uint8_t h[EHDR] = { type, (uint8_t)pos, (uint8_t)(pos >> 8), (uint8_t)(pos >> 16), (uint8_t)(pos >> 24) };
    off_t at = FCB_ENTRY_FA_DATA_OFF(loc);
    int fill = 0;
    if (stream(&at, &fill, h, EHDR) != 0 || (len > 0 && stream(&at, &fill, buf, len) != 0)) return -1;
    if (fill > 0) {
        int padded = (int)al((uint32_t)fill);
        memset(wbuf + fill, erased, (size_t)(padded - fill));
        if (flash_area_write(fcb.fap, at, wbuf, (size_t)padded) != 0) return -1;
    }
    if (fcb_append_finish(&fcb, &loc) != 0) return -1;
    return dropped;
}

int journal_append(const uint8_t *buf, int len, uint32_t *pos) {
    if (!is_open || len < 0) return -1;
    int r = put(T_DATA, next_pos, buf, len);
    if (r < 0) return r;
    *pos = next_pos++;
    return r;
}

int journal_retire(uint32_t pos) {
    if (!is_open) return -1;
    if (pos <= retired) return 0;
    /* Retired first, as the posix backend does: rows this marker acks are not counted as
       dropped by the rotation the marker itself may cause. */
    uint32_t was = retired;
    retired = pos;
    int r = put(T_MARK, pos, NULL, 0);
    if (r < 0) retired = was;
    return r;
}

uint32_t journal_last_retired(void) { return retired; }

int journal_next(uint32_t after, uint32_t *pos, uint8_t *buf, int cap) {
    struct fcb_entry loc;
    if (!is_open) return -1;
    if (cur_ok && after == cur_pos) loc = cur;
    else memset(&loc, 0, sizeof loc);          /* fe_sector NULL: from the oldest entry */
    while (fcb_getnext(&fcb, &loc) == 0) {
        uint8_t type; uint32_t p;
        if (entry_head(&loc, &type, &p) != 0 || type != T_DATA || p <= after) continue;
        int len = loc.fe_data_len - EHDR;
        if (len > cap) return -2;
        if (len > 0 && flash_area_read(fcb.fap, FCB_ENTRY_FA_DATA_OFF(loc) + EHDR, buf, (size_t)len) != 0)
            return -1;
        *pos = p;
        cur = loc; cur_ok = 1; cur_pos = p;
        if (p > loaded) loaded = p;
        return len;
    }
    return -1;
}

int journal_lost_at_open(void) { return lost_at_open; }

#else /* no FCB or no `outbox` partition: this board has no journal */

int journal_open(void) { return 0; }
void journal_close(void) {}
int journal_append(const uint8_t *buf, int len, uint32_t *pos) { (void)buf; (void)len; (void)pos; return -1; }
int journal_retire(uint32_t pos) { (void)pos; return -1; }
uint32_t journal_last_retired(void) { return 0; }
int journal_next(uint32_t after, uint32_t *pos, uint8_t *buf, int cap) {
    (void)after; (void)pos; (void)buf; (void)cap; return -1;
}
int journal_lost_at_open(void) { return 0; }

#endif
