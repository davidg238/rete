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

#endif /* OEVM_HTTP_BACKEND_H */
