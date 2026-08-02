/* oevm/src/backend/zephyr/platform_zephyr.c
   Zephyr console backend for the platform print:/printNewline: prims. Writes to the
   Zephyr console (uart0/VCOM on the nRF52840-DK, the SoC USB-CDC on the dongle,
   native_sim stdout otherwise). stdout is wired to the console by
   CONFIG_STDOUT_CONSOLE; main() sets _IONBF so each line appears as produced.
   platform_debug_readline is a stub: the R3 debug transport is out of scope here
   (no keeper on device yet). */
#include "platform_backend.h"
#include <stdio.h>

#ifdef CONFIG_STDOUT_CONSOLE

void platform_console_write(const char *bytes, size_t n) {
    if (n) fwrite(bytes, 1, n, stdout);
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
