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

/* tuvm#88: the display character grid, in ONE place because the host grid in
 * platform.c and the CFB grid in zephyr/src/main.c must agree exactly — a
 * divergence makes every host-green display test prove nothing about the board.
 * 128x296 px panel with CFB's font1016 (10x16): 128/10 = 12, 296/16 = 18.
 * NOT 8x8 — CFB ships no 8x8 font; its `ppt` of 8 is a tiling granularity. */
#define OEVM_DISPLAY_ROWS 18
#define OEVM_DISPLAY_COLS 12
#endif
