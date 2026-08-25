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

/* rete#162/#117 — the node's per-boot identity, a counter incremented once per
   boot and persisted (PROTOCOL-NSL-RECORDS.md §2.4). Backed by NVS on Zephyr
   (the settings key "oevm/bootid") and by an env-named file on posix.

   THE LATCH IS PART OF THE CONTRACT, not an optimisation: every call within one
   boot must answer the same number, because porta uses a CHANGE in this value as
   the sole evidence that the node's outbound sequence restarted. Two different
   answers in one boot would rewind porta's high-water mid-run and re-deliver
   rows it had already committed. Each backend increments-and-stores exactly
   once, on the first call, and answers a cached value thereafter — so this also
   costs exactly one flash write per boot.

   Answers a value below 2^30 so the caller can TAG_SMI it without allocating on
   a 32-bit target. */
long platform_backend_boot_id(void);

/* tuvm#88: the display character grid, in ONE place because the host grid in
 * platform.c and the CFB grid in zephyr/src/main.c must agree exactly — a
 * divergence makes every host-green display test prove nothing about the board.
 * 128x296 px panel with CFB's font1016 (10x16): 128/10 = 12, 296/16 = 18.
 * NOT 8x8 — CFB ships no 8x8 font; its `ppt` of 8 is a tiling granularity. */
#define OEVM_DISPLAY_ROWS 18
#define OEVM_DISPLAY_COLS 12
#endif
