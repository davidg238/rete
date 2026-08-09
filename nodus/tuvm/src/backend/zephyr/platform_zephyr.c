/* oevm/src/backend/zephyr/platform_zephyr.c
   Zephyr console backend for the platform print:/printNewline: prims. Writes to the
   Zephyr console (uart0/VCOM on the nRF52840-DK, the SoC USB-CDC on the dongle,
   native_sim stdout otherwise). stdout is wired to the console by
   CONFIG_STDOUT_CONSOLE; main() sets _IONBF so each line appears as produced.
   platform_debug_readline is a stub: the R3 debug transport is out of scope here
   (no keeper on device yet). */
#include "platform_backend.h"
#include <stdio.h>
#include <errno.h>
#include <zephyr/kernel.h>
#ifdef CONFIG_SETTINGS
#include <zephyr/settings/settings.h>
#endif

/* The remote console (#56 §10). Weak so this TU still links for any Zephyr build
   that does not include zephyr/src/log_udp.c; the strong definition tees into a
   ring that a UDP thread drains. Called BESIDE fwrite rather than replacing it,
   so the local console is byte-for-byte unchanged.
   An explicit tee, not a stdout hook: picolibc's __stdout_hook_install has no
   getter, so hooking it would REPLACE the console path instead of chaining it —
   and nsl's `out` is the output that matters most here (it is what prints the
   bell lines), so a console that loses it locally to gain it remotely is a bad
   trade. */
/* DECLARED here, DEFINED in zephyr/src/log_udp.c (a no-op there when the image
   was built without OEVM_LOG_UDP6). The weak stub that used to live here was
   defined in THIS translation unit — so the compiler bound the call below to its
   own local definition and the strong one was never reached. The tee silently did
   nothing, and the remote log carried every C line and none of nsl's. A weak
   fallback must not live in the TU that calls it. */
void log_udp_tee(const char *bytes, size_t n);

#ifdef CONFIG_STDOUT_CONSOLE

void platform_console_write(const char *bytes, size_t n) {
    if (n) {
        fwrite(bytes, 1, n, stdout);
        log_udp_tee(bytes, n);
    }
}
void platform_console_flush(void) {
    /* Deliberately empty. main() sets stdout to _IONBF, so there is never
       anything buffered to flush. Calling fflush() here is not merely
       redundant on ESP32 — it is fatal: esp32.rom.newlib-funcs.ld assigns
       `fflush` as a STRONG symbol (not PROVIDE) to ROM newlib, overriding
       picolibc's by design. ROM fflush calls ROM __getreent, which reads an
       ESP-IDF/FreeRTOS TCB reent pointer that does not exist under Zephyr,
       and faults on a NULL load (EXCCAUSE 28). */
}

#else

/* NO CONSOLE ON THIS BOARD -> printing must be a DISCARD, and must never block.
   A node with no console still has to run: on a USB-CDC board the console is a
   cdc-acm uart, and with CONFIG_UART_LINE_CTRL=y its FIRST write BLOCKS until a host
   opens the port and asserts DTR. An unattended dongle has no such host, so a single
   `println` would wedge the VM forever — before it bound a socket or registered a
   timer — while OpenThread (already running in its own thread) kept the node attached
   to the mesh. That failure looks exactly like a healthy node from the outside, which
   is how a dead dongle passed for "working but headless" across two rungs.

   A headless node's evidence belongs on the wire, not on a console it does not have
   (see zephyr/ThreadTimerNode.nsl — the self-gating train). So when the board has no
   stdout console, discard the bytes and return immediately. */
void platform_console_write(const char *bytes, size_t n) { (void)bytes; (void)n; }
void platform_console_flush(void) { }

#endif /* CONFIG_STDOUT_CONSOLE */

int platform_debug_readline(char *buf, int cap) {
    (void)buf; (void)cap;
    return -1;   /* no interactive debug transport on this node */
}

/* rete#162 — the per-boot counter, in NVS under the settings key "tuvm/bootid".
 *
 * NO NEW SUBSYSTEM. Every board that can reach porta reaches it over Thread, and
 * every prj_*_thread.conf already sets CONFIG_NVS=y / CONFIG_SETTINGS_NVS=y
 * because OpenThread persists its own dataset there — prj_esp32c6_thread.conf
 * says it outright ("the persisted NVS copy wins after"). The C6's
 * storage_partition is 192 KB at 0x3B0000 from the vendor's default 4M table.
 * So this is one key in storage that is already mounted and already proven to
 * survive a power cycle on this exact board, not a capability being added.
 *
 * ONE FLASH WRITE PER BOOT, on the first call, latched thereafter — see
 * platform_backend.h on why the latch is correctness and not thrift. The write
 * is kept off the check-in path by kernel.nsl latching this at bridge
 * construction: a flash erase/write inside `checkIn` would land in the middle of
 * the very spans #161 exists to measure.
 *
 * WHY NOT A RANDOM NONCE. RECORDS §3 claims (bootId, monoMs) totally ORDERS
 * every observation a node makes. A nonce distinguishes boots without ordering
 * them, and would have meant weakening that line instead of finally making it
 * true.
 */
#ifdef CONFIG_SETTINGS

static uint32_t boot_id_stored;

static int boot_id_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (!settings_name_steq(key, "bootid", NULL)) return -ENOENT;
    if (len != sizeof(boot_id_stored)) return -EINVAL;
    ssize_t n = read_cb(cb_arg, &boot_id_stored, sizeof(boot_id_stored));
    return (n < 0) ? (int)n : 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(tuvm_bootid, "tuvm", NULL, boot_id_set, NULL, NULL);

long platform_backend_boot_id(void) {
    static long latched = -1;
    if (latched >= 0) return latched;

    /* settings_subsys_init() is idempotent and guarded by its own static, so
       OpenThread having already called it is expected rather than a conflict. */
    int rc = settings_subsys_init();
    if (rc == 0) {
        boot_id_stored = 0;
        (void)settings_load_subtree("tuvm");     /* absent key -> stays 0 */
        uint32_t next = boot_id_stored + 1u;
        if (next >= (1u << 30)) next = 1u;       /* stay an SMI on 32-bit */
        rc = settings_save_one("tuvm/bootid", &next, sizeof next);
        if (rc == 0) {
            latched = (long)next;
            return latched;
        }
    }
    /* A node that cannot persist this has NO per-boot identity, and must not
       answer a plausible-looking number: answering 1 here is precisely the old
       source constant, which is what let c6-bell report healthy for ten hours
       while delivering nothing. The 0 sentinel is the one value porta reads as
       "restart detection is unavailable for this node", so the failure is
       visible at both ends instead of at neither. */
    printk("VM: bootId: settings/NVS unavailable (rc %d) — no persistent boot identity\n", rc);
    latched = 0;
    return latched;
}

#else

/* No settings backend on this board. Reachable only for images that cannot talk
   to porta at all (the non-Thread confs: prj_esp32c6.conf, prj_esp32s2.conf,
   prj_esp32.conf, prj_dk.conf), since space-sync rides the mesh. Answers the
   same 0 sentinel rather than pretending to an identity it has no storage for. */
long platform_backend_boot_id(void) { return 0; }

#endif /* CONFIG_SETTINGS */
