/* oevm/src/backend/zephyr/platform_zephyr.c
   Zephyr console backend for the platform print:/printNewline: prims. Writes to the
   Zephyr console (uart0/VCOM on the nRF52840-DK, the SoC USB-CDC on the dongle,
   native_sim stdout otherwise). stdout is wired to the console by
   CONFIG_STDOUT_CONSOLE; main() sets _IONBF so each line appears as produced.
   platform_debug_readline is a stub: the R3 debug transport is out of scope here
   (no keeper on device yet). */
#include "platform_backend.h"
#include <stdio.h>

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
