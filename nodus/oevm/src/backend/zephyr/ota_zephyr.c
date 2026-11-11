/* Copyright (c) 2026 Ekorau LLC */
#include "ota_backend.h"
#include <stddef.h>
#include <string.h>
#ifdef __ZEPHYR__
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/storage/flash_map.h>
#endif

/* rete#25 / rete#233 — the zephyr half of the ota seam.
 *
 * The staging slot is the inactive PROGRAM slot (code-push design §10 T2): two equal
 * partitions, `program_a` and `program_b`, and the firmware runs the active one in
 * place. The seam below writes the inactive one through flash_map, append-only, which
 * is the contract the kernel's resume arithmetic (OtaFetch, rete#440 R5) was written against. Where the posix
 * backend has files and an environment, this one has:
 *
 *   OEVM_OTA_STAGE    -> the inactive slot, by FIXED_PARTITION_ID (no copy of an offset)
 *   the slot length   -> `staged_len`, in RAM. A flash slot has no length of its own,
 *                        so a reboot mid-fetch refetches from block 0 (§10 T7: the
 *                        degenerate case; a stored cursor is earned by a measured
 *                        mid-fetch reboot rate). Block 0 erases the whole slot first.
 *   OEVM_OTA_READY    -> the runner's ready record in the settings manifest
 *   OEVM_OTA_CONFIRM  -> the manifest's trial flag cleared, the trial timer cancelled
 *   exit(42)          -> sys_reboot: the runner is the next boot (zephyr/src/runner.c)
 *
 * A board without program slots (every non-ESP target today) keeps the inert
 * answers: no getenv, ota_env NULL by construction, ota_stage_size -1. One seam, one
 * shape, everywhere — the reason this file exists at all (rete#25 task 6).
 *
 * Write alignment: both ESP flashes declare write-block-size 4; a block is 256 B and
 * only the image's last block can be shorter, so the only padding ever written is at
 * the very end, with 0xFF (the erased value), and `staged_len` counts real bytes so
 * the padding is never hashed. */

/* tests/ota_zephyr_test.c compiles this file with a plain host compiler to prove the
   inert half; __ZEPHYR__ keeps the slot half, and its headers, on the board. */
#ifdef __ZEPHYR__
#include "runner.h"   /* rete#233 S2: the manifest, the slots, the trial — zephyr/src/runner.c */
#if defined(CONFIG_FLASH_MAP) && FIXED_PARTITION_EXISTS(program_b)
#define OTA_HAVE_SLOT 1
static long staged_len;                                     /* 0 at every boot */
#endif
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
    /* No runner environment on target. Two names answer: the hash of the program
       main.c loaded (rete#310), and whether this boot is a trial (rete#233 S2, from
       the runner's manifest). */
    if (running_hex[0] != '\0' && strcmp(name, "OEVM_IMAGE_HASH") == 0) return running_hex;
#ifdef __ZEPHYR__
    if (strcmp(name, "OEVM_OTA_TRIAL") == 0 && runner_trial()) return "1";
#endif
    return NULL;
}

#ifdef OTA_HAVE_SLOT

static const struct flash_area *slot(void) {
    const struct flash_area *fa = NULL;
    if (flash_area_open(runner_inactive_slot_id(), &fa) != 0) return NULL;
    return fa;
}

/* -1 without a key: a build that cannot verify a program must not fetch one (T4). */
long ota_stage_size(void) { return runner_has_key() ? staged_len : -1; }

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
    if (ok) {
        staged_len += len;
        /* rete#233 S1: the fetch rate over the radio is the number this rung is for,
           so the progress is printed from here, every 64 blocks (16 KB), against the
           slot erase as t0. One append is one block of the kernel's fetch (rete#440 R5). */
        static int64_t t0;
        if (staged_len == len) t0 = k_uptime_get();
        else if ((staged_len / 256) % 64 == 0)
            printk("ota: staged %ld B at +%u ms\n", staged_len, (unsigned)(k_uptime_get() - t0));
    }
    return ok;
}

void ota_stage_truncate(void) { staged_len = 0; }   /* the next block 0 erases */

#else  /* no program slots: the inert answers of rete#25 task 6 */

long ota_stage_size(void) { return -1; }
int  ota_stage_read(uint8_t *buf, long off, long len) { (void)buf; (void)off; (void)len; return 0; }
int  ota_stage_append(const uint8_t *data, int len) { (void)data; (void)len; return 0; }
void ota_stage_truncate(void) {}

#endif

int ota_write_ready(const uint8_t hash[32], const uint8_t *sig, int sig_len) {
#ifdef __ZEPHYR__
    return runner_ready(hash, sig, sig_len);      /* the manifest's ready record (S2) */
#else
    (void)hash; (void)sig; (void)sig_len;
    return 0;
#endif
}

void ota_touch_confirm(void) {
#ifdef __ZEPHYR__
    runner_confirm();                             /* trial := 0, the timer cancelled */
#endif
}

void ota_exit_stage_ready(void) {
#ifdef __ZEPHYR__
    /* The host analog is exit(42) into the runner; here the runner is the next boot.
       The console line that announced this must get out first. */
    printk("ota: rebooting into the runner\n");
    k_msleep(100);
    sys_reboot(SYS_REBOOT_COLD);
#endif
}
