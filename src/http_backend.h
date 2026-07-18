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

/* Frame a complete HTTP/1.0 response into out. Returns bytes written, or -1 on
   overflow. Reason phrase derived from status. */
int  http_frame_response(int status, const char *body, int body_len,
                         char *out, int out_cap);

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

#endif /* OEVM_HTTP_BACKEND_H */
