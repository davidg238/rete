/* Copyright (c) 2026 Ekorau LLC */
#ifndef OEVM_OTA_H
#define OEVM_OTA_H
#include <stdint.h>
/* rete#25 — the node's image-push seam, VM-facing half.
 *
 * The data plane stays OFF the message algebra (design G3): a staged image is a
 * byte slab that must live outside the VM heap, so it is fetched here in C, by
 * content hash, straight into the inactive slot. Nothing in this header
 * allocates, and nothing here knows what an nsl object is — platform.c's three
 * prims (otaStage:sig:, otaActivate, otaConfirm) are the only callers.
 *
 * Every answer is 1 = yes/ok, 0 = no/failure. Failure DETAIL goes to the
 * console; publishing it as `tel/log` is the nsl caller's job, because only the
 * caller knows the cadence tick it belongs to.
 *
 * The fetch loop is written against udp_backend.h, not against sockets, so it
 * ports to the zephyr backend unchanged. The file/env/exit half is
 * ota_backend.h.
 */

/* Fill `out` with the 32-byte digest of the image this process is running,
 * parsed from OEVM_IMAGE_HASH. 0 (leaving `out` untouched) when the variable is
 * unset or is not 64 hex digits — a node launched with no runner is normal, not
 * an error. */
int  ota_running_hash(uint8_t out[32]);
/* SHA-256 of p[0..n). rete#310: the C6 hashes the program it loaded with this. */
void ota_sha256(const uint8_t *p, long n, uint8_t out[32]);

/* 1 when `hash` is the running image. The `goal/apps/main` vs OEVM_IMAGE_HASH
 * comparison of design D6, in one place so no caller re-derives it. */
int  ota_is_running(const uint8_t hash[32]);

/* 1 when this boot is a trial boot (OEVM_OTA_TRIAL=1), which is the runner
 * telling the VM that an unconfirmed image is under test. */
int  ota_trial(void);

/* BLOCKING stop-and-wait fetch of the image with digest `hash` from the block
 * server at host:port, appended into the staging slot (design D7 — the exchange
 * cadence is seconds and a stage is bounded, so a synchronous fetch costs one
 * late tick and buys no scheduler).
 *
 *   - a staged slot that already hashes to `hash` answers 1 immediately, so a
 *     crash after staging costs one re-hash and no traffic;
 *   - otherwise the fetch RESUMES at block staged_len/256 — the slot's own
 *     length is the cursor, and no cursor state is invented;
 *   - a whole-file digest mismatch at the end truncates the slot and answers 0.
 *
 * `sig`/`sig_len` are carried, not checked: verification has exactly one seat
 * and it is the runner (design D4). Pass NULL/0 when there is none. */
int  ota_stage(const uint8_t hash[32], const uint8_t *sig, int sig_len,
               const char *host, int port);

/* Touch the confirm marker — "this trial image works". Becomes
 * boot_write_img_confirmed() on target. Idempotent; never fails loudly. */
void ota_confirm(void);

/* Write stage/ready.json as exactly {"hash":"<hex>","sig":"<hex>"} and exit the
 * process with code 42 (STAGE_READY), the host analog of rebooting into the
 * trial slot. Does not return when the backend really exits. */
void ota_activate(const uint8_t hash[32], const uint8_t *sig, int sig_len);

#endif /* OEVM_OTA_H */
