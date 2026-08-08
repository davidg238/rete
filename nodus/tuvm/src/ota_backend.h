/* Copyright (c) 2026 Ekorau LLC */
#ifndef OEVM_OTA_BACKEND_H
#define OEVM_OTA_BACKEND_H
#include <stdint.h>
/* rete#25 — the ota seam's per-target half: the staging slot, the runner's
 * environment, the two markers and the exit. Exactly one impl is linked per
 * build from src/backend/$OEVM_TARGET/, the same rule as udp_backend.h and
 * platform_backend.h:
 *   backend/posix/ota_posix.c   — files under a runner-owned node directory.
 *   backend/zephyr/ota_zephyr.c — NOT WRITTEN YET. The staging slot becomes the
 *                                 inactive MCUboot flash partition, the markers
 *                                 become boot_write_img_confirmed()/the mcuboot
 *                                 swap flags, and the exit becomes sys_reboot().
 *                                 Until it exists, ota.c is a POSIX-only source
 *                                 and must not be enrolled in zephyr's
 *                                 CMakeLists — the seam is posix-first by
 *                                 design (design §4).
 *
 * CONTRACT: every function here must NO-OP OR FAIL GRACEFULLY when its
 * environment variable is unset. A node launched with no runner and no ota env
 * at all — which is every host test in this tree — must never crash in this
 * seam. That is why nothing here aborts and why ota_stage_size answers -1
 * rather than trapping.
 */

/* The runner's environment. getenv on posix; a fixed table on target. NULL when
 * unset. */
const char *ota_env(const char *name);

/* Length of the staging slot in bytes: >=0 when a slot is configured (0 when it
 * does not exist yet), -1 when OEVM_OTA_STAGE is unset or unusable. The length
 * IS the fetch cursor — see ota.h. */
long ota_stage_size(void);

/* Read `len` bytes at `off` from the slot. 1 = ok, 0 = short/failed. */
int  ota_stage_read(uint8_t *buf, long off, long len);

/* Append `len` bytes to the slot, creating it if needed. 1 = ok, 0 = failed.
 * Append-only is deliberate: it is the flash analog, and it is what makes the
 * slot length a truthful resume cursor after a kill at any instant. */
int  ota_stage_append(const uint8_t *data, int len);

/* Truncate the slot to zero — the only length a torn or wrong stage may be
 * reset to, so the next tick refetches from block 0. */
void ota_stage_truncate(void);

/* Write `json` to the OEVM_OTA_READY path. 1 = ok, 0 = unset/failed. */
int  ota_write_ready(const char *json);

/* Create/refresh the OEVM_OTA_CONFIRM marker. Silent no-op when unset. */
void ota_touch_confirm(void);

/* Leave the process in the STAGE_READY state the runner watches for: exit(42)
 * on posix, sys_reboot() on target. Returns only if the target cannot. */
void ota_exit_stage_ready(void);

/* Sleep roughly `ms` milliseconds. The block wait is a deadline poll loop
 * because udp_receive is non-blocking on every backend, and a poll loop needs
 * exactly one portable primitive; adding it here keeps ota.c free of both
 * <unistd.h> and any clock (the host timer clock is VIRTUAL — see
 * timer_backend.h — so it cannot time a real network wait). */
void ota_sleep_ms(int ms);

#endif /* OEVM_OTA_BACKEND_H */
