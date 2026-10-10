/* Copyright (c) 2026 Ekorau LLC */
#ifndef OEVM_RUNNER_H
#define OEVM_RUNNER_H
#include <stddef.h>
#include <stdint.h>
/* rete#233 S2 — the firmware's half of the image-push runner: what oevm_runner.py is
 * on the host (code-push design §5, transcribed step for step per §10 T1), for the two
 * program slots `program_a` / `program_b` (T2). The manifest lives in `settings`
 * under "ota/" (T3); the signature seat is MCUboot's ed25519 over the 32 raw hash
 * bytes, against the public key compiled in from porta's pub file (T4); a slot the
 * manifest does not describe was written by the cable and is trusted as the cable is
 * (T5); a trial image gets ONE boot, plus a timer that resets a trial that never
 * confirms (T6). On a board without program slots every function below is a stub. */

/* Decide which slot to run and map it. 1 and the program's base/size; 0 when no slot is
   trustworthy (the node then idles with its console up, as an invalid program does).
   Prints one `runner: …` line per decision, the host runner's vocabulary. */
int  runner_select(const uint8_t **base, size_t *size);

/* The ota seam's three markers (ota_backend.h), in manifest terms. */
int  runner_ready(const uint8_t hash[32], const uint8_t *sig, int sig_len);  /* 1 = recorded */
void runner_confirm(void);                                                    /* trial := 0 */
int  runner_trial(void);                                                      /* this boot is a trial */
uint8_t runner_inactive_slot_id(void);     /* FIXED_PARTITION_ID of the staging slot */
int  runner_has_key(void);                 /* 0 = built without OEVM_OTA_PUBKEY: no OTA */

#endif
