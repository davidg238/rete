# UDP Networking Capability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a confined `net` native capability giving the Newspeak image UDP datagram sockets (send/receive/close), over a real dual-stack (IPv4+IPv6) POSIX backend on the desktop.

**Architecture:** Mirror the proven `serial` native-capability spine exactly — one C module (`udp.c`) + a link-time backend seam (`udp_backend.h`) + a host impl (`udp_posix.c`), reached by a receiver-identity hook in `dispatch.c`; a thin `.ns` `Net`/`Socket`/`Datagram` facade in `lib/kernel.ns` minted by a non-public `Kernel>>netOn:`; granted by boot injection on both image paths (Path A solidified-C, Path B `.oimg`); confinement proven by a runtime DNU.

**Tech Stack:** ISO C99 (VM + backend), POSIX sockets (`getaddrinfo`/`recvfrom`/`sendto`, non-blocking), Python (boot codegen + `.oimg` serializer), Newspeak dialect (`lib/kernel.ns`), Python test runner.

## Global Constraints

- **Three repos**, all on `master` today, all clean: `oevm` (VM/C, sibling at `../oevm`), `ns-oevm` (compiler/boot/lib, the cwd), `ns-tests` (suite, sibling at `../ns-tests`). Create `feat/udp-networking` branches in all three before Task 1.
- **Run conformance from the `ns-oevm` cwd**: `python3 tools/run_tests_oevm.py` (NOT from `../oevm` — wrong cwd picks a stale harness and falsely reports 0). It builds and runs BOTH paths (A solidified + B `.oimg`).
- **Capability spine is fixed by `serial` precedent** — receiver-identity dispatch (NO numbered prim-id, nothing added to `PRIM_NAMES`); `&UdpObj`/`&UdpVtable` are static BSS so `gc.c mark()` skips them (NO new GC root).
- **Newspeak dialect rules:** comments are `(* ... *)` NEVER inline `"..."`; the entry receives the **Kernel wrapper** as its `platform`, so use `println` not `platform print:` inside `.ns`; hoist temps to method top; `basicAt:` is 1-based; reach kernel classes via the Kernel instance; nil-check native results with `== nil`.
- **Source decomposition (oevm#10):** `udp.c` + `udp_backend.h` are language-neutral oevm core; `udp_posix.c` is a language-neutral target backend in `oevm/src/backend/posix/`; only the `.ns` facade + boot wiring is ns-specific.
- **Format version:** `.oimg` `format_version` current max = **5** (Supervisor descriptor). UDP becomes **6**, appended at header tail offset `A+12W`. `registry_version` stays **4** (UDP adds no builtins).
- **Canonical capability order:** entry signature is `usingPlatform: platform net: net` → params `[platform, net]`. Path B mints one 2nd-cap slot (serial OR net per entry, as today); document the no-both-caps limitation (extends the ns-oevm#2 hazard).
- **Datagram buffer cap = 2048 bytes** (single datagram). **Non-blocking** receive: `-1`/nil when no datagram waiting.
- Commit after every task. Do NOT push (push only on David's explicit say-so).

---

## File Structure

**Created:**
- `oevm/src/udp.h` — declares `oop udp_object(void)`, `oop udp_dispatch(oop,int,oop*)`.
- `oevm/src/udp.c` — singleton + receiver-identity dispatch (mirror `serial.c`).
- `oevm/src/udp_backend.h` — link-time seam (5 funcs).
- `oevm/src/backend/posix/udp_posix.c` — real dual-stack POSIX UDP host backend.
- `oevm/tests/test_udp.c` — backend-only C smoke test.
- `ns-tests/04_somns_basic/42_udp.ns` — capability round-trip (loopback v4 + v6).
- `ns-tests/04_somns_basic/43_udp_confined.ns` — runtime-DNU confinement pin.

**Modified:**
- `oevm/src/dispatch.c` — add `udp_object()` receiver hook beside the serial hook.
- `ns-oevm/lib/kernel.ns` — add `Net`/`Socket`/`Datagram` classes + non-public `netOn:`.
- `ns-oevm/tools/ns_compile_oevm.py` — Path A: thread `net` cap (`netOn:` name + boot send).
- `ns-oevm/tools/oimg_serialize.py` — Path B: `net_on_sel_ref` header word (v6, A+12W).
- `oevm/src/oimg_loader.c` — Path B: read `img_net_on_sel_ref`, mint Net wrapper.
- `ns-oevm/tools/run_tests_oevm.py` — add `udp.c` + net backend to the 3 gcc lists.
- `oevm/bench.sh` (+ any other oevm `*.sh` that lists `serial.c`) — add `udp.c` + net backend.
- `oevm/src/solidified_image.c` — committed default image regenerated in Task 4.
- `ns-oevm/docs/NEWSPEAK_DIALECT.md` — add dialect #14.

---

## Task 1: UDP C backend + seam + native module + smoke test

**Files:**
- Create: `oevm/src/udp_backend.h`, `oevm/src/backend/posix/udp_posix.c`, `oevm/tests/test_udp.c`, `oevm/src/udp.h`, `oevm/src/udp.c`
- Modify: `oevm/src/dispatch.c` (add hook beside serial)

**Interfaces:**
- Produces (seam, `udp_backend.h`):
  - `void udp_open(void);`
  - `int  udp_socket_bind(int port);` — returns socket fd ≥ 0, or -1; `port == 0` = ephemeral.
  - `int  udp_send(int sock, const char *host, int port, const uint8_t *data, int len);` — bytes sent, or -1.
  - `int  udp_receive(int sock, uint8_t *buf, int buf_cap, char *host_out, int host_cap, int *sender_port);` — payload length (≥ 0), or -1 when no datagram waiting.
  - `void udp_close(int sock);`
- Produces (native module, `udp.h`): `oop udp_object(void);` `oop udp_dispatch(oop selector, int argc, oop *args);`
- The C-VM dispatch boundary (consumed by Task 2's `.ns`): selectors `basicBind:` (SMI port → SMI fd or SMI -1), `basicSend:to:port:data:` (fd, hostString, port, dataString → self), `basicReceive:` (fd → 3-elem Array `{dataString, hostString, portSMI}` or nil), `basicClose:` (fd → self).

- [ ] **Step 1: Write the backend seam header `oevm/src/udp_backend.h`**

```c
#ifndef OEVM_UDP_BACKEND_H
#define OEVM_UDP_BACKEND_H
#include <stdint.h>
/* Fixed per-target seam. Exactly one impl is linked per build from
   src/backend/$OEVM_TARGET/: backend/posix/udp_posix.c (real dual-stack POSIX
   sockets) now; backend/zephyr/udp_zephyr.c (Zephyr net / Thread, IPv6) later.
   Addresses are plain strings (IPv4 dotted or IPv6); the backend owns the family. */
void udp_open(void);                       /* idempotent init at boot */
int  udp_socket_bind(int port);            /* fd >=0, or -1; port 0 = ephemeral */
int  udp_send(int sock, const char *host, int port,
              const uint8_t *data, int len);   /* bytes sent, or -1 */
/* Non-blocking. Fills buf (payload), host_out (numeric sender, NUL-terminated),
   *sender_port. Returns payload length (>=0), or -1 when no datagram waiting. */
int  udp_receive(int sock, uint8_t *buf, int buf_cap,
                 char *host_out, int host_cap, int *sender_port);
void udp_close(int sock);
#endif
```

- [ ] **Step 2: Write the C smoke test `oevm/tests/test_udp.c` (the failing test)**

```c
/* Standalone smoke test for the UDP backend (real dual-stack loopback). Not part
   of the conformance runner; build + run manually from the oevm repo root:
     gcc -std=c99 -Isrc tests/test_udp.c src/backend/posix/udp_posix.c -o /tmp/test_udp && /tmp/test_udp
   Links only the backend (the seam funcs) — not udp.c, which needs the full VM. */
#include "../src/udp_backend.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static void roundtrip(const char *loop) {
    int rx = udp_socket_bind(0);          /* ephemeral receiver */
    assert(rx >= 0);
    /* discover the bound port via a self-send loop is overkill; bind a known port */
    int rxp = udp_socket_bind(54330);
    assert(rxp >= 0);
    int tx = udp_socket_bind(0);
    assert(tx >= 0);

    /* empty receiver -> -1 (non-blocking) */
    uint8_t buf[2048]; char host[64]; int port = 0;
    assert(udp_receive(rxp, buf, sizeof buf, host, sizeof host, &port) == -1);

    /* send 'hi' to rxp over the given loopback literal, read it back */
    const uint8_t msg[2] = { 'h', 'i' };
    assert(udp_send(tx, loop, 54330, msg, 2) == 2);
    int n = -1;
    for (int i = 0; i < 100 && n < 0; i++)   /* loopback is ~instant; poll a few times */
        n = udp_receive(rxp, buf, sizeof buf, host, sizeof host, &port);
    assert(n == 2);
    assert(buf[0] == 'h' && buf[1] == 'i');
    /* v4 sender must be plain dotted-quad, not ::ffff:127.0.0.1 */
    if (strcmp(loop, "127.0.0.1") == 0) assert(strcmp(host, "127.0.0.1") == 0);

    udp_close(rx); udp_close(rxp); udp_close(tx);
}

int main(void) {
    udp_open();
    roundtrip("127.0.0.1");   /* IPv4 (v4-mapped onto the dual-stack socket) */
    roundtrip("::1");         /* IPv6 */
    printf("test_udp OK\n");
    return 0;
}
```

- [ ] **Step 3: Run the smoke test to verify it fails (no backend yet)**

Run: `cd ../oevm && gcc -std=c99 -Isrc tests/test_udp.c src/backend/posix/udp_posix.c -o /tmp/test_udp 2>&1 | head`
Expected: FAIL — `udp_posix.c: No such file or directory`.

- [ ] **Step 4: Write the host backend `oevm/src/backend/posix/udp_posix.c`**

```c
#include "udp_backend.h"
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
/* POSIX (x86-linux / macOS desktop) UDP backend: real dual-stack sockets. Each
   bound socket is AF_INET6 with IPV6_V6ONLY=0, so one socket receives both IPv6
   and IPv4 (v4-mapped) datagrams. Non-blocking (O_NONBLOCK): udp_receive returns
   -1 when nothing is queued. Pure POSIX; udp_zephyr.c replaces it on device. */

void udp_open(void) { /* nothing global to init */ }

int udp_socket_bind(int port) {
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int off = 0;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);  /* dual-stack */
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof a);
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_any;
    a.sin6_port = htons((unsigned short)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    return fd;
}

int udp_send(int sock, const char *host, int port, const uint8_t *data, int len) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET6;            /* socket is v6; map v4 targets */
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_V4MAPPED | AI_ALL | AI_NUMERICSERV;
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return -1;
    ssize_t n = sendto(sock, data, (size_t)len, 0, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    return (int)n;
}

int udp_receive(int sock, uint8_t *buf, int buf_cap,
                char *host_out, int host_cap, int *sender_port) {
    struct sockaddr_storage src;
    socklen_t slen = sizeof src;
    ssize_t n = recvfrom(sock, buf, (size_t)buf_cap, 0,
                         (struct sockaddr *)&src, &slen);
    if (n < 0) return -1;                  /* EWOULDBLOCK/EAGAIN -> nothing queued */
    char h[NI_MAXHOST];
    int port = 0;
    if (getnameinfo((struct sockaddr *)&src, slen, h, sizeof h,
                    NULL, 0, NI_NUMERICHOST) != 0) h[0] = '\0';
    if (src.ss_family == AF_INET6) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&src;
        port = ntohs(s6->sin6_port);
        /* normalize a v4-mapped sender ::ffff:1.2.3.4 -> 1.2.3.4 */
        if (IN6_IS_ADDR_V4MAPPED(&s6->sin6_addr)) {
            const char *p = strrchr(h, ':');
            if (p) memmove(h, p + 1, strlen(p + 1) + 1);
        }
    } else if (src.ss_family == AF_INET) {
        port = ntohs(((struct sockaddr_in *)&src)->sin_port);
    }
    snprintf(host_out, (size_t)host_cap, "%s", h);
    *sender_port = port;
    return (int)n;
}

void udp_close(int sock) { if (sock >= 0) close(sock); }
```

- [ ] **Step 5: Run the smoke test to verify it passes**

Run: `cd ../oevm && gcc -std=c99 -Isrc tests/test_udp.c src/backend/posix/udp_posix.c -o /tmp/test_udp && /tmp/test_udp`
Expected: PASS — prints `test_udp OK`.

- [ ] **Step 6: Write the native module `oevm/src/udp.h`**

```c
#ifndef OEVM_UDP_H
#define OEVM_UDP_H
#include "oevm.h"
oop udp_object(void);                              /* native UDP capability singleton */
oop udp_dispatch(oop selector, int argc, oop *args);
#endif
```

- [ ] **Step 7: Write the native module `oevm/src/udp.c`**

Mirror `serial.c`. `basicReceive:` builds a 3-element Array `{dataString, hostString, portSMI}` using the same helpers serial uses for Strings (`make_string`) and Arrays (`platform basicNew:`-equivalent C path — reuse `indexable_new`/the array helper used elsewhere in the VM; locate by how `platform string:` builds its String and how `udp_dispatch` can allocate a 3-slot Array). Use the array-construction helper already in the VM (grep `indexable_new` / how `platform_dispatch` builds Arrays).

```c
#include "udp.h"
#include "udp_backend.h"
#include "oimg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern oop VtableVtable;
extern oop make_string(const uint8_t *bytes, size_t len);   /* existing String ctor */
extern oop indexable_new(oop vtable, int n);                /* existing Array ctor; confirm name */
extern oop kernel_array_vtable_oop(void);                   /* however Array vtable is reached; confirm */

static struct Object UdpObj;
static struct Vtable UdpVtable;
static int udp_initialized = 0;

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
    if (strcmp(sel, "basicSend:to:port:data:") == 0) { /* fd, hostStr, portSMI, dataStr */
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
        oop dataS = make_string(buf, (size_t)n);
        oop hostS = make_string((const uint8_t *)host, strlen(host));
        oop arr = indexable_new(kernel_array_vtable_oop(), 3);  /* confirm helper */
        /* Array slot0 = TAG_SMI(len); elems 1..3 (basicAt: is 1-based) */
        basic_at_put(arr, 1, dataS);
        basic_at_put(arr, 2, hostS);
        basic_at_put(arr, 3, TAG_SMI(port));
        return arr;
    }
    if (strcmp(sel, "basicClose:") == 0) {             /* fd -> self */
        if (IS_SMI(args[0])) udp_close((int)UNTAG_SMI(args[0]));
        return (oop)&UdpObj;
    }
    fprintf(stderr, "VM Error: Udp doesNotUnderstand #%s\n", sel);
    abort();
}
```

> **VERIFY-FIRST (do before trusting Step 7):** the Array-build helpers above (`indexable_new`, `kernel_array_vtable_oop`, `basic_at_put`) are placeholders — confirm the real names/signatures by reading how `platform string:` (in `platform.c`) and the existing indexable allocation build/return Arrays and Strings. If allocating a 3-slot Array inside `udp_dispatch` proves awkward (GC/vtable reachability), FALL BACK to the spec §3(b) seam: return the raw payload via `make_string` + stash sender host/port on small statics read by two extra unary selectors (`basicLastHost`/`basicLastPort`), and let `.ns Socket>>receive` assemble the `Datagram`. Decide empirically; keep the `.ns` boundary (Task 2) matching whatever you choose.

- [ ] **Step 8: Add the receiver-identity hook in `oevm/src/dispatch.c`**

Locate the serial hook anchor:
```c
    if (receiver == serial_object()) return serial_dispatch(selector, argc, args);
```
Add immediately below it:
```c
    if (receiver == udp_object()) return udp_dispatch(selector, argc, args);
```
Add `#include "udp.h"` beside the existing `#include "serial.h"` near the top of `dispatch.c`.

- [ ] **Step 9: Wire `udp.c` + net backend into all builds**

In `ns-oevm/tools/run_tests_oevm.py`: locate the `OEVM_BACKEND` definition (it lists the platform + serial backend pair) and add the net backend file; then in EACH of the three gcc lists, add `"src/udp.c",` immediately after `"src/serial.c",`. Grep `serial.c` and `serial_{` to find every site:
- `OEVM_BACKEND = [... "src/backend/{OEVM_TARGET}/serial_{OEVM_TARGET}.c"]` → append `"src/backend/{OEVM_TARGET}/udp_{OEVM_TARGET}.c"`.
- Path B image gcc list, C-unit-test gcc list, Path A solidified gcc list — add `"src/udp.c"` after `"src/serial.c"` in each.

In `oevm/bench.sh` (and any other `oevm/*.sh` that names `serial.c`): add `src/udp.c` after `src/serial.c` and `"src/backend/$TARGET/udp_$TARGET.c"` after the serial backend.

- [ ] **Step 10: Build the full suite to confirm everything still compiles (hooks inert until Task 2)**

Run: `python3 tools/run_tests_oevm.py 2>&1 | tail -5`
Expected: build succeeds, suite still **63 pass / 0 fail / 3 skip** BOTH paths (no `.ns` uses `net` yet; `udp.c`/hook are compiled-but-inert).

- [ ] **Step 11: Commit**

```bash
cd ../oevm && git add src/udp.h src/udp.c src/udp_backend.h src/backend/posix/udp_posix.c tests/test_udp.c src/dispatch.c bench.sh && git commit -m "feat(udp): native UDP capability C module + dual-stack POSIX backend + smoke test"
cd ../ns-oevm && git add tools/run_tests_oevm.py && git commit -m "build(udp): link udp.c + net backend into all gcc lists"
```

---

## Task 2: Newspeak facade + Path A grant → `42_udp.ns` green on Path A

**Files:**
- Modify: `ns-oevm/lib/kernel.ns` (add `Net`/`Socket`/`Datagram` + `netOn:`)
- Modify: `ns-oevm/tools/ns_compile_oevm.py` (Path A boot threading)
- Create: `ns-tests/04_somns_basic/42_udp.ns`

**Interfaces:**
- Consumes (from Task 1): native `udp_object()` reached by sending `basicBind:` / `basicSend:to:port:data:` / `basicReceive:` / `basicClose:` to the `port` slot.
- Produces (for Task 3): non-public `Kernel>>netOn: aPort` returning a `Net`; the boot path sends `#netOn:` with `udp_object()`.

- [ ] **Step 1: Write the failing conformance test `ns-tests/04_somns_basic/42_udp.ns`**

```newspeak
(*
UDP — confined native capability, datagram socket over a real dual-stack POSIX
backend. R4. Entry declares the second capability `net:`.
expect:
UDP OK
*)
class UdpTest usingPlatform: platform net: net = () (
  public main: platform2 args: args = (
    (roundTrip: '127.0.0.1') ifFalse: [ ^ self fail: 'v4 loopback' ].
    (roundTrip: '::1')       ifFalse: [ ^ self fail: 'v6 loopback' ].
    'UDP OK' println.
    ^ 0
  )
  roundTrip: host = (
    | rx tx dg got |
    rx:: net udpBindPort: 54331.
    rx == nil ifTrue: [ ^ false ].
    tx:: net udp.
    tx sendTo: host port: 54331 data: 'ping'.
    got:: nil.
    [ got == nil ] whileTrue: [ got:: rx receive ].   (* loopback is ~instant *)
    dg:: got.
    rx close. tx close.
    ^ dg data = 'ping'
  )
  fail: msg = ( msg println. ^ 1 )
) : (
  public packageUsing: manifest = ( ^ self new )
)
```

- [ ] **Step 2: Run it on Path A to verify it fails**

Run: `OEVM_PATH=A python3 tools/run_tests_oevm.py 04_somns_basic/42_udp 2>&1 | tail -15`
Expected: FAIL — `net` undeclared/DNU (facade + grant not wired yet). (If the runner takes no single-test arg, run the full suite and look for `42_udp` FAIL.)

- [ ] **Step 3: Add `Net`/`Socket`/`Datagram` + `netOn:` to `ns-oevm/lib/kernel.ns`**

Locate the `Serial` class anchor `class Serial = ( | port | ) (` and add the three classes beside it (as Kernel-nested siblings, same level as `Serial`):

```newspeak
  class Net = ( | port | ) (
    (* Confined native capability: vends UDP Sockets over a link-time backend. *)
    setPort: aPort = ( port:: aPort. ^ self )
    public udp = ( ^ self udpBindPort: 0 )                       (* ephemeral port *)
    public udpBindPort: aPort = ( | h |
      h:: port basicBind: aPort.
      h < 0 ifTrue: [ ^ nil ].
      ^ Socket onPort: port handle: h )
  ) : (
    public onPort: aPort = ( ^ self new setPort: aPort )
  )

  class Socket = ( | port handle | ) (
    setPort: aPort handle: h = ( port:: aPort. handle:: h. ^ self )
    public sendTo: host port: aPort data: aString = (
      port basicSend: handle to: host port: aPort data: aString. ^ self )
    public receive = ( | t |
      t:: port basicReceive: handle.
      t == nil ifTrue: [ ^ nil ].
      ^ Datagram data: (t at: 1) senderHost: (t at: 2) senderPort: (t at: 3) )
    public close = ( port basicClose: handle. ^ self )
  ) : (
    public onPort: aPort handle: h = ( ^ self new setPort: aPort handle: h )
  )

  class Datagram = ( | data senderHost senderPort | ) (
    setData: d host: h port: p = ( data:: d. senderHost:: h. senderPort:: p. ^ self )
    public data = ( ^ data )
    public senderHost = ( ^ senderHost )
    public senderPort = ( ^ senderPort )
  ) : (
    public data: d senderHost: h senderPort: p = ( ^ self new setData: d host: h port: p )
  )
```

Then add the non-public minter beside `serialOn:` (anchor `serialOn: aPort = ( ^ Serial onPort: aPort )`):
```newspeak
  netOn: aPort = ( ^ Net onPort: aPort )
```
(NO `public` — keeps it confined, exactly like `serialOn:`.)

> If Task 1 Step 7 chose the §3(b) fallback (statics + `basicLastHost`/`basicLastPort`), rewrite `Socket>>receive` to read those instead of indexing a returned Array. Keep the `Datagram`-building shape.

- [ ] **Step 4: Thread the `net` cap on Path A in `ns-oevm/tools/ns_compile_oevm.py`**

(a) Locate the serial selector-name anchor `_serial_on_sel_name = "serialOn:" if "serial" in _params else None` and add below it:
```python
        _net_on_sel_name = "netOn:" if "net" in _params else None
```
and locate the `generate(...)` call passing `serial_on_sel_name=_serial_on_sel_name,` and add beside it:
```python
            net_on_sel_name=_net_on_sel_name,
```

(b) Locate the Path A boot threading anchor `if "serial" in params:` (the block that builds `cap_expr["serial"]`) and add a parallel block immediately after it:
```python
        if "net" in params:
            net_on_sym = compiler.solidify.symbols["netOn:"]
            c_code += "    extern oop udp_object(void);\n"
            c_code += "    oop net_port = udp_object();\n"
            c_code += "    oop net_args[1] = { net_port };\n"
            c_code += f"    oop net_inst = send(kernel_inst, (oop)&{net_on_sym}, 1, net_args);\n"
            cap_expr["net"] = "net_inst"
```
(The existing `argv = ", ".join(cap_expr.get(p, ...) for p in params)` then places `net_inst` in declared order automatically.)

- [ ] **Step 5: Run `42_udp` on Path A to verify it passes**

Run: `OEVM_PATH=A python3 tools/run_tests_oevm.py 2>&1 | grep -E "42_udp|pass|fail"`
Expected: `42_udp` PASS on Path A; Path A total **64 pass** (Path B will still fail `42_udp` — that's Task 3).

- [ ] **Step 6: Commit**

```bash
cd ../ns-oevm && git add lib/kernel.ns tools/ns_compile_oevm.py && git commit -m "feat(udp): Net/Socket/Datagram facade + Path A net-capability grant"
cd ../ns-tests && git add 04_somns_basic/42_udp.ns && git commit -m "test(udp): 42_udp capability round-trip (v4 + v6 loopback)"
```

---

## Task 3: Path B `.oimg` grant (format_version 6) → `42_udp` green on BOTH paths

**Files:**
- Modify: `ns-oevm/tools/oimg_serialize.py` (header word + version)
- Modify: `oevm/src/oimg_loader.c` (read + mint)

**Interfaces:**
- Consumes: `Kernel>>netOn:` (Task 2), `udp_object()` (Task 1).
- Produces: `.oimg` format_version 6 with `net_on_sel_ref` at header offset `A+12W`.

- [ ] **Step 1: Run `42_udp` on Path B to confirm it currently fails**

Run: `OEVM_PATH=B python3 tools/run_tests_oevm.py 2>&1 | grep -E "42_udp"`
Expected: `42_udp` FAIL on Path B (no net grant in the loader yet).

- [ ] **Step 2: Emit `net_on_sel_ref` in the serializer `ns-oevm/tools/oimg_serialize.py`**

(a) `header_bytes` signature (anchor `serial_on_sel_ref=0,`) — add a trailing param:
```python
                     supervisor_run_sel_ref=0, supervisor_nslots=0,
                     net_on_sel_ref=0):
```

(b) Version select (anchor `if supervisor_vtable_ref:\n            version = 5`) — add net as the new highest, FIRST:
```python
        if net_on_sel_ref:
            version = 6
        elif supervisor_vtable_ref:
            version = 5
```
(keep the existing `elif serial_on_sel_ref` / `elif kernel_vtable_ref` chain below.)

(c) Keep serial + supervisor slots present when net is set, so `net_on_sel_ref` lands at the fixed `A+12W`:
- serial slot (anchor `if serial_on_sel_ref or supervisor_vtable_ref:`) →
  ```python
        if serial_on_sel_ref or supervisor_vtable_ref or net_on_sel_ref:
  ```
- supervisor block (anchor `if supervisor_vtable_ref:` that emits the 4 supervisor words) →
  ```python
        if supervisor_vtable_ref or net_on_sel_ref:
  ```
  (emits zero-words for the supervisor fields when only net is present.)

(d) After the supervisor block's closing pad (anchor `h += self.u16(supervisor_nslots)` … `return h`), insert BEFORE `return h`:
```python
        # R4: UDP capability ref (format_version 6): net_on_sel_ref @a+12W, tail.
        if net_on_sel_ref:
            h += self.word(net_on_sel_ref)
            if len(h) % W:
                h += b"\x00" * (W - (len(h) % W))
        return h
```

(e) `generate()` signature (anchor `serial_on_sel_name=None,`) — add `net_on_sel_name=None,`.

(f) Sentinels for `header_len` (anchors `_serial_sentinel = ...` / `_supervisor_sentinel = ...`) — make lower slots present when net is, and add the net sentinel:
```python
        _serial_sentinel = 1 if (serial_on_sel_name or supervisor_vtable_name or net_on_sel_name) else 0
        _supervisor_sentinel = 1 if (supervisor_vtable_name or net_on_sel_name) else 0
        _net_sentinel = 1 if net_on_sel_name else 0
```
and the `header_len = ... header_bytes(...)` call gains `net_on_sel_ref=_net_sentinel,`.

(g) Ref resolution (anchor `serial_on_sel_ref = self._symbol_ref(serial_on_sel_name) if serial_on_sel_name else 0`) — add below:
```python
        net_on_sel_ref = self._symbol_ref(net_on_sel_name) if net_on_sel_name else 0
```
and the real `header = self.w.header_bytes(...)` call gains `net_on_sel_ref=net_on_sel_ref,`.

- [ ] **Step 3: Read + mint in the loader `oevm/src/oimg_loader.c`**

(a) Declare the static (anchor `static uintptr_t img_serial_on_sel_ref   = 0;`):
```c
/* R4: UDP capability (format_version 6+). 0 when the entry takes no net. */
static uintptr_t img_net_on_sel_ref      = 0;
```

(b) Read it (anchor `img_supervisor_nslots       = (format_version >= 5) ? read_u16 (base + a + 11 * W) : 0;`) — add below:
```c
    /* R4: UDP capability ref (format_version 6+) at the header tail. */
    img_net_on_sel_ref = (format_version >= 6) ? read_word(base + a + 12 * W) : 0;
```

(c) Update the `format_version < 3` reject comment is fine; no gate change needed (v6 ≥ 3).

(d) Mint the Net wrapper into the 2nd-cap slot (anchor the serial mint block
`if (img_init_argc >= 2 && img_serial_on_sel_ref != 0) { ... serial_inst = send(...); }`
and `oop init_args[3] = { kernel_inst, serial_inst, TAG_SMI(0) };`). Replace that
assembly so the 2nd cap is serial OR net (whichever the image declares):
```c
    oop second_cap = NULL;
    if (img_init_argc >= 2 && img_serial_on_sel_ref != 0) {
        extern oop serial_object(void);
        oop serial_args[1] = { serial_object() };
        second_cap = send(kernel_inst, oimg_resolve(img_serial_on_sel_ref), 1, serial_args);
    } else if (img_init_argc >= 2 && img_net_on_sel_ref != 0) {
        extern oop udp_object(void);
        oop net_args[1] = { udp_object() };
        second_cap = send(kernel_inst, oimg_resolve(img_net_on_sel_ref), 1, net_args);
    }
    oop init_args[3] = { kernel_inst, second_cap, TAG_SMI(0) };
    send(inst, oimg_resolve(img_init_sel_ref), (int)img_init_argc, init_args);
```
(Document inline: one 2nd-cap slot per entry — serial XOR net — matching the canonical-order assumption tracked by ns-oevm#2.)

- [ ] **Step 4: Run the full suite to verify `42_udp` passes on BOTH paths**

Run: `python3 tools/run_tests_oevm.py 2>&1 | tail -6`
Expected: **64 pass / 0 fail / 3 skip BOTH paths** (`42_udp` green on A and B; `43_udp_confined` is Task 4). Verify the `.oimg` header parity by confirming the existing serial test `38_serial` still passes (offsets undisturbed).

- [ ] **Step 5: Commit**

```bash
cd ../ns-oevm && git add tools/oimg_serialize.py && git commit -m "feat(udp): Path B .oimg net_on_sel_ref header word (format_version 6)"
cd ../oevm && git add src/oimg_loader.c && git commit -m "feat(udp): Path B loader reads net_on_sel_ref, mints Net wrapper"
```

---

## Task 4: Confinement pin + dialect #14 + regenerate default image → 65/0/3

**Files:**
- Create: `ns-tests/04_somns_basic/43_udp_confined.ns`
- Modify: `ns-oevm/docs/NEWSPEAK_DIALECT.md` (dialect #14)
- Modify: `oevm/src/solidified_image.c` (regenerated default image)

**Interfaces:** none new — closes the cycle.

- [ ] **Step 1: Write the confinement test `ns-tests/04_somns_basic/43_udp_confined.ns`**

```newspeak
(*
Confinement pin: a class declaring only usingPlatform: platform cannot reach net.
R4. Compile succeeds (net is an unknown unary send), but runtime aborts with DNU —
"Message 'net' not understood by receiver" — because the entry module instance has
no net: slot. Compiler exit 0, runner exit 134. Mirrors 39_serial_confined.ns.
expect-runtime-fail: Message 'net' not understood by receiver
*)
class UdpConfinedTest usingPlatform: platform = () (
  public main: platform2 args: args = (
    (net udp) sendTo: '127.0.0.1' port: 9 data: 'oops'.
    'SHOULD NOT REACH' println.
    ^ 0
  )
) : (
  public packageUsing: manifest = ( ^ self new )
)
```

- [ ] **Step 2: Run the suite to verify `43_udp_confined` passes (the DNU IS the pass)**

Run: `python3 tools/run_tests_oevm.py 2>&1 | grep -E "43_udp_confined|pass|fail"`
Expected: `43_udp_confined` PASS on both paths (expect-runtime-fail matched); suite **65 pass / 0 fail / 3 skip BOTH paths**.

- [ ] **Step 3: Add dialect #14 to `ns-oevm/docs/NEWSPEAK_DIALECT.md`**

Locate the dialect table (the row for #13 serial) and append:
```
| #14 | UDP native capability (`net`). Socket-vends-bound facade (`net udpBindPort:`/`udp` → `Socket`); non-blocking `receive` returns a `Datagram` (`data`/`senderHost`/`senderPort`) or nil when no datagram is queued (cf. #13 serial nil-at-empty). Addresses are String host + SMI port, address-family-agnostic (host backend is dual-stack IPv4+IPv6). Payloads are immutable String byte-containers (#10). | Confined native cap on the active node; real POSIX sockets on host, Zephyr/Thread (IPv6) backend additive at R6. Blocking/timeout receive, multicast, TCP, DNS DEFERRED (reversible; blocking needs fibers/R7). |
```
(Match the existing column format exactly.)

- [ ] **Step 4: Regenerate the committed default solidified image**

The committed `oevm/src/solidified_image.c` is the default-app image; regenerate it from `42_udp.ns` (or whatever the repo convention is — match how `38_serial` regenerated it; grep the runner / a `solidify` invocation). Run the regen command, then:

Run: `python3 tools/run_tests_oevm.py 2>&1 | tail -3`
Expected: still **65 pass / 0 fail / 3 skip BOTH paths** after regeneration.

- [ ] **Step 5: Commit**

```bash
cd ../ns-tests && git add 04_somns_basic/43_udp_confined.ns && git commit -m "test(udp): 43_udp_confined runtime-DNU confinement pin"
cd ../ns-oevm && git add docs/NEWSPEAK_DIALECT.md && git commit -m "docs(dialect): add #14 UDP native capability"
cd ../oevm && git add src/solidified_image.c && git commit -m "chore(udp): regenerate default solidified image"
```

---

## Self-Review

**Spec coverage:** §2 architecture → Task 1 (udp.c/seam/posix/hook). §3 facade → Task 2 (Net/Socket/Datagram). §4 protocol → Task 2 test 42. §5 grant/confinement/both-paths → Task 2 (Path A) + Task 3 (Path B) + Task 4 (43 confinement). §6 testing → test_udp.c (T1), 42_udp (T2), 43_udp_confined (T4). §7 dialect #14 → Task 4. §8 decomposition → file structure honors oevm#10. §9 verify-first probes → embedded as VERIFY-FIRST callouts in T1 Step 7 and T2 Step 3.

**Placeholder scan:** The only deliberately-open item is the `basicReceive:` Array-build mechanism (T1 Step 7) — flagged VERIFY-FIRST with a concrete fallback (§3(b) statics) and a matching note in T2 Step 3, per the spec's own §9 open item. The Array-ctor helper names (`indexable_new`/`kernel_array_vtable_oop`/`basic_at_put`) are explicitly marked "confirm by reading platform.c" — the executor resolves them at the code, as the codebase intends.

**Type consistency:** selectors are consistent A↔B↔facade: `basicBind:`/`basicSend:to:port:data:`/`basicReceive:`/`basicClose:` (C dispatch) ↔ `port basic...` sends (`.ns`). Facade method names match the test: `udp`/`udpBindPort:`/`sendTo:port:data:`/`receive`/`close`/`data`. `netOn:` consistent across kernel.ns / ns_compile_oevm.py / oimg_serialize.py / oimg_loader.c / solidified boot. format_version 6 + offset A+12W consistent across serializer (emit) and loader (read).
