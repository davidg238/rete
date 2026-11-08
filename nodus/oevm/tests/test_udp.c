/* Unit test for the UDP backend (real dual-stack loopback), registered in
   nodus/nsl/tools/run_tests_oevm.py C_UNIT_TESTS. Links only the backend (the seam
   funcs) — not udp.c, which needs the full VM — so the pulse mailbox the doorbells
   ring is stubbed below and records every post.

   rete#440 invariant 4: every reaction that owns a socket reads it to empty, and the
   socket's doorbell stays parked until it has. The host pump cannot otherwise see a
   lost restore (it never blocks), so these tests drive the posix poll by hand and
   assert the bell: one per arrival burst, none while parked, again after the read
   to empty. */
#define _POSIX_C_SOURCE 200809L
#include "../src/udp_backend.h"
#include "../src/backend/posix/udp_posix.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

static int posts, last_chan, last_token;
void pulse_posix_ring(int chan_id, int fd) {
    posts++; last_chan = chan_id; last_token = fd;
}

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

/* ---- rete#440 doorbells ------------------------------------------------------------ */

#define BELL_PORT 54338
#define BELL_CHAN 7

/* Poll until the bell rings or ~1 s passes; answers how many posts the polls made. */
static int poll_for_bell(void) {
    int before = posts;
    struct timespec ms = { 0, 1000000 };
    for (int i = 0; i < 1000 && posts == before; i++) { udp_posix_poll(); nanosleep(&ms, NULL); }
    return posts - before;
}

/* Poll a few times with no wait for an arrival: answers how many posts they made. */
static int poll_quiet(void) {
    int before = posts;
    struct timespec ms = { 0, 1000000 };
    for (int i = 0; i < 20; i++) { udp_posix_poll(); nanosleep(&ms, NULL); }
    return posts - before;
}

/* The reaction, as R2's kernel will run it: read until empty. Answers datagrams read. */
static int read_to_empty(int sock, uint8_t *first) {
    uint8_t buf[2048]; char host[64]; int port = 0, got = 0, n;
    while ((n = udp_receive(sock, buf, sizeof buf, host, sizeof host, &port)) >= 0) {
        if (got < 4 && first) first[got] = buf[0];
        got++;
    }
    return got;
}

static void send1(int tx, uint8_t b) {
    assert(udp_send(tx, "::1", BELL_PORT, &b, 1) == 1);
}

/* Two datagrams under one doorbell are both read, and the socket rings again after. */
static void two_under_one_bell(void) {
    int rx = udp_socket_bind(BELL_PORT), tx = udp_socket_bind(0);
    assert(rx >= 0 && tx >= 0);
    udp_doorbell(rx, BELL_CHAN);
    assert(poll_quiet() == 0 && "nothing arrived: no bell");

    send1(tx, 'a'); send1(tx, 'b');
    assert(poll_for_bell() == 1 && "a burst rings once");
    assert(last_chan == BELL_CHAN && last_token == rx && "the constant token");
    assert(poll_quiet() == 0 && "parked: an unread socket does not ring again");

    uint8_t got[4] = {0};
    assert(read_to_empty(rx, got) == 2 && got[0] == 'a' && got[1] == 'b' && "both read");

    send1(tx, 'c');
    assert(poll_for_bell() == 1 && "read to empty restored the bell");
    assert(read_to_empty(rx, got) == 1 && got[0] == 'c');
    udp_close(rx); udp_close(tx);
}

/* A miss (the budget expires with nothing read: the reaction still reads, finds the
   socket empty), then a late reply: the reply must ring. A lost restore leaves the
   socket parked, and the reply is never heard. */
static void miss_then_reply(void) {
    int rx = udp_socket_bind(BELL_PORT), tx = udp_socket_bind(0);
    assert(rx >= 0 && tx >= 0);
    udp_doorbell(rx, BELL_CHAN);

    send1(tx, 'x');                      /* an earlier exchange's reply rings and parks */
    assert(poll_for_bell() == 1);
    assert(read_to_empty(rx, NULL) == 1);

    assert(read_to_empty(rx, NULL) == 0 && "the miss: the expiry's read finds nothing");
    send1(tx, 'y');                      /* the reply, after the miss */
    assert(poll_for_bell() == 1 && "a reply after a miss still rings");
    uint8_t got[4] = {0};
    assert(read_to_empty(rx, got) == 1 && got[0] == 'y');

    /* A closed socket is forgotten: its fd, reused by the next bind, rings nothing
       until that bind registers it. */
    udp_close(rx);
    int again = udp_socket_bind(BELL_PORT);
    assert(again >= 0);
    send1(tx, 'z');
    assert(poll_quiet() == 0 && "an unregistered socket never rings");
    udp_close(again); udp_close(tx);
}

int main(void) {
    udp_open();
    roundtrip("127.0.0.1");   /* IPv4 (v4-mapped onto the dual-stack socket) */
    roundtrip("::1");         /* IPv6 */
    assert(posts == 0 && "an unregistered socket (OTA, the tee) never rings");
    two_under_one_bell();
    miss_then_reply();
    printf("test_udp OK\n");
    return 0;
}
