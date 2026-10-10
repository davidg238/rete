/* Copyright (c) 2026 Ekorau LLC */
#include "ota_backend.h"
#include <stddef.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/storage/flash_map.h>

/* rete#25 / rete#233 — the zephyr half of the ota seam.
 *
 * The staging slot is the inactive PROGRAM slot (code-push design §10 T2): two equal
 * partitions, `program_a` and `program_b`, and the firmware runs the active one in
 * place. The seam below writes the inactive one through flash_map, append-only, which
 * is the contract ota.c's resume arithmetic was written against. Where the posix
 * backend has files and an environment, this one has:
 *
 *   OEVM_OTA_STAGE    -> the inactive slot, by FIXED_PARTITION_ID (no copy of an offset)
 *   the slot length   -> `staged_len`, in RAM. A flash slot has no length of its own,
 *                        so a reboot mid-fetch refetches from block 0 (§10 T7: the
 *                        degenerate case; a stored cursor is earned by a measured
 *                        mid-fetch reboot rate). Block 0 erases the whole slot first.
 *   OEVM_OTA_READY    -> the runner's `ota/ready` record in settings (S2, rete#233;
 *                        answers 0 until then, so a staged image is logged and NOT
 *                        booted — ota.c's "cannot write ready" line, every tick)
 *   OEVM_OTA_CONFIRM  -> `ota/trial := 0` in settings (S2)
 *   exit(42)          -> sys_reboot (S2); until then it returns, which ota.h allows
 *
 * A board without program slots (every non-ESP target today) keeps the inert
 * answers: no getenv, ota_env NULL by construction, ota_stage_size -1. One seam, one
 * shape, everywhere — the reason this file exists at all (rete#25 task 6).
 *
 * Write alignment: both ESP flashes declare write-block-size 4; a block is 256 B and
 * only the image's last block can be shorter, so the only padding ever written is at
 * the very end, with 0xFF (the erased value), and `staged_len` counts real bytes so
 * the padding is never hashed. */

#if defined(CONFIG_FLASH_MAP) && FIXED_PARTITION_EXISTS(program_b)
#define OTA_HAVE_SLOT 1
static uint8_t stage_id = FIXED_PARTITION_ID(program_b);   /* the inactive slot */
static long    staged_len;                                  /* 0 at every boot */
#endif

static char running_hex[65];   /* "" until main.c notes the loaded program (rete#310) */

void ota_note_running_hash(const uint8_t h[32]) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        running_hex[2 * i]     = digits[h[i] >> 4];
        running_hex[2 * i + 1] = digits[h[i] & 0xf];
    }
    running_hex[64] = '\0';
}

const char *ota_env(const char *name) {
    /* No runner environment on target. The one name that answers is the hash of
       the program main.c loaded from program_a (rete#310). */
    if (running_hex[0] != '\0' && strcmp(name, "OEVM_IMAGE_HASH") == 0) return running_hex;
    return NULL;
}

#ifdef OTA_HAVE_SLOT

static const struct flash_area *slot(void) {
    const struct flash_area *fa = NULL;
    if (flash_area_open(stage_id, &fa) != 0) return NULL;
    return fa;
}

long ota_stage_size(void) { return staged_len; }

int ota_stage_read(uint8_t *buf, long off, long len) {
    if (!buf || off < 0 || len < 0 || off + len > staged_len) return 0;
    if (len == 0) return 1;
    const struct flash_area *fa = slot();
    if (!fa) return 0;
    int ok = flash_area_read(fa, (off_t)off, buf, (size_t)len) == 0;
    flash_area_close(fa);
    return ok;
}

int ota_stage_append(const uint8_t *data, int len) {
    if (!data || len < 0) return 0;
    if (len == 0) return 1;
    const struct flash_area *fa = slot();
    if (!fa) return 0;
    if ((long)fa->fa_size < staged_len + len) { flash_area_close(fa); return 0; }
    if (staged_len == 0) {
        /* Block 0 erases the slot: the flash analog of fopen("wb"). Whole-slot,
           because a partial erase would be a cursor of its own. Timed, not guessed. */
        int64_t t0 = k_uptime_get();
        if (flash_area_erase(fa, 0, fa->fa_size) != 0) { flash_area_close(fa); return 0; }
        printk("ota: slot erased, %u B in %u ms\n", (unsigned)fa->fa_size,
               (unsigned)(k_uptime_get() - t0));
    }
    /* Pad the last short block to the 4-byte write unit with the erased value. */
    uint8_t padded[260];
    const uint8_t *p = data;
    int n = len;
    if (len % 4 != 0) {
        if (len > (int)sizeof padded) { flash_area_close(fa); return 0; }
        memset(padded, 0xFF, sizeof padded);
        memcpy(padded, data, (size_t)len);
        p = padded;
        n = (len + 3) & ~3;
    }
    int ok = flash_area_write(fa, (off_t)staged_len, p, (size_t)n) == 0;
    flash_area_close(fa);
    if (ok) staged_len += len;
    return ok;
}

void ota_stage_truncate(void) { staged_len = 0; }   /* the next block 0 erases */

#else  /* no program slots: the inert answers of rete#25 task 6 */

long ota_stage_size(void) { return -1; }
int  ota_stage_read(uint8_t *buf, long off, long len) { (void)buf; (void)off; (void)len; return 0; }
int  ota_stage_append(const uint8_t *data, int len) { (void)data; (void)len; return 0; }
void ota_stage_truncate(void) {}

#endif

int ota_write_ready(const char *json) {
    (void)json;
    return 0;        /* S2 (rete#233): the runner's ota/ready record in settings */
}

void ota_touch_confirm(void) {
    /* S2 (rete#233): ota/trial := 0 in settings */
}

void ota_exit_stage_ready(void) {
    /* S2 (rete#233): sys_reboot(SYS_REBOOT_WARM). Returning is what ota.h documents
       for a target that cannot exit, and strictly better than a hang: the node keeps
       serving its space. */
}

void ota_sleep_ms(int ms) {
    if (ms > 0) k_msleep(ms);
}
