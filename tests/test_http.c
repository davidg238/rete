#include "http_backend.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static void test_frame_200(void) {
    char buf[512];
    int n = http_frame_response(200, "hello", 5, buf, sizeof buf);
    assert(n > 0);
    const char *expect =
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: 5\r\n"
        "Connection: close\r\n"
        "\r\n"
        "hello";
    assert(strcmp(buf, expect) == 0);
}

static void test_frame_404(void) {
    char buf[512];
    int n = http_frame_response(404, "nope", 4, buf, sizeof buf);
    assert(n > 0);
    assert(strstr(buf, "HTTP/1.0 404 Not Found\r\n") == buf);
    assert(strstr(buf, "Content-Length: 4\r\n") != NULL);
}

static void test_frame_503(void) {
    char buf[512];
    int n = http_frame_response(503, "", 0, buf, sizeof buf);
    assert(n > 0);
    assert(strstr(buf, "HTTP/1.0 503 Service Unavailable\r\n") == buf);
    assert(strstr(buf, "Content-Length: 0\r\n") != NULL);
}

static void test_frame_overflow(void) {
    char buf[16];
    int n = http_frame_response(200, "a really long body that will not fit", 36, buf, sizeof buf);
    assert(n == -1);
}

static void test_admit_take_respond(void) {
    http_backend_reset();
    int id = http_accept_request(-1, "GET", 3, "/hello", 6);
    assert(id >= 0);
    assert(http_inflight_count() == 1);

    int tid; char m[8]; char p[64];
    assert(http_take_request(&tid, m, sizeof m, p, sizeof p) == 1);
    assert(tid == id);
    assert(strcmp(m, "GET") == 0);
    assert(strcmp(p, "/hello") == 0);
    assert(http_take_request(&tid, m, sizeof m, p, sizeof p) == 0); /* ring now empty */
    assert(http_inflight_count() == 1);                             /* still in flight */

    http_respond(id, 200, "hello", 5);                             /* frees the slot */
    assert(http_inflight_count() == 0);
}

static void test_backpressure_shed(void) {
    http_backend_reset();
    for (int i = 0; i < OEVM_HTTP_INFLIGHT; i++)
        assert(http_accept_request(-1, "GET", 3, "/x", 2) >= 0);
    assert(http_inflight_count() == OEVM_HTTP_INFLIGHT);
    /* the (N+1)th is shed with 503 */
    assert(http_accept_request(-1, "GET", 3, "/x", 2) == -1);
    assert(http_inflight_count() == OEVM_HTTP_INFLIGHT);
}

/* Drive more requests through than fit in-flight at once, wrapping ring_head,
   and assert strict FIFO order survives the wrap. */
static void test_ring_wraparound_fifo(void) {
    http_backend_reset();
    int id0 = http_accept_request(-1, "GET", 3, "/a", 2);
    int id1 = http_accept_request(-1, "GET", 3, "/b", 2);
    int id2 = http_accept_request(-1, "GET", 3, "/c", 2);
    int id3 = http_accept_request(-1, "GET", 3, "/d", 2);
    assert(id0 >= 0 && id1 >= 0 && id2 >= 0 && id3 >= 0);
    assert(http_inflight_count() == OEVM_HTTP_INFLIGHT);

    int tid; char m[8]; char p[64];
    assert(http_take_request(&tid, m, sizeof m, p, sizeof p) == 1);
    assert(tid == id0 && strcmp(p, "/a") == 0);
    assert(http_take_request(&tid, m, sizeof m, p, sizeof p) == 1);
    assert(tid == id1 && strcmp(p, "/b") == 0);
    http_respond(id0, 200, "", 0);
    http_respond(id1, 200, "", 0);

    /* two free in-flight slots now; accepting these wraps ring_head/tail */
    int id4 = http_accept_request(-1, "GET", 3, "/e", 2);
    int id5 = http_accept_request(-1, "GET", 3, "/f", 2);
    assert(id4 >= 0 && id5 >= 0);

    assert(http_take_request(&tid, m, sizeof m, p, sizeof p) == 1);
    assert(tid == id2 && strcmp(p, "/c") == 0);
    assert(http_take_request(&tid, m, sizeof m, p, sizeof p) == 1);
    assert(tid == id3 && strcmp(p, "/d") == 0);
    assert(http_take_request(&tid, m, sizeof m, p, sizeof p) == 1);
    assert(tid == id4 && strcmp(p, "/e") == 0);
    assert(http_take_request(&tid, m, sizeof m, p, sizeof p) == 1);
    assert(tid == id5 && strcmp(p, "/f") == 0);
    assert(http_take_request(&tid, m, sizeof m, p, sizeof p) == 0); /* drained */

    http_respond(id2, 200, "", 0);
    http_respond(id3, 200, "", 0);
    http_respond(id4, 200, "", 0);
    http_respond(id5, 200, "", 0);
    assert(http_inflight_count() == 0);
}

/* Prove the Critical fix: respond-before-take frees in-flight slots without
   draining the ring. A naive admission that only checks the in-flight table
   would then let http_accept_request overwrite still-occupied ring slots and
   push ring_count past OEVM_HTTP_INFLIGHT (silent overwrite + duplicate
   delivery on the next take). The fix must gate admission on ring capacity
   too, so this respond-before-take ordering still sheds cleanly. */
static void test_respond_before_take_stays_bounded(void) {
    http_backend_reset();
    int ids[OEVM_HTTP_INFLIGHT];
    for (int i = 0; i < OEVM_HTTP_INFLIGHT; i++) {
        ids[i] = http_accept_request(-1, "GET", 3, "/x", 2);
        assert(ids[i] >= 0);
    }
    assert(http_inflight_count() == OEVM_HTTP_INFLIGHT);

    /* respond to all WITHOUT taking first -- frees every in-flight slot while
       the ring is still full of un-taken entries. */
    for (int i = 0; i < OEVM_HTTP_INFLIGHT; i++) http_respond(ids[i], 200, "", 0);
    assert(http_inflight_count() == 0);

    /* in-flight table looks entirely free, but the ring is still full: a
       correct admission must shed here, not overwrite a live ring slot. */
    assert(http_accept_request(-1, "GET", 3, "/y", 2) == -1);
    assert(http_inflight_count() == 0);

    /* the ring must still yield exactly the original N entries, each exactly
       once, in FIFO order -- no duplicate delivery, no overwrite. */
    for (int i = 0; i < OEVM_HTTP_INFLIGHT; i++) {
        int tid; char m[8]; char p[64];
        assert(http_take_request(&tid, m, sizeof m, p, sizeof p) == 1);
        assert(tid == ids[i]);
        assert(strcmp(p, "/x") == 0);
    }
    int tid; char m[8]; char p[64];
    assert(http_take_request(&tid, m, sizeof m, p, sizeof p) == 0); /* drained, not duplicated */

    /* ring now empty and in-flight table empty (those slots were freed by the
       earlier respond() calls) -- a fresh accept must succeed again. */
    assert(http_accept_request(-1, "GET", 3, "/z", 2) >= 0);
}

int main(void) {
    test_frame_200();
    test_frame_404();
    test_frame_503();
    test_frame_overflow();
    test_admit_take_respond();
    test_backpressure_shed();
    test_ring_wraparound_fifo();
    test_respond_before_take_stays_bounded();
    printf("test_http OK\n");
    return 0;
}
