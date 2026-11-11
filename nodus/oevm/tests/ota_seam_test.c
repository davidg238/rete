/* Copyright (c) 2026 Ekorau LLC */
/* rete#25 — the C `ota` seam, tested through its two seams and nothing else.
 *
 * Link set (see nodus/nsl/tools/run_tests_oevm.py (C_UNIT_TESTS)): src/ota.c + the posix console backend
 * ONLY. ota.c's one dependency, the file/env/exit half (ota_backend.h), is stubbed
 * IN THIS FILE, so the stage digest and the SHA-256 are proven without a
 * filesystem. The fetch is not here: since rete#440 R5 it is the kernel's OtaFetch,
 * and its resume, retry, peer-error and mismatch cases are nodus/tests/24_ota/06-08,
 * against the posix stage file.
 *
 * The expected SHA-256 digests below were computed with python hashlib, not
 * with this tree's own implementation — so the stage-digest assertions double as
 * a conformance check of ota.c's file-local SHA-256 against a reference.
 */
#include "../src/ota.h"
#include "../src/ota_backend.h"

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
/* rete#233 S2: the seam carries the raw hash and signature; the JSON is the posix
   backend's format, reproduced here so the trial/activate assertion keeps its text. */
int ota_write_ready(const uint8_t hash[32], const uint8_t *sig, int sig_len) {
    static const char d[] = "0123456789abcdef";
    char hh[65], sh[257];
    for (int i = 0; i < 32; i++) { hh[2*i] = d[hash[i] >> 4]; hh[2*i+1] = d[hash[i] & 15]; }
    hh[64] = 0;
    for (int i = 0; i < sig_len; i++) { sh[2*i] = d[sig[i] >> 4]; sh[2*i+1] = d[sig[i] & 15]; }
    sh[2*sig_len] = 0;
    snprintf(ready_json, sizeof ready_json, "{\"hash\":\"%s\",\"sig\":\"%s\"}", hh, sh);
    ready_writes++;
    return 1;
}
void ota_touch_confirm(void) { confirm_touches++; }
void ota_exit_stage_ready(void) { exits++; }   /* recorded, not taken */

static void reset(void) {
    stage_len = 0; stage_configured = 1;
    env_image_hash_set = 0; env_trial = 0;
    ready_writes = 0; confirm_touches = 0; exits = 0;
}

/* 1. ota_stage_matches: the whole slot, and only the whole slot. A complete stage
 *    of blob A matches hash A and not hash B; a prefix of it matches neither, so
 *    a partial stage is never taken for a staged image; an empty slot is not A. */
static void test_stage_matches(void) {
    reset();
    assert(ota_stage_matches(hash_a) == 0);         /* empty */
    memcpy(stage_buf, blob_a, BLOB_LEN);
    stage_len = BLOB_LEN;
    assert(ota_stage_matches(hash_a) == 1);
    assert(ota_stage_matches(hash_b) == 0);
    stage_len = 2 * BLOCK;                          /* a resumable prefix */
    assert(ota_stage_matches(hash_a) == 0);
    memcpy(stage_buf, blob_b, BLOB_LEN);
    stage_len = BLOB_LEN;
    assert(ota_stage_matches(hash_b) == 1);
    assert(ota_stage_matches(NULL) == 0);
    printf("  stage matches     OK\n");
}

/* 2. no stage slot configured: the seam must fail, never crash. A node
 *    launched with no ota env at all still runs. */
static void test_unconfigured(void) {
    reset();
    stage_configured = 0;
    assert(ota_stage_size() == -1);
    assert(ota_stage_matches(hash_a) == 0);
    uint8_t got[32];
    assert(ota_running_hash(got) == 0);
    assert(ota_is_running(hash_a) == 0);
    assert(ota_trial() == 0);
    ota_confirm();                          /* must not crash */
    printf("  unconfigured      OK\n");
}

/* 3. ota_running_hash / ota_is_running read OEVM_IMAGE_HASH. */
static void test_running_hash(void) {
    reset();
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

/* 4. trial / confirm / activate. */
static void test_trial_confirm_activate(void) {
    reset();
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

/* rete#310: ota_sha256 is the hash the C6 prints for the program it loaded.
   HASH_A_HEX was computed with python hashlib, so this is a conformance check. */
static void test_ota_sha256_known_answer(void) {
    uint8_t h[32];
    char hex[65];
    ota_sha256(blob_a, BLOB_LEN, h);
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", h[i]);
    assert(strcmp(hex, HASH_A_HEX) == 0);
    printf("ota_sha256 known answer: ok\n");
}

int main(void) {
    for (int i = 0; i < BLOB_LEN; i++) {
        blob_a[i] = (uint8_t)((i * 7 + 3) & 0xff);
        blob_b[i] = (uint8_t)((i * 11 + 5) & 0xff);
    }
    test_ota_sha256_known_answer();
    unhex(HASH_A_HEX, hash_a, 32);
    unhex(HASH_B_HEX, hash_b, 32);

    test_stage_matches();       /* proves the SHA-256 against python hashlib */
    test_unconfigured();
    test_running_hash();
    test_trial_confirm_activate();
    printf("ota_seam_test OK\n");
    return 0;
}
