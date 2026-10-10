/* Copyright (c) 2026 Ekorau LLC */
#include "runner.h"
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/storage/flash_map.h>
#include "ota.h"          /* ota_sha256 */

/* rete#233 S2 — see runner.h. Reads like oevm_runner.py's loop on purpose:
 *   1. a READY record names the staged slot: hash it, verify the signature, and make
 *      it active on trial — or spoil it, so the kernel refetches rather than re-boots it;
 *   2. a boot that still finds TRIAL set is the reset the trial image never confirmed
 *      before: revert to prev, exactly once (MCUboot's swap-test; this is what makes the
 *      rule hold through a deep-sleep node, whose sleep is a reset);
 *   3. slot a whose hash the manifest did not see was written by the cable: adopt it;
 *   4. VERIFY the active slot on every boot, when it carries a signature; a failure
 *      reverts to prev, or refuses when there is none.
 * The whole state is the manifest below, in settings under "ota/"; every change is
 * saved before the decision it records is acted on. */

#if FIXED_PARTITION_EXISTS(program_a) && FIXED_PARTITION_EXISTS(program_b) && defined(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#include <spi_flash_mmap.h>
#include "curve25519.h"   /* ED25519_verify — MCUboot's, vendored (src/ed25519/README.md) */

#define SLOT_A    0
#define SLOT_B    1
#define SLOT_NONE 0xff
#define TRIAL_MS  150000   /* §10 T6: five of the bell's 30 s cadences, the host gate's ratio */

static const struct { uint8_t id; uint32_t off; uint32_t size; char name; } slots[2] = {
    { FIXED_PARTITION_ID(program_a), FIXED_PARTITION_OFFSET(program_a), FIXED_PARTITION_SIZE(program_a), 'a' },
    { FIXED_PARTITION_ID(program_b), FIXED_PARTITION_OFFSET(program_b), FIXED_PARTITION_SIZE(program_b), 'b' },
};

static struct {
    uint8_t active, prev, trial, ready;
    uint8_t hash[2][32];
    uint8_t sig[2][64];
    uint8_t siglen[2];
    uint8_t rhash[32];
    uint8_t rsig[64];
    uint8_t rsiglen;
} m;

/* ---- the public key, from the build (T4) ------------------------------------------ */
#ifdef OEVM_OTA_PUBKEY
static uint8_t pubkey[32];
static int have_key;
static void key_init(void) {
    const char *hex = OEVM_OTA_PUBKEY;
    if (strlen(hex) != 64) { printk("runner: OEVM_OTA_PUBKEY is not 64 hex digits — no OTA\n"); return; }
    for (int i = 0; i < 32; i++) {
        unsigned v = 0;
        for (int j = 0; j < 2; j++) {
            char c = hex[2 * i + j];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else { printk("runner: OEVM_OTA_PUBKEY is not hex — no OTA\n"); return; }
        }
        pubkey[i] = (uint8_t)v;
    }
    have_key = 1;
}
#else
static const uint8_t pubkey[32];
static const int have_key = 0;
static void key_init(void) {}
#endif
int runner_has_key(void) { return have_key; }

/* ---- the manifest in settings (T3) ------------------------------------------------- */
static int m_set(const char *key, size_t len, settings_read_cb read_cb, void *arg) {
    void *dst = NULL; size_t cap = 0; uint8_t *lenp = NULL;
    if      (settings_name_steq(key, "active", NULL)) { dst = &m.active; cap = 1; }
    else if (settings_name_steq(key, "prev",   NULL)) { dst = &m.prev;   cap = 1; }
    else if (settings_name_steq(key, "trial",  NULL)) { dst = &m.trial;  cap = 1; }
    else if (settings_name_steq(key, "ready",  NULL)) { dst = &m.ready;  cap = 1; }
    else if (settings_name_steq(key, "ha", NULL)) { dst = m.hash[0]; cap = 32; }
    else if (settings_name_steq(key, "hb", NULL)) { dst = m.hash[1]; cap = 32; }
    else if (settings_name_steq(key, "rh", NULL)) { dst = m.rhash;   cap = 32; }
    else if (settings_name_steq(key, "sa", NULL)) { dst = m.sig[0]; cap = 64; lenp = &m.siglen[0]; }
    else if (settings_name_steq(key, "sb", NULL)) { dst = m.sig[1]; cap = 64; lenp = &m.siglen[1]; }
    else if (settings_name_steq(key, "rs", NULL)) { dst = m.rsig;   cap = 64; lenp = &m.rsiglen; }
    else return -ENOENT;
    if (len > cap) return -EINVAL;
    if (len == 0) { if (lenp) *lenp = 0; return 0; }
    ssize_t n = read_cb(arg, dst, len);
    if (n < 0) return (int)n;
    if (lenp) *lenp = (uint8_t)n;
    return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(oevm_ota, "ota", NULL, m_set, NULL, NULL);

static void m_save(void) {
    int rc = 0;
    rc |= settings_save_one("ota/active", &m.active, 1);
    rc |= settings_save_one("ota/prev",   &m.prev,   1);
    rc |= settings_save_one("ota/trial",  &m.trial,  1);
    rc |= settings_save_one("ota/ready",  &m.ready,  1);
    rc |= settings_save_one("ota/ha", m.hash[0], 32);
    rc |= settings_save_one("ota/hb", m.hash[1], 32);
    rc |= settings_save_one("ota/sa", m.sig[0], m.siglen[0]);
    rc |= settings_save_one("ota/sb", m.sig[1], m.siglen[1]);
    rc |= settings_save_one("ota/rh", m.rhash, 32);
    rc |= settings_save_one("ota/rs", m.rsig, m.rsiglen);
    if (rc) printk("runner: manifest save failed (%d) — the next boot decides from the old one\n", rc);
}

static void m_load(void) {
    (void)settings_subsys_init();          /* idempotent; platform_zephyr.c calls it too */
    memset(&m, 0, sizeof m);
    m.prev = SLOT_NONE;
    (void)settings_load_subtree("ota");    /* absent keys keep the defaults above */
    if (m.active > SLOT_B) m.active = SLOT_A;
}

/* ---- the slots --------------------------------------------------------------------- */
static spi_flash_mmap_handle_t handles[2];
static const uint8_t *bases[2];

static int map_slot(int s, const uint8_t **base, size_t *size) {
    if (!bases[s]) {
        const void *p;
        if (spi_flash_mmap(slots[s].off, slots[s].size, SPI_FLASH_MMAP_DATA, &p, &handles[s]) != 0) {
            printk("runner: slot %c at 0x%x would not map\n", slots[s].name, (unsigned)slots[s].off);
            return 0;
        }
        bases[s] = p;
    }
    uint32_t n;
    memcpy(&n, bases[s] + 12, 4);          /* blob_size, IMAGE-ARTIFACTS.md §2 */
    *base = bases[s];
    *size = (n >= 20 && n <= slots[s].size) ? n : 0;   /* erased reads 0xFFFFFFFF */
    return 1;
}

static int hash_slot(int s, uint8_t out[32]) {
    const uint8_t *b; size_t n;
    if (!map_slot(s, &b, &n) || n == 0) return 0;
    ota_sha256(b, (long)n, out);
    return 1;
}

static int verified(const uint8_t hash[32], const uint8_t *sig, int siglen) {
    if (!have_key || siglen != 64) return 0;
    return ED25519_verify(hash, 32, sig, pubkey) == 1;
}

/* Make a rejected staged image unrecognisable, so ota.c's "already staged" check
   fails and the kernel refetches rather than asking to boot it again. The host runner
   truncates the file; one erased sector is the flash analog. */
static void spoil(int s) {
    const struct flash_area *fa;
    if (flash_area_open(slots[s].id, &fa) == 0) {
        (void)flash_area_erase(fa, 0, 4096);
        flash_area_close(fa);
    }
}

static void hex8(const uint8_t h[32], char out[17]) {
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) { out[2*i] = d[h[i] >> 4]; out[2*i+1] = d[h[i] & 15]; }
    out[16] = 0;
}

/* ---- the trial timer (T6) ----------------------------------------------------------- */
static void trial_expired(struct k_work *w) {
    (void)w;
    printk("runner: trial %c did not confirm in %u s — resetting; the next boot reverts\n",
           slots[m.active].name, (unsigned)(TRIAL_MS / 1000));
    k_msleep(100);                         /* let the line out */
    sys_reboot(SYS_REBOOT_COLD);
}
static K_WORK_DELAYABLE_DEFINE(trial_work, trial_expired);

/* ---- the loop ----------------------------------------------------------------------- */
int runner_select(const uint8_t **base, size_t *size) {
    char hx[17];
    uint8_t h[32];
    key_init();
    m_load();

    if (m.ready) {                                           /* 1. a staged image waits */
        int s = m.active == SLOT_A ? SLOT_B : SLOT_A;
        m.ready = 0;
        if (!hash_slot(s, h) || memcmp(h, m.rhash, 32) != 0) {
            printk("runner: staged-reject slot %c does not hash to the ready record\n", slots[s].name);
        } else if (!verified(h, m.rsig, m.rsiglen)) {
            hex8(h, hx);
            printk("runner: staged-reject bad signature hash=%s…\n", hx);
            spoil(s);
        } else {
            m.prev = m.active; m.active = (uint8_t)s; m.trial = 1;
            memcpy(m.hash[s], h, 32); memcpy(m.sig[s], m.rsig, 64); m.siglen[s] = 64;
            hex8(h, hx);
            printk("runner: staged-ok -> trial %c hash=%s…\n", slots[s].name, hx);
        }
        m_save();
    } else if (m.trial) {                                    /* 2. a trial boot that reset */
        int from = m.active;
        if (m.prev != SLOT_NONE) m.active = m.prev;
        m.prev = SLOT_NONE; m.trial = 0;
        printk("runner: revert %c -> %c (the trial boot ended without a confirm)\n",
               slots[from].name, slots[m.active].name);
        m_save();
    }

    if (hash_slot(SLOT_A, h) && memcmp(h, m.hash[SLOT_A], 32) != 0) {   /* 3. the cable */
        hex8(h, hx);
        printk("runner: slot a was written by the cable (hash=%s…) — active, unsigned, no trial\n", hx);
        m.active = SLOT_A; m.prev = SLOT_NONE; m.trial = 0;
        memcpy(m.hash[SLOT_A], h, 32); m.siglen[SLOT_A] = 0;
        m_save();
    }

    for (int tries = 0; tries < 2; tries++) {                /* 4. verify, then run */
        int s = m.active;
        int ok = hash_slot(s, h) && memcmp(h, m.hash[s], 32) == 0 &&
                 (m.siglen[s] == 0 || verified(h, m.sig[s], m.siglen[s]));
        if (ok) {
            hex8(h, hx);
            printk("runner: exec %c hash=%s… %s%s\n", slots[s].name, hx,
                   m.siglen[s] ? "signed" : "cable", m.trial ? " trial" : "");
            if (m.trial) k_work_schedule(&trial_work, K_MSEC(TRIAL_MS));
            return map_slot(s, base, size);
        }
        printk("runner: verify-fail slot %c\n", slots[s].name);
        if (m.prev == SLOT_NONE) break;
        m.active = m.prev; m.prev = SLOT_NONE; m.trial = 0;
        m_save();
    }
    printk("runner: no trustworthy slot — the node idles\n");
    *base = NULL; *size = 0;
    return 0;
}

int runner_ready(const uint8_t hash[32], const uint8_t *sig, int sig_len) {
    if (!have_key || !hash) return 0;
    if (sig_len < 0) sig_len = 0;
    if (sig_len > 64) sig_len = 64;
    memcpy(m.rhash, hash, 32);
    if (sig_len) memcpy(m.rsig, sig, (size_t)sig_len);
    m.rsiglen = (uint8_t)sig_len;
    m.ready = 1;
    m_save();
    return 1;
}

void runner_confirm(void) {
    if (!m.trial) return;
    m.trial = 0;
    k_work_cancel_delayable(&trial_work);
    m_save();
    printk("runner: confirmed %c\n", slots[m.active].name);
}

int runner_trial(void) { return m.trial; }
uint8_t runner_inactive_slot_id(void) { return slots[m.active == SLOT_A ? SLOT_B : SLOT_A].id; }

#else  /* no program slots on this board: the host runner's absence, honestly */

int  runner_select(const uint8_t **base, size_t *size) { *base = NULL; *size = 0; return 0; }
int  runner_ready(const uint8_t hash[32], const uint8_t *sig, int sig_len) { (void)hash; (void)sig; (void)sig_len; return 0; }
void runner_confirm(void) {}
int  runner_trial(void) { return 0; }
uint8_t runner_inactive_slot_id(void) { return 0; }
int  runner_has_key(void) { return 0; }

#endif
