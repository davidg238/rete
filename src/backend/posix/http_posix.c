/* Posix (host) impl of the HTTP driver seam. Pure C, no VM deps — unit-testable
   in isolation. The accept thread + sockets arrive in Task 6; Tasks 1-2 are the
   pure ring / in-flight / framing core. */
#include "http_backend.h"
#include <stdio.h>
#include <string.h>

static const char *reason_phrase(int status) {
    switch (status) {
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default:  return "OK";
    }
}

int http_frame_response(int status, const char *body, int body_len,
                        char *out, int out_cap) {
    int n = snprintf(out, (size_t)out_cap,
        "HTTP/1.0 %d %s\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, reason_phrase(status), body_len);
    if (n < 0 || n + body_len >= out_cap) return -1;
    if (body_len > 0) memcpy(out + n, body, (size_t)body_len);
    out[n + body_len] = '\0';
    return n + body_len;
}
