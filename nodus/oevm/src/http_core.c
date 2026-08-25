/* Transport-agnostic HTTP core: HTTP/1.0 framing, the request ring, and the
   bounded in-flight table with G2 shed. Pure C — NO sockets, NO threads, NO VM
   headers. Shared by every OEVM_TARGET; the transport (listen/poll/emit) lives
   in src/backend/<target>/http_<target>.c. Single-threaded: http_poll,
   http_take_request (via http_pump) and http_respond all run on the one VM
   reactor thread at the pumpEvents safe point, so no lock is needed. */
#include "http_backend.h"
#include "span.h"
#include "span_backend.h"
#include <stdio.h>
#include <string.h>

/* Parse "METHOD SP PATH ..." from a request line into bounded, NUL-terminated
   buffers (method truncated to mcap-1, path to pcap-1 — same bound as the old
   "%7s %63s"). Returns 1 iff both a non-empty method and path were found, else 0.
   Hand-rolled ON PURPOSE: picolibc's sscanf/vfscanf faults on the ESP32 (a
   ROM/stack gotcha in the same family as the ROM fflush fault — see
   zephyr/prj_esp32.conf), and vfscanf's large stack frame is unsafe this deep in
   the interpreter's C call chain. This uses negligible stack and no stdio. */
int http_parse_reqline(const char *buf, char *method, int mcap,
                       char *path, int pcap) {
    const char *p = buf;
    int i = 0, j = 0;
    while (*p == ' ' || *p == '\t') p++;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
        if (i < mcap - 1) method[i++] = *p;
        p++;
    }
    method[i] = '\0';
    if (i == 0) return 0;
    while (*p == ' ' || *p == '\t') p++;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
        if (j < pcap - 1) path[j++] = *p;
        p++;
    }
    path[j] = '\0';
    return j > 0 ? 1 : 0;
}

static const char *reason_phrase(int status) {
    switch (status) {
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default:  return "OK";
    }
}

/* Header block only (status line + headers + blank line); body is streamed
   separately by http_emit, so an arbitrarily large body needs no scratch. */
int http_frame_headers(int status, int body_len, char *out, int out_cap) {
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

/* HTTP/1.1 chunked variant: no Content-Length (body length isn't known up
   front for a streamed reply); the framing itself communicates it in
   per-chunk size prefixes instead. */
int http_frame_chunked_headers(int status, char *out, int out_cap) {
    int n = snprintf(out, (size_t)out_cap,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/html\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, reason_phrase(status));
    if (n < 0 || n >= out_cap) return -1;
    return n;
}

/* One chunk's size-line prefix: hex length + CRLF (RFC 7230 4.1). Caller
   writes the chunk header, chunk_len bytes of data, then a trailing CRLF. */
int http_frame_chunk_header(int len, char *out, int out_cap) {
    int n = snprintf(out, (size_t)out_cap, "%x\r\n", len);
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

struct InflightSlot { int live; int id; int fd; };
struct RingItem {
    int id; char method[HTTP_METHOD_MAX]; char path[HTTP_PATH_MAX];
    /* #61: span_stamp() at accept, closed at take. An OPAQUE backend unit, not
       microseconds — only span_elapsed_us() may interpret it (span_backend.h).
       The stamp lives on the RING item
       rather than on a shared slot start because several requests can be queued at
       once and one shared start would have them overwrite each other. It rides the
       ring rather than the in-flight table because the interval being measured is
       exactly the ring's — enqueue to dequeue; the in-flight slot outlives it. */
    uint32_t accepted_at;
};

static struct InflightSlot inflight[OEVM_HTTP_INFLIGHT];
static struct RingItem ring[OEVM_HTTP_INFLIGHT];
static int ring_head = 0, ring_count = 0;
static int next_id = 1;

static void copy_bounded(char *dst, int cap, const char *src, int len) {
    if (cap <= 0) return;
    int n = len < cap - 1 ? len : cap - 1;
    if (n > 0) memcpy(dst, src, (size_t)n);
    dst[n] = '\0';
}

/* Frame the header block then hand header+body to the per-target transport. */
static void core_emit(int fd, int status, const char *body, int body_len) {
    char hdr[256];
    int hn = http_frame_headers(status, body_len, hdr, sizeof hdr);
    if (hn < 0) { hn = http_frame_headers(500, 0, hdr, sizeof hdr); body = ""; body_len = 0; }
    http_emit(fd, hdr, hn, body, body_len);
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
    /* Gate on ring capacity too, not just a free in-flight slot: http_respond
       frees an in-flight slot without touching the ring, so gating on the slot
       alone could overwrite a still-live ring[tail]. (Same reasoning as the
       original posix core.) */
    if (slot < 0 || ring_count >= OEVM_HTTP_INFLIGHT) {   /* G2 shed */
        core_emit(fd, 503, "", 0);
        return -1;
    }
    int id = next_id++;
    inflight[slot].live = 1; inflight[slot].id = id; inflight[slot].fd = fd;
    int tail = (ring_head + ring_count) % OEVM_HTTP_INFLIGHT;
    ring[tail].id = id;
    copy_bounded(ring[tail].method, HTTP_METHOD_MAX, method, ml);
    copy_bounded(ring[tail].path, HTTP_PATH_MAX, path, pl);
    ring[tail].accepted_at = span_stamp();    /* #61: opens the httpQueue span */
    ring_count++;
    return id;
}

int http_take_request(int *id, char *method, int mcap, char *path, int pcap) {
    if (ring_count == 0) return 0;
    struct RingItem *it = &ring[ring_head];
    *id = it->id;
    copy_bounded(method, mcap, it->method, (int)strlen(it->method));
    copy_bounded(path, pcap, it->path, (int)strlen(it->path));
    /* #61: close the httpQueue span. span_record rather than span_begin/span_end
       because the start is per-REQUEST, and the u32 difference is correct across a
       counter wrap so long as the wait is shorter than the wrap period. The shed
       path above never reaches here, so a 503 contributes no sample — it is a
       failure, counted in nsl, not a fast job. */
    span_record(SPAN_SLOT_HTTP_QUEUE, span_elapsed_us(it->accepted_at));
    ring_head = (ring_head + 1) % OEVM_HTTP_INFLIGHT;
    ring_count--;
    return 1;
}

void http_respond(int id, int status, const char *body, int body_len) {
    for (int i = 0; i < OEVM_HTTP_INFLIGHT; i++) {
        if (inflight[i].live && inflight[i].id == id) {
            core_emit(inflight[i].fd, status, body, body_len);
            inflight[i].live = 0;
            break;
        }
    }
}

/* Streamed reply lifecycle (Transfer-Encoding: chunked), keyed by request id
   like http_respond. The in-flight slot stays live across begin/chunk/end
   (no separate "streaming" flag needed) and is freed by http_end, mirroring
   how http_respond frees it in one shot. */
static int fd_for_id(int id) {
    for (int i = 0; i < OEVM_HTTP_INFLIGHT; i++)
        if (inflight[i].live && inflight[i].id == id) return inflight[i].fd;
    return -1;
}

void http_begin(int id, int status) {
    int fd = fd_for_id(id);            /* -1 == injected sink OR unknown id */
    char hdr[128];
    int hn = http_frame_chunked_headers(status, hdr, sizeof hdr);
    if (hn < 0) return;
    http_write(fd, hdr, hn);
}

void http_chunk(int id, const char *data, int len) {
    if (len <= 0) return;                             /* never emit a 0-size data chunk */
    int fd = fd_for_id(id);
    char szline[16];
    int sn = http_frame_chunk_header(len, szline, sizeof szline);
    if (sn < 0) return;
    {   /* #61: the egress span — one sample per emitted chunk, covering all three
           writes because they are one chunk's worth of transport cost and a caller
           tuning page size acts on the total, not on the payload write alone.
           http_begin/http_end are deliberately outside: their framing is fixed-size
           and per-response, so folding them in would smear a constant across a
           per-chunk distribution. */
        uint32_t t0 = span_stamp();
        http_write(fd, szline, sn);
        http_write(fd, data, len);
        http_write(fd, "\r\n", 2);
        span_record(SPAN_SLOT_EGRESS, span_elapsed_us(t0));
    }
}

void http_end(int id) {
    int fd = fd_for_id(id);
    http_write(fd, "0\r\n\r\n", 5);                   /* terminating chunk */
    http_close(fd);
    for (int i = 0; i < OEVM_HTTP_INFLIGHT; i++)
        if (inflight[i].live && inflight[i].id == id) { inflight[i].live = 0; break; }
}
