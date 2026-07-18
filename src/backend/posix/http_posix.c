/* Posix (host) impl of the HTTP driver seam. Pure C, no VM deps — unit-testable
   in isolation. The accept thread + sockets arrive in Task 6; Tasks 1-2 are the
   pure ring / in-flight / framing core. */
#include "http_backend.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>   /* write(), close() */

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

#define HTTP_METHOD_MAX 8
#define HTTP_PATH_MAX   64
#define HTTP_RESP_MAX   4096

struct InflightSlot { int live; int id; int fd; };
struct RingItem { int id; char method[HTTP_METHOD_MAX]; char path[HTTP_PATH_MAX]; };

static struct InflightSlot inflight[OEVM_HTTP_INFLIGHT];
static struct RingItem ring[OEVM_HTTP_INFLIGHT];
static int ring_head = 0, ring_count = 0;
static int next_id = 1;

static void copy_bounded(char *dst, int cap, const char *src, int len) {
    int n = len < cap - 1 ? len : cap - 1;
    if (n > 0) memcpy(dst, src, (size_t)n);
    dst[n] = '\0';
}

/* Emit a framed response to fd (>=0) or stdout (-1). */
static void emit(int fd, int status, const char *body, int body_len) {
    char resp[HTTP_RESP_MAX];
    int n = http_frame_response(status, body, body_len, resp, sizeof resp);
    if (n < 0) { n = http_frame_response(500, "", 0, resp, sizeof resp); }
    if (fd == -1) { fwrite(resp, 1, (size_t)n, stdout); fputc('\n', stdout); fflush(stdout); }
    else          { (void)write(fd, resp, (size_t)n); close(fd); }
}

int http_inflight_count(void) {
    int c = 0;
    for (int i = 0; i < OEVM_HTTP_INFLIGHT; i++) if (inflight[i].live) c++;
    return c;
}

void http_backend_reset(void) {
    memset(inflight, 0, sizeof inflight);
    ring_head = ring_count = 0;
    next_id = 1;
}

int http_accept_request(int fd, const char *method, int ml,
                        const char *path, int pl) {
    int slot = -1;
    for (int i = 0; i < OEVM_HTTP_INFLIGHT; i++) if (!inflight[i].live) { slot = i; break; }
    if (slot < 0) { emit(fd, 503, "", 0); return -1; }          /* G2 shed */
    int id = next_id++;
    inflight[slot].live = 1; inflight[slot].id = id; inflight[slot].fd = fd;
    int tail = (ring_head + ring_count) % OEVM_HTTP_INFLIGHT;
    ring[tail].id = id;
    copy_bounded(ring[tail].method, HTTP_METHOD_MAX, method, ml);
    copy_bounded(ring[tail].path, HTTP_PATH_MAX, path, pl);
    ring_count++;
    return id;
}

int http_take_request(int *id, char *method, int mcap, char *path, int pcap) {
    if (ring_count == 0) return 0;
    struct RingItem *it = &ring[ring_head];
    *id = it->id;
    copy_bounded(method, mcap, it->method, (int)strlen(it->method));
    copy_bounded(path, pcap, it->path, (int)strlen(it->path));
    ring_head = (ring_head + 1) % OEVM_HTTP_INFLIGHT;
    ring_count--;
    return 1;
}

void http_respond(int id, int status, const char *body, int body_len) {
    for (int i = 0; i < OEVM_HTTP_INFLIGHT; i++) {
        if (inflight[i].live && inflight[i].id == id) {
            emit(inflight[i].fd, status, body, body_len);
            inflight[i].live = 0;
            return;
        }
    }
}
