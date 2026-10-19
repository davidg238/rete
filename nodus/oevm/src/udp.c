/* Native UDP capability module.
   Mirrors serial.c exactly: a static BSS singleton (UdpObj/UdpVtable), reached
   by receiver-identity dispatch in dispatch.c, with its own selector matching.
   Adds nothing to PRIM_NAMES.

   basicReceive: returns a 3-element indexable Array (via indexable_new from
   platform.c) at 1-based indices: 1=dataString, 2=hostString, 3=portSMI.
   Strings are built with gc_allocate(StringVtable,...) directly (same pattern
   as make_string in dispatch.c, which is file-static and not reachable here).
   indexable_new(3) is non-static in platform.c; declared extern below.
   Array slots are set by direct C struct access (no basic_at_put function). */
#include "udp.h"
#include "udp_backend.h"
#include "oimg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern oop VtableVtable;
extern oop StringVtable;
extern oop indexable_new(intptr_t n);   /* platform.c: alloc n-slot indexable */

static struct Object UdpObj;
static struct Vtable UdpVtable;
static int udp_initialized = 0;

/* Local String ctor: same pattern as the static make_string in dispatch.c. */
static oop udp_make_string(const uint8_t *bytes, size_t len) {
    oop s = gc_allocate(StringVtable, sizeof(struct String) + len);
    ((struct String *)s)->length = len;
    if (len > 0) memcpy(((struct String *)s)->bytes, bytes, len);
    return s;
}

extern oop RawBytesVtable;   /* Milestone B RawBytes leaf; set by kernel_boot.c (host) / solidified_image.c (Model A) */

/* Datagram payloads are opaque bytes, not text (Rung 6 Layer 2). Same {length,
   inline bytes} layout as udp_make_string, distinct vtable so the leaf never
   gains String methods and reads back as an immutable Bytes in nsl. */
static oop udp_make_bytes(const uint8_t *bytes, size_t len) {
    oop s = gc_allocate(RawBytesVtable, sizeof(struct String) + len);
    ((struct String *)s)->length = len;
    if (len > 0) memcpy(((struct String *)s)->bytes, bytes, len);
    return s;
}

oop udp_object(void) {
    if (!udp_initialized) {
        udp_open();
        UdpVtable.base.vtable = VtableVtable;
        UdpVtable.delegate = NULL; UdpVtable.bindings = NULL;
        UdpVtable.capacity = 0; UdpVtable.size = 0; UdpVtable.is_nested = 0;
        UdpObj.vtable = (oop)&UdpVtable;
        udp_initialized = 1;
    }
    return (oop)&UdpObj;
}

oop udp_dispatch(oop selector, int argc, oop *args) {
    (void)argc;
    const char *sel = symbol_name((struct Symbol *)selector);

    if (strcmp(sel, "basicBind:") == 0) {              /* SMI port -> SMI fd / SMI -1 */
        int port = IS_SMI(args[0]) ? (int)UNTAG_SMI(args[0]) : 0;
        return TAG_SMI(udp_socket_bind(port));
    }
    if (strcmp(sel, "basicSend:to:port:data:") == 0) { /* fd, hostStr, portSMI, dataBytesLeaf */
        int fd = (int)UNTAG_SMI(args[0]);
        struct String *host = (struct String *)args[1];
        int port = (int)UNTAG_SMI(args[2]);
        struct String *data = (struct String *)args[3];
        char hostbuf[128];
        size_t hl = host->length < sizeof hostbuf - 1 ? host->length : sizeof hostbuf - 1;
        memcpy(hostbuf, host->bytes, hl); hostbuf[hl] = '\0';
        udp_send(fd, hostbuf, port, data->bytes, (int)data->length);
        return (oop)&UdpObj;
    }
    if (strcmp(sel, "basicReceive:") == 0) {           /* fd -> {dataStr,hostStr,portSMI} | nil */
        int fd = (int)UNTAG_SMI(args[0]);
        uint8_t buf[2048]; char host[128]; int port = 0;
        int n = udp_receive(fd, buf, sizeof buf, host, sizeof host, &port);
        if (n < 0) return NULL;                        /* nil: nothing queued */
        /* GC is non-moving (blocks never relocate), but that does NOT keep an
           unrooted block alive — reclamation still sweeps it. The conservative
           C-stack scan used to keep these bare-local oops live; with it gone,
           dataS is live across two later allocations and hostS across one, so
           each must be pinned as a temp root before those allocations run. */
        oop dataS = udp_make_bytes(buf, (size_t)n);
        gc_temp_root_push(dataS);
        oop hostS = udp_make_string((const uint8_t *)host, strlen(host));
        gc_temp_root_push(hostS);
        oop arr = indexable_new(3);  /* slots[0]=TAG_SMI(3); slots[1..3]=NULL */
        /* basicAt: is 1-based: slots[1]=data, slots[2]=host, slots[3]=portSMI */
        ((struct ObjectInstance *)arr)->slots[1] = dataS;
        ((struct ObjectInstance *)arr)->slots[2] = hostS;
        ((struct ObjectInstance *)arr)->slots[3] = TAG_SMI(port);
        gc_temp_roots_clear();
        return arr;
    }
    if (strcmp(sel, "basicClose:") == 0) {             /* fd -> self */
        if (IS_SMI(args[0])) udp_close((int)UNTAG_SMI(args[0]));
        return (oop)&UdpObj;
    }
    fprintf(stderr, "VM Error: Udp doesNotUnderstand #%s\n", sel);
    abort();
}
