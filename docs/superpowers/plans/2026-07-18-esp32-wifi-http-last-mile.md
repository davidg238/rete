# ESP32/WiFi HTTP Last Mile (#46) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Serve the frozen channel-native HTTP node (`/`, `/thermal`, `/stats`, `/hello`) from a real ESP32-D0WDQ6 / WROOM-32 over WiFi, viewable in a browser at `http://<dhcp-ip>:18080/thermal`.

**Architecture:** Factor the transport-agnostic HTTP logic (framing, request ring, bounded in-flight table + G2 shed) out of the posix backend into a shared `src/http_core.c`, leaving only three per-target transport hooks (`http_listen`, `http_poll`, `http_emit`). Unify both posix and device on the **non-blocking poll** model (mirroring `udp_zephyr.c`'s `DONTWAIT` idiom): `http_poll()` accepts+reads one connection each `pumpEvents` tick on the single VM thread, so the pthread accept-thread and its ring mutex disappear. Add a Zephyr `zsock_*` transport (`http_zephyr.c`), a WiFi+TCP+DHCPv4 Kconfig, and a WiFi-associate boot hook, then flash and view.

**Tech Stack:** C99 (VM core + posix/zephyr backends), Zephyr/upstream-ESP32 (`~/zephyr-esp32` workspace, `west`), Newspeak-dialect "nsl" (frozen node), the live Path-B (`.oimg`) host harness at `~/workspace/nsl-tuvm/tools/run_tests_oevm.py`, `curl` + a browser for the WiFi transport check.

## Global Constraints

- **Node is FROZEN.** `zephyr/HttpServerNode.nsl` and all routing/reduce/render nsl is untouched — this is transport only. It binds `httpListen: 18080`; do not change the port (18080 is deliberately unprivileged and dodges host-side 8080 collisions — see `tests/http_roundtrip_host.sh:15`).
- **Single VM thread.** VM objects (oops) are built/touched ONLY on the reactor thread at the `pumpEvents` safe point (in `http_pump`, `src/http.c`). Transport hooks move raw bytes only; after the poll conversion there is no second thread anywhere in the http path.
- **Capacities (unchanged):** `OEVM_HTTP_INFLIGHT` = 4, method buffer 8 B (`HTTP_METHOD_MAX`), path buffer 64 B (`HTTP_PATH_MAX`). HTTP/1.0, `Connection: close`, GET only, request-line parse only, headers discarded.
- **Secrets:** WiFi SSID/PSK live ONLY in the git-ignored `zephyr/prj_esp32_wifi.local.conf` (already generated from `~/workspaceToit/nodus/docs/secrets.md`; already in `.gitignore`). Never paste the values into any tracked file, commit message, or test.
- **Host test discipline:** Path B only (`.oimg`), no per-test gcc rebuild (project standing rule, memory `no-path-a-suite`). Single conformance test via `nsl-tuvm/tools/run-one.py <file.nsl>`; the C unit via `tools/run_c_tests.sh`.
- **SDK reconciliation:** exact `CONFIG_WIFI_*` / `zsock_*` / `wifi_mgmt` symbol names are validated against the installed `~/zephyr-esp32` SDK at build time — the `west build`/`west flash` is the gate. Where a symbol differs, adjust to the SDK (same posture as `udp_zephyr.c`'s header note); do not invent.
- **Commit trailers** (every commit):
  ```
  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_015er6825Dm9pSqPucVEemNY
  ```
- **Promotion rule (CLAUDE.md):** the issue is not closed until durable knowledge is promoted to a mainline doc of record (`review/model-capability-findings.md`, `review/program-sequence.md`). Task 4 does this.

Design record: `docs/design/2026-07-18-esp32-wifi-http-last-mile-design.md`. Predecessor spike: `docs/superpowers/plans/2026-07-18-webserver-channel-device-spike.md`.

---

## File Structure

- `src/http_core.c` — **new.** Transport-agnostic core: `http_frame_response`, `http_frame_headers`, the request ring, the in-flight table, `next_id`, `http_accept_request`, `http_take_request`, `http_respond`, `http_inflight_count`, `http_backend_reset`. Emits via the `http_emit` hook. NO sockets, NO threads, NO mutex.
- `src/http_backend.h` — **modify.** Declare the two new per-target hooks (`http_poll`, `http_emit`); keep the existing seam declarations.
- `src/backend/posix/http_posix.c` — **modify (gut).** Now only the posix transport hooks: `http_listen` (bind + non-blocking listener, no thread), `http_poll` (non-blocking accept + bounded read + parse), `http_emit` (stdout for fd == -1; `write`+`close` for fd ≥ 0). All framing/ring/inflight moves to `http_core.c`.
- `src/backend/zephyr/http_zephyr.c` — **new.** Zephyr `zsock_*` transport hooks (`http_listen`, `http_poll`, `http_emit`), IPv4.
- `src/platform.c` — **modify.** Add `http_poll()` to `PRIM_pf_pumpEvents` (before `http_pump()`).
- `zephyr/src/main.c` — **modify.** `wifi_bringup()` (guarded on `CONFIG_WIFI`), called before the VM bootstrap.
- `zephyr/prj_esp32.conf` — **modify.** WiFi + TCP + DHCPv4/IPv4; drop the loopback `::1` block.
- `zephyr/Kconfig` — **new.** App Kconfig defining `TUVM_WIFI_SSID` / `TUVM_WIFI_PSK` (string).
- `zephyr/CMakeLists.txt` — **modify.** Link `src/http.c`, `src/http_core.c`, `src/backend/zephyr/http_zephyr.c` (none are linked on Zephyr today).
- Host build glue — **modify.** `tools/run_c_tests.sh`, `~/workspace/nsl-tuvm/tools/run_tests_oevm.py`, `bench.sh`: add `src/http_core.c` to the http link sets; drop the http-justified `-lpthread`.
- `review/model-capability-findings.md`, `review/program-sequence.md` — **modify.** On-device close-out + the cross-source scheduling design point (Task 4).

---

## Task 1: Unify HTTP on the poll model via a shared `src/http_core.c`

Extract the transport-agnostic core, reduce both future backends to three hooks, and convert posix from the accept-thread to non-blocking poll. Deliverable: the C unit test, the curl integration test, and the nsl conformance tests are all green on the new shape — with no pthread anywhere in the http path.

**Files:**
- Create: `src/http_core.c`
- Modify: `src/http_backend.h`, `src/backend/posix/http_posix.c`, `src/platform.c:763`
- Modify (build glue): `tools/run_c_tests.sh:78`, `tools/run_c_tests.sh:26`, `~/workspace/nsl-tuvm/tools/run_tests_oevm.py`, `bench.sh:9`
- Test (existing, must stay green): `tests/test_http.c`, `tests/http_roundtrip_host.sh`, `~/workspace/nsl-tests/14_http/0{1,2,3}_*.nsl`

**Interfaces:**
- Produces (in `src/http_backend.h`), unchanged signatures kept: `http_frame_response`, `http_accept_request`, `http_take_request`, `http_respond`, `http_inflight_count`, `http_backend_reset`, `http_listen`.
- Produces (new hooks):
  - `void http_poll(void)` — non-blocking: accept+read at most one pending connection, parse `METHOD SP PATH`, call `http_accept_request(fd, …)`. No-op if nothing is pending. Implemented per target.
  - `void http_emit(int fd, const char *hdr, int hlen, const char *body, int body_len)` — write an already-framed header block then the body to the transport for `fd` (`fd == -1` = the host "injected" sink). Implemented per target.
- Consumes: `http_emit` is called by `http_core.c`'s internal reply/shed path.

- [ ] **Step 1: Add the two hook declarations to `src/http_backend.h`**

Insert before `#endif`:
```c
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
```
Also update the stale `http_listen` comment (drop "start the accept thread"):
```c
/* Bind a TCP listener on port (non-blocking) so http_poll can accept on it.
   Returns 0 on success, -1 on any socket setup failure. */
int  http_listen(int port);
```

- [ ] **Step 2: Create `src/http_core.c` (move the transport-agnostic core verbatim, mutex removed)**

```c
/* Transport-agnostic HTTP core: HTTP/1.0 framing, the request ring, and the
   bounded in-flight table with G2 shed. Pure C — NO sockets, NO threads, NO VM
   headers. Shared by every OEVM_TARGET; the transport (listen/poll/emit) lives
   in src/backend/<target>/http_<target>.c. Single-threaded: http_poll,
   http_take_request (via http_pump) and http_respond all run on the one VM
   reactor thread at the pumpEvents safe point, so no lock is needed. */
#include "http_backend.h"
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
            core_emit(inflight[i].fd, status, body, body_len);
            inflight[i].live = 0;
            break;
        }
    }
}
```
Note: `http_frame_headers` is now a shared (non-static) helper so `http_emit` implementations could reuse it if desired; declare it in `http_backend.h` too:
```c
/* Frame just the HTTP/1.0 header block. Returns bytes written or -1 on overflow. */
int  http_frame_headers(int status, int body_len, char *out, int out_cap);
```

- [ ] **Step 3: Reduce `src/backend/posix/http_posix.c` to the three transport hooks (poll, no thread, no mutex)**

Replace the ENTIRE file with:
```c
/* Posix (host) HTTP transport hooks: a non-blocking TCP listener + poll +
   socket/stdout emit. All framing/ring/in-flight logic lives in src/http_core.c.
   Single-threaded: http_poll runs on the VM reactor thread inside pumpEvents
   (no accept thread, no mutex). fd == -1 is the injected/test sink -> stdout. */
#define _POSIX_C_SOURCE 200809L
#include "http_backend.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>       /* read(), write(), close() */
#include <fcntl.h>        /* fcntl O_NONBLOCK */
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>     /* struct timeval for SO_RCVTIMEO */
#include <netinet/in.h>
#include <arpa/inet.h>

#define HTTP_METHOD_MAX 8
#define HTTP_PATH_MAX   64

static int listen_fd = -1;

void http_emit(int fd, const char *hdr, int hlen, const char *body, int body_len) {
    if (fd == -1) {                          /* injected/test sink */
        fwrite(hdr, 1, (size_t)hlen, stdout);
        if (body_len > 0) fwrite(body, 1, (size_t)body_len, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        return;
    }
    (void)write(fd, hdr, (size_t)hlen);
    if (body_len > 0) (void)write(fd, body, (size_t)body_len);
    close(fd);
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
    fcntl(listen_fd, F_SETFL, O_NONBLOCK);   /* accept() returns EWOULDBLOCK when idle */
    return 0;
}

/* Accept+read at most one pending connection each tick. Non-blocking accept;
   a short SO_RCVTIMEO bounds the read so a connected-but-silent client can never
   wedge the single-threaded reactor. */
void http_poll(void) {
    if (listen_fd < 0) return;
    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) return;                      /* EWOULDBLOCK -> nothing pending */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };  /* 200 ms */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char buf[1024];
    int n = (int)read(fd, buf, sizeof buf - 1);
    if (n <= 0) { close(fd); return; }
    buf[n] = '\0';
    char method[HTTP_METHOD_MAX] = {0}, path[HTTP_PATH_MAX] = {0};
    if (sscanf(buf, "%7s %63s", method, path) != 2) { close(fd); return; }
    http_accept_request(fd, method, (int)strlen(method), path, (int)strlen(path));
}
```

- [ ] **Step 4: Add `http_poll()` to the pump in `src/platform.c`**

Change `PRIM_pf_pumpEvents` (currently at `src/platform.c:763`):
```c
    case PRIM_pf_pumpEvents: {
        int n = pulse_pump();
        http_pump();                            /* drain http request ring at the safe point */
        *out = TAG_SMI(n);
        return 1;
    }
```
to (add `http_poll()` FIRST — it feeds the ring that `http_pump` drains):
```c
    case PRIM_pf_pumpEvents: {
        http_poll();                            /* accept/read pending sockets -> request ring */
        int n = pulse_pump();
        http_pump();                            /* drain http request ring at the safe point */
        *out = TAG_SMI(n);
        return 1;
    }
```

- [ ] **Step 5: Update the host build glue — add `http_core.c`, drop the http `-lpthread`**

`tools/run_c_tests.sh` line 26 (the shared link set that lists `src/backend/posix/http_posix.c`): add `src/http_core.c` to it. Line 78 — change:
```bash
run test_http   "src/backend/posix/http_posix.c" -lpthread
```
to:
```bash
run test_http   "src/http_core.c src/backend/posix/http_posix.c"
```
(Leave the unrelated `-lpthread` on line 97's `test_gc_mark_iterative` alone.)

`~/workspace/nsl-tuvm/tools/run_tests_oevm.py`: in every fixed VM source list where `"src/http.c"` appears (grep shows lines ~93, 124, 174, 433), add `"src/http_core.c"` immediately after it. Remove the two http-justified `-lpthread` entries (lines ~100 and ~439, each commented `# http accept thread`).

`bench.sh` line 9: after `src/http.c "src/backend/$TARGET/http_$TARGET.c"` add `src/http_core.c`; remove the trailing `-lpthread`.

- [ ] **Step 6: Run the C unit test — framing/ring/in-flight still pass through the core**

Run: `cd ~/workspace/tuvm && bash tools/run_c_tests.sh test_http`
Expected: `test_http OK` (test_http.c is unchanged; `http_accept_request`/`take`/`respond`/`inflight_count`/`backend_reset`/`http_frame_response` now resolve from `http_core.c`, `http_emit` from `http_posix.c`).

- [ ] **Step 7: Run the curl integration test — the poll transport serves and sheds**

Run: `cd ~/workspace/tuvm && bash tests/http_roundtrip_host.sh`
Expected: `http_roundtrip OK` — `/thermal` → 200 with `rgb(`, `/nope` → 404, the 8-way burst shows a 200/503 mix. (This now exercises `http_poll` on the reactor thread instead of the retired accept thread.)

- [ ] **Step 8: Run the nsl conformance tests — the injected (fd == -1) path is intact**

Run:
```bash
for t in 01_hello_roundtrip 02_thermal_render 03_backpressure; do
  python3 ~/workspace/nsl-tuvm/tools/run-one.py ~/workspace/nsl-tests/14_http/$t.nsl
done
```
Expected: each PASS. (`httpInject:path:` still calls `http_accept_request` directly; `http_respond` → `core_emit` → `http_emit(fd=-1)` reproduces the old stdout framing byte-for-byte: header, body, newline, flush.)

- [ ] **Step 9: Commit**

```bash
cd ~/workspace/tuvm
git add src/http_core.c src/http_backend.h src/backend/posix/http_posix.c src/platform.c \
        tools/run_c_tests.sh bench.sh
git commit -m "refactor(http): extract src/http_core.c; unify posix+device on non-blocking poll

Move framing/ring/in-flight out of http_posix.c into a shared transport-agnostic
core; reduce posix to http_listen/http_poll/http_emit. Poll runs on the VM
reactor thread inside pumpEvents, so the accept thread + ring mutex are gone.
Prep for the Zephyr transport (#46).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_015er6825Dm9pSqPucVEemNY"
# nsl-tuvm harness change is a separate repo:
git -C ~/workspace/nsl-tuvm add tools/run_tests_oevm.py
git -C ~/workspace/nsl-tuvm commit -m "test(http): link src/http_core.c; drop http accept-thread -lpthread

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_015er6825Dm9pSqPucVEemNY"
```

---

## Task 2: Zephyr transport + WiFi/TCP Kconfig + associate hook — the ESP32 image builds clean

Add the device transport and everything the image needs, so `west build` produces a flashable ESP32 image. The gate is a clean build (there is no host unit test for the `zsock_*`/`wifi_mgmt` layer — the posix poll code proved the core in Task 1; only real silicon exercises WiFi association, done live in Task 3).

**Files:**
- Create: `src/backend/zephyr/http_zephyr.c`, `zephyr/Kconfig`
- Modify: `zephyr/prj_esp32.conf`, `zephyr/src/main.c`, `zephyr/CMakeLists.txt`

**Interfaces:**
- Consumes: `http_accept_request` (Task 1 core), the `http_poll`/`http_emit`/`http_listen` hook contract (Task 1 header).
- Produces: the Zephyr implementations of `http_listen`, `http_poll`, `http_emit`; `wifi_bringup()` in `main.c`; `CONFIG_TUVM_WIFI_SSID` / `CONFIG_TUVM_WIFI_PSK`.

- [ ] **Step 1: Create `src/backend/zephyr/http_zephyr.c` (zsock transport, mirrors udp_zephyr)**

```c
/* Zephyr HTTP transport hooks: a non-blocking TCP listener over the Zephyr net
   stack (IPv4, WiFi/DHCP), polled from the VM reactor thread. All framing/ring/
   in-flight logic lives in src/http_core.c. Mirrors udp_zephyr.c's non-blocking
   idiom (ZSOCK_MSG_DONTWAIT). Symbol names target Zephyr/upstream BSD sockets;
   adjust to the installed ~/zephyr-esp32 SDK if one differs — the build is the
   gate. fd == -1 (injected/test) never occurs on device -> http_emit no-ops it. */
#include "http_backend.h"
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <string.h>

#define HTTP_METHOD_MAX 8
#define HTTP_PATH_MAX   64

static int listen_fd = -1;

void http_emit(int fd, const char *hdr, int hlen, const char *body, int body_len) {
    if (fd == -1) return;                    /* no injected sink on device */
    (void)zsock_send(fd, hdr, (size_t)hlen, 0);
    if (body_len > 0) (void)zsock_send(fd, body, (size_t)body_len, 0);
    zsock_close(fd);
}

int http_listen(int port) {
    listen_fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) return -1;
    int one = 1;
    zsock_setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);   /* listen on the DHCP-assigned addr */
    a.sin_port = htons((unsigned short)port);
    if (zsock_bind(listen_fd, (struct sockaddr *)&a, sizeof a) < 0) {
        zsock_close(listen_fd); listen_fd = -1; return -1;
    }
    if (zsock_listen(listen_fd, 4) < 0) {
        zsock_close(listen_fd); listen_fd = -1; return -1;
    }
    zsock_fcntl(listen_fd, F_SETFL, O_NONBLOCK);
    return 0;
}

void http_poll(void) {
    if (listen_fd < 0) return;
    int fd = zsock_accept(listen_fd, NULL, NULL);
    if (fd < 0) return;                      /* EWOULDBLOCK -> nothing pending */
    /* bound the read so a silent client can't wedge the reactor */
    struct zsock_timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    zsock_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char buf[1024];
    int n = (int)zsock_recv(fd, buf, sizeof buf - 1, 0);
    if (n <= 0) { zsock_close(fd); return; }
    buf[n] = '\0';
    char method[HTTP_METHOD_MAX] = {0}, path[HTTP_PATH_MAX] = {0};
    if (sscanf(buf, "%7s %63s", method, path) != 2) { zsock_close(fd); return; }
    http_accept_request(fd, method, (int)strlen(method), path, (int)strlen(path));
}
```
(If the SDK lacks `struct zsock_timeval`, use `struct timeval` from `<zephyr/posix/sys/time.h>`; if `F_SETFL`/`O_NONBLOCK` aren't pulled in, include `<zephyr/posix/fcntl.h>`. These are the expected SDK-reconciliation points.)

- [ ] **Step 2: Create the app `zephyr/Kconfig` with the two WiFi-credential symbols**

```
# tuvm app Kconfig — WiFi credentials for the ESP32 HTTP node (#46).
# Values are supplied by the git-ignored overlay prj_esp32_wifi.local.conf;
# the defaults below keep non-WiFi boards (DK/dongle) building.
config TUVM_WIFI_SSID
	string "WiFi SSID for the HTTP node"
	default ""

config TUVM_WIFI_PSK
	string "WiFi WPA2 PSK for the HTTP node"
	default ""

source "Kconfig.zephyr"
```
(Zephyr auto-sources an app-dir `Kconfig` when present; it must end by sourcing `Kconfig.zephyr` so the rest of the tree still resolves.)

- [ ] **Step 3: Add the `wifi_bringup()` hook to `zephyr/src/main.c`**

Add the includes near the top (after the OpenThread block, ~line 63), guarded on `CONFIG_WIFI`:
```c
#ifdef CONFIG_WIFI
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/dhcpv4.h>
#include <string.h>

/* Associate to WPA2 WiFi (2.4 GHz) from the CONFIG_TUVM_WIFI_* symbols, start
   DHCPv4, and wait for an IPv4 lease before the VM binds the listener. Mirrors
   thread_bringup()'s poll-until-ready style. Loud on the console so the operator
   can read the assigned IP and point a browser at it. */
static void wifi_bringup(void) {
    struct net_if *iface = net_if_get_first_wifi();
    if (!iface) { printk("wifi: no wifi iface\n"); return; }

    struct wifi_connect_req_params p;
    memset(&p, 0, sizeof p);
    p.ssid        = (const uint8_t *)CONFIG_TUVM_WIFI_SSID;
    p.ssid_length = strlen(CONFIG_TUVM_WIFI_SSID);
    p.psk         = (const uint8_t *)CONFIG_TUVM_WIFI_PSK;
    p.psk_length  = strlen(CONFIG_TUVM_WIFI_PSK);
    p.security    = WIFI_SECURITY_TYPE_PSK;
    p.channel     = WIFI_CHANNEL_ANY;
    p.band        = WIFI_FREQ_BAND_2_4_GHZ;
    p.mfp         = WIFI_MFP_OPTIONAL;

    int cr = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &p, sizeof p);
    printk("wifi: connect(ssid_len=%u) req=%d\n", (unsigned)p.ssid_length, cr);

    net_dhcpv4_start(iface);

    char ipbuf[NET_IPV4_ADDR_LEN];
    for (int i = 0; i < 300; i++) {           /* up to ~30 s for associate + lease */
        struct net_if_addr *ua = &iface->config.ip.ipv4->unicast[0].ipv4;
        if (iface->config.ip.ipv4 &&
            ua->addr_state == NET_ADDR_PREFERRED &&
            ua->address.in_addr.s_addr != 0) {
            net_addr_ntop(AF_INET, &ua->address.in_addr, ipbuf, sizeof ipbuf);
            printk("wifi: up, ip=%s -> http://%s:18080/thermal\n", ipbuf, ipbuf);
            return;
        }
        k_msleep(100);
    }
    printk("wifi: no IPv4 lease after 30s, continuing\n");
}
#endif /* CONFIG_WIFI */
```
Call it in `main()` where `thread_bringup()` is called (~line 188), as a sibling guard — before the VM bootstrap so the node is on the network first:
```c
#ifdef CONFIG_NET_L2_OPENTHREAD
    thread_bringup();    /* Rung-7: enable OT + admin-up the net_if before the VM binds sockets */
#endif
#ifdef CONFIG_WIFI
    wifi_bringup();      /* #46: associate to WiFi + DHCPv4 before the VM binds the http listener */
#endif
```
(The `iface->config.ip.ipv4->unicast[0]` access is the expected SDK-reconciliation point — if the NCS/upstream struct layout differs, switch to a `NET_EVENT_IPV4_ADDR_ADD` `net_mgmt` event callback. The build error will point at it.)

- [ ] **Step 4: Rewrite `zephyr/prj_esp32.conf`'s networking block for WiFi + TCP + DHCPv4**

Remove the loopback block (the `CONFIG_NET_LOOPBACK=y` line, the two `CONFIG_NET_CONFIG_MY_IPV6_ADDR`/`_IPV4_ADDR` lines, `CONFIG_NET_CONFIG_NEED_IPV6=y`, and the "never moves a packet" comment). Keep `CONFIG_ESP32_USE_UNSUPPORTED_REVISION`, the console block, and the picolibc/FPU/heap block unchanged. Replace the networking section with:
```
# Networking: WiFi (2.4 GHz WROOM-32 radio) + TCP/IPv4 + DHCPv4 for the HTTP node.
CONFIG_NETWORKING=y
CONFIG_NET_IPV4=y
CONFIG_NET_TCP=y
CONFIG_NET_UDP=y
CONFIG_NET_SOCKETS=y
CONFIG_NET_DHCPV4=y

# WiFi driver + the wifi_mgmt / net_mgmt request API used by wifi_bringup().
CONFIG_WIFI=y
CONFIG_WIFI_ESP32=y
CONFIG_NET_L2_WIFI_MGMT=y
CONFIG_NET_MGMT=y
CONFIG_NET_MGMT_EVENT=y
CONFIG_NET_L2_ETHERNET=y

# Net-buffer / context counts for a small TCP server. Starting point; trim or
# raise at bring-up if RAM is tight or connections stall (see the design risk).
CONFIG_NET_MAX_CONTEXTS=8
CONFIG_NET_PKT_RX_COUNT=8
CONFIG_NET_PKT_TX_COUNT=8
CONFIG_NET_BUF_RX_COUNT=16
CONFIG_NET_BUF_TX_COUNT=16
CONFIG_NET_TCP_MAX_SEND_WINDOW_SIZE=2048
CONFIG_NET_CONFIG_SETTINGS=n
```
(`CONFIG_NET_CONFIG_SETTINGS=n`: association + DHCP are driven manually by `wifi_bringup()`, so the auto-config machinery is off. Exact `CONFIG_WIFI_ESP32`/`CONFIG_NET_L2_WIFI_MGMT` names get reconciled against `~/zephyr-esp32` in Step 6.)

- [ ] **Step 5: Link `http.c` + `http_core.c` + `http_zephyr.c` into the Zephyr image**

In `zephyr/CMakeLists.txt`, in the `target_sources(app PRIVATE ...)` block (the one listing `src/serial.c ${TUVM_SRC}/src/udp.c ...`), add:
```cmake
  ${TUVM_SRC}/src/http.c
  ${TUVM_SRC}/src/http_core.c
  ${TUVM_SRC}/src/backend/zephyr/http_zephyr.c
```
(None are linked on Zephyr today — `platform.c` references `http_pump`/`http_poll`/`http_listen`/`http_respond`/`http_accept_request`, so all three files must link or the image won't resolve.)

- [ ] **Step 6: Build the ESP32 image — the gate**

Run:
```bash
cd ~/workspace/tuvm/zephyr
west build -b esp32_devkitc/esp32/procpu -p auto . -- \
  -DCONF_FILE=prj_esp32.conf \
  -DEXTRA_CONF_FILE=prj_esp32_wifi.local.conf \
  -DTUVM_BLOB=$(pwd)/HttpServerNode.oimg
```
Expected: a clean build producing `build/zephyr/zephyr.bin`. If a `CONFIG_WIFI_ESP32`/`CONFIG_NET_L2_WIFI_MGMT`/`zsock_*`/`wifi_mgmt` symbol is rejected or a struct field is wrong, reconcile the name/field against the installed `~/zephyr-esp32` SDK (grep the SDK's `Kconfig`/headers) and rebuild — this is the expected embedded reconciliation, and the build is the gate. `HttpServerNode.oimg` is baked in Task 3 Step 1 if it does not exist yet; for a pure build-gate you may point `-DTUVM_BLOB` at the committed `TimerNode.oimg` to confirm compilation independent of the node image.

- [ ] **Step 7: Commit**

```bash
cd ~/workspace/tuvm
git add src/backend/zephyr/http_zephyr.c zephyr/Kconfig zephyr/prj_esp32.conf \
        zephyr/src/main.c zephyr/CMakeLists.txt
git commit -m "feat(http): Zephyr zsock transport + ESP32 WiFi/TCP/DHCPv4 Kconfig + associate hook (#46)

http_zephyr.c mirrors udp_zephyr's non-blocking poll; prj_esp32.conf swaps
loopback for WiFi+TCP+DHCPv4; wifi_bringup() associates from the git-ignored
CONFIG_TUVM_WIFI_* overlay before the VM binds the listener. ESP32 image builds.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_015er6825Dm9pSqPucVEemNY"
```

---

## Task 3: Live bring-up — flash, associate, serve `/thermal` over WiFi

Flash the ESP32 on `/dev/ttyUSB0`, watch it join WiFi and print its DHCP IP, then hit the routes from the host with `curl` and a browser. Deliverable: the 8×8 heat-grid renders in a browser over WiFi.

**Files:**
- No source files. May create/refresh `zephyr/HttpServerNode.oimg` (generated blob, not tracked source).

**Interfaces:**
- Consumes: the built image from Task 2; the frozen `zephyr/HttpServerNode.nsl`; the git-ignored WiFi overlay.

- [ ] **Step 1: Bake the node image and build with it**

Use the project's `run.sh` recipe if it bakes+flashes, else bake the `.oimg` and build explicitly:
```bash
cd ~/workspace/tuvm/zephyr
# Bake HttpServerNode.oimg from the frozen node (same tool run.sh uses):
python3 ~/workspace/nsl-tuvm/tools/ns_compile_oevm.py \
  --oimg HttpServerNode.nsl -o HttpServerNode.oimg   # confirm exact flags via `run.sh`/existing *.oimg recipe
west build -b esp32_devkitc/esp32/procpu -p auto . -- \
  -DCONF_FILE=prj_esp32.conf \
  -DEXTRA_CONF_FILE=prj_esp32_wifi.local.conf \
  -DTUVM_BLOB=$(pwd)/HttpServerNode.oimg
```
Expected: clean build with `HttpServerNode.oimg` embedded. (If `run.sh <node>` already encapsulates bake+build+flash, prefer it: `bash run.sh HttpServerNode` with the esp32 board/conf args — inspect `run.sh` first.)

- [ ] **Step 2: Flash the device on `/dev/ttyUSB0`**

```bash
cd ~/workspace/tuvm/zephyr
west flash --esp-device /dev/ttyUSB0
```
Expected: esptool writes the image and resets the board. (Per memory `tuvm-esp32-port`: esptool defaults to USB0; the bench WROOM-32 is on USB0. If two identical WROOM boards are attached, confirm the jag board is not on USB0.)

- [ ] **Step 3: Watch the console for association + the DHCP IP**

```bash
west espressif monitor -p /dev/ttyUSB0   # or: python3 -m serial.tools.miniterm /dev/ttyUSB0 115200
```
Expected: a line `wifi: up, ip=<A.B.C.D> -> http://<A.B.C.D>:18080/thermal`. Record `<A.B.C.D>`. If instead `wifi: no IPv4 lease after 30s`, the associate/DHCP path needs reconciliation — check the SSID is on 2.4 GHz, the PSK in the overlay is correct, and the `iface->config.ip.ipv4` access in Step 3 of Task 2 matches the SDK (switch to a `NET_EVENT_IPV4_ADDR_ADD` callback if needed). Detach the monitor (Ctrl-] / Ctrl-\) before curling so it does not hold the port.

- [ ] **Step 4: Curl the routes over WiFi**

With `IP=<A.B.C.D>` from Step 3:
```bash
IP=<A.B.C.D>
curl -s -o /tmp/esp_thermal -w 'thermal:%{http_code}\n' http://$IP:18080/thermal
grep -q 'rgb(' /tmp/esp_thermal && echo "svg-ok"
curl -s -o /dev/null -w 'nope:%{http_code}\n'   http://$IP:18080/nope
curl -s -o /dev/null -w 'hello:%{http_code}\n'  http://$IP:18080/hello
curl -s -o /dev/null -w 'stats:%{http_code}\n'  http://$IP:18080/stats
# concurrent burst: >4 in flight sheds >=1 503
for i in $(seq 1 8); do curl -s -o /dev/null -w '%{http_code}\n' http://$IP:18080/thermal & done | sort | uniq -c
```
Expected: `thermal:200` + `svg-ok`, `nope:404`, `hello:200`, `stats:200`; the burst shows a mix of `200` and at least one `503`.

- [ ] **Step 5: View the heat-grid in a browser**

Open `http://<A.B.C.D>:18080/thermal` in a browser. Expected: the 8×8 heat-grid SVG renders with the caption `max 31.2C min 22.0C hot (3,5)` (the seeded hot pixel), and `/` shows the index with working links to `/thermal`, `/stats`, `/hello`. This is the issue's deliverable.

- [ ] **Step 6: Record the live result (no source commit)**

No source changes to commit here. Capture the observed IP, the five HTTP codes, the burst 200/503 split, and (optionally) a browser screenshot into the Task 4 findings write-up. If `HttpServerNode.oimg` was (re)baked and is tracked, commit it:
```bash
cd ~/workspace/tuvm
git add zephyr/HttpServerNode.oimg 2>/dev/null && \
  git commit -m "chore(http): refresh HttpServerNode.oimg for the esp32 wifi run (#46)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_015er6825Dm9pSqPucVEemNY" || echo "no oimg change to commit"
```

---

## Task 4: Close-out — promote findings to the mainline docs of record

Per the CLAUDE.md promotion rule, #46 is not closed until the durable knowledge lands in a mainline doc. Record the on-device verdict and the cross-source scheduling design point.

**Files:**
- Modify: `review/model-capability-findings.md`, `review/program-sequence.md`

**Interfaces:**
- Consumes: the live results from Task 3.

- [ ] **Step 1: Append the on-device close-out to `review/model-capability-findings.md`**

Add a subsection to the spike #1 entry titled "On-device last mile (#46)" recording, from what actually happened:
- **Verdict:** the poll-unified transport served `/thermal` over WiFi from the ESP32 (cite the observed IP behavior, the five route codes, the burst 200/503 split from Task 3).
- **Portability of the seam:** posix→device was a backend-only change (three hooks `http_listen`/`http_poll`/`http_emit` + Kconfig + a boot hook); zero nsl change, confirming the "poll↔thread is invisible above the channel" claim from the design.
- **Cross-source scheduling design point:** `pumpEvents` drains sources in fixed order, bounded per source (`http_poll; pulse_pump; http_pump`), NOT strict round-robin; starvation is bounded structurally because each source is capacity-bounded (HTTP in-flight = 4 → 503). Strict round-robin is a future pump-policy knob. Adding a source (e.g. SPI) is one `*_pump()` line — N sources cost N drains, zero threads.
- **Any bring-up surprises:** SDK symbol/struct reconciliations, RAM tuning (which `OEVM_HEAP_SIZE`/net-buf values were needed), 2.4 GHz/DHCP notes.

- [ ] **Step 2: Update the ladder pointer in `review/program-sequence.md`**

Under the Addendum where spike #1 was noted, add one line that the #46 on-device last mile landed (ESP32 serves `/thermal` over WiFi) and point to the findings subsection. Do not restructure the ladder.

- [ ] **Step 3: Commit**

```bash
cd ~/workspace/tuvm
git add review/model-capability-findings.md review/program-sequence.md
git commit -m "docs(review): #46 on-device close-out — ESP32 serves /thermal over WiFi

Records the on-device verdict, the seam-portability confirmation (backend-only,
zero nsl change), and the cross-source scheduling design point (bounded-per-source
fixed order, round-robin as a future pump knob).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_015er6825Dm9pSqPucVEemNY"
```

- [ ] **Step 4: Close the issue**

```bash
gh issue close 46 --comment "Landed: ESP32 (WROOM-32) serves /thermal (+ /, /stats, /hello) over WiFi at http://<dhcp-ip>:18080/. Poll-unified transport (shared src/http_core.c), Zephyr zsock backend, WiFi+DHCPv4 Kconfig, associate boot hook. Findings: review/model-capability-findings.md. Design: docs/design/2026-07-18-esp32-wifi-http-last-mile-design.md."
```

---

## Self-Review

**Spec coverage:** shared core extraction + posix→poll (Task 1, §Architecture/§Code-org); Zephyr `zsock` transport (Task 2 Step 1, §Device transport); WiFi/TCP/DHCPv4 Kconfig (Task 2 Step 4, §Network config); associate boot hook (Task 2 Step 3, §WiFi association); credentials via git-ignored overlay (Global Constraints + Task 2 Steps 2–3, §Decisions-4); build gate (Task 2 Step 6, §Verification-a); host regression (Task 1 Steps 6–8, §Verification-b); live flash/associate/curl/browser (Task 3, §Verification-c); cross-source scheduling design point (Task 4 Step 1, §Cross-source scheduling); findings + ladder promotion (Task 4, promotion rule); port 18080 frozen (Global Constraints, §Risks). Covered.

**Placeholder scan:** the only deferred-precision points are explicitly flagged SDK-reconciliation spots (`zsock_timeval`, `F_SETFL` include, `CONFIG_WIFI_ESP32`/`CONFIG_NET_L2_WIFI_MGMT` names, the `iface->config.ip.ipv4` field, the `ns_compile_oevm.py` bake flags) — each names the concrete fallback and states the build/flash is the gate, which is correct for embedded bring-up rather than a placeholder. No "TBD"/"add error handling"/"similar to Task N".

**Type consistency:** `http_poll(void)` and `http_emit(int fd, const char *hdr, int hlen, const char *body, int body_len)` are declared identically in `http_backend.h` (Task 1 Step 1) and implemented identically in both `http_posix.c` (Task 1 Step 3) and `http_zephyr.c` (Task 2 Step 1). `http_frame_headers(int,int,char*,int)` declared in the header and defined in `http_core.c`. `http_accept_request`/`http_take_request`/`http_respond` signatures are carried verbatim from the existing seam. `CONFIG_TUVM_WIFI_SSID`/`_PSK` spelled identically in the overlay, `zephyr/Kconfig`, and `main.c`. `HTTP_METHOD_MAX`(8)/`HTTP_PATH_MAX`(64) match `OEVM_HTTP_INFLIGHT`(4) usage across core and both backends. Consistent.
