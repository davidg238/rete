/* Copyright (c) 2026 Ekorau LLC */
#include "ota.h"
#include "ota_backend.h"
#include "platform_backend.h"   /* console output goes through the seam, NOT printf */
#include <string.h>

/* rete#25 — the node's image-push seam. See ota.h for the contract and
 * docs/design/2026-08-07-code-push-host-slice-design.md §4 for the why.
 *
 * Everything below is written against one seam (ota_backend.h) and the console,
 * so the whole file ports to zephyr through backend/zephyr/ota_zephyr.c. The fetch
 * is not here: since rete#440 R5 it is the kernel's OtaFetch, reactions on the VM
 * thread (nodus/nsl/lib/kernel.nsl), and C keeps the bytes and the digest.
 *
 * The SHA-256 here is FILE-LOCAL ON PURPOSE. A separate sha256.c would be a
 * tenth entry in every one of the enumerated source lists (#63), and this tree
 * has exactly one hash user. rete#310 is the second user (the C6 hashes the
 * program it loaded), so `ota_sha256` is public; the block functions stay static.
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

void ota_sha256(const uint8_t *p, long n, uint8_t out[32]) {
    struct sha256 c;
    sha256_init(&c);
    sha256_update(&c, p, n);
    sha256_final(&c, out);
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

/* ------------------------------------------------------------------ */
/* hex                                                                 */
/* ------------------------------------------------------------------ */

static int unhex_nib(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}


/* ------------------------------------------------------------------ */
/* staged-slot digest                                                  */
/* ------------------------------------------------------------------ */
#define OTA_CHUNK 256   /* the read size of the digest walk; any size gives the same hash */

int ota_stage_matches(const uint8_t want[32]) {
    struct sha256 c;
    uint8_t chunk[OTA_CHUNK];
    uint8_t got[32];
    long len = ota_stage_size();
    if (!want || len < 0) return 0;
    sha256_init(&c);
    for (long off = 0; off < len; off += OTA_CHUNK) {
        long n = len - off;
        if (n > OTA_CHUNK) n = OTA_CHUNK;
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

void ota_confirm(void) {
    ota_touch_confirm();
    say("confirmed");
}

void ota_activate(const uint8_t hash[32], const uint8_t *sig, int sig_len) {
    if (!hash) return;
    if (!sig || sig_len < 0) sig_len = 0;
    if (sig_len > 128) sig_len = 128;    /* ed25519 is 64; the cap is a guard */
    if (!ota_write_ready(hash, sig, sig_len)) say("cannot record ready (no runner, or no key)");
    else say("activate: ready recorded, leaving for the runner");
    ota_exit_stage_ready();
}
