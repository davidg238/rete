#include "platform_backend.h"
#include <stdio.h>
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
