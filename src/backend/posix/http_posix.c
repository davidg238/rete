/* Posix (host) impl of the HTTP driver seam. Pure C, no VM deps — unit-testable
   in isolation. Tasks 1-2 are the pure ring / in-flight / framing core; Task 6
   adds the real TCP listener + accept thread (http_listen), which runs on its own
   thread and shares the ring / in-flight table with the reactor thread — so all
   three seam entry points lock http_mu. */
#define _POSIX_C_SOURCE 200809L
#include "http_backend.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>       /* read(), write(), close() */
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static const char *reason_phrase(int status) {
    switch (status) {
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default:  return "OK";
    }
}

/* Frame just the HTTP/1.0 header block (status line + headers + blank line) into
   out. Returns bytes written, or -1 on overflow. The header is small and bounded
   regardless of body size — the body is streamed separately (see emit), so an
   arbitrarily large body (e.g. a 4 KB+ SVG) needs no large scratch buffer. */
static int http_frame_headers(int status, int body_len, char *out, int out_cap) {
    int n = snprintf(out, (size_t)out_cap,
        "HTTP/1.0 %d %s\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, reason_phrase(status), body_len);
    if (n < 0 || n >= out_cap) return -1;
    return n;
}

int http_frame_response(int status, const char *body, int body_len,
                        char *out, int out_cap) {
    int n = http_frame_headers(status, body_len, out, out_cap);
    if (n < 0 || n + body_len >= out_cap) return -1;
    if (body_len > 0) memcpy(out + n, body, (size_t)body_len);
    out[n + body_len] = '\0';
    return n + body_len;
}

#define HTTP_METHOD_MAX 8
#define HTTP_PATH_MAX   64

struct InflightSlot { int live; int id; int fd; };
struct RingItem { int id; char method[HTTP_METHOD_MAX]; char path[HTTP_PATH_MAX]; };

static struct InflightSlot inflight[OEVM_HTTP_INFLIGHT];
static struct RingItem ring[OEVM_HTTP_INFLIGHT];
static int ring_head = 0, ring_count = 0;
static int next_id = 1;

/* Guards ring + inflight + next_id: the accept thread admits (http_accept_request)
   while the reactor thread takes (http_take_request) and responds (http_respond).
   The emit() socket I/O in http_respond / the shed path runs under the lock too —
   bounded and simplest-correct for the spike (single accept thread, single
   reactor). */
static pthread_mutex_t http_mu = PTHREAD_MUTEX_INITIALIZER;

static void copy_bounded(char *dst, int cap, const char *src, int len) {
    if (cap <= 0) return;
    int n = len < cap - 1 ? len : cap - 1;
    if (n > 0) memcpy(dst, src, (size_t)n);
    dst[n] = '\0';
}

/* Emit a response to fd (>=0) or stdout (-1): the small header block first, then
   the body streamed directly from the caller's buffer (the VM String) — no
   response-sized scratch buffer, so the body may be arbitrarily large. */
static void emit(int fd, int status, const char *body, int body_len) {
    char hdr[256];
    int hn = http_frame_headers(status, body_len, hdr, sizeof hdr);
    if (hn < 0) { hn = http_frame_headers(500, 0, hdr, sizeof hdr); body = ""; body_len = 0; }
    if (fd == -1) {
        fwrite(hdr, 1, (size_t)hn, stdout);
        if (body_len > 0) fwrite(body, 1, (size_t)body_len, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    } else {
        (void)write(fd, hdr, (size_t)hn);
        if (body_len > 0) (void)write(fd, body, (size_t)body_len);
        close(fd);
    }
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
    pthread_mutex_lock(&http_mu);
    int slot = -1;
    for (int i = 0; i < OEVM_HTTP_INFLIGHT; i++) if (!inflight[i].live) { slot = i; break; }
    /* Gate on ring capacity too, not just a free in-flight slot: http_respond
       frees an in-flight slot without touching the ring, so a respond() that
       happens without a preceding take() for that id would otherwise leave
       the ring slot occupied while the in-flight table reports free — a
       later accept would then overwrite a still-live ring[tail] and push
       ring_count past OEVM_HTTP_INFLIGHT (silent overwrite + duplicate
       delivery on the next take's wrap). */
    if (slot < 0 || ring_count >= OEVM_HTTP_INFLIGHT) {   /* G2 shed */
        emit(fd, 503, "", 0);
        pthread_mutex_unlock(&http_mu);
        return -1;
    }
    int id = next_id++;
    inflight[slot].live = 1; inflight[slot].id = id; inflight[slot].fd = fd;
    int tail = (ring_head + ring_count) % OEVM_HTTP_INFLIGHT;
    ring[tail].id = id;
    copy_bounded(ring[tail].method, HTTP_METHOD_MAX, method, ml);
    copy_bounded(ring[tail].path, HTTP_PATH_MAX, path, pl);
    ring_count++;
    pthread_mutex_unlock(&http_mu);
    return id;
}

int http_take_request(int *id, char *method, int mcap, char *path, int pcap) {
    pthread_mutex_lock(&http_mu);
    if (ring_count == 0) { pthread_mutex_unlock(&http_mu); return 0; }
    struct RingItem *it = &ring[ring_head];
    *id = it->id;
    copy_bounded(method, mcap, it->method, (int)strlen(it->method));
    copy_bounded(path, pcap, it->path, (int)strlen(it->path));
    ring_head = (ring_head + 1) % OEVM_HTTP_INFLIGHT;
    ring_count--;
    pthread_mutex_unlock(&http_mu);
    return 1;
}

void http_respond(int id, int status, const char *body, int body_len) {
    pthread_mutex_lock(&http_mu);
    for (int i = 0; i < OEVM_HTTP_INFLIGHT; i++) {
        if (inflight[i].live && inflight[i].id == id) {
            emit(inflight[i].fd, status, body, body_len);
            inflight[i].live = 0;
            break;
        }
    }
    pthread_mutex_unlock(&http_mu);
}

/* ---- Task 6: real TCP listener + accept thread -------------------------- */

static int listen_fd = -1;

/* One accept thread: block on accept(), read the request line, parse
   "METHOD SP PATH SP...", discard the rest, and hand {fd, method, path} to the
   admission core. http_accept_request records fd in the in-flight slot (or sheds
   503 straight to the socket) — the reactor thread later frames the reply and
   writes it to that fd via http_respond. */
static void *accept_loop(void *arg) {
    (void)arg;
    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) continue;
        char buf[1024];
        int n = (int)read(fd, buf, sizeof buf - 1);
        if (n <= 0) { close(fd); continue; }
        buf[n] = '\0';
        char method[HTTP_METHOD_MAX] = {0}, path[HTTP_PATH_MAX] = {0};
        if (sscanf(buf, "%7s %63s", method, path) != 2) { close(fd); continue; }
        http_accept_request(fd, method, (int)strlen(method), path, (int)strlen(path));
    }
    return NULL;
}

int http_listen(int port) {
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return -1;
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)port);
    if (bind(listen_fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(listen_fd); listen_fd = -1; return -1;
    }
    if (listen(listen_fd, 8) < 0) {
        close(listen_fd); listen_fd = -1; return -1;
    }
    pthread_t th;
    if (pthread_create(&th, NULL, accept_loop, NULL) != 0) {
        close(listen_fd); listen_fd = -1; return -1;
    }
    pthread_detach(th);
    return 0;
}
