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

int main(void) {
    test_frame_200();
    test_frame_404();
    test_frame_503();
    test_frame_overflow();
    printf("test_http OK\n");
    return 0;
}
