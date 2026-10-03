/* sigaction, F_SETOWN and O_ASYNC under -std=c99 (the host gates build with it). */
#define _GNU_SOURCE
#include "platform_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
/* POSIX (x86-linux / macOS desktop) console backend: stdout via ISO C stdio.
   The flush on newline closes the long-deferred std-lib step-1 minor (platform
   print:/printNewline: were unflushed). platform_zephyr.c (printk) /
   platform_esp.c (uart0) replace it on device. */
void platform_console_write(const char *bytes, size_t n) {
    if (n) fwrite(bytes, 1, n, stdout);
}
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
static void on_sigio(int sig) { (void)sig; if (!debug_held) platform_debug_pending = 1; }
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
    static int armed;
    int n = 0;
    if (!armed) { arm_sigio(); armed = 1; }
    debug_held = 1;
    for (;;) {
        char c;
        ssize_t r = read(fileno(stdin), &c, 1);   /* fileno, not 0: test_debugger.c swaps stdin */
        if (r <= 0) { debug_held = 0; platform_debug_pending = 0; return -1; }
        if (c == '\n') break;
        if (c == '\r') continue;
        if (n < cap - 1) buf[n++] = c;
    }
    debug_held = 0;
    platform_debug_pending = 0;
    buf[n] = '\0';
    return n;
}
int platform_debug_enabled(void) { return getenv("OEVM_DEBUG") != NULL; }
int platform_debug_held(void)    { return debug_held; }

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
