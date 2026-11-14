/* The outbox journal on the host (rete#443): the journal_backend.h contract over a
   plain file, laid out the way flash is so the host gate exercises the same rules the
   board's FCB backend obeys — erased bytes are 0xFF, a sector is erased whole, and a
   record is only ever written once.

   File = OEVM_JOURNAL_SECTORS (default 4) sectors of JSECTOR bytes.
   Sector = 16-byte header {magic "OJRN", version u8, 3 pad, generation u32 LE, 4 pad},
            then records {type u8 (0 data, 1 marker), len u16 LE, pos u32 LE, payload,
            crc8 over everything before it}. A marker's payload is the retired pos.
   The live sectors are taken in generation order, oldest first; the write sector is
   the newest. A record whose CRC fails (a write cut short by a power loss) ends its
   sector: what precedes it stands, and the next append starts a fresh sector rather
   than writing over bytes that are no longer erased. */
#include "../../journal_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSECTOR   4096
#define JHDR      16
#define JREC_OVH  8           /* type + len + pos + crc */
#define JVERSION  1
#define T_DATA    0
#define T_MARK    1

static FILE    *jf;
static uint8_t *img;          /* the whole ring, mirrored in RAM */
static int      nsec;
static uint32_t gen[256];
static int      ws, wo;       /* write sector, write offset in it */
static uint32_t next_pos, retired;
static int      lost_at_open;
/* The read cursor: where the last journal_next answer ended. A call whose `after` is
   that answer's pos resumes there; any other call (or a sector rotated since) starts
   from the oldest sector. */
static int      cs = -1, coff;
static uint32_t cpos, cgen;

static uint8_t crc8(const uint8_t *p, int n) {
    uint8_t c = 0;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : c << 1);
    }
    return c;
}
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint8_t *sec(int s) { return img + (size_t)s * JSECTOR; }

static int blank(int s) {
    for (int i = 0; i < JSECTOR; i++) if (sec(s)[i] != 0xFF) return 0;
    return 1;
}
static void flush_range(int s, int off, int n) {
    fseek(jf, (long)s * JSECTOR + off, SEEK_SET);
    fwrite(sec(s) + off, 1, (size_t)n, jf);
    fflush(jf);
}
static void erase(int s) { memset(sec(s), 0xFF, JSECTOR); gen[s] = 0; flush_range(s, 0, JSECTOR); }
static void start_sector(int s, uint32_t g) {
    memcpy(sec(s), "OJRN", 4); sec(s)[4] = JVERSION; wr32(sec(s) + 8, g);
    gen[s] = g; flush_range(s, 0, JHDR);
}

/* Visit sector s's valid records; answers the offset just past the last one, or
   JSECTOR when the sector ends in a torn record (so nothing more may be written). */
typedef int (*visit_fn)(int type, uint32_t pos, const uint8_t *pl, int len, void *ctx);
static int scan(int s, visit_fn fn, void *ctx) {
    int off = JHDR;
    while (off + JREC_OVH <= JSECTOR) {
        const uint8_t *r = sec(s) + off;
        if (r[0] == 0xFF) return off;                      /* erased: the end */
        int len = r[1] | r[2] << 8;
        if (off + JREC_OVH + len > JSECTOR || (r[0] != T_DATA && r[0] != T_MARK)
            || crc8(r, 7 + len) != r[7 + len]) return JSECTOR;   /* torn */
        if (fn) { int rc = fn(r[0], rd32(r + 3), r + 7, len, ctx); if (rc) return -rc; }
        off += JREC_OVH + len;
    }
    return off;
}

/* Sector order, oldest first: the live sectors follow the write sector cyclically. */
static int nth_oldest(int i) { return (ws + 1 + i) % nsec; }

static int note_open(int type, uint32_t pos, const uint8_t *pl, int len, void *ctx) {
    (void)ctx;
    if (type == T_DATA && pos >= next_pos) next_pos = pos + 1;
    if (type == T_MARK && len == 4 && rd32(pl) > retired) retired = rd32(pl);
    return 0;
}
static int count_unacked(int type, uint32_t pos, const uint8_t *pl, int len, void *ctx) {
    (void)pl; (void)len;
    if (type == T_DATA && pos > retired) ++*(int *)ctx;
    return 0;
}

int journal_open(void) {
    const char *path = getenv("OEVM_JOURNAL");
    if (path == NULL || *path == 0) return 0;
    const char *ns = getenv("OEVM_JOURNAL_SECTORS");
    nsec = ns ? atoi(ns) : 4;
    if (nsec < 2 || nsec > 255) nsec = 4;
    img = malloc((size_t)nsec * JSECTOR);
    if (img == NULL) return 0;
    memset(img, 0xFF, (size_t)nsec * JSECTOR);
    /* OEVM_JOURNAL_RESET=1 erases the file on this process's FIRST open only, so a host
       test never reads the last run's records yet can still reopen ("reboot") over its own. */
    static int opened_once;
    const char *reset = getenv("OEVM_JOURNAL_RESET");
    if (!opened_once && reset && *reset == '1') remove(path);
    opened_once = 1;
    jf = fopen(path, "r+b");
    if (jf == NULL) jf = fopen(path, "w+b");
    if (jf == NULL) { free(img); img = NULL; return 0; }
    size_t got = fread(img, 1, (size_t)nsec * JSECTOR, jf);
    if (got != (size_t)nsec * JSECTOR) {           /* new or resized: lay it out erased */
        memset(img + got, 0xFF, (size_t)nsec * JSECTOR - got);
        fseek(jf, 0, SEEK_SET); fwrite(img, 1, (size_t)nsec * JSECTOR, jf); fflush(jf);
    }
    lost_at_open = 0; next_pos = 1; retired = 0; ws = -1;
    int foreign = 0, used = 0;
    for (int s = 0; s < nsec; s++) {
        gen[s] = 0;
        if (blank(s)) continue;
        used++;
        if (memcmp(sec(s), "OJRN", 4) != 0 || sec(s)[4] != JVERSION) { foreign = 1; continue; }
        gen[s] = rd32(sec(s) + 8);
        if (ws < 0 || gen[s] > gen[ws]) ws = s;
    }
    if (foreign) {
        for (int s = 0; s < nsec; s++) erase(s);
        lost_at_open = used;
        ws = -1;
    }
    if (ws < 0) { ws = 0; start_sector(0, 1); wo = JHDR; return 1; }
    for (int i = 0; i < nsec; i++) { int s = nth_oldest(i); if (gen[s]) scan(s, note_open, NULL); }
    wo = scan(ws, NULL, NULL);
    return 1;
}

void journal_close(void) {
    cs = -1;
    if (jf) fclose(jf);
    free(img);
    jf = NULL; img = NULL;
}

static int put(int type, const uint8_t *buf, int len, uint32_t pos) {
    int dropped = 0;
    if (wo + JREC_OVH + len > JSECTOR) {
        int ns = (ws + 1) % nsec;
        if (gen[ns]) scan(ns, count_unacked, &dropped);
        uint32_t g = gen[ws] + 1;
        erase(ns);
        start_sector(ns, g);
        ws = ns; wo = JHDR;
    }
    uint8_t *r = sec(ws) + wo;
    r[0] = (uint8_t)type; r[1] = (uint8_t)len; r[2] = (uint8_t)(len >> 8); wr32(r + 3, pos);
    memcpy(r + 7, buf, (size_t)len);
    r[7 + len] = crc8(r, 7 + len);
    flush_range(ws, wo, JREC_OVH + len);
    wo += JREC_OVH + len;
    return dropped;
}

int journal_append(const uint8_t *buf, int len, uint32_t *pos) {
    if (img == NULL || len < 0 || JHDR + JREC_OVH + len > JSECTOR) return -1;
    *pos = next_pos++;
    return put(T_DATA, buf, len, *pos);
}

int journal_retire(uint32_t pos) {
    if (img == NULL) return -1;
    if (pos <= retired) return 0;
    uint8_t pl[4]; wr32(pl, pos);
    retired = pos;
    return put(T_MARK, pl, 4, 0);
}

uint32_t journal_last_retired(void) { return retired; }

int journal_next(uint32_t after, uint32_t *pos, uint8_t *buf, int cap) {
    if (img == NULL) return -1;
    int k0 = 0, off = JHDR;
    if (cs >= 0 && after == cpos && gen[cs] == cgen) {
        k0 = (cs - ws - 1 + 2 * nsec) % nsec;
        off = coff;
    }
    for (int k = k0; k < nsec; k++, off = JHDR) {
        int s = nth_oldest(k);
        if (!gen[s]) continue;
        while (off + JREC_OVH <= JSECTOR) {
            const uint8_t *r = sec(s) + off;
            if (r[0] == 0xFF) break;
            int len = r[1] | r[2] << 8;
            if (off + JREC_OVH + len > JSECTOR || (r[0] != T_DATA && r[0] != T_MARK)
                || crc8(r, 7 + len) != r[7 + len]) break;            /* torn: sector ends */
            off += JREC_OVH + len;
            if (r[0] != T_DATA || rd32(r + 3) <= after) continue;
            if (len > cap) return -2;
            memcpy(buf, r + 7, (size_t)len);
            *pos = rd32(r + 3);
            cs = s; coff = off; cpos = *pos; cgen = gen[s];
            return len;
        }
    }
    return -1;
}

int journal_lost_at_open(void) { return lost_at_open; }
