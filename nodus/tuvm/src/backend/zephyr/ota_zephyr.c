/* Copyright (c) 2026 Ekorau LLC */
#include "ota_backend.h"
#include <stddef.h>   /* NULL — this file includes no zephyr header (see below) */

/* rete#25 — the zephyr half of the ota seam: PRESENT AND DELIBERATELY INERT.
 *
 * Why it exists at all. platform.c now calls ota_running_hash() and its five
 * siblings unconditionally (prims 145-150), and zephyr/CMakeLists.txt compiles
 * platform.c. Without this file the zephyr link fails on nine undefined
 * symbols — so the choice was never "posix-first or not", it was between a
 * no-op backend and a #ifdef fence through six prim cases. The fence loses:
 * it would make `platform otaTrial` a doesNotUnderstand on target and an answer
 * on host, i.e. the same app would be a different app depending on where it
 * ran. An inert backend keeps ONE seam with one shape everywhere, and every
 * answer it produces is one the contract already defines.
 *
 * Why inertness is CORRECT here rather than a stub-shaped lie. ota_backend.h's
 * standing contract is that every function no-ops or fails gracefully when its
 * environment variable is unset, and on target NOTHING is set: there is no
 * getenv, so ota_env answers NULL by construction and not by omission. Follow
 * that through ota.c and every VM-facing answer is already the honest one —
 * ota_running_hash 0 (nil), ota_trial 0, ota_stage 0 after "no staging slot".
 * A target node therefore reports "I do not know what I am running", which is
 * true, instead of claiming a hash it cannot verify.
 *
 * What the real one becomes, when the target rung comes: the staging slot is
 * the inactive MCUboot flash partition (flash_area_* against
 * FIXED_PARTITION_ID(slot1_partition)), ota_touch_confirm becomes
 * boot_write_img_confirmed(), ota_write_ready becomes boot_request_upgrade(),
 * ota_exit_stage_ready becomes sys_reboot(SYS_REBOOT_WARM), and ota_sleep_ms
 * becomes k_msleep(). None of that is guessed at here — an invented flash
 * layout would be a tunable chosen before there is a board to measure it on.
 *
 * Pure C99 on purpose: no zephyr header is included, so this file compiles and
 * links with a plain host compiler, which is how it was proved (task-6 report).
 */

const char *ota_env(const char *name) {
    (void)name;
    return NULL;                 /* no runner environment on target */
}

long ota_stage_size(void) {
    return -1;                   /* "unset or unusable" — no slot is configured */
}

int ota_stage_read(uint8_t *buf, long off, long len) {
    (void)buf; (void)off; (void)len;
    return 0;
}

int ota_stage_append(const uint8_t *data, int len) {
    (void)data; (void)len;
    return 0;
}

void ota_stage_truncate(void) {
}

int ota_write_ready(const char *json) {
    (void)json;
    return 0;
}

void ota_touch_confirm(void) {
}

void ota_exit_stage_ready(void) {
    /* Returns, which ota.h documents as what happens when the target cannot
       exit. On a real target this becomes sys_reboot(); returning is strictly
       better than a silent hang, because the caller's next line runs and the
       node keeps serving its space. */
}

void ota_sleep_ms(int ms) {
    (void)ms;
    /* Never reached: the only caller is ota.c's block-fetch poll loop, which
       ota_stage cannot get to while ota_stage_size answers -1. Left as a no-op
       rather than a busy-wait so that if it ever IS reached, it burns no time
       pretending to. */
}
