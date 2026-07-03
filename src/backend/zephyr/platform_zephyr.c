/* oevm/src/backend/zephyr/platform_zephyr.c
   Zephyr console backend for the platform print:/printNewline: prims. Writes to
   the Zephyr console (printk-backed; the USB-CDC ACM console on the dongle, the
   native_sim stdout otherwise). printf is fine — newlib stdout is wired to the
   console by CONFIG_STDOUT_CONSOLE; ns-zephyr's main sets _IONBF so each line
   appears as produced. platform_debug_readline is a stub: the R3 debug transport
   is out of scope for R6 (no keeper on device yet). */
#include "platform_backend.h"
#include <stdio.h>

void platform_console_write(const char *bytes, size_t n) {
    if (n) fwrite(bytes, 1, n, stdout);
}
void platform_console_flush(void) {
    fflush(stdout);
}
int platform_debug_readline(char *buf, int cap) {
    (void)buf; (void)cap;
    return -1;   /* no interactive debug transport on the R6 node */
}
