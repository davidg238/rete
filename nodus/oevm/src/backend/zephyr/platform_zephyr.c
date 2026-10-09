/* oevm/src/backend/zephyr/platform_zephyr.c
   Zephyr console backend for the platform print:/printNewline: prims. Writes to the
   Zephyr console (uart0/VCOM on the nRF52840-DK, the SoC USB-CDC on the dongle,
   native_sim stdout otherwise). stdout is wired to the console by
   CONFIG_STDOUT_CONSOLE; main() sets _IONBF so each line appears as produced.
   platform_debug_readline reads the console cable (rete#311, below): the debug
   transport IS the console, one cable, plain text lines. */
#include "platform_backend.h"
#include "pulse_backend.h"
#include <stdio.h>
#include <errno.h>
#include <string.h>
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

/* rete#311 — one line in, over the same cable the console goes out on. Plain text,
   no framing: the spike asked whether SLIP is needed for one cable, and this
   degenerate case answered no on all three board families (docs/results/
   2026-10-02-311-line-in-spike.md). Blocks until a newline, as the posix fgets does —
   a blocked VM is a HELD VM, and `debug_held` is how the liveness observer is told
   not to count the hold as a wedge (observer_core.h §3.2).

   THE ENABLE FOLLOWS THE BUILD. A board has no environment to carry OEVM_DEBUG, so a
   hooks-enabled image (OEVM_DEBUG_HOOKS=1) is the debug image: the Supervisor prints
   `dbg:ready` at boot and takes the boot stop. The production image, hooks compiled
   out, never enters the loop. One switch (platform_backend.h).

   A CABLE NOBODY HAS SPOKEN ON IS NOBODY'S (rete#376, D19). A debug image is debugged
   over USB or over porta, so it must boot with no cable attached -- a MagTag in a
   drawer, a bell on the wall. stdin can say nobody is there (EOF); a UART cannot, and
   the 2026-10-02 spike's boot wait (no timeout, "a debug image is on a cable by
   definition") held such a node at `dbg:ready` forever. So readline answers EOF until
   the FIRST BYTE EVER arrives on the cable: the boot stop resumes at once on a silent
   cable and the image runs; a driver that then speaks stops it (pending, below), and
   from that byte on every read blocks as the posix one does. No timeout, still: the
   decision is "has anyone spoken", never "how long". The receive path is armed when
   platform_debug_enabled() is asked at boot, before any `dbg:ready`, so a line sent
   the moment that prompt is seen cannot be lost.

   TWO receive paths, because the bench said so on 2026-10-02 (docs/spikes/):
     USB-CDC console (dongle, RP2350)  POLLED, zero RAM. The class driver never starts
       its first OUT transfer for a pure poller — the host's write just times out —
       so reception is armed once with irq_rx_enable and then read with poll_in.
     hardware UART (C6)                INTERRUPT-DRIVEN into a 256-byte queue. Polled,
       the C6 HANGS outright once ~CONFIG_UART_ESP32_RX_FIFO_THRESH bytes queue in the
       FIFO (any unpaced line over ~24 chars) and the hardware watchdog resets it 22 s
       later. CONFIG_UART_INTERRUPT_DRIVEN is in prj_esp32c6.conf for every C6 image. */
#if defined(CONFIG_UART_CONSOLE) && DT_HAS_CHOSEN(zephyr_console)
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
static volatile int debug_held;   /* written by the VM thread, read by the observer and the ISR */
volatile int platform_debug_pending;
#define RL_IS_CDC DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_console), zephyr_cdc_acm_uart)

#if !RL_IS_CDC && defined(CONFIG_UART_INTERRUPT_DRIVEN)
K_MSGQ_DEFINE(rl_q, 1, 256, 1);
/* dbg:pause (platform_backend.h): a newline queued while the VM is NOT in readline
   raises the flag a running VM reads on every bytecode; rl_getc lowers it as the
   paused loop consumes the line. */
/* rete#312 Q1: complete lines queued, and the channel that announces each one. */
static atomic_t rl_lines;
static volatile int rl_spoken;   /* rete#376: any byte ever received on the cable */
static int console_chan = -1;
static int console_seq;
static void rl_isr(const struct device *dev, void *user) {
    uint8_t c;
    (void)user;
    while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
        if (uart_fifo_read(dev, &c, 1) != 1) break;
        rl_spoken = 1;
        if (k_msgq_put(&rl_q, &c, K_NO_WAIT) != 0 || c != '\n') continue;   /* full queue: dropped */
        atomic_inc(&rl_lines);
        if (!debug_held) {
            platform_debug_pending = 1;
            if (console_chan >= 0)
                pulse_post_isr_value(console_chan, TAG_SMI(++console_seq & 0xffff));
        }
    }
}
static void rl_arm(const struct device *dev) {
    static bool armed;
    if (!armed) {
        uart_irq_callback_user_data_set(dev, rl_isr, NULL);
        uart_irq_rx_enable(dev);
        armed = true;
    }
}
static void rl_line_taken(void) {
    platform_debug_pending = 0;
    if (atomic_get(&rl_lines) > 0) atomic_dec(&rl_lines);
}
static int rl_getc(const struct device *dev) {
    uint8_t c;
    rl_arm(dev);
    k_msgq_get(&rl_q, &c, K_FOREVER);
    if (c == '\n') rl_line_taken();
    return c;
}
/* rete#376: nobody has ever spoken on this cable. Armed first, so a byte that
   landed in the FIFO before the question is counted, not lost. */
static int rl_nobody(const struct device *dev) {
    rl_arm(dev);
    return !rl_spoken;
}
int platform_console_arm(int chan_id) {
    const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
    if (!device_is_ready(dev)) return 0;
    console_chan = chan_id;
    rl_arm(dev);
    return 1;
}
int platform_console_take_line(char *buf, int cap) {
    int n = 0;
    uint8_t c;
    if (cap < 1 || atomic_get(&rl_lines) == 0) return -1;
    while (k_msgq_get(&rl_q, &c, K_NO_WAIT) == 0) {
        if (c == '\n') { rl_line_taken(); buf[n] = '\0'; return n; }
        if (c != '\r' && n < cap - 1) buf[n++] = (char)c;
    }
    buf[n] = '\0';      /* counted a line the queue no longer holds: answer what came */
    return n;
}
#else
/* Polled: nothing runs between reads, so nothing can count a line in. A line sent
   while the program runs waits in the FIFO for the next hold. */
static int rl_spoken;
static unsigned char rl_held_byte;   /* a byte the nobody-probe took; rl_getc hands it back */
static int rl_have_byte;
static void rl_arm(const struct device *dev) {
#if RL_IS_CDC && defined(CONFIG_UART_INTERRUPT_DRIVEN)
    static bool armed;
    if (!armed) { uart_irq_rx_enable(dev); armed = true; }
#else
    (void)dev;
#endif
}
static int rl_getc(const struct device *dev) {
    unsigned char c;
    rl_arm(dev);
    if (rl_have_byte) { rl_have_byte = 0; return rl_held_byte; }
    while (uart_poll_in(dev, &c) != 0) k_msleep(2);
    rl_spoken = 1;
    return c;
}
/* rete#376 on the polled path: nothing runs between reads, so "ever spoken" can
   only be learned by polling now. A byte found is kept for rl_getc. Once spoken,
   every later read blocks. KNOWN HOLE: a polled console raises no pending flag
   (rete#311), so a driver that attaches AFTER the boot stop resumed can never stop
   the program from this cable; it is debugged through porta, or the console goes
   interrupt-driven like the C6's. */
static int rl_nobody(const struct device *dev) {
    unsigned char c;
    rl_arm(dev);
    if (rl_spoken || rl_have_byte) return 0;
    if (uart_poll_in(dev, &c) != 0) return 1;
    rl_held_byte = c; rl_have_byte = 1; rl_spoken = 1;
    return 0;
}
#endif


int platform_debug_readline(char *buf, int cap) {
    const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
    int n = 0;
    if (cap < 1 || !device_is_ready(dev)) return -1;
    if (rl_nobody(dev)) return -1;   /* rete#376: a silent cable is nobody's -> EOF */
    debug_held = 1;
    for (;;) {
        int c = rl_getc(dev);
        if (c == '\n') break;
        if (c == '\r') continue;
        if (n < cap - 1) buf[n++] = (char)c;
    }
    debug_held = 0;
    buf[n] = '\0';
    return n;
}
int platform_debug_held(void) { return debug_held; }
static void rl_arm_boot(void) {
    const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
    if (device_is_ready(dev)) rl_arm(dev);
}
#if RL_IS_CDC || !defined(CONFIG_UART_INTERRUPT_DRIVEN)
/* Polled: nothing runs between reads, so nothing can announce a line (rete#312). */
int platform_console_arm(int chan_id) { (void)chan_id; return 0; }
int platform_console_take_line(char *buf, int cap) { (void)buf; (void)cap; return -1; }
#endif
#else
static void rl_arm_boot(void) { }
int platform_debug_readline(char *buf, int cap) {
    (void)buf; (void)cap;
    return -1;   /* no console device to read a line from */
}
int platform_debug_held(void) { return 0; }
int platform_console_arm(int chan_id) { (void)chan_id; return 0; }
int platform_console_take_line(char *buf, int cap) { (void)buf; (void)cap; return -1; }
volatile int platform_debug_pending;
#endif
/* rete#312: the ISR posts its own announcement; nothing to report at the safe point. */
int platform_console_service(void) { return 0; }


int platform_debug_enabled(void) {
#ifdef OEVM_NO_DEBUG_HOOKS
    return 0;   /* production image: the hooks are compiled out, so is the loop */
#else
    rl_arm_boot();   /* rete#376: a debug image listens from boot; see readline */
    return 1;        /* debug image (OEVM_DEBUG_HOOKS=1): the boot stop, then run */
#endif
}

/* rete#162 — the per-boot counter, in NVS under the settings key "oevm/bootid".
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

SETTINGS_STATIC_HANDLER_DEFINE(oevm_bootid, "oevm", NULL, boot_id_set, NULL, NULL);

long platform_backend_boot_id(void) {
    static long latched = -1;
    if (latched >= 0) return latched;

    /* settings_subsys_init() is idempotent and guarded by its own static, so
       OpenThread having already called it is expected rather than a conflict. */
    int rc = settings_subsys_init();
    if (rc == 0) {
        boot_id_stored = 0;
        (void)settings_load_subtree("oevm");     /* absent key -> stays 0 */
        uint32_t next = boot_id_stored + 1u;
        if (next >= (1u << 30)) next = 1u;       /* stay an SMI on 32-bit */
        rc = settings_save_one("oevm/bootid", &next, sizeof next);
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

/* rete#118: platform nodeId = the board's factory id, from silicon, per family: the factory
   link address where the board has a radio, the flash serial where it has none. Which one
   is a per-family statement, NEVER the #else: a new board port must say where its id is,
   so it cannot inherit another family's answer by omission. */
#include "link_addr.h"
#define LINK_ADDR_MAX 8
#if defined(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32) || defined(CONFIG_SOC_FAMILY_RPI_PICO)
#  ifndef CONFIG_HWINFO
#    error "node id: ESP32 and RP ports read their factory id through hwinfo — set CONFIG_HWINFO=y in the prj_*.conf"
#  endif
#  include <zephyr/drivers/hwinfo.h>
/* hwinfo_esp32.c: the 6-byte eFuse base MAC, most significant byte first.
   hwinfo_rpi_pico.c: the 8-byte flash serial (RP2040 / RP2350 have no radio, so no link
   address) — the same bytes the USB stack prints as the board's serial string, which is
   the nearest thing an RP board has to a sticker. ssize_t length or <0. */
static size_t link_addr_read(uint8_t b[LINK_ADDR_MAX]) {
    ssize_t n = hwinfo_get_device_id(b, LINK_ADDR_MAX);
    return n > 0 ? (size_t)n : 0;
}
#elif defined(CONFIG_SOC_SERIES_NRF52X)
#  include <soc.h>             /* NRF_FICR, as drivers/hwinfo/hwinfo_nrf.c does */
#  include <hal/nrf_ficr.h>    /* nrf_ficr_deviceaddr_get — soc.h alone leaves it implicit, and it fails at LINK */
/* FICR DEVICEADDR, NOT hwinfo: hwinfo answers DEVICEID on nRF52, a different register.
   The top two bits are FORCED: the id is the sticker value, and the sticker (like the
   Open Bootloader's USB serial and the BLE controller) prints the address as a random
   static one, 0b11 on top. Raw DEVICEADDR need not have them set — nrf4ec0's happens to
   (D2…), which is why one board could not tell the two formulas apart (David, 2026-10-02). */
static size_t link_addr_read(uint8_t b[LINK_ADDR_MAX]) {
    uint32_t lo = nrf_ficr_deviceaddr_get(NRF_FICR, 0);
    uint32_t hi = nrf_ficr_deviceaddr_get(NRF_FICR, 1) & 0xFFFFu;
    b[0] = (uint8_t)(hi >> 8) | 0xC0u;  b[1] = (uint8_t)hi;
    b[2] = (uint8_t)(lo >> 24); b[3] = (uint8_t)(lo >> 16); b[4] = (uint8_t)(lo >> 8); b[5] = (uint8_t)lo;
    return 6;
}
#else
#  error "node id: a new board port must say where its factory id is read from"
#endif

size_t platform_backend_node_id(char *out, size_t cap) {
    uint8_t b[LINK_ADDR_MAX];
    size_t n = link_addr_read(b);
    return n == 0 ? 0 : link_addr_hex(b, n, out, cap);
}

/* rete#118: the build-time board name — a C string generated by
   cmake/gen_board_name.cmake from run.sh's OEVM_BOARD_NAME; empty = this build was not told. */
extern const char oevm_board_name[];
size_t platform_backend_board_name(char *out, size_t cap) {
    size_t n = strlen(oevm_board_name);
    if (n == 0 || n + 1 > cap) return 0;
    memcpy(out, oevm_board_name, n + 1);
    return n;
}
