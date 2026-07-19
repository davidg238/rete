#ifndef OEVM_HTTP_BACKEND_H
#define OEVM_HTTP_BACKEND_H
/* The HTTP request/reply driver seam. Pure C — NO VM headers here. Exactly one
   impl is linked per OEVM_TARGET from src/backend/<target>/http_<target>.c.
   The request half mirrors the pulse seam (mailbox drained at the pumpEvents
   safe point by http_pump in src/http.c); the reply half is the NetBridge egress
   idiom (an nsl reaction forwards dev/http/reply records to httpRespondId:...,
   which lands in http_respond). See the design spec. */
#include <stddef.h>

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
/* Frame just the HTTP/1.0 header block. Returns bytes written or -1 on overflow. */
int  http_frame_headers(int status, int body_len, char *out, int out_cap);

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

/* Bind a TCP listener on port (non-blocking) so http_poll can accept on it.
   Returns 0 on success, -1 on any socket setup failure. */
int  http_listen(int port);

/* Per-target transport hooks (implemented in src/backend/<target>/http_<target>.c).
   After the poll unification there is no accept thread: http_poll runs on the VM
   reactor thread inside pumpEvents. */
/* Accept+read at most one pending connection (non-blocking) and admit it via
   http_accept_request. No-op when nothing is pending. */
void http_poll(void);
/* Write a framed response (header block hdr[0..hlen) then body[0..body_len)) to
   the transport for fd. fd == -1 is the host "injected" sink (stdout on posix,
   discarded on device). */
void http_emit(int fd, const char *hdr, int hlen, const char *body, int body_len);

#endif /* OEVM_HTTP_BACKEND_H */
