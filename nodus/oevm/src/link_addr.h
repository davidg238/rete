#ifndef OEVM_LINK_ADDR_H
#define OEVM_LINK_ADDR_H
/* rete#118: the node id is the board's factory link address as lowercase hex, most
   significant byte first, no separators. Every board family reads its own bytes and
   formats them HERE, so the rule exists once. A buffer with no room for the NUL is
   refused (returns 0), never truncated: a truncated id is a different board's id. */
#include <stddef.h>
#include <stdint.h>
static inline size_t link_addr_hex(const uint8_t *b, size_t n, char *out, size_t cap) {
    static const char d[] = "0123456789abcdef";
    size_t i;
    if (n == 0 || cap < 2 * n + 1) { if (cap > 0) out[0] = '\0'; return 0; }
    for (i = 0; i < n; i++) { out[2*i] = d[b[i] >> 4]; out[2*i + 1] = d[b[i] & 15]; }
    out[2 * n] = '\0';
    return 2 * n;
}
#endif
