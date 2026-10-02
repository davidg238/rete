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
/* Read one line from the debug transport (stdin on posix, the console cable on a
   board); newline stripped. Returns byte length >= 0, or -1 at EOF/error. BLOCKS
   until a line arrives: a VM in here is a HELD VM. */
int platform_debug_readline(char *buf, int cap);

/* rete#311 — the two facts the in-image debugger needs from the target.

   platform_debug_enabled: should the Supervisor run the debugger (print `dbg:ready`,
   take `dbg:*` lines, hold at breakpoints)? Read ONCE at boot by both image paths.
   posix: the OEVM_DEBUG environment variable, as before. Zephyr: a board has no
   environment, so the answer follows the BUILD — an image built with the per-bytecode
   hooks (OEVM_DEBUG_HOOKS=1, zephyr/CMakeLists.txt) is a debug image and waits at boot
   for its cable; the production image, hooks compiled out, answers 0 and the
   Supervisor is transparent. One switch, not two: the hooks are useless without the
   loop and the loop is useless without the hooks.

   platform_debug_held: is the VM blocked in platform_debug_readline right now? Read
   by the liveness observer every sample (zephyr/src/observer.c): a held VM stops the
   pulse feed exactly as a wedge does, and this is what tells them apart. */
int platform_debug_enabled(void);
int platform_debug_held(void);

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

/* rete#118: the node id (`platform nodeId`) is the board's factory id as lowercase hex
   (link_addr.h): its link address, or the flash serial on a board with no radio. The backend writes it into `out` and answers its
   strlen; 0 means THIS BOARD HAS NONE (or it does not fit in `cap`), and the
   primitive then fails rather than invent one. PLATFORM_NODE_ID_MAX is the buffer
   every caller offers. */
#define PLATFORM_NODE_ID_MAX 96
size_t platform_backend_node_id(char *out, size_t cap);
size_t platform_backend_board_name(char *out, size_t cap);   /* same contract, for `platform boardName` (prim 159); 0 = not told */

/* tuvm#88: the display character grid, in ONE place because the host grid in
 * platform.c and the CFB grid in zephyr/src/main.c must agree exactly — a
 * divergence makes every host-green display test prove nothing about the board.
 * 128x296 px panel with CFB's font1016 (10x16): 128/10 = 12, 296/16 = 18.
 * NOT 8x8 — CFB ships no 8x8 font; its `ppt` of 8 is a tiling granularity. */
#define OEVM_DISPLAY_ROWS 18
#define OEVM_DISPLAY_COLS 12
#endif
