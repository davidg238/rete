/* Copyright (c) 2026 Ekorau LLC */
#define _POSIX_C_SOURCE 200809L
#include "ota_backend.h"
#include <stdio.h>
#include <stdlib.h>
/* POSIX (x86-linux / macOS desktop) ota backend: the staging slot and the two
 * markers are plain files under the runner's per-node directory, named by
 * environment variables the runner sets (design §5):
 *
 *   OEVM_OTA_STAGE    slots/<inactive>.oimg   the staging slot (append-only)
 *   OEVM_OTA_READY    stage/ready.json        {"hash":...,"sig":...}
 *   OEVM_OTA_CONFIRM  confirm                 the trial-confirm marker
 *
 * Every one of them is optional. With none set — which is every host test in
 * this tree and every hand-started VM — the whole seam degrades to "no slot",
 * the stage size answers -1, and nothing crashes. That property is asserted by
 * tests/ota_seam_test.c's `unconfigured` case.
 *
 * There is no rename and no temp file: the slot is written in place, exactly as
 * the target will write flash, so the resume arithmetic that ships is the
 * arithmetic the device runs.
 */

const char *ota_env(const char *name) { return getenv(name); }

long ota_stage_size(void) {
    const char *path = ota_env("OEVM_OTA_STAGE");
    if (!path || !*path) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;                    /* not created yet == empty slot */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long n = ftell(f);
    fclose(f);
    return n < 0 ? -1 : n;
}

int ota_stage_read(uint8_t *buf, long off, long len) {
    const char *path = ota_env("OEVM_OTA_STAGE");
    if (!path || !*path || off < 0 || len < 0) return 0;
    if (len == 0) return 1;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int ok = (fseek(f, off, SEEK_SET) == 0) &&
             (fread(buf, 1, (size_t)len, f) == (size_t)len);
    fclose(f);
    return ok;
}

int ota_stage_append(const uint8_t *data, int len) {
    const char *path = ota_env("OEVM_OTA_STAGE");
    if (!path || !*path || len < 0) return 0;
    if (len == 0) return 1;
    FILE *f = fopen(path, "ab");
    if (!f) return 0;
    int ok = (fwrite(data, 1, (size_t)len, f) == (size_t)len);
    if (fflush(f) != 0) ok = 0;          /* the length is the cursor: land it */
    if (fclose(f) != 0) ok = 0;
    return ok;
}

void ota_stage_truncate(void) {
    const char *path = ota_env("OEVM_OTA_STAGE");
    if (!path || !*path) return;
    FILE *f = fopen(path, "wb");
    if (f) fclose(f);
}

static void tohex(const uint8_t *p, int n, char *out) {
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[2*i] = d[p[i] >> 4]; out[2*i+1] = d[p[i] & 15]; }
    out[2*n] = 0;
}
int ota_write_ready(const uint8_t hash[32], const uint8_t *sig, int sig_len) {
    const char *path = ota_env("OEVM_OTA_READY");
    if (!path || !*path || !hash) return 0;
    if (sig_len < 0) sig_len = 0;
    if (sig_len > 128) sig_len = 128;
    /* Exactly {"hash":"<hex>","sig":"<hex>"} — the runner's ready.json (design §5). */
    char hash_hex[65], sig_hex[257];
    tohex(hash, 32, hash_hex);
    tohex(sig, sig_len, sig_hex);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fprintf(f, "{\"hash\":\"%s\",\"sig\":\"%s\"}", hash_hex, sig_hex) > 0;
    if (fflush(f) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    return ok;
}
void ota_touch_confirm(void) {
    const char *path = ota_env("OEVM_OTA_CONFIRM");
    if (!path || !*path) return;
    FILE *f = fopen(path, "wb");
    if (f) { fflush(f); fclose(f); }
}

void ota_exit_stage_ready(void) {
    fflush(NULL);                        /* the console line must outlive us */
    exit(42);                            /* STAGE_READY — the runner's contract */
}
