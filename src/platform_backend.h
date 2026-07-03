#ifndef OEVM_PLATFORM_BACKEND_H
#define OEVM_PLATFORM_BACKEND_H
#include <stddef.h>
/* Per-target console sink for the platform `print:`/`printNewline:` prims.
   Exactly one impl is linked per build from src/backend/$OEVM_TARGET/:
   backend/posix/platform_posix.c (stdio) now; backend/zephyr/platform_zephyr.c
   (printk/console) / backend/esp32/platform_esp.c (uart0) later. Mirrors the
   serial_backend.h seam so the platform capability is no longer libc-coupled. */
void platform_console_write(const char *bytes, size_t n);  /* write n bytes to the console */
void platform_console_flush(void);                         /* flush buffered output (called after a newline) */
/* Read one line from stdin (debug transport input); newline stripped.
   Returns byte length >= 0, or -1 at EOF/error. */
int platform_debug_readline(char *buf, int cap);
#endif
