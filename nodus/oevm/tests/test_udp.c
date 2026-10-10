/* Standalone smoke test for the UDP backend (real dual-stack loopback). Not part
   of the conformance runner; build + run manually from the oevm repo root:
     gcc -std=c99 -Isrc tests/test_udp.c src/backend/posix/udp_posix.c -o /tmp/test_udp && /tmp/test_udp
   Links only the backend (the seam funcs) — not udp.c, which needs the full VM. */
#include "../src/udp_backend.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static void roundtrip(const char *loop) {
    int rx = udp_socket_bind(0);          /* ephemeral receiver */
    assert(rx >= 0);
    /* discover the bound port via a self-send loop is overkill; bind a known port.
       NOT 54330: that is log_udp.c's UDP-console port, and a bench listener holding it
       (board-console-over-udp) turned this gate red for a reason that was not udp's
       (rete#380, 2026-10-09). */
    int rxp = udp_socket_bind(54337);
    assert(rxp >= 0);
    int tx = udp_socket_bind(0);
    assert(tx >= 0);

    /* empty receiver -> -1 (non-blocking) */
    uint8_t buf[2048]; char host[64]; int port = 0;
    assert(udp_receive(rxp, buf, sizeof buf, host, sizeof host, &port) == -1);

    /* send 'hi' to rxp over the given loopback literal, read it back */
    const uint8_t msg[2] = { 'h', 'i' };
    assert(udp_send(tx, loop, 54337, msg, 2) == 2);
    int n = -1;
    for (int i = 0; i < 100 && n < 0; i++)   /* loopback is ~instant; poll a few times */
        n = udp_receive(rxp, buf, sizeof buf, host, sizeof host, &port);
    assert(n == 2);
    assert(buf[0] == 'h' && buf[1] == 'i');
    /* v4 sender must be plain dotted-quad, not ::ffff:127.0.0.1 */
    if (strcmp(loop, "127.0.0.1") == 0) assert(strcmp(host, "127.0.0.1") == 0);

    udp_close(rx); udp_close(rxp); udp_close(tx);
}

int main(void) {
    udp_open();
    roundtrip("127.0.0.1");   /* IPv4 (v4-mapped onto the dual-stack socket) */
    roundtrip("::1");         /* IPv6 */
    printf("test_udp OK\n");
    return 0;
}
