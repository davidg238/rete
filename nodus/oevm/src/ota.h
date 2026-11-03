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

/* The fetch of the image with digest `hash` from the block server at host:port,
 * into the staging slot — OFF THIS THREAD (rete#233 T9, 2026-10-10). The bulk plane
 * is off the message plane on the wire (D1, G3) and, since the first target run,
 * off the VM thread too: on a board the fetch takes ~55 s over Thread, and a prim
 * that blocks that long is exactly what the observer's app-stale rule reboots.
 *
 * So this call never blocks for long. It answers:
 *    1  staged: the slot holds `hash`, digest verified (`*staged` = its length);
 *    0  in progress: a fetch is running, `*staged` = bytes on flash so far;
 *   -1  failed this round: no slot, no thread, a peer error, a timeout, or a
 *       digest mismatch (the slot is then truncated).
 * The caller is the check-in tail, so it is called again at the node's own
 * cadence until it answers 1; nothing else polls. Between calls the thread:
 *   - resumes at block staged_len/256 — the slot's own length is the cursor;
 *   - a staged slot that already hashes to `hash` is answered 1 at once, with
 *     no thread, so a crash after staging costs one re-hash and no traffic;
 *   - a whole-file digest mismatch at the end truncates the slot.
 * A fetch that finished for ANOTHER hash (a retarget) is dropped on the next call,
 * and the new image is fetched into whatever the slot holds — the failure table's
 * self-healing case, at the price of one transfer.
 *
 * `sig`/`sig_len` are carried, not checked: verification has exactly one seat
 * and it is the runner (design D4). Pass NULL/0 when there is none. */
int  ota_stage(const uint8_t hash[32], const uint8_t *sig, int sig_len,
               const char *host, int port, long *staged);

/* rete#432: a small blob (a debug image's patch set), fetched on the CALLER's
 * thread from the block server at host:port into buf[0..size): the same block
 * protocol as ota_stage, but into memory, not the slot, and blocking — a patch set
 * is a few blocks, not a program. 1 = buf holds exactly `size` bytes whose SHA-256
 * is `hash`; 0 = it does not (a timeout, a peer error, a length or digest
 * mismatch; the reason went to the console). `size` comes from the BlobRef, so a
 * longer blob is refused at the block that overruns it, before buf does. */
int  ota_fetch_blob(const char *host, int port, const uint8_t hash[32], long size, uint8_t *buf);
/* The largest blob ota_fetch_blob is asked for: METHOD_OVERRIDES_MAX (16) patches of
 * up to 1 KiB of text each. Invented (#432); a set refused by it in use is what
 * would move it. */
#define OTA_BLOB_MAX  16384

/* rete#432: restart the node, keeping what a software reset keeps (RTC memory on
 * Zephyr); the host exits 75, "relaunch me". Prim 166. Does not return where the
 * backend can leave. */
void ota_reboot(void);

/* Touch the confirm marker — "this trial image works". Becomes
 * boot_write_img_confirmed() on target. Idempotent; never fails loudly. */
void ota_confirm(void);

/* Write stage/ready.json as exactly {"hash":"<hex>","sig":"<hex>"} and exit the
 * process with code 42 (STAGE_READY), the host analog of rebooting into the
 * trial slot. Does not return when the backend really exits. */
void ota_activate(const uint8_t hash[32], const uint8_t *sig, int sig_len);

#endif /* OEVM_OTA_H */
