/* Copyright (c) 2026 Ekorau LLC */
#include "ota.h"
#include "ota_backend.h"
#include "udp_backend.h"
#include "platform_backend.h"   /* console output goes through the seam, NOT printf */
#include <string.h>

/* rete#25 — the node's image-push seam. See ota.h for the contract and
 * docs/design/2026-08-07-code-push-host-slice-design.md §4 for the why.
 *
 * Everything below is written against two seams and nothing else, so the whole
 * file ports to zephyr the day backend/zephyr/ota_zephyr.c exists.
 *
 * The SHA-256 here is FILE-LOCAL ON PURPOSE. A separate sha256.c would be a
 * tenth entry in every one of the enumerated source lists (#63), and this tree
 * has exactly one hash user. If a second one ever appears, promote it then —
 * not in anticipation.
 */

/* ------------------------------------------------------------------ */
/* SHA-256 (FIPS 180-4), public-domain reference shape, static.        */
/* ------------------------------------------------------------------ */
#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)  (ROTR32(x, 2) ^ ROTR32(x,13) ^ ROTR32(x,22))
#define EP1(x)  (ROTR32(x, 6) ^ ROTR32(x,11) ^ ROTR32(x,25))
#define SIG0(x) (ROTR32(x, 7) ^ ROTR32(x,18) ^ ((x) >> 3))
#define SIG1(x) (ROTR32(x,17) ^ ROTR32(x,19) ^ ((x) >> 10))

static const uint32_t sha_k[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
    0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
    0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
    0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
    0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
    0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

struct sha256 {
    uint32_t state[8];
    uint64_t bits;
    uint8_t  buf[64];
    int      buflen;
};

static void sha256_block(struct sha256 *c, const uint8_t *p) {
    uint32_t m[64], a, b, d, e, f, g, h, t1, t2, cc;
    int i;
    for (i = 0; i < 16; i++)
        m[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (; i < 64; i++)
        m[i] = SIG1(m[i-2]) + m[i-7] + SIG0(m[i-15]) + m[i-16];

    a = c->state[0]; b = c->state[1]; cc = c->state[2]; d = c->state[3];
    e = c->state[4]; f = c->state[5]; g  = c->state[6]; h = c->state[7];
    for (i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e, f, g) + sha_k[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, cc);
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g;  c->state[7] += h;
}

static void sha256_init(struct sha256 *c) {
    c->state[0] = 0x6a09e667u; c->state[1] = 0xbb67ae85u;
    c->state[2] = 0x3c6ef372u; c->state[3] = 0xa54ff53au;
    c->state[4] = 0x510e527fu; c->state[5] = 0x9b05688cu;
    c->state[6] = 0x1f83d9abu; c->state[7] = 0x5be0cd19u;
    c->bits = 0;
    c->buflen = 0;
}

static void sha256_update(struct sha256 *c, const uint8_t *p, long n) {
    for (long i = 0; i < n; i++) {
        c->buf[c->buflen++] = p[i];
        if (c->buflen == 64) {
            sha256_block(c, c->buf);
            c->bits += 512;
            c->buflen = 0;
        }
    }
}

static void sha256_final(struct sha256 *c, uint8_t out[32]) {
    int i = c->buflen;
    c->bits += (uint64_t)c->buflen * 8;
    c->buf[i++] = 0x80;
    if (i > 56) {
        while (i < 64) c->buf[i++] = 0;
        sha256_block(c, c->buf);
        i = 0;
    }
    while (i < 56) c->buf[i++] = 0;
    for (int j = 0; j < 8; j++)
        c->buf[56 + j] = (uint8_t)(c->bits >> (56 - 8*j));
    sha256_block(c, c->buf);
    for (int j = 0; j < 8; j++)
        for (int k = 0; k < 4; k++)
            out[j*4 + k] = (uint8_t)(c->state[j] >> (24 - 8*k));
}

/* ------------------------------------------------------------------ */
/* console                                                             */
/* ------------------------------------------------------------------ */
static void say(const char *msg) {
    platform_console_write("ota: ", 5);
    platform_console_write(msg, strlen(msg));
    platform_console_write("\n", 1);
    platform_console_flush();
}

static void say_n(const char *msg, long n) {
    char num[24];
    int i = 0;
    if (n < 0) { num[i++] = '-'; n = -n; }
    char d[20]; int nd = 0;
    do { d[nd++] = (char)('0' + (int)(n % 10)); n /= 10; } while (n);
    while (nd) num[i++] = d[--nd];
    num[i] = '\0';
    platform_console_write("ota: ", 5);
    platform_console_write(msg, strlen(msg));
    platform_console_write(num, strlen(num));
    platform_console_write("\n", 1);
    platform_console_flush();
}

/* ------------------------------------------------------------------ */
/* hex                                                                 */
/* ------------------------------------------------------------------ */
static const char hexdig[] = "0123456789abcdef";

static int unhex_nib(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static void tohex(const uint8_t *in, int n, char *out) {
    for (int i = 0; i < n; i++) {
        out[2*i]     = hexdig[in[i] >> 4];
        out[2*i + 1] = hexdig[in[i] & 0x0f];
    }
    out[2*n] = '\0';
}

/* ------------------------------------------------------------------ */
/* the block-fetch client — porta/internal/blockserv is the counterparty */
/* ------------------------------------------------------------------ */
#define OTA_BLOCK        256   /* blockserv.BlockSize; a 6LoWPAN-safe payload */
#define OTA_REQ_LEN      37    /* 0x01 | hash[32] | blockNo u32 BE */
#define OTA_RESP_HDR     6     /* 0x02 | blockNo u32 BE | more u8 */
#define OTA_ATTEMPTS     5     /* per block, then the stage fails this tick */
#define OTA_POLL_MS      5     /* udp_receive is non-blocking on every backend */
#define OTA_POLLS        200   /* 200 * 5 ms == the 1 s per-attempt budget */

/* Answers 1 on success (filling out, out_len and more), 0 on an exhausted retry
 * budget, -1 on a peer error frame (0x7F) — which is NOT retried, because
 * "unknown hash" is a state at porta, not a lost datagram. */
static int fetch_block(int sock, const char *host, int port,
                       const uint8_t hash[32], uint32_t blk,
                       uint8_t *out, int *out_len, int *more) {
    uint8_t req[OTA_REQ_LEN];
    req[0] = 0x01;
    memcpy(req + 1, hash, 32);
    req[33] = (uint8_t)(blk >> 24); req[34] = (uint8_t)(blk >> 16);
    req[35] = (uint8_t)(blk >> 8);  req[36] = (uint8_t)blk;

    uint8_t resp[OTA_RESP_HDR + OTA_BLOCK + 64];
    char peer[64];
    int peer_port = 0;

    for (int attempt = 0; attempt < OTA_ATTEMPTS; attempt++) {
        if (udp_send(sock, host, port, req, OTA_REQ_LEN) != OTA_REQ_LEN) {
            ota_sleep_ms(OTA_POLL_MS);
            continue;
        }
        for (int poll = 0; poll < OTA_POLLS; poll++) {
            int n = udp_receive(sock, resp, (int)sizeof resp,
                                peer, (int)sizeof peer, &peer_port);
            if (n < 0) { ota_sleep_ms(OTA_POLL_MS); continue; }
            if (n >= 1 && resp[0] == 0x7F) {
                char msg[96];
                int m = n - 1;
                if (m > (int)sizeof msg - 1) m = (int)sizeof msg - 1;
                memcpy(msg, resp + 1, (size_t)m);
                msg[m] = '\0';
                platform_console_write("ota: peer error: ", 17);
                platform_console_write(msg, (size_t)m);
                platform_console_write("\n", 1);
                platform_console_flush();
                return -1;
            }
            if (n < OTA_RESP_HDR || resp[0] != 0x02) continue;  /* not ours */
            uint32_t got = ((uint32_t)resp[1] << 24) | ((uint32_t)resp[2] << 16) |
                           ((uint32_t)resp[3] << 8)  |  (uint32_t)resp[4];
            if (got != blk) continue;                 /* a late duplicate */
            int len = n - OTA_RESP_HDR;
            if (len > OTA_BLOCK) return -1;           /* over-long: a bad peer */
            memcpy(out, resp + OTA_RESP_HDR, (size_t)len);
            *out_len = len;
            *more = resp[5] ? 1 : 0;
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* staged-slot digest                                                  */
/* ------------------------------------------------------------------ */
/* 1 when the first `len` bytes of the slot hash to `want`. */
static int staged_hash_is(const uint8_t want[32], long len) {
    struct sha256 c;
    uint8_t chunk[OTA_BLOCK];
    uint8_t got[32];
    sha256_init(&c);
    for (long off = 0; off < len; off += OTA_BLOCK) {
        long n = len - off;
        if (n > OTA_BLOCK) n = OTA_BLOCK;
        if (!ota_stage_read(chunk, off, n)) return 0;
        sha256_update(&c, chunk, n);
    }
    sha256_final(&c, got);
    return memcmp(got, want, 32) == 0;
}

/* ------------------------------------------------------------------ */
/* the VM-facing half                                                  */
/* ------------------------------------------------------------------ */
int ota_running_hash(uint8_t out[32]) {
    const char *hex = ota_env("OEVM_IMAGE_HASH");
    if (!hex) return 0;
    if (strlen(hex) != 64) return 0;
    uint8_t tmp[32];
    for (int i = 0; i < 32; i++) {
        int hi = unhex_nib((unsigned char)hex[2*i]);
        int lo = unhex_nib((unsigned char)hex[2*i + 1]);
        if (hi < 0 || lo < 0) return 0;
        tmp[i] = (uint8_t)((hi << 4) | lo);
    }
    memcpy(out, tmp, 32);        /* only on success: a bad env leaves out alone */
    return 1;
}

int ota_is_running(const uint8_t hash[32]) {
    uint8_t cur[32];
    if (!hash) return 0;
    if (!ota_running_hash(cur)) return 0;
    return memcmp(cur, hash, 32) == 0;
}

int ota_trial(void) {
    const char *v = ota_env("OEVM_OTA_TRIAL");
    return (v && v[0] == '1' && v[1] == '\0') ? 1 : 0;
}

int ota_stage(const uint8_t hash[32], const uint8_t *sig, int sig_len,
              const char *host, int port) {
    (void)sig; (void)sig_len;   /* carried by the record; verified by the runner (D4) */
    if (!hash || !host) return 0;

    long staged = ota_stage_size();
    if (staged < 0) { say("no staging slot (OEVM_OTA_STAGE unset)"); return 0; }

    /* 1. Idempotent re-entry: a slot that already holds the wanted image costs
     *    one re-hash and no traffic. This is the crash-after-staging path. */
    if (staged > 0 && staged_hash_is(hash, staged)) {
        say_n("already staged, bytes=", staged);
        return 1;
    }

    /* 2. Resume at the slot's own length. A partial trailing block cannot be
     *    resumed from (the seam truncates to zero only — a length-preserving
     *    truncate would be a second mechanism for a case that only arises from
     *    a torn write), so an off-boundary slot restarts from block 0. */
    if (staged % OTA_BLOCK != 0) {
        say_n("slot not on a block boundary, restarting; bytes=", staged);
        ota_stage_truncate();
        staged = 0;
    }

    int sock = udp_socket_bind(0);
    if (sock < 0) { say("cannot bind a socket"); return 0; }

    uint8_t data[OTA_BLOCK];
    uint32_t blk = (uint32_t)(staged / OTA_BLOCK);
    const uint32_t first_blk = blk;      /* non-zero == this fetch is a RESUME */
    long total = staged;
    int ok = 1;
    for (;;) {
        int len = 0, more = 0;
        int r = fetch_block(sock, host, port, hash, blk, data, &len, &more);
        if (r <= 0) {
            if (r == 0) {
                /* A timeout is porta being unreachable. The staged bytes are
                 * still a good prefix, so the slot stays and IS the cursor for
                 * the next tick — that is the whole point of resuming. */
                say_n("block timed out after 5 attempts, block=", (long)blk);
            } else if (first_blk > 0) {
                /* A peer ERROR while resuming means the staged bytes are not a
                 * prefix of the wanted image at all. The commonest cause is a
                 * retarget: the goal hash changed under a partial slot and the
                 * new image is SHORTER, so the resume block is past its end and
                 * blockserv answers "block past end". A wrong full-length slot
                 * on an exact block boundary lands on the identical frame.
                 *
                 * The slot carries no record of which image its bytes belong
                 * to, so there is nothing to consult and no way to narrow the
                 * cursor — dropping it is the only exit. Leaving it makes every
                 * later tick repeat this exact request forever, and NO operator
                 * write can clear it: the node is wedged permanently. One
                 * discarded partial download is the bounded cost of that.
                 *
                 * A fresh stage (first_blk == 0) is deliberately left alone.
                 * That is the record-pushed-before-the-blob case of the design's
                 * failure table — the slot is empty, there is nothing to
                 * discard, and it self-heals when the blob lands. */
                say("peer rejected a resumed block — discarding the stale slot");
                ota_stage_truncate();
            }
            ok = 0;
            break;
        }
        if (len > 0 && !ota_stage_append(data, len)) {
            say("staging slot write failed");
            ok = 0;
            break;
        }
        total += len;
        if (!more) break;
        blk++;
    }
    udp_close(sock);
    if (!ok) return 0;

    /* 3. Whole-image verify. A mismatch is not recoverable by resuming — the
     *    bytes on hand are wrong, not incomplete — so the slot goes to zero and
     *    the next cadence tick refetches from block 0. */
    if (!staged_hash_is(hash, total)) {
        say_n("staged image hash MISMATCH, discarding bytes=", total);
        ota_stage_truncate();
        return 0;
    }
    say_n("staged ok, bytes=", total);
    return 1;
}

void ota_confirm(void) {
    ota_touch_confirm();
    say("confirmed");
}

void ota_activate(const uint8_t hash[32], const uint8_t *sig, int sig_len) {
    if (!hash) return;
    if (!sig || sig_len < 0) sig_len = 0;
    if (sig_len > 128) sig_len = 128;    /* ed25519 is 64; the cap is a guard */

    char hash_hex[65];
    char sig_hex[257];
    tohex(hash, 32, hash_hex);
    if (sig_len) tohex(sig, sig_len, sig_hex); else sig_hex[0] = '\0';

    /* Exactly {"hash":"<hex>","sig":"<hex>"} — the runner's ready.json (§5). */
    char json[400];
    size_t i = 0;
    const char *p1 = "{\"hash\":\"";
    const char *p2 = "\",\"sig\":\"";
    const char *p3 = "\"}";
    memcpy(json + i, p1, strlen(p1)); i += strlen(p1);
    memcpy(json + i, hash_hex, strlen(hash_hex)); i += strlen(hash_hex);
    memcpy(json + i, p2, strlen(p2)); i += strlen(p2);
    memcpy(json + i, sig_hex, strlen(sig_hex)); i += strlen(sig_hex);
    memcpy(json + i, p3, strlen(p3)); i += strlen(p3);
    json[i] = '\0';

    if (!ota_write_ready(json)) say("cannot write ready.json (OEVM_OTA_READY unset?)");
    else say("activate: ready.json written, exiting STAGE_READY(42)");
    ota_exit_stage_ready();
}
