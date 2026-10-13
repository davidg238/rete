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
   until a line arrives: a VM in here is a HELD VM.
   rete#376: -1 also means NOBODY IS ON THIS TRANSPORT. stdin says so by itself
   (/dev/null, a closed pipe); a board's cable cannot, so the Zephyr backend answers
   -1 until the first byte ever arrives on it, and blocks from then on. That is what
   lets a debug image boot with no cable: the boot stop reads EOF and the program
   runs, and the first line a driver sends stops it (platform_debug_pending). */
int platform_debug_readline(char *buf, int cap);

/* rete#311 — the two facts the in-image debugger needs from the target.

   platform_debug_enabled: should the Supervisor run the debugger (print `dbg:ready`,
   take `dbg:*` lines, hold at breakpoints)? Read ONCE at boot by both image paths.
   posix: the OEVM_DEBUG environment variable, as before. Zephyr: a board has no
   environment, so the answer follows the BUILD — an image built with the per-bytecode
   hooks (OEVM_DEBUG_HOOKS=1, zephyr/CMakeLists.txt) is a debug image: it takes the
   boot stop, which holds only if someone is on the cable (readline above), and it arms
   the cable's receive path in this very call so nothing sent after `dbg:ready` is
   lost (rete#376); the production image, hooks compiled out, answers 0 and the
   Supervisor is transparent. One switch, not two: the hooks are useless without the
   loop and the loop is useless without the hooks.

   platform_debug_hold / platform_debug_held: is the VM HELD BY THE DEBUGGER right now?
   Read by the liveness observer every sample (zephyr/src/observer.c): a held VM stops
   the pulse feed exactly as a wedge does, and this is what tells them apart. The
   receive paths read it too: a line arriving while held is not a pause request, the
   command loop is already reading lines in order.
   rete#377: the DEBUGGER owns this flag, not the read. It was set inside
   platform_debug_readline, so only a cable-served hold was a hold; a stop served
   through porta (the loop runs held exchanges between lines, never blocking in
   readline) read as APP_STALE and the node rebooted at appCad — measured on c6spare,
   16 s after the breakpoint, every boot. Now the Supervisor's Debugger sets it before
   its command loop and clears it on the continue (`platform debugHeld:`), whichever
   transport serves the stop; stepping stays held until the continue. The readline
   itself writes nothing here. */
int platform_debug_enabled(void);
int platform_debug_held(void);
void platform_debug_hold(int held);

/* rete#311 — dbg:pause. Set when a line ARRIVES on the debug transport while the VM
   is not held in platform_debug_readline; cleared when readline consumes a line. The
   interpreter's single-step hook reads it on every bytecode (a load, not a call: that
   is why this is a variable in the interface), and a set flag stops the running program
   at its next bytecode exactly as a step does; the paused command loop then reads the
   line. So the rule is one sentence: A LINE ARRIVING WHILE THE PROGRAM RUNS STOPS IT.
   Lines that arrive while the VM is held are not counted: the loop is already reading
   them in order (a driver may pipe a whole script at once). The backend's receive path
   writes it from interrupt or signal context (Zephyr: the UART ISR; posix: SIGIO on
   stdin). A transport with no asynchronous receive path (the polled USB-CDC console)
   leaves it at 0: there a line waits for the next hold. */
extern volatile int platform_debug_pending;

/* rete#312 Q1 — the console as a LINE SOURCE in every image, beside the debug transport
   above. The kernel's Profiler asks once at boot (prim consoleLine): arm announces each
   complete line on chan_id, from interrupt context on a board (the UART ISR posts a pulse,
   which also wakes a VM asleep in pulse_pump) and from service() on the host (SIGIO only
   raises a flag; the mailbox is not signal-safe). take_line NEVER blocks: an app that
   reads the console itself may already have consumed the line a pulse announced, and a
   kernel that then blocked would hang the node.
     arm         1: lines will be announced; 0: this console cannot (polled USB-CDC, none)
     take_line   a complete queued line's length (newline stripped), or -1: none yet
     service     thread context, every pumpEvents: 1 if a line arrived that the backend
                 could not announce itself (the host's SIGIO); platform.c then posts it */
int  platform_console_arm(int chan_id);
int  platform_console_take_line(char *buf, int cap);
int  platform_console_service(void);

/* rete#415 — the console's remote copy, the TEE: every console line also leaves as
   one UDP datagram to a target set at RUN time, from the goal/console cell (prim
   consoleTee:port:), so the custos that asked for a profile is the one that hears it.
     tee      addr6 a literal IPv6 address (the mesh has no name service) and a port:
              1 when it parsed and the tee now points there; 0 when it did not (the
              tee is unchanged). "" turns the tee off and answers 1.
     err      the profile dump's write: stderr, as before, AND the tee. The dump is C,
              so it reaches neither printk's hook nor `out` (F5 of the #415 plan);
              this is the one door it leaves by. Thread context only: on a board it
              may wait for the tee to drain, so a dump larger than the ring is slowed,
              not truncated (F6).
     drain    wait, at most max_ms, until the queued tee lines have left: deepSleep:
              calls it, because a sleep kills the sender with the dump's tail queued. */
int  platform_console_tee(const char *addr6, size_t addr_len, int port);
void platform_console_err(const char *bytes, size_t n);
void platform_console_drain(int max_ms);

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
