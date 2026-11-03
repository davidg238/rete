# Webserver Channel-Device Spike (#1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove request/reply (RPC) over the run-to-completion channel model by building an HTTP server as a channel-native driver, and ship a thermal-display node whose page renders an 8×8 heat-grid — all host-testable, no hardware.

**Architecture:** The http server is a driver like gpio/pulse: C owns the datapath (TCP + HTTP envelope framing), records/values cross the seam, nsl owns coordination. The **request half** mirrors the pulse seam (external event → mailbox → `pumpEvents` safe point → `space_post 'dev/http/request'`). The **reply half** is the NetBridge egress idiom (an nsl `whenever:` reaction forwards `dev/http/reply` records to the `platform httpRespondId:status:body:` send-prim). A per-request **id** (minted by C) correlates reply→socket via a bounded in-flight table.

**Tech Stack:** C99 (VM core + posix backend), Newspeak-dialect "nsl" (node + reactions), the live Path-B (`.oimg`) test harness at `~/workspace/nsl-tuvm/tools/run_tests_oevm.py`, `curl` for the transport integration test.

## Global Constraints

- **Prim id numbering:** append-only; a new `PRIM_pf_*` id is arrived at by **counting `PRIM_NAMES` entries** in `src/dispatch.c` (its 1-based position), **never** `lastId + 1` — 6 predicate slots + on:do:/ensure sit in the enum without a `prim_ids.h` mirror. Enum, `PRIM_NAMES[]`, and `prim_ids.h` stay index-parallel, all appended last.
- **`builtins.def` is NOT touched; `BUILTIN_REGISTRY_VERSION` does NOT bump** — platform selectors resolve by name via `PRIM_NAMES`.
- **`src/space.c` stays pure storage — add no policy there.**
- **C never holds a raw oop across a thread boundary.** The accept thread copies request *bytes* into the ring; VM objects are built only in `http_pump` (reactor thread, at the `pumpEvents` safe point).
- **Reliability:** G2 (bounded in-flight table, (N+1)th → 503), G3 (request buffers in a C arena, never the VM heap), G9 (act only on inbound requests).
- **Host test path:** Path B only (`.oimg`), no per-test gcc rebuild (project standing rule). New nsl tests live under `~/workspace/nsl-tests/14_http/`.
- **Capacities:** `OEVM_HTTP_INFLIGHT` = 4, method buffer 8 B, path buffer 64 B, response buffer 4096 B.
- **HTTP scope:** HTTP/1.0, `Connection: close`, GET only, request-line parse only, headers discarded to the blank line.
- **Frame contract (`dev/thermal0/sample`):** a **cell** holding an `Array` of 64 SMIs, row-major (`at: (row*8 + col + 1)`, 1-based), centi-°C (0.01 °C; 21.4 °C → `2140`).

Design record: `docs/design/2026-07-18-webserver-channel-device-spike-design.md`. Series: `docs/design/2026-07-18-coordination-model-spike-series-design.md`.

---

## File Structure

- `src/http_backend.h` — **new.** Pure backend seam (no VM headers): request ring, bounded in-flight table, HTTP framing, inject/accept, take, respond, listen. One impl per `OEVM_TARGET`.
- `src/backend/posix/http_posix.c` — **new.** Pure-C posix implementation of the seam. Tasks 1, 2, 6.
- `src/http.c` + `src/http.h` — **new.** VM glue: `http_pump()` drains the backend ring, builds `[id,method,path]` Arrays (mirrors `src/udp.c`'s Array/String construction), `space_post`s them. Task 3.
- `src/prim_ids.h`, `src/dispatch.c`, `src/platform.c` — **modify.** Three new platform prims (`httpListen:`, `httpRespondId:status:body:`, `httpInject:path:`) + `http_pump()` hooked into `PRIM_pf_pumpEvents`. Task 3.
- `tests/test_http.c` — **new.** C unit test for framing + ring + in-flight shed. Tasks 1, 2.
- `tools/run_c_tests.sh` — **modify.** Register `test_http`. Task 1.
- `~/workspace/nsl-tuvm/tools/run_tests_oevm.py`, `bench.sh` — **modify.** Add `src/http.c` + `src/backend/$TARGET/http_$TARGET.c` to the link lists. Task 3.
- `~/workspace/tuvm/zephyr/HttpNode.nsl` — **new.** The node: app reaction (route + render) + egress reaction (forward reply). Tasks 3, 4.
- `~/workspace/nsl-tests/14_http/0{1,2,3}_*.nsl` — **new.** Model-level conformance tests (stdout assertions). Tasks 3, 4, 5.
- `tests/http_roundtrip_host.sh` — **new.** curl transport integration test. Task 6.
- `~/workspace/tuvm/review/model-capability-findings.md` — **new.** The spike verdict. Task 7.

**Out of scope for this plan (deferred follow-on):** `src/backend/zephyr/http_zephyr.c` + ESP32/WiFi bring-up — the last mile after the model verdict.

---

### Task 1: HTTP envelope framing (pure C)

**Files:**
- Create: `src/http_backend.h`
- Create: `src/backend/posix/http_posix.c`
- Test: `tests/test_http.c`
- Modify: `tools/run_c_tests.sh`

**Interfaces:**
- Produces: `int http_frame_response(int status, const char *body, int body_len, char *out, int out_cap)` — writes the HTTP/1.0 response into `out`, returns bytes written (or `-1` if it would overflow `out_cap`). Reason phrase derived from `status` (200 OK, 404 Not Found, 405 Method Not Allowed, 500 Internal Server Error, 503 Service Unavailable; any other → "OK").

- [ ] **Step 1: Write the failing test**

Create `tests/test_http.c`:
```c
#include "http_backend.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static void test_frame_200(void) {
    char buf[512];
    int n = http_frame_response(200, "hello", 5, buf, sizeof buf);
    assert(n > 0);
    const char *expect =
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: 5\r\n"
        "Connection: close\r\n"
        "\r\n"
        "hello";
    assert(strcmp(buf, expect) == 0);
}

static void test_frame_404(void) {
    char buf[512];
    int n = http_frame_response(404, "nope", 4, buf, sizeof buf);
    assert(n > 0);
    assert(strstr(buf, "HTTP/1.0 404 Not Found\r\n") == buf);
    assert(strstr(buf, "Content-Length: 4\r\n") != NULL);
}

static void test_frame_503(void) {
    char buf[512];
    int n = http_frame_response(503, "", 0, buf, sizeof buf);
    assert(n > 0);
    assert(strstr(buf, "HTTP/1.0 503 Service Unavailable\r\n") == buf);
    assert(strstr(buf, "Content-Length: 0\r\n") != NULL);
}

static void test_frame_overflow(void) {
    char buf[16];
    int n = http_frame_response(200, "a really long body that will not fit", 36, buf, sizeof buf);
    assert(n == -1);
}

int main(void) {
    test_frame_200();
    test_frame_404();
    test_frame_503();
    test_frame_overflow();
    printf("test_http OK\n");
    return 0;
}
```

- [ ] **Step 2: Create the header and a minimal impl so it compiles but fails**

Create `src/http_backend.h`:
```c
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
```

Create `src/backend/posix/http_posix.c`:
```c
/* Posix (host) impl of the HTTP driver seam. Pure C, no VM deps — unit-testable
   in isolation. The accept thread + sockets arrive in Task 6; Tasks 1-2 are the
   pure ring / in-flight / framing core. */
#include "http_backend.h"
#include <stdio.h>
#include <string.h>

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
```

- [ ] **Step 3: Register the test in `tools/run_c_tests.sh`**

Find the backend-smoke block (near the `run test_udp "src/backend/posix/udp_posix.c"` line) and add below it:
```bash
run test_http "src/backend/posix/http_posix.c"
```
(`run <name> <extra-src>` compiles `tests/<name>.c` + the extra source with `-Isrc` and executes it — mirror the exact `run` helper already in the file.)

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd ~/workspace/tuvm && bash tools/run_c_tests.sh test_http`
Expected: `test_http OK` (the framing asserts pass).

- [ ] **Step 5: Commit**

```bash
git checkout -b spike1-webserver
git add src/http_backend.h src/backend/posix/http_posix.c tests/test_http.c tools/run_c_tests.sh
git commit -m "feat(http): HTTP/1.0 response framing (pure C)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Request ring + bounded in-flight table (pure C)

**Files:**
- Modify: `src/http_backend.h`
- Modify: `src/backend/posix/http_posix.c`
- Test: `tests/test_http.c`

**Interfaces:**
- Consumes: `http_frame_response` (Task 1).
- Produces:
  - `int http_accept_request(int fd, const char *method, int ml, const char *path, int pl)` — core admission. If the in-flight table is full: frames a 503, emits it (to `fd`, or to stdout when `fd == -1`), and returns `-1` (shed, G2). Else: mints an id, records `{id, fd}` in the in-flight table, pushes `{id, method, path}` to the ring, returns the id.
  - `int http_take_request(int *id, char *method, int mcap, char *path, int pcap)` — pops the oldest ring entry into caller buffers (NUL-terminated). Returns `1` if one was taken, `0` if the ring is empty.
  - `void http_respond(int id, int status, const char *body, int body_len)` — looks up the in-flight slot by `id`, frames the response, emits it (to the slot's `fd`, or stdout when `fd == -1`), frees the slot. No-op if `id` is unknown.
  - `int http_inflight_count(void)` — number of occupied slots.
  - `void http_backend_reset(void)` — test helper: clear ring + table.

- [ ] **Step 1: Write the failing test** — append to `tests/test_http.c` (and call from `main`):
```c
static void test_admit_take_respond(void) {
    http_backend_reset();
    int id = http_accept_request(-1, "GET", 3, "/hello", 6);
    assert(id >= 0);
    assert(http_inflight_count() == 1);

    int tid; char m[8]; char p[64];
    assert(http_take_request(&tid, m, sizeof m, p, sizeof p) == 1);
    assert(tid == id);
    assert(strcmp(m, "GET") == 0);
    assert(strcmp(p, "/hello") == 0);
    assert(http_take_request(&tid, m, sizeof m, p, sizeof p) == 0); /* ring now empty */
    assert(http_inflight_count() == 1);                             /* still in flight */

    http_respond(id, 200, "hello", 5);                             /* frees the slot */
    assert(http_inflight_count() == 0);
}

static void test_backpressure_shed(void) {
    http_backend_reset();
    for (int i = 0; i < OEVM_HTTP_INFLIGHT; i++)
        assert(http_accept_request(-1, "GET", 3, "/x", 2) >= 0);
    assert(http_inflight_count() == OEVM_HTTP_INFLIGHT);
    /* the (N+1)th is shed with 503 */
    assert(http_accept_request(-1, "GET", 3, "/x", 2) == -1);
    assert(http_inflight_count() == OEVM_HTTP_INFLIGHT);
}
```
Add to `main`: `test_admit_take_respond(); test_backpressure_shed();`

- [ ] **Step 2: Run to verify it fails**

Run: `cd ~/workspace/tuvm && bash tools/run_c_tests.sh test_http`
Expected: FAIL — `http_accept_request` / `http_take_request` / `http_respond` / `http_inflight_count` / `http_backend_reset` undefined.

- [ ] **Step 3: Declare the new seam functions in `src/http_backend.h`**

Add before `#endif`:
```c
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
```

- [ ] **Step 4: Implement in `src/backend/posix/http_posix.c`**

Add after `http_frame_response`:
```c
#include <unistd.h>   /* write(), close() */

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
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cd ~/workspace/tuvm && bash tools/run_c_tests.sh test_http`
Expected: `test_http OK`.

- [ ] **Step 6: Commit**
```bash
git add src/http_backend.h src/backend/posix/http_posix.c tests/test_http.c
git commit -m "feat(http): request ring + bounded in-flight table with G2 shed

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Platform prims + pump hook + `/hello` model round-trip

**Files:**
- Create: `src/http.c`, `src/http.h`
- Modify: `src/prim_ids.h`, `src/dispatch.c`, `src/platform.c`
- Modify: `~/workspace/nsl-tuvm/tools/run_tests_oevm.py`, `bench.sh`
- Create: `~/workspace/tuvm/zephyr/HttpNode.nsl`
- Test: `~/workspace/nsl-tests/14_http/01_hello_roundtrip.nsl`

**Interfaces:**
- Consumes: `http_accept_request`, `http_take_request`, `http_respond` (Task 2).
- Produces:
  - C: `void http_pump(void)` (in `src/http.c`) — drains the backend ring, builds an `Array [id, method, path]` (1-based slots: 1=SMI id, 2=method String, 3=path String), `space_post`s it to `dev/http/request`.
  - nsl prims on `platform`: `httpInject: method path: p` → `http_accept_request(-1, …)`; `httpRespondId: id status: n body: s` → `http_respond`; `httpListen: port` → stub returning self for now (real in Task 6).

- [ ] **Step 1: nsl idiom verification probe (cheap insurance)**

Create `~/workspace/nsl-tests/14_http/00_idioms.nsl`:
```
(*
expect:
eq true
ne false
tag response
status 200
*)
class Idioms packageUsing: manifest = () (
  public main: platform args: args = (
    | r |
    ('eq ' , ('/hello' = '/hello') printString) out.
    ('ne ' , ('/hello' = '/nope') printString) out.
    r:: platform Record tag: #response with: #status -> 200 with: #body -> 'x'.
    ('tag ' , r tag printString) out.
    ('status ' , (r at: #status ifAbsent: [ 0 ]) printString) out.
  )
) : (
)
```
Run: `cd ~/workspace/tuvm && python3 ~/workspace/nsl-tuvm/tools/run_tests_oevm.py 14_http/00_idioms`
Expected: PASS. If `eq` is `false`, String `=` is identity here — then in every `path = 'X'` below, replace with `(path sameAs: 'X')` using the byteCompare helper in `review/lib/strings.ns`, and note it in the findings doc. If the Record lines fail, adjust to the exact `Record`/`at:ifAbsent:` selectors that pass. Do not proceed until this passes.

- [ ] **Step 2: Write the failing round-trip test**

Create `~/workspace/nsl-tests/14_http/01_hello_roundtrip.nsl`:
```
(*
expect:
HTTP/1.0 200 OK
Content-Type: text/html
Content-Length: 5
Connection: close

hello
finished
*)
class HttpHello packageUsing: manifest = () (
  public main: platform args: args = (
    | space req reply |
    space:: platform space.
    req::   space channel: 'dev/http/request'.
    reply:: space channel: 'dev/http/reply'.
    req whenever: '' do: [ :r |
      | id path status body |
      id::   r basicAt: 1.
      path:: r basicAt: 3.
      (path = '/hello')
        ifTrue:  [ status:: 200. body:: 'hello' ]
        ifFalse: [ status:: 404. body:: 'not found' ].
      reply out: '' value: (platform Record tag: #response
                              with: #id -> id with: #status -> status with: #body -> body) ].
    reply whenever: '' do: [ :resp |
      platform httpRespondId: (resp at: #id ifAbsent: [ 0 ])
               status:        (resp at: #status ifAbsent: [ 500 ])
               body:          (resp at: #body ifAbsent: [ '' ]) ].
    platform httpInject: 'GET' path: '/hello'.
    platform pumpEvents.
    space drain.
    'finished' out.
  )
) : (
)
```
Run: `cd ~/workspace/tuvm && python3 ~/workspace/nsl-tuvm/tools/run_tests_oevm.py 14_http/01_hello_roundtrip`
Expected: FAIL — `httpInject:path:` / `httpRespondId:status:body:` unknown, and the http request never posts.

- [ ] **Step 3: Add the three prims (enum + names + mirror)**

`src/dispatch.c` — append to the `enum` (immediately before its closing `};` near line 731):
```c
    PRIM_pf_httpListen,
    PRIM_pf_httpRespond,
    PRIM_pf_httpInject,
```
`src/dispatch.c` — append to `PRIM_NAMES[]` (before its closing `};` near line 779), in the SAME order:
```c
    "httpListen:", "httpRespondId:status:body:", "httpInject:path:",
```
`src/prim_ids.h` — **count the `PRIM_NAMES` entries** to get the 1-based positions of the three new strings, then append (the file's last id today is `PRIM_pf_sysStats 109`; confirm by counting, do not assume 110/111/112):
```c
#define PRIM_pf_httpListen  110   /* verify by counting PRIM_NAMES */
#define PRIM_pf_httpRespond 111
#define PRIM_pf_httpInject  112
```

- [ ] **Step 4: Create the VM glue `src/http.c` + `src/http.h`**

`src/http.h`:
```c
#ifndef OEVM_HTTP_H
#define OEVM_HTTP_H
/* VM-side glue for the http driver: drain the backend ring at the pumpEvents
   safe point and post [id,method,path] Arrays to dev/http/request. */
void http_pump(void);
#endif
```

`src/http.c` (Array/String construction mirrors `src/udp.c` exactly):
```c
#include "http.h"
#include "http_backend.h"
#include "space.h"
#include "oimg.h"
#include <string.h>

extern oop StringVtable;
extern oop gc_allocate(oop vtable, size_t size);
extern oop indexable_new(intptr_t n);
extern void gc_temp_root_push(oop o);
extern void gc_temp_roots_clear(void);

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
```

- [ ] **Step 5: Add the prim handlers in `src/platform.c` + hook the pump**

Include the headers near the top of `src/platform.c` (with the other `#include`s):
```c
#include "http.h"
#include "http_backend.h"
```
In `platform_dispatch`'s `switch (pid)`, before `default: return 0;`, add:
```c
    case PRIM_pf_httpListen: {                 /* platform httpListen: port -> self (real in Task 6) */
        *out = platform_object();
        return 1;
    }
    case PRIM_pf_httpRespond: {                /* httpRespondId: id status: n body: s */
        int id = IS_SMI(args[0]) ? (int)UNTAG_SMI(args[0]) : 0;
        int st = IS_SMI(args[1]) ? (int)UNTAG_SMI(args[1]) : 500;
        oop body = args[2];
        if (body == NULL || IS_SMI(body) || StringVtable == NULL ||
            obj_vtable(body) != StringVtable) {
            fprintf(stderr, "VM Error: platform httpRespondId:status:body: expects a String body\n");
            abort();
        }
        struct String *b = (struct String *)body;
        http_respond(id, st, (const char *)b->bytes, (int)b->length);
        *out = platform_object();
        return 1;
    }
    case PRIM_pf_httpInject: {                 /* httpInject: method path: p  (test-seam, fd=-1) */
        oop m = args[0], p = args[1];
        if (m == NULL || IS_SMI(m) || obj_vtable(m) != StringVtable ||
            p == NULL || IS_SMI(p) || obj_vtable(p) != StringVtable) {
            fprintf(stderr, "VM Error: platform httpInject:path: expects String args\n");
            abort();
        }
        struct String *ms = (struct String *)m, *ps = (struct String *)p;
        http_accept_request(-1, (const char *)ms->bytes, (int)ms->length,
                                (const char *)ps->bytes, (int)ps->length);
        *out = platform_object();
        return 1;
    }
```
Extend the existing pump prim so the standard reactor loop also drains http. Change:
```c
    case PRIM_pf_pumpEvents:
        *out = TAG_SMI(pulse_pump());
        return 1;
```
to:
```c
    case PRIM_pf_pumpEvents: {
        int n = pulse_pump();
        http_pump();                            /* drain http request ring at the safe point */
        *out = TAG_SMI(n);
        return 1;
    }
```

- [ ] **Step 6: Wire `src/http.c` + the backend into the host build**

In `~/workspace/nsl-tuvm/tools/run_tests_oevm.py`, add to the `OEVM_BACKEND` list:
```python
                f"src/backend/{OEVM_TARGET}/http_{OEVM_TARGET}.c",
```
and add `"src/http.c"` to the fixed VM source list used in both the Path-B image-VM build (`_build_pathb_once`) and the Path-A build — wherever `src/udp.c` appears, add `src/http.c` beside it.

In `~/workspace/tuvm/bench.sh` line 9, add `src/http.c "src/backend/$TARGET/http_$TARGET.c"` beside the `src/udp.c "src/backend/$TARGET/udp_$TARGET.c"` pair.

- [ ] **Step 7: Create the node `zephyr/HttpNode.nsl`**

Same body as the test's `HttpHello` class (Step 2), but named `HttpNode`, and with the drive loop bounded for a live run — for now it mirrors the test (single inject). This file is the durable node; the test is its conformance twin. Copy the Step-2 class body verbatim, renaming the class to `HttpNode`.

- [ ] **Step 8: Run the round-trip test to verify it passes**

Run: `cd ~/workspace/tuvm && python3 ~/workspace/nsl-tuvm/tools/run_tests_oevm.py 14_http/01_hello_roundtrip`
Expected: PASS — stdout is the framed `200 OK` response with body `hello`, then `finished`.

- [ ] **Step 9: Full suite regression + commit**

Run: `cd ~/workspace/tuvm && python3 ~/workspace/nsl-tuvm/tools/run_tests_oevm.py`
Expected: the prior floor holds (no regressions) and `14_http/00,01` pass.
```bash
git add src/http.c src/http.h src/prim_ids.h src/dispatch.c src/platform.c \
        zephyr/HttpNode.nsl ~/workspace/nsl-tests/14_http/00_idioms.nsl \
        ~/workspace/nsl-tests/14_http/01_hello_roundtrip.nsl bench.sh
git commit -m "feat(http): platform prims + pump hook; /hello request-reply round-trip over channels

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
# nsl-tuvm harness change is a separate repo:
git -C ~/workspace/nsl-tuvm add tools/run_tests_oevm.py
git -C ~/workspace/nsl-tuvm commit -m "test(http): link src/http.c + http backend into the Path-B image VM

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: `/thermal` render — reduce + SVG heat-grid

**Files:**
- Modify: `~/workspace/tuvm/zephyr/HttpNode.nsl`
- Test: `~/workspace/nsl-tests/14_http/02_thermal_render.nsl`

**Interfaces:**
- Consumes: the request/reply loop + prims from Task 3.
- Produces: routes `/thermal` (render the `dev/thermal0/sample` cell) and `/nope` (404); a nsl `renderFrame:` helper on the node.

- [ ] **Step 1: Array idiom probe** — extend `00_idioms.nsl` `expect:` and body to confirm Array build/read selectors:
```
    | frame |
    frame:: platform basicNew: Array indexable: 4.
    frame at: 1 put: 2140.
    ('cell ' , (frame basicAt: 1) printString) out.
```
add `cell 2140` to `expect:`. Run `…run_tests_oevm.py 14_http/00_idioms`. If `at:put:` or `basicAt:` is wrong, substitute the selectors that pass (check `review/lib/` and existing nodes) and record it. Do not proceed until green.

- [ ] **Step 2: Write the failing thermal test**

Create `~/workspace/nsl-tests/14_http/02_thermal_render.nsl`. Seed a 64-SMI dummy frame with a single hot pixel at row 3,col 5 (index `3*8+5+1 = 30`) = `3120` (31.2 °C), the rest `2200` (22.0 °C). Assert the response is `200`, contains one `rgb(` fill and the caption:
```
(*
expect-contains:
HTTP/1.0 200 OK
<svg viewBox='0 0 8 8'
rgb(
max 31.2C  min 22.0C  hot (3,5)
notfound
*)
```
(If the harness has no `expect-contains:`, assert exact stdout: build the full expected SVG string, or assert on a handful of `out`-printed summary lines the node emits — e.g. the node also does `('served ' , path , ' ' , status printString) out`. Prefer a summary-line assertion to avoid pinning all 64 rects.)

The node drives two injects: `platform httpInject: 'GET' path: '/thermal'.` then `platform httpInject: 'GET' path: '/nope'.`, pumping+draining after each, and prints a summary line per served request.

- [ ] **Step 3: Implement `/thermal` routing + render in `HttpNode.nsl`**

Seed the cell once in `main:` before the loop:
```
    | thermal frame i |
    thermal:: space channel: 'dev/thermal0/sample'.
    frame:: platform basicNew: Array indexable: 64.
    i:: 1. [ i <= 64 ] whileTrue: [ frame at: i put: 2200. i:: i + 1 ].
    frame at: 30 put: 3120.                         (* hot pixel row 3 col 5 *)
    thermal put: '' value: frame.
```
Route in the app reaction:
```
      (path = '/hello') ifTrue: [ status:: 200. body:: 'hello' ] ifFalse: [
      (path = '/thermal') ifTrue: [ status:: 200. body:: self renderFrame: (thermal peek: '' ifAbsent: [ nil ]) ]
                          ifFalse: [ status:: 404. body:: 'notfound' ] ].
```
Add the render helper (folds reduce + SVG, all `printString` + `,`; `,` is String↔String so stringify every number):
```
  renderFrame: f = (
    | row col svg maxV minV hotR hotC t k |
    (f == nil) ifTrue: [ ^ '<html><body>no frame</body></html>' ].
    maxV:: f basicAt: 1. minV:: f basicAt: 1. hotR:: 0. hotC:: 0.
    row:: 0. [ row <= 7 ] whileTrue: [
      col:: 0. [ col <= 7 ] whileTrue: [
        t:: f basicAt: (row * 8 + col + 1).
        (t > maxV) ifTrue: [ maxV:: t. hotR:: row. hotC:: col ].
        (t < minV) ifTrue: [ minV:: t ].
        col:: col + 1 ]. row:: row + 1 ].
    svg:: '<html><body><svg viewBox=''0 0 8 8'' width=''320'' height=''320'' shape-rendering=''crispEdges''>'.
    row:: 0. [ row <= 7 ] whileTrue: [
      col:: 0. [ col <= 7 ] whileTrue: [
        t:: f basicAt: (row * 8 + col + 1).
        k:: ((t - 1500) * 255) / 2000.
        (k < 0) ifTrue: [ k:: 0 ]. (k > 255) ifTrue: [ k:: 255 ].
        svg:: svg , '<rect x=''' , col printString , ''' y=''' , row printString ,
              ''' width=''1'' height=''1'' fill=''rgb(' , k printString , ',0,' ,
              (255 - k) printString , ')''/>'.
        col:: col + 1 ]. row:: row + 1 ].
    ^ svg , '</svg><p>max ' , (self centi: maxV) , 'C  min ' , (self centi: minV) ,
      'C  hot (' , hotR printString , ',' , hotC printString , ')</p></body></html>'
  )
  centi: v = ( ^ (v / 100) printString , '.' , ((v \\ 100) / 10) printString )
```
(`\\` is integer modulo; `centi:` renders `3120` → `31.2`. If `\\` is spelled differently in this dialect, use the modulo selector that the `00_idioms` probe confirms.)

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd ~/workspace/tuvm && python3 ~/workspace/nsl-tuvm/tools/run_tests_oevm.py 14_http/02_thermal_render`
Expected: PASS — `/thermal` → 200 with the SVG + caption `max 31.2C  min 22.0C  hot (3,5)`; `/nope` → 404 `notfound`.

- [ ] **Step 5: Commit**
```bash
git add zephyr/HttpNode.nsl ~/workspace/nsl-tests/14_http/00_idioms.nsl \
        ~/workspace/nsl-tests/14_http/02_thermal_render.nsl
git commit -m "feat(http): /thermal route renders 8x8 heat-grid SVG with on-node reduce

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: G2 back-pressure at the model level (503)

**Files:**
- Test: `~/workspace/nsl-tests/14_http/03_backpressure.nsl`

**Interfaces:**
- Consumes: the loop + prims from Task 3 (the C shed is already covered by Task 2's `test_backpressure_shed`; this asserts it is observable through the nsl seam).

- [ ] **Step 1: Write the failing/asserting test**

Create `~/workspace/nsl-tests/14_http/03_backpressure.nsl`. Register the app + egress reactions, but do **not** pump/drain between injects — inject `OEVM_HTTP_INFLIGHT + 1` (5) requests back-to-back so the table fills and the 5th is shed with a 503 straight to stdout (no id, no reply channel). Then pump+drain to flush the 4 admitted. Assert the stdout contains exactly one `503 Service Unavailable` and four `200 OK`:
```
(*
expect-contains:
HTTP/1.0 503 Service Unavailable
*)
```
Drive:
```
    i:: 1. [ i <= 5 ] whileTrue: [ platform httpInject: 'GET' path: '/hello'. i:: i + 1 ].
    platform pumpEvents. space drain.
    'finished' out.
```
(If the harness lacks `expect-contains:`, print a per-response marker: have the node’s egress reaction print `('served ' , (resp at: #status ifAbsent: [0]) printString) out`, and the 503 path prints from C — assert the counts via exact stdout, or grep in a wrapper. Keep the shed assertion the point of the test.)

- [ ] **Step 2: Run to verify it passes**

Run: `cd ~/workspace/tuvm && python3 ~/workspace/nsl-tuvm/tools/run_tests_oevm.py 14_http/03_backpressure`
Expected: PASS — one 503 emitted at the moment the 5th inject overflows the in-flight table; the 4 admitted requests still serve 200.

- [ ] **Step 3: Commit**
```bash
git add ~/workspace/nsl-tests/14_http/03_backpressure.nsl
git commit -m "test(http): G2 back-pressure — (N+1)th concurrent request sheds 503

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 6: Real posix sockets — accept thread + `httpListen:` + curl integration

**Files:**
- Modify: `src/http_backend.h`, `src/backend/posix/http_posix.c`, `src/platform.c`
- Modify: build glue (add `-lpthread` to host link lines)
- Test: `tests/http_roundtrip_host.sh`

**Interfaces:**
- Consumes: `http_accept_request` (called now with a real `fd`), `http_respond` (writes to that `fd`).
- Produces: `int http_listen(int port)` — bind a TCP listener on `port`, start the accept thread; returns 0 or -1. The accept thread: `accept()`, read the request line, parse `METHOD SP PATH SP`, discard headers, call `http_accept_request(fd, method, ml, path, pl)` under the ring mutex.

- [ ] **Step 1: Write the failing integration test**

Create `tests/http_roundtrip_host.sh` (model on `tests/hanoi_ab_host.sh`): build `/tmp/oevm_img` (the Path-B image VM, now including `src/http.c` + `http_posix.c` + `-lpthread`), bake a `HttpServerNode.oimg` that calls `platform httpListen: 8080` then loops `[ true ] whileTrue: [ platform pumpEvents. space drain ]`, run it in the background, then:
```bash
set -e
sleep 0.5
code=$(curl -s -o /tmp/http_body -w '%{http_code}' http://127.0.0.1:8080/thermal)
test "$code" = "200"
grep -q "rgb(" /tmp/http_body
test "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:8080/nope)" = "404"
# concurrent burst: 8 parallel; at least one 503 once >4 are in flight
for i in $(seq 1 8); do curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8080/thermal & done | sort | uniq -c
echo "http_roundtrip OK"
kill %1 2>/dev/null || true
```
Run: `bash tests/http_roundtrip_host.sh`
Expected: FAIL — `httpListen:` is a stub; nothing listens on 8080.

- [ ] **Step 2: Add the listener + accept thread to `http_posix.c`**

Add at the top: `#define _POSIX_C_SOURCE 200809L`, then `#include <pthread.h>`, `<sys/socket.h>`, `<netinet/in.h>`, `<arpa/inet.h>`, `<fcntl.h>`. Guard the ring/in-flight table with a `static pthread_mutex_t http_mu = PTHREAD_MUTEX_INITIALIZER;` — lock it inside `http_accept_request`, `http_take_request`, `http_respond`. Add:
```c
static int listen_fd = -1;

static void *accept_loop(void *arg) {
    (void)arg;
    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) continue;
        char buf[1024];
        int n = (int)read(fd, buf, sizeof buf - 1);
        if (n <= 0) { close(fd); continue; }
        buf[n] = '\0';
        /* parse "METHOD SP PATH SP..." from the request line */
        char method[16] = {0}, path[64] = {0};
        if (sscanf(buf, "%15s %63s", method, path) != 2) { close(fd); continue; }
        http_accept_request(fd, method, (int)strlen(method), path, (int)strlen(path));
    }
    return NULL;
}

int http_listen(int port) {
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return -1;
    int one = 1; setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)port);
    if (bind(listen_fd, (struct sockaddr *)&a, sizeof a) < 0) { close(listen_fd); listen_fd = -1; return -1; }
    if (listen(listen_fd, 8) < 0) { close(listen_fd); listen_fd = -1; return -1; }
    pthread_t th; pthread_create(&th, NULL, accept_loop, NULL); pthread_detach(th);
    return 0;
}
```
Declare `int http_listen(int port);` in `src/http_backend.h`. Wrap the bodies of `http_accept_request`, `http_take_request`, `http_respond` in `pthread_mutex_lock(&http_mu); … pthread_mutex_unlock(&http_mu);` (the `emit()` call in `http_respond`/shed does socket I/O — do the framing under the lock but the `write()`+`close()` can stay inside; it is bounded and simplest correct).

- [ ] **Step 3: Make `httpListen:` real in `src/platform.c`**

Replace the `PRIM_pf_httpListen` stub body:
```c
    case PRIM_pf_httpListen: {
        int port = IS_SMI(args[0]) ? (int)UNTAG_SMI(args[0]) : 0;
        http_listen(port);
        *out = platform_object();
        return 1;
    }
```

- [ ] **Step 4: Add `-lpthread` to the host link lines**

In `~/workspace/nsl-tuvm/tools/run_tests_oevm.py` (both build sites) and `bench.sh`, append `-lpthread` to the gcc link flags (beside `-lm`). In `tools/run_c_tests.sh`, the `test_http` entry now also needs `-lpthread` — add it to that run line.

- [ ] **Step 5: Run the integration test to verify it passes**

Run: `cd ~/workspace/tuvm && bash tests/http_roundtrip_host.sh`
Expected: `http_roundtrip OK` — `/thermal` returns 200 with an SVG containing `rgb(`, `/nope` returns 404, and the 8-way burst shows a mix of 200 and at least one 503. Also re-run the C unit test: `bash tools/run_c_tests.sh test_http` → `test_http OK`.

- [ ] **Step 6: Commit**
```bash
git add src/http_backend.h src/backend/posix/http_posix.c src/platform.c \
        tests/http_roundtrip_host.sh tools/run_c_tests.sh bench.sh
git commit -m "feat(http): posix TCP accept thread + httpListen:; curl round-trip integration test

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
git -C ~/workspace/nsl-tuvm add tools/run_tests_oevm.py
git -C ~/workspace/nsl-tuvm commit -m "test(http): -lpthread for the http accept thread

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 7: Write the spike-1 findings entry

**Files:**
- Create: `~/workspace/tuvm/review/model-capability-findings.md`

**Interfaces:**
- Consumes: the results of Tasks 3–6.

- [ ] **Step 1: Write the findings entry**

Create `review/model-capability-findings.md` with a header describing the series (link both design docs) and the **spike #1** entry filled from what actually happened, using the series rubric — (a) expressible? (b) the idiom (paste the `whenever:` app + egress reactions and the `httpRespondId:` send-prim shape), (c) **verdict: clean / needs-a-helper / corner** for each half, (d) any helper needed, (e) rough cost. State plainly whether request/reply decomposed into pulse-ingress + NetBridge-egress + a correlation id with no channel reification, and note any nsl-idiom surprises the `00_idioms` probe turned up (String `=`, Array `at:put:`, modulo). If a half was not **clean**, say so and why.

- [ ] **Step 2: Update the ladder pointer**

In `~/workspace/tuvm/review/program-sequence.md`, add a one-line note under the Addendum that spike #1 (webserver-channel-device) landed and where the findings live. Do not restructure the ladder.

- [ ] **Step 3: Commit**
```bash
git add review/model-capability-findings.md review/program-sequence.md
git commit -m "docs(review): spike #1 model-capability findings — request/reply over channels

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review

**Spec coverage:** request half (Task 3), reply/egress half (Task 3), correlation id + bounded in-flight/G2 (Tasks 2, 5), frame contract `dev/thermal0/sample` (Task 4), SVG render + reduce (Task 4), HTTP scope (Tasks 1, 6), `httpInject` test-seam + stdout-vs-socket branch (Tasks 2, 3, 6), real sockets/browser (Task 6), findings entry (Task 7), host-only/Path-B discipline (throughout). ESP32/WiFi explicitly deferred. Covered.

**Type consistency:** `http_frame_response`, `http_accept_request`, `http_take_request`, `http_respond`, `http_inflight_count`, `http_backend_reset`, `http_listen`, `http_pump` — signatures identical across the header, impl, tests, and callers. Prims `httpListen:` / `httpRespondId:status:body:` / `httpInject:path:` spelled identically in `PRIM_NAMES`, the nsl tests, and the node. `PRIM_pf_httpListen/Respond/Inject` appear in the same order in the enum, names table, and `prim_ids.h`.

**Known idiom risks flagged with probes (not placeholders):** String `=` value-vs-identity, Array `at:put:`/`basicAt:`, integer modulo `\\` — each has a `00_idioms` verification step with a concrete fallback before it is relied on.
