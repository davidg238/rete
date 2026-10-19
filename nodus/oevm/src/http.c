/* VM-side glue for the http driver (webserver-channel-device spike, Task 3).
   Mirrors src/udp.c's Array/String construction idiom exactly. Drains the
   pure-C http_backend request ring at the pumpEvents safe point and posts an
   Array [id, method, path] (1-based slots: 1=SMI id, 2=method String,
   3=path String) to the 'dev/http/request' channel. The reply half is the
   NetBridge egress idiom, entirely in nsl + the httpRespondId:status:body:
   prim in platform.c — this file only drains the REQUEST side. */
#include "http.h"
#include "platform.h"
#include "http_backend.h"
#include "space.h"
#include "oimg.h"
#include <string.h>

extern oop StringVtable;

static oop http_make_string(const char *bytes, size_t len) {
    oop s = gc_allocate(StringVtable, sizeof(struct String) + len);
    ((struct String *)s)->length = len;
    if (len > 0) memcpy(((struct String *)s)->bytes, bytes, len);
    return s;
}

void http_pump(void) {
    int id; char method[16]; char path[64];
    while (http_take_request(&id, method, sizeof method, path, sizeof path)) {
        oop mS = http_make_string(method, strlen(method));
        gc_temp_root_push(mS);
        oop pS = http_make_string(path, strlen(path));
        gc_temp_root_push(pS);
        oop arr = indexable_new(3);                 /* slots[0]=TAG_SMI(3); [1..3]=NULL */
        ((struct ObjectInstance *)arr)->slots[1] = TAG_SMI(id);
        ((struct ObjectInstance *)arr)->slots[2] = mS;
        ((struct ObjectInstance *)arr)->slots[3] = pS;
        gc_temp_root_push(arr);
        space_post("dev/http/request", strlen("dev/http/request"), arr);
        gc_temp_roots_clear();
    }
}
