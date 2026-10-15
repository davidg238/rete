/* sigaction, F_SETOWN and O_ASYNC under -std=c99 (the host gates build with it). */
#define _GNU_SOURCE
#include "platform_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
/* POSIX (x86-linux / macOS desktop) console backend: stdout via ISO C stdio.
   The flush on newline closes the long-deferred std-lib step-1 minor (platform
   print:/printNewline: were unflushed). platform_zephyr.c (printk) /
   platform_esp.c (uart0) replace it on device. */
/* rete#415: the tee (platform_backend.h), the host twin of the board's log_udp.c, so
   the host gate exercises the same cell -> prim -> datagram path a board does. One
   datagram per line, sent at once and never waited on (MSG_DONTWAIT): loopback does not
   drop, and a host has no ring to overflow. stdout and stderr assemble their lines
   apart, so a dump row never splices into half a line of `out`. */
#define TEE_LINE_MAX 256
struct tee_line { char b[TEE_LINE_MAX]; int n; };
static struct tee_line tee_out, tee_err;
static int tee_sock = -1;
static struct sockaddr_in6 tee_dst;

static void tee_feed(struct tee_line *l, const char *bytes, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        char c = bytes[i];
        if (c != '\n' && c != '\r') l->b[l->n++] = c;
        if (c == '\n' || l->n == TEE_LINE_MAX) {
            if (l->n > 0 && tee_sock >= 0)
                (void)sendto(tee_sock, l->b, (size_t)l->n, MSG_DONTWAIT,
                             (struct sockaddr *)&tee_dst, sizeof tee_dst);
            l->n = 0;
        }
    }
}

/* rete#415: the target survives deep sleep, as the board keeps it in RTC memory
   (log_udp.c), so a waking node tees from its first line, before any check-in. The
   host's sleep is a re-exec, so the target is kept beside OEVM_RTC_FILE, in
   `<file>.tee` ("<addr> <port>", or "off"); with no file it lasts one process. */
static int tee_open(const char *a, int port) {
    struct sockaddr_in6 dst;
    memset(&dst, 0, sizeof dst);
    dst.sin6_family = AF_INET6;
    dst.sin6_port = htons((unsigned short)port);
    if (inet_pton(AF_INET6, a, &dst.sin6_addr) != 1) return 0;
    if (tee_sock < 0) {
        tee_sock = socket(AF_INET6, SOCK_DGRAM, 0);
        if (tee_sock < 0) return 0;
    }
    tee_dst = dst;
    return 1;
}
static FILE *tee_keep(const char *mode) {
    char path[512];
    const char *p = getenv("OEVM_RTC_FILE");
    if (!p || snprintf(path, sizeof path, "%s.tee", p) >= (int)sizeof path) return NULL;
    return fopen(path, mode);
}
static void tee_boot_once(void) {
    static int done;
    char a[INET6_ADDRSTRLEN];
    int port;
    FILE *f;
    if (done) return;
    done = 1;
    if (!(f = tee_keep("r"))) return;
    if (fscanf(f, "%45s %d", a, &port) == 2) (void)tee_open(a, port);
    fclose(f);
}

int platform_console_tee(const char *addr6, size_t addr_len, int port) {
    char a[INET6_ADDRSTRLEN];
    FILE *f;
    tee_boot_once();
    if (addr_len == 0) {
        if (tee_sock >= 0) close(tee_sock);
        tee_sock = -1;
        if ((f = tee_keep("w"))) { fputs("off\n", f); fclose(f); }
        return 1;
    }
    if (addr_len >= sizeof a || port < 1 || port > 65535) return 0;
    memcpy(a, addr6, addr_len);
    a[addr_len] = '\0';
    if (!tee_open(a, port)) return 0;
    if ((f = tee_keep("w"))) { fprintf(f, "%s %d\n", a, port); fclose(f); }
    return 1;
}

void platform_console_write(const char *bytes, size_t n) {
    if (n) {
        fwrite(bytes, 1, n, stdout);
        tee_boot_once();
        tee_feed(&tee_out, bytes, n);
    }
}
void platform_console_err(const char *bytes, size_t n) {
    if (n) {
        fwrite(bytes, 1, n, stderr);
        tee_boot_once();
        tee_feed(&tee_err, bytes, n);
    }
}
void platform_console_drain(int max_ms) { (void)max_ms; }   /* sent at once: nothing queues */
void platform_console_flush(void) {
    fflush(stdout);
}
static volatile int debug_held;
volatile int platform_debug_pending;

/* dbg:pause on the host (platform_backend.h): SIGIO on stdin is the posix stand-in for
   the board's UART interrupt — raised when input lands while the VM is not in
   readline, lowered as readline consumes a line. The line is read byte by byte with
   read(2) rather than fgets so stdio never holds a line the VM has not seen. Armed on
   the first read, as the board arms its ISR; SA_RESTART so the VM's blocking reads are
   not cut short. A regular file never raises SIGIO (a pipe or tty does), which is fine:
   a file cannot want to stop anything. The descriptor is stdin's, not 0: a C test swaps
   the stdin FILE for a temp file and expects readLine to follow it. */
static volatile sig_atomic_t console_sig;   /* rete#312: input landed; service() posts */
static void on_sigio(int sig) { (void)sig; if (!debug_held) { platform_debug_pending = 1; console_sig = 1; } }
static int sigio_armed;
static void arm_sigio(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigio;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGIO, &sa, NULL);
    fcntl(fileno(stdin), F_SETOWN, getpid());
    fcntl(fileno(stdin), F_SETFL, fcntl(fileno(stdin), F_GETFL) | O_ASYNC);
}
int platform_debug_readline(char *buf, int cap) {
    int n = 0;
    if (!sigio_armed) { arm_sigio(); sigio_armed = 1; }
    for (;;) {
        char c;
        ssize_t r = read(fileno(stdin), &c, 1);   /* fileno, not 0: test_debugger.c swaps stdin */
        if (r <= 0) { platform_debug_pending = 0; return -1; }
        if (c == '\n') break;
        if (c == '\r') continue;
        if (n < cap - 1) buf[n++] = c;
    }
    platform_debug_pending = 0;
    buf[n] = '\0';
    return n;
}

/* rete#312 Q1: the console as a line source (platform_backend.h). SIGIO only raises a
   flag; service() reports it in thread context and platform.c posts. take_line reads only what poll() says is
   there, keeping a partial line across calls, so it never blocks. A regular file never
   raises SIGIO, so a VM whose stdin is /dev/null or a file never spins on its EOF. */
static int console_armed;
static char console_part[512];
static int console_plen;
int platform_console_arm(int chan_id) {
    (void)chan_id;                    /* service() reports; platform.c posts */
    if (!sigio_armed) { arm_sigio(); sigio_armed = 1; }
    console_armed = 1;
    return 1;
}
int platform_console_service(void) {
    if (!console_armed || !console_sig) return 0;
    console_sig = 0;
    return 1;
}
int platform_console_take_line(char *buf, int cap) {
    struct pollfd pfd = { .fd = fileno(stdin), .events = POLLIN };
    while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
        char c;
        if (read(fileno(stdin), &c, 1) <= 0) return -1;
        if (c == '\r') continue;
        if (c == '\n') {
            int n = console_plen < cap - 1 ? console_plen : cap - 1;
            memcpy(buf, console_part, (size_t)n);
            buf[n] = '\0';
            console_plen = 0;
            return n;
        }
        if (console_plen < (int)sizeof console_part) console_part[console_plen++] = c;
    }
    return -1;
}
int platform_debug_enabled(void) { return getenv("OEVM_DEBUG") != NULL; }
int platform_debug_held(void)    { return debug_held; }
void platform_debug_hold(int held) { debug_held = held ? 1 : 0; }   /* rete#377: the debugger's, not readline's */

/* rete#118: the host's stand-in for silicon. A host VM has no factory link address, and
   host_ota runs several VMs per machine, so an interface MAC or a fixed value would give
   them all one id and one porta space; the caller names each VM in OEVM_NODE_ID. Unset or
   empty means "no id" and the primitive fails. An id that does not fit is refused, never
   truncated into a colliding one. */
size_t platform_backend_board_name(char *out, size_t cap) {
    const char *e = getenv("OEVM_BOARD_NAME");
    size_t n;
    if (e == NULL || e[0] == '\0') return 0;
    n = strlen(e);
    if (n + 1 > cap) return 0;
    memcpy(out, e, n + 1);
    return n;
}

size_t platform_backend_node_id(char *out, size_t cap) {
    const char *e = getenv("OEVM_NODE_ID");
    size_t n;
    if (e == NULL || e[0] == '\0') return 0;
    n = strlen(e);
    if (n + 1 > cap) return 0;
    memcpy(out, e, n + 1);
    return n;
}

/* rete#162 — the per-boot counter on host, in a file named by OEVM_BOOTID_FILE.
   The env var is the whole switch, and it defaults to OFF for a reason: a host
   process "reboots" on every run, so a counter that persisted by default would
   make every test in this tree order-dependent and every golden a moving target.
   Off, this answers the 0 sentinel and behaves exactly as the source constant
   did. On, the increment is real, which is what lets the reboot semantics be
   exercised on host through run_tests_oevm.py's per-test `env:` rather than only
   on a board — and a defect that can only be reproduced by power-cycling a C6 is
   a defect that gets reproduced once. */
long platform_backend_boot_id(void) {
    static long latched = -1;
    if (latched >= 0) return latched;
    const char *path = getenv("OEVM_BOOTID_FILE");
    if (path == NULL || path[0] == '\0') {
        latched = 0;                    /* no persistent identity — see the header */
        return latched;
    }
    long v = 0;
    FILE *f = fopen(path, "r");
    if (f != NULL) {
        if (fscanf(f, "%ld", &v) != 1 || v < 0) v = 0;
        fclose(f);
    }
    if (++v >= (1L << 30)) v = 1;       /* stay an SMI on a 32-bit target */
    f = fopen(path, "w");
    if (f == NULL) {
        /* Readable but not writable: the counter would repeat forever, which is
           the failure this whole change exists to make impossible. Say so and
           answer the sentinel rather than a plausible-looking number. */
        fprintf(stderr, "VM: bootId: cannot write %s — no persistent boot identity\n", path);
        latched = 0;
        return latched;
    }
    fprintf(f, "%ld\n", v);
    fclose(f);
    latched = v;
    return latched;
}
