#include "platform_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
int platform_debug_readline(char *buf, int cap) {
    if (fgets(buf, cap, stdin) == NULL) return -1;
    int n = (int)strlen(buf);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
    return n;
}

/* rete#162 — the per-boot counter on host, in a file named by TUVM_BOOTID_FILE.
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
    const char *path = getenv("TUVM_BOOTID_FILE");
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
