#ifndef OEVM_HTTP_BACKEND_H
#define OEVM_HTTP_BACKEND_H
/* The HTTP request/reply driver seam. Pure C — NO VM headers here. Exactly one
   impl is linked per OEVM_TARGET from src/backend/<target>/http_<target>.c.
   The request half mirrors the pulse seam (mailbox drained at the pumpEvents
   safe point by http_pump in src/http.c); the reply half is the NetBridge egress
   idiom (an nsl reaction forwards dev/http/reply records to httpRespondId:...,
   which lands in http_respond). See the design spec. */
#include <stddef.h>
#include <stdint.h>

#ifndef OEVM_HTTP_INFLIGHT
#define OEVM_HTTP_INFLIGHT 4
#endif

/* Method/path scratch-buffer caps, shared by http_core.c's ring and every
   backend's request-line scan (posix/zephyr) so the three copies can never
   drift out of sync (found in Task 1 review). */
#ifndef HTTP_METHOD_MAX
#define HTTP_METHOD_MAX 8
#endif
#ifndef HTTP_PATH_MAX
#define HTTP_PATH_MAX   64
#endif

/* Frame a complete HTTP/1.0 response into out. Returns bytes written, or -1 on
   overflow. Reason phrase derived from status. */
int  http_frame_response(int status, const char *body, int body_len,
                         char *out, int out_cap);

/* Parse "METHOD SP PATH ..." into bounded NUL-terminated buffers. 1 if both
   found, else 0. Hand-rolled (no sscanf): picolibc vfscanf faults on ESP32. */
int  http_parse_reqline(const char *buf, char *method, int mcap,
                        char *path, int pcap);
/* Frame just the HTTP/1.0 header block. Returns bytes written or -1 on overflow. */
int  http_frame_headers(int status, int body_len, char *out, int out_cap);

/* HTTP/1.1 chunked framing (for streamed bodies). */
int  http_frame_chunked_headers(int status, char *out, int out_cap);
int  http_frame_chunk_header(int len, char *out, int out_cap);

/* Streamed reply lifecycle, keyed by request id (mirrors http_respond). */
void http_begin(int id, int status);
void http_chunk(int id, const char *data, int len);
void http_end(int id);

/* Raw transport hooks (per target). http_write writes all len bytes (non-closing);
   returns 0 on success, -1 on peer error. http_close closes the fd. */
int  http_write(int fd, const char *buf, int len);
void http_close(int fd);

/* Admit a request. fd == -1 means "injected" (framed reply goes to stdout);
   fd >= 0 is a real socket. Returns a fresh id, or -1 when the in-flight table
   is full (a 503 has already been emitted — G2 shed). */
int  http_accept_request(int fd, const char *method, int ml,
                         const char *path, int pl);
/* Pop the oldest pending request into caller buffers. 1 if taken, 0 if empty. */
int  http_take_request(int *id, char *method, int mcap, char *path, int pcap);
/* Frame + emit the reply for id (to its fd, or stdout if fd == -1), free the slot. */
void http_respond(int id, int status, const char *body, int body_len);
int  http_inflight_count(void);
void http_backend_reset(void);

/* Bind a TCP listener on port (non-blocking) and register it with the Net doorbells
   (udp_backend.h udp_doorbell) on chan_id, so a pending connection rings that channel
   (rete#440: readiness only; the listener's reaction calls http_poll). Returns 0 on
   success, -1 on any socket setup failure. */
int  http_listen(int port, int chan_id);

/* Count of accept() failures that were NOT "nothing pending" (#65). Non-zero means
   the listener could not accept despite work being available — resource
   exhaustion, historically silent and terminal. 0 on backends that cannot fail
   this way. */
uint32_t http_accept_failures(void);

/* Per-target transport hooks (implemented in src/backend/<target>/http_<target>.c).
   There is no accept thread: http_poll runs on the VM thread, in the prim
   (`platform httpAccept`) that the listener's doorbell reaction calls (rete#440). */
/* Accept+read at most one pending connection (non-blocking) and admit it via
   http_accept_request. Answers 1 when it accepted a connection (whether or not the
   request line parsed), 0 when none was pending; a 0 also re-arms the listener's
   doorbell, so the reaction loops until 0 (it reads the listener to empty). */
int  http_poll(void);
/* Write a framed response (header block hdr[0..hlen) then body[0..body_len)) to
   the transport for fd. fd == -1 is the host "injected" sink (stdout on posix,
   discarded on device). */
void http_emit(int fd, const char *hdr, int hlen, const char *body, int body_len);

#endif /* OEVM_HTTP_BACKEND_H */
