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

int main(void) {
    test_frame_200();
    test_frame_404();
    test_frame_503();
    test_frame_overflow();
    test_admit_take_respond();
    test_backpressure_shed();
    printf("test_http OK\n");
    return 0;
}
