/* Copyright (c) 2026 Ekorau LLC */
#define _POSIX_C_SOURCE 200809L
#include "ota_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
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
 * ota_stage answers 0, and nothing crashes. That property is asserted by
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

int ota_write_ready(const char *json) {
    const char *path = ota_env("OEVM_OTA_READY");
    if (!path || !*path || !json) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t n = 0;
    while (json[n]) n++;
    int ok = (fwrite(json, 1, n, f) == n);
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

/* rete#233 T9: one detached pthread per fetch; ota.c runs one fetch at a time, so a
   single static start record is enough and nothing is malloc'd. */
static struct { void (*fn)(void *); void *arg; } start_rec;
static void *trampoline(void *p) {
    (void)p;
    start_rec.fn(start_rec.arg);
    return NULL;
}
int ota_thread_start(void (*fn)(void *), void *arg) {
    pthread_t t;
    pthread_attr_t a;
    start_rec.fn = fn; start_rec.arg = arg;
    if (pthread_attr_init(&a) != 0) return 0;
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int ok = pthread_create(&t, &a, trampoline, NULL) == 0;
    pthread_attr_destroy(&a);
    return ok;
}

void ota_sleep_ms(int ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
