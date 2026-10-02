/* Unit test for the node id's one formatting rule (src/link_addr.h, rete#118 /
   the 2026-10-02 addendum to the node-identity design). The id is the board's
   factory link address as lowercase hex; every board family hands the bytes to
   this one function, so the golden lives here and not on a board.

   Build (header only, no link set):
     gcc -std=c99 -Isrc tests/test_link_addr.c -o /tmp/tla && /tmp/tla

   Verifies: lowercase, most significant byte first, no separators, a leading
   zero byte is kept, and a buffer with no room for the NUL is REFUSED rather
   than truncated into an id that collides with another board's. */
#include "../src/link_addr.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    char out[32];
    const uint8_t a[6] = {0xde,0xad,0xbe,0xef,0x00,0x0a};
    const uint8_t z[6] = {0x00,0x01,0x02,0x03,0x04,0x05};
    assert(link_addr_hex(a, 6, out, sizeof out) == 12);
    assert(strcmp(out, "deadbeef000a") == 0);            /* lowercase, MSB first, no colons */
    assert(link_addr_hex(z, 6, out, sizeof out) == 12);
    assert(strcmp(out, "000102030405") == 0);            /* a leading zero byte is KEPT */
    assert(link_addr_hex(a, 6, out, 12) == 0 && out[0] == '\0');   /* no room for NUL: refuse, never truncate */
    assert(link_addr_hex(a, 0, out, sizeof out) == 0);
    printf("test_link_addr OK\n");
    return 0;
}
