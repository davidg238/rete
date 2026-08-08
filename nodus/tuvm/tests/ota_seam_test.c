/* Copyright (c) 2026 Ekorau LLC */
/* rete#25 — the C `ota` seam, tested through its two seams and nothing else.
 *
 * Link set (see tools/run_c_tests.sh): src/ota.c + the posix console backend
 * ONLY. Both of ota.c's dependencies — the file/env/exit half (ota_backend.h)
 * and the UDP half (udp_backend.h) — are stubbed IN THIS FILE, which is the
 * point: the fetch loop, the resume arithmetic and the SHA-256 verify are pure
 * given those two seams, so they can be proven without a socket, a filesystem
 * or a porta.
 *
 * The stub UDP peer is a faithful copy of porta/internal/blockserv (Task 4):
 *   request  0x01 | hash[32] | blockNo u32 BE            (37 bytes)
 *   response 0x02 | blockNo u32 BE | more u8 | data[<=256]
 *   error    0x7F | text
 * It also SCRIPTS A DROP of the first request for block 1, so the retry path is
 * exercised on every run rather than only when the network misbehaves.
 *
 * The expected SHA-256 digests below were computed with python hashlib, not
 * with this tree's own implementation — so the idempotence assertion doubles as
 * a conformance check of ota.c's file-local SHA-256 against a reference.
 */
#include "../src/ota.h"
#include "../src/ota_backend.h"
#include "../src/udp_backend.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define BLOB_LEN 600
#define BLOCK 256

/* blob A: b[i] = (i*7+3) & 0xff  -> sha256 below */
static const char *HASH_A_HEX =
    "1783f1f6842889ff855d25b6d45d33dd7401ffa94eb93704f6a374c264cde486";
/* blob B: b[i] = (i*11+5) & 0xff — same length, different bytes */
static const char *HASH_B_HEX =
    "464ae8453d00456341944fb3d1495600ef818c24e9d3e0d446b84c20c3df6869";

static uint8_t blob_a[BLOB_LEN];
static uint8_t blob_b[BLOB_LEN];
static uint8_t hash_a[32];
static uint8_t hash_b[32];

static void unhex(const char *hex, uint8_t *out, int n) {
    for (int i = 0; i < n; i++) {
        int hi = hex[2*i], lo = hex[2*i+1];
        hi = (hi <= '9') ? hi - '0' : (hi | 32) - 'a' + 10;
        lo = (lo <= '9') ? lo - '0' : (lo | 32) - 'a' + 10;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
}

/* ------------------------------------------------------------------ */
/* stub ota_backend: an in-memory stage slot, a fixed env map, recorded exit */
/* ------------------------------------------------------------------ */
static uint8_t stage_buf[8192];
static long    stage_len;
static int     stage_configured = 1;   /* 0 models OEVM_OTA_STAGE unset */
static char    env_image_hash[80];
static int     env_image_hash_set;
static int     env_trial;
static char    ready_json[1024];
static int     ready_writes;
static int     confirm_touches;
static int     exits;

const char *ota_env(const char *name) {
    if (strcmp(name, "OEVM_OTA_STAGE") == 0)
        return stage_configured ? "stub-slot" : NULL;
    if (strcmp(name, "OEVM_IMAGE_HASH") == 0)
        return env_image_hash_set ? env_image_hash : NULL;
    if (strcmp(name, "OEVM_OTA_TRIAL") == 0)
        return env_trial ? "1" : NULL;
    return NULL;
}
long ota_stage_size(void) { return stage_configured ? stage_len : -1; }
int ota_stage_read(uint8_t *buf, long off, long len) {
    if (!stage_configured) return 0;
    if (off < 0 || len < 0 || off + len > stage_len) return 0;
    memcpy(buf, stage_buf + off, (size_t)len);
    return 1;
}
int ota_stage_append(const uint8_t *data, int len) {
    if (!stage_configured) return 0;
    if (stage_len + len > (long)sizeof stage_buf) return 0;
    memcpy(stage_buf + stage_len, data, (size_t)len);
    stage_len += len;
    return 1;
}
void ota_stage_truncate(void) { stage_len = 0; }
int ota_write_ready(const char *json) {
    snprintf(ready_json, sizeof ready_json, "%s", json);
    ready_writes++;
    return 1;
}
void ota_touch_confirm(void) { confirm_touches++; }
void ota_exit_stage_ready(void) { exits++; }   /* recorded, not taken */
void ota_sleep_ms(int ms) { (void)ms; }        /* the poll loop runs at full speed */

/* ------------------------------------------------------------------ */
/* stub udp_backend: the blockserv counterparty, in memory              */
/* ------------------------------------------------------------------ */
static const uint8_t *serve_body;
static long           serve_len;
static uint8_t        serve_hash[32];
static uint32_t       requested[64];
static int            n_requested;
static int            drop_block1_once;
static uint8_t        pending[512];
static int            pending_len = -1;    /* -1 = nothing queued */
static int            binds;

void udp_open(void) {}
int  udp_socket_bind(int port) { (void)port; binds++; return 7; }
void udp_close(int sock) { (void)sock; }

int udp_send(int sock, const char *host, int port, const uint8_t *data, int len) {
    (void)sock; (void)host; (void)port;
    assert(len == 37);                 /* the frame is fixed-width by spec */
    assert(data[0] == 0x01);
    uint32_t blk = ((uint32_t)data[33] << 24) | ((uint32_t)data[34] << 16) |
                   ((uint32_t)data[35] << 8)  |  (uint32_t)data[36];
    if (n_requested < 64) requested[n_requested++] = blk;

    if (memcmp(data + 1, serve_hash, 32) != 0) {       /* unknown hash */
        pending[0] = 0x7F;
        memcpy(pending + 1, "unknown hash", 12);
        pending_len = 13;
        return len;
    }
    if (blk == 1 && drop_block1_once) {                /* scripted loss */
        drop_block1_once = 0;
        return len;                                    /* nothing queued */
    }
    long off = (long)blk * BLOCK;
    if (off > serve_len || (off == serve_len && blk != 0)) {
        pending[0] = 0x7F;
        memcpy(pending + 1, "block past end", 14);
        pending_len = 15;
        return len;
    }
    long end = off + BLOCK;
    uint8_t more = 0;
    if (end < serve_len) more = 1; else end = serve_len;
    pending[0] = 0x02;
    pending[1] = (uint8_t)(blk >> 24); pending[2] = (uint8_t)(blk >> 16);
    pending[3] = (uint8_t)(blk >> 8);  pending[4] = (uint8_t)blk;
    pending[5] = more;
    memcpy(pending + 6, serve_body + off, (size_t)(end - off));
    pending_len = 6 + (int)(end - off);
    return len;
}

int udp_receive(int sock, uint8_t *buf, int buf_cap,
                char *host_out, int host_cap, int *sender_port) {
    (void)sock;
    if (pending_len < 0) return -1;                    /* non-blocking: nothing waiting */
    int n = pending_len < buf_cap ? pending_len : buf_cap;
    memcpy(buf, pending, (size_t)n);
    pending_len = -1;
    snprintf(host_out, (size_t)host_cap, "127.0.0.1");
    *sender_port = 5684;
    return n;
}

/* ------------------------------------------------------------------ */
static void reset(const uint8_t *body, const uint8_t *hash) {
    stage_len = 0; stage_configured = 1;
    serve_body = body; serve_len = BLOB_LEN;
    memcpy(serve_hash, hash, 32);
    n_requested = 0; pending_len = -1; drop_block1_once = 1;
    env_image_hash_set = 0; env_trial = 0;
    ready_writes = 0; confirm_touches = 0; exits = 0;
}

static int requested_count(uint32_t blk) {
    int c = 0;
    for (int i = 0; i < n_requested; i++) if (requested[i] == blk) c++;
    return c;
}

/* 1. resume: a stage holding block 0 must ask for 1 and 2 only, and the
 *    dropped first request for block 1 must be recovered by the retry. */
static void test_resume(void) {
    reset(blob_a, hash_a);
    memcpy(stage_buf, blob_a, BLOCK);
    stage_len = BLOCK;

    assert(ota_stage(hash_a, NULL, 0, "::1", 5684) == 1);
    assert(stage_len == BLOB_LEN);
    assert(memcmp(stage_buf, blob_a, BLOB_LEN) == 0);
    assert(requested_count(0) == 0);        /* block 0 already staged */
    assert(requested_count(1) == 2);        /* dropped once, retried once */
    assert(requested_count(2) == 1);
    assert(n_requested == 3);
    printf("  resume            OK\n");
}

/* 2. idempotent re-entry: a complete stage costs a re-hash and no traffic. */
static void test_idempotent(void) {
    reset(blob_a, hash_a);
    memcpy(stage_buf, blob_a, BLOB_LEN);
    stage_len = BLOB_LEN;

    assert(ota_stage(hash_a, NULL, 0, "::1", 5684) == 1);
    assert(n_requested == 0);
    assert(stage_len == BLOB_LEN);
    printf("  idempotent        OK\n");
}

/* 3. corrupt: bytes that do not hash to the requested digest are rejected and
 *    the slot is truncated, so the next tick refetches from block 0. */
static void test_corrupt(void) {
    reset(blob_b, hash_a);       /* peer answers to hash A but serves blob B */
    memcpy(serve_hash, hash_a, 32);

    assert(ota_stage(hash_a, NULL, 0, "::1", 5684) == 0);
    assert(stage_len == 0);
    printf("  corrupt           OK\n");
}

/* 4. a full cold fetch from an empty slot. */
static void test_cold_fetch(void) {
    reset(blob_a, hash_a);
    assert(ota_stage(hash_a, NULL, 0, "::1", 5684) == 1);
    assert(stage_len == BLOB_LEN);
    assert(memcmp(stage_buf, blob_a, BLOB_LEN) == 0);
    assert(requested_count(0) == 1);
    assert(requested_count(1) == 2);
    assert(requested_count(2) == 1);
    printf("  cold fetch        OK\n");
}

/* 5. unknown hash at the peer: an 0x7F frame aborts without retrying. */
static void test_unknown_hash(void) {
    reset(blob_a, hash_a);
    memcpy(serve_hash, hash_b, 32);         /* peer knows B, we ask for A */
    drop_block1_once = 0;
    assert(ota_stage(hash_a, NULL, 0, "::1", 5684) == 0);
    assert(n_requested == 1);               /* an error frame is not retried */
    printf("  unknown hash      OK\n");
}

/* 6. the retarget wedge: a STALE slot longer than the new target.
 *
 * The slot carries no record of which image its bytes belong to, and the fetch
 * resumes at staged/256. Retarget a node while a partial slot survives, to an
 * image SHORTER than what is already staged, and the resume block is past the
 * new image's end: blockserv answers 0x7F "block past end". Without a truncate
 * on that path every cadence tick repeats the identical request forever and no
 * operator write can clear it — the node is wedged permanently.
 *
 * Asserted here as the two-tick self-heal the cadence actually performs: the
 * first call discards the slot and fails, the SECOND one succeeds from block 0.
 * The same wedge shape covers a wrong FULL-length slot sitting on an exact 256
 * multiple (off == len && blk != 0 is the same peer error), which this fix
 * closes by the same path.
 */
static void test_retarget_wedge(void) {
    reset(blob_a, hash_a);           /* the peer serves the 600-byte blob A */
    drop_block1_once = 0;            /* retry is covered above; keep counts exact */
    for (int i = 0; i < 11 * BLOCK; i++)      /* 2816 bytes of a former image */
        stage_buf[i] = (uint8_t)(i ^ 0x5a);
    stage_len = 11 * BLOCK;

    /* tick 1: resume asks for block 11, the peer says "block past end". */
    assert(ota_stage(hash_a, NULL, 0, "::1", 5684) == 0);
    assert(n_requested == 1);
    assert(requested[0] == 11);
    assert(stage_len == 0);          /* the wedge: this was 2816 before the fix */

    /* tick 2: the same call, unchanged, now completes from block 0. */
    n_requested = 0;
    assert(ota_stage(hash_a, NULL, 0, "::1", 5684) == 1);
    assert(stage_len == BLOB_LEN);
    assert(memcmp(stage_buf, blob_a, BLOB_LEN) == 0);
    assert(n_requested == 3);
    assert(requested[0] == 0 && requested[1] == 1 && requested[2] == 2);
    printf("  retarget wedge    OK\n");
}

/* 7. no stage slot configured: the seam must fail, never crash. A node
 *    launched with no ota env at all still runs. */
static void test_unconfigured(void) {
    reset(blob_a, hash_a);
    stage_configured = 0;
    assert(ota_stage(hash_a, NULL, 0, "::1", 5684) == 0);
    assert(n_requested == 0);
    uint8_t got[32];
    assert(ota_running_hash(got) == 0);
    assert(ota_is_running(hash_a) == 0);
    assert(ota_trial() == 0);
    ota_confirm();                          /* must not crash */
    printf("  unconfigured      OK\n");
}

/* 8. ota_running_hash / ota_is_running read OEVM_IMAGE_HASH. */
static void test_running_hash(void) {
    reset(blob_a, hash_a);
    uint8_t got[32];
    assert(ota_running_hash(got) == 0);     /* unset */

    snprintf(env_image_hash, sizeof env_image_hash, "%s", HASH_A_HEX);
    env_image_hash_set = 1;
    assert(ota_running_hash(got) == 1);
    assert(memcmp(got, hash_a, 32) == 0);
    assert(ota_is_running(hash_a) == 1);
    assert(ota_is_running(hash_b) == 0);

    snprintf(env_image_hash, sizeof env_image_hash, "not-hex");
    assert(ota_running_hash(got) == 0);     /* bad -> 0, not garbage */
    assert(ota_is_running(hash_a) == 0);
    printf("  running hash      OK\n");
}

/* 9. trial / confirm / activate. */
static void test_trial_confirm_activate(void) {
    reset(blob_a, hash_a);
    assert(ota_trial() == 0);
    env_trial = 1;
    assert(ota_trial() == 1);

    ota_confirm();
    assert(confirm_touches == 1);

    const uint8_t sig[4] = { 0xde, 0xad, 0xbe, 0xef };
    ota_activate(hash_a, sig, 4);
    assert(ready_writes == 1);
    assert(exits == 1);
    char want[256];
    snprintf(want, sizeof want, "{\"hash\":\"%s\",\"sig\":\"deadbeef\"}", HASH_A_HEX);
    if (strcmp(ready_json, want) != 0) {
        printf("ready.json mismatch\n  got  %s\n  want %s\n", ready_json, want);
        assert(0);
    }
    printf("  trial/activate    OK\n");
}

int main(void) {
    for (int i = 0; i < BLOB_LEN; i++) {
        blob_a[i] = (uint8_t)((i * 7 + 3) & 0xff);
        blob_b[i] = (uint8_t)((i * 11 + 5) & 0xff);
    }
    unhex(HASH_A_HEX, hash_a, 32);
    unhex(HASH_B_HEX, hash_b, 32);

    test_idempotent();          /* proves the SHA-256 against python hashlib */
    test_resume();
    test_cold_fetch();
    test_corrupt();
    test_unknown_hash();
    test_retarget_wedge();
    test_unconfigured();
    test_running_hash();
    test_trial_confirm_activate();
    printf("ota_seam_test OK\n");
    return 0;
}
