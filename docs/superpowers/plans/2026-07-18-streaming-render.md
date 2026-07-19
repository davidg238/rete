# Streaming Render — Chunked HTTP Egress Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reclaim the ~15 KB per-render VM-heap peak in `HttpServerNode` by streaming HTTP response bodies as HTTP/1.1 chunked fragments from nsl, instead of building each page as one big String.

**Architecture:** Add three `id`-keyed streaming primitives (`httpBeginId:status:` / `httpChunkId:data:` / `httpEndId:`) that write chunked-transfer-encoding frames straight to the request socket. HTTP framing stays in the transport-agnostic `src/http_core.c`; backends gain raw write-all + close hooks reusing the existing cooperative-yield backpressure loop. The nsl render contract changes from "return a body String" to "write yourself into a sink", carried through the existing reply channel as a **block** (contract (b)).

**Tech Stack:** C99 (VM engine + posix/Zephyr transports), nsl (`.nsl` sources compiled by `~/workspace/nsl-tuvm/tools/ns_compile_oevm.py`), Zephyr/ESP32 (WROOM-32), bash test harnesses.

**Design doc:** `~/workspace/tuvm/docs/design/2026-07-18-streaming-render-chunked-egress-design.md`

## Global Constraints

- **nsl source extension is `.nsl`** (hard cut, all repos on master). Kernel library is `~/workspace/nsl-tuvm/lib/kernel.nsl`.
- **Primitive id sync is load-bearing:** the anonymous enum in `src/dispatch.c` is authoritative; `src/prim_ids.h` macros MUST mirror it; `PRIM_NAMES[]` in `dispatch.c` MUST stay parallel to the enum (index `i` → id `i+1`). **Append new platform prims AFTER the last existing slot** (`PRIM_pf_httpInject`) so no pre-existing id shifts — exactly as `PRIM_pf_basicNewBytes`/`freezeBytes` and the http trio did before.
- **ESP32 ROM constraints unchanged:** never call `fflush`; the seven OEVM env knobs and `OEVM_HEAP_SIZE=53248` default are set by `zephyr/run.sh` — do not change them in this work.
- **Single-threaded reactor:** `http_poll` / `http_take_request` / `http_respond` and the new begin/chunk/end all run on the one VM thread at the `pumpEvents` safe point. No locks.
- **Device default is Model B** (`.oimg` blob), validated on ESP32. Host tests use `OEVM_TARGET=posix`.
- **C unit tests** run via `bash tools/run_c_tests.sh`; the single http test builds as `gcc -std=c99 -Isrc tests/test_http.c src/http_core.c src/backend/posix/http_posix.c -lm -o /tmp/tuvm_ctest && /tmp/tuvm_ctest`.

---

### Task 1: Feasibility spike — blocks through channels (the §4 gate)

Prove contract (b) — a block stored in a channel Record survives `out:`/`whenever:` delivery and is invocable at egress with an argument — BEFORE building on it. If it fails, the fallback is contract (a) (router streams inline, no channel-carried block), which changes Task 4 only.

**Files:**
- Create: `zephyr/StreamSpike.nsl`
- Test: run via the posix host build recipe (below)

**Interfaces:**
- Produces: a yes/no decision recorded in the design doc's §4. No code consumed by later tasks — this task de-risks Task 4's contract choice.

- [ ] **Step 1: Write the spike node**

Create `zephyr/StreamSpike.nsl`:

```
(*
StreamSpike.nsl — feasibility gate for the streaming-render design §4. Proves a
block can be carried through a channel Record and invoked (with an argument) in a
DIFFERENT reaction at delivery time. If this prints "spike: 84", contract (b)
(body-as-block through the reply channel) is sound. Throwaway — delete after Task 1.
*)
class StreamSpike packageUsing: manifest = () (
  public main: platform args: args = (
    | space ch |
    space:: platform space.
    ch:: space channel: 'dev/spike'.
    ch whenever: '' do: [ :rec |
      | blk |
      blk:: rec at: #body ifAbsent: [ nil ].
      (blk == nil)
        ifTrue:  [ platform print: 'spike: FAIL nil block'; printNewline ]
        ifFalse: [ platform print: 'spike: '; print: (blk value: 42) printString; printNewline ] ].
    ch out: '' value: (platform Record tag: #response with: #body -> [ :n | n * 2 ]).
    space drain.
  )
) : (
)
```

- [ ] **Step 2: Build + run the spike on the posix host**

Run:
```bash
cd ~/workspace/tuvm
NSL="$HOME/workspace/nsl-tuvm/tools/ns_compile_oevm.py"
gcc -std=c99 -Isrc tools/dump_layout.c -o /tmp/dl && /tmp/dl > /tmp/layout.json
python3 "$NSL" zephyr/StreamSpike.nsl /tmp/spike.oimg --format=oimg --layout=/tmp/layout.json
gcc -Wall -std=c99 -Isrc \
    src/vm.c src/gc.c src/dispatch.c src/platform.c src/space.c src/handles.c \
    src/backend/posix/platform_posix.c src/serial.c src/backend/posix/serial_posix.c \
    src/udp.c src/backend/posix/udp_posix.c src/http.c src/http_core.c \
    src/backend/posix/http_posix.c src/backend/posix/pulse_posix.c \
    src/backend/posix/timer_posix.c src/mirror.c src/oimg_loader.c \
    src/kernel_boot.c oevm_image_main.c -o /tmp/oevm_spike -lm
/tmp/oevm_spike /tmp/spike.oimg
```
Expected: prints `spike: 84`.

- [ ] **Step 3: Record the decision**

If `spike: 84` printed → contract (b) confirmed; proceed with Task 4 as written.
If it failed (nil block, crash, or GC error) → edit the design doc §3 to select fallback **(a) inline-stream**, and note it here. Either way, append one line to the design doc §4 stating the observed result and date.

- [ ] **Step 4: Delete the spike and commit the decision**

```bash
git rm zephyr/StreamSpike.nsl
git add docs/design/2026-07-18-streaming-render-chunked-egress-design.md
git commit -m "spike(http): confirm blocks-through-channels for streaming render (§4 gate)"
```

---

### Task 2: Chunked framing + raw write/close transport hooks (C)

Add HTTP/1.1 chunked framing to the transport-agnostic core and the raw write-all/close backend hooks it needs, refactoring the existing `http_emit` to share them. This is pure C, unit-tested on the host with no VM.

**Files:**
- Modify: `src/http_backend.h` (declare new hooks + core functions)
- Modify: `src/http_core.c` (chunked header/chunk framing + `http_begin`/`http_chunk`/`http_end`; refactor `core_emit`)
- Modify: `src/backend/posix/http_posix.c` (`http_write`/`http_close`; refactor `http_emit`)
- Modify: `src/backend/zephyr/http_zephyr.c` (`http_write`/`http_close` via `send_all`; refactor `http_emit`)
- Test: `tests/test_http.c`

**Interfaces:**
- Produces (C, called by Task 3's prims):
  - `void http_begin(int id, int status);`
  - `void http_chunk(int id, const char *data, int len);`
  - `void http_end(int id);`
- Produces (backend hooks, per target):
  - `int  http_write(int fd, const char *buf, int len);` — write all `len` bytes, non-closing; 0 on success, -1 on peer error.
  - `void http_close(int fd);`
- Produces (pure framing, unit-tested): `int http_frame_chunk_header(int len, char *out, int out_cap);` — writes `"%x\r\n"`; returns bytes or -1. `int http_frame_chunked_headers(int status, char *out, int out_cap);` — status line + `Transfer-Encoding: chunked`, no Content-Length.

- [ ] **Step 1: Write failing unit tests for the pure framing functions**

In `tests/test_http.c`, add before `main`:

```c
static void test_frame_chunk_header(void) {
    char buf[16];
    int n = http_frame_chunk_header(5, buf, sizeof buf);
    assert(n == 3);
    assert(strcmp(buf, "5\r\n") == 0);
    n = http_frame_chunk_header(255, buf, sizeof buf);   /* hex */
    assert(strcmp(buf, "ff\r\n") == 0);
}

static void test_frame_chunked_headers(void) {
    char buf[256];
    int n = http_frame_chunked_headers(200, buf, sizeof buf);
    assert(n > 0);
    assert(strstr(buf, "HTTP/1.1 200 OK\r\n") == buf);
    assert(strstr(buf, "Transfer-Encoding: chunked\r\n") != NULL);
    assert(strstr(buf, "Content-Length:") == NULL);   /* chunked => no length */
}
```

Register them in `main` (alongside the existing `test_frame_200();` calls):

```c
    test_frame_chunk_header();
    test_frame_chunked_headers();
```

- [ ] **Step 2: Run to verify it fails**

Run: `gcc -std=c99 -Isrc tests/test_http.c src/http_core.c src/backend/posix/http_posix.c -lm -o /tmp/tuvm_ctest`
Expected: FAIL — compile error, `http_frame_chunk_header` / `http_frame_chunked_headers` undeclared.

- [ ] **Step 3: Declare the new core + backend functions in the header**

In `src/http_backend.h`, after the existing `http_frame_headers` declaration, add:

```c
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
```

- [ ] **Step 4: Implement the framing functions in `src/http_core.c`**

Add after `http_frame_headers` (around line 60):

```c
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

int http_frame_chunk_header(int len, char *out, int out_cap) {
    int n = snprintf(out, (size_t)out_cap, "%x\r\n", len);
    if (n < 0 || n >= out_cap) return -1;
    return n;
}
```

- [ ] **Step 5: Run to verify the framing tests pass**

Run: `gcc -std=c99 -Isrc tests/test_http.c src/http_core.c src/backend/posix/http_posix.c -lm -o /tmp/tuvm_ctest && /tmp/tuvm_ctest && echo ALLPASS`
Expected: PASS (`ALLPASS` printed) — but only after Step 6 provides `http_write`/`http_close`/`http_begin`/`http_chunk`/`http_end` for the link. If the link fails on those symbols, proceed to Step 6 then re-run.

- [ ] **Step 6: Add the streamed lifecycle to `src/http_core.c`**

Add a `streaming` guard is not needed (the slot stays live between begin and end; `http_respond` and `http_begin` both look up by id). Add after `http_respond` (end of file):

```c
static int fd_for_id(int id) {
    for (int i = 0; i < OEVM_HTTP_INFLIGHT; i++)
        if (inflight[i].live && inflight[i].id == id) return inflight[i].fd;
    return -1;
}

void http_begin(int id, int status) {
    int fd = fd_for_id(id);
    if (fd == -1 && !slot_is_injected(id)) return;   /* unknown id */
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
    http_write(fd, szline, sn);
    http_write(fd, data, len);
    http_write(fd, "\r\n", 2);
}

void http_end(int id) {
    int fd = fd_for_id(id);
    http_write(fd, "0\r\n\r\n", 5);                   /* terminating chunk */
    http_close(fd);
    for (int i = 0; i < OEVM_HTTP_INFLIGHT; i++)
        if (inflight[i].live && inflight[i].id == id) { inflight[i].live = 0; break; }
}
```

Note: `fd_for_id` returns the slot fd (which is `-1` for injected/test requests — that is a valid sink on posix, so `http_begin`/`chunk`/`end` must pass `-1` straight through to `http_write`/`http_close`). Remove the `slot_is_injected` guard — it does not exist; instead let a genuinely-unknown id fall through harmlessly: replace the `http_begin` guard line with:

```c
    int fd = fd_for_id(id);            /* -1 == injected sink OR unknown id */
```

and let `http_write(-1, …)` route to the injected sink (posix) / no-op (device), matching `core_emit`.

- [ ] **Step 7: Refactor posix backend — add `http_write`/`http_close`, rebuild `http_emit` on them**

Replace `http_emit` in `src/backend/posix/http_posix.c` with:

```c
int http_write(int fd, const char *buf, int len) {
    if (fd == -1) {                          /* injected/test sink -> stdout */
        fwrite(buf, 1, (size_t)len, stdout);
        return 0;
    }
    int off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, (size_t)(len - off));
        if (w <= 0) { if (errno == EINTR) continue; return -1; }
        off += (int)w;
    }
    return 0;
}

void http_close(int fd) {
    if (fd == -1) { fputc('\n', stdout); fflush(stdout); return; }
    close(fd);
}

void http_emit(int fd, const char *hdr, int hlen, const char *body, int body_len) {
    http_write(fd, hdr, hlen);
    if (body_len > 0) http_write(fd, body, body_len);
    http_close(fd);
}
```

- [ ] **Step 8: Refactor Zephyr backend — expose `send_all` as `http_write`, add `http_close`**

In `src/backend/zephyr/http_zephyr.c`, keep `send_all` as-is and add wrappers, then rebuild `http_emit`:

```c
int http_write(int fd, const char *buf, int len) {
    if (fd == -1) return 0;                  /* no injected sink on device */
    return send_all(fd, buf, len);
}

void http_close(int fd) {
    if (fd != -1) zsock_close(fd);
}

void http_emit(int fd, const char *hdr, int hlen, const char *body, int body_len) {
    if (fd == -1) return;
    if (http_write(fd, hdr, hlen) == 0 && body_len > 0)
        (void)http_write(fd, body, body_len);
    http_close(fd);
}
```

- [ ] **Step 9: Write a lifecycle test using the injected sink (fd == -1 → stdout capture)**

In `tests/test_http.c`, add:

```c
static void test_stream_lifecycle(void) {
    http_backend_reset();
    int id = http_accept_request(-1, "GET", 3, "/thermal", 8);
    assert(id >= 0);
    /* drain the ring so only the in-flight slot remains (mirrors real flow) */
    int tid; char m[8]; char p[64];
    http_take_request(&tid, m, sizeof m, p, sizeof p);

    freopen("/tmp/tuvm_stream.out", "w", stdout);
    http_begin(id, 200);
    http_chunk(id, "AB", 2);
    http_chunk(id, "CDE", 3);
    http_end(id);
    fflush(stdout);
    freopen("/dev/tty", "w", stdout);        /* best-effort restore; ok if it fails in CI */

    FILE *f = fopen("/tmp/tuvm_stream.out", "r");
    char got[256] = {0};
    fread(got, 1, sizeof got - 1, f); fclose(f);
    assert(strstr(got, "HTTP/1.1 200 OK\r\n") == got);
    assert(strstr(got, "Transfer-Encoding: chunked\r\n") != NULL);
    assert(strstr(got, "\r\n2\r\nAB\r\n") != NULL);       /* chunk 1: size 2 */
    assert(strstr(got, "3\r\nCDE\r\n") != NULL);          /* chunk 2: size 3 */
    assert(strstr(got, "0\r\n\r\n") != NULL);             /* terminator */
    assert(http_inflight_count() == 0);                   /* http_end freed the slot */
}
```

Register `test_stream_lifecycle();` in `main`.

- [ ] **Step 10: Run the full http C test**

Run: `gcc -std=c99 -Isrc tests/test_http.c src/http_core.c src/backend/posix/http_posix.c -lm -o /tmp/tuvm_ctest && /tmp/tuvm_ctest && echo ALLPASS`
Expected: PASS (`ALLPASS`). If the `freopen("/dev/tty")` restore errors in a headless shell, it is non-fatal — the assertions above it are what matter; if it aborts, guard it with `if (!freopen("/dev/tty","w",stdout)) {}`.

- [ ] **Step 11: Run the whole C suite to confirm no regression**

Run: `bash tools/run_c_tests.sh`
Expected: `test_http ... PASS` and every other line PASS/SKIP (no new FAIL).

- [ ] **Step 12: Commit**

```bash
git add src/http_backend.h src/http_core.c src/backend/posix/http_posix.c src/backend/zephyr/http_zephyr.c tests/test_http.c
git commit -m "feat(http): chunked framing + raw write/close transport hooks"
```

---

### Task 3: nsl-visible streaming primitives

Expose `http_begin`/`http_chunk`/`http_end` to nsl as three platform capability selectors, wired through the prim-id triple and a kernel forwarder.

**Files:**
- Modify: `src/dispatch.c` (enum + `PRIM_NAMES[]`)
- Modify: `src/prim_ids.h` (mirror macros)
- Modify: `src/platform.c` (case handlers)
- Modify: `~/workspace/nsl-tuvm/lib/kernel.nsl` (forwarders)
- Test: `tests/http_roundtrip_host.sh` exercises them end-to-end in Task 5; a compile+boot smoke here.

**Interfaces:**
- Consumes: `http_begin`/`http_chunk`/`http_end` (Task 2).
- Produces (nsl, used by Task 4): `platform httpBeginId: id status: st`, `platform httpChunkId: id data: aString`, `platform httpEndId: id`.

- [ ] **Step 1: Append the enum slots in `src/dispatch.c`**

In the anonymous prim enum, immediately after `PRIM_pf_httpInject,` (the current last entry), add:

```c
    /* streaming-render (2026-07-18): chunked egress prims — appended after
       httpInject, no pre-existing id shifts. 113/114/115, mirrored in prim_ids.h. */
    PRIM_pf_httpBegin,
    PRIM_pf_httpChunk,
    PRIM_pf_httpEnd,
```

- [ ] **Step 2: Append the parallel selector names in `PRIM_NAMES[]`**

In `PRIM_NAMES[]`, immediately after `"httpInject:path:",` (the current last entry), add — SAME order as the enum:

```c
    "httpBeginId:status:", "httpChunkId:data:", "httpEndId:",
```

- [ ] **Step 3: Append the mirror macros in `src/prim_ids.h`**

After the existing `#define PRIM_pf_httpInject …` line (the http trio group; ids 110-112), add:

```c
#define PRIM_pf_httpBegin  113
#define PRIM_pf_httpChunk  114
#define PRIM_pf_httpEnd    115
```

(Confirm 110-112 are `httpListen`/`httpRespond`/`httpInject` in that file and that 113-115 are unused; if the base differs, keep them contiguous after `httpInject`.)

- [ ] **Step 4: Add the case handlers in `src/platform.c`**

After the `PRIM_pf_httpInject` case block, add:

```c
    case PRIM_pf_httpBegin: {                  /* httpBeginId: id status: st */
        int id = IS_SMI(args[0]) ? (int)UNTAG_SMI(args[0]) : 0;
        int st = IS_SMI(args[1]) ? (int)UNTAG_SMI(args[1]) : 500;
        http_begin(id, st);
        *out = platform_object();
        return 1;
    }
    case PRIM_pf_httpChunk: {                   /* httpChunkId: id data: s */
        int id = IS_SMI(args[0]) ? (int)UNTAG_SMI(args[0]) : 0;
        oop data = args[1];
        if (data == NULL || IS_SMI(data) || StringVtable == NULL ||
            obj_vtable(data) != StringVtable) {
            fprintf(stderr, "VM Error: platform httpChunkId:data: expects a String\n");
            abort();
        }
        struct String *s = (struct String *)data;
        http_chunk(id, (const char *)s->bytes, (int)s->length);
        *out = platform_object();
        return 1;
    }
    case PRIM_pf_httpEnd: {                      /* httpEndId: id */
        int id = IS_SMI(args[0]) ? (int)UNTAG_SMI(args[0]) : 0;
        http_end(id);
        *out = platform_object();
        return 1;
    }
```

- [ ] **Step 5: Add the kernel forwarders in `~/workspace/nsl-tuvm/lib/kernel.nsl`**

After the `public httpInject: m path: p = ( … )` line (~2023), add:

```
  public httpBeginId: id status: st = ( ^ platform httpBeginId: id status: st )
  public httpChunkId: id data: s    = ( ^ platform httpChunkId: id data: s )
  public httpEndId: id              = ( ^ platform httpEndId: id )
```

- [ ] **Step 6: Smoke-build the host image VM to confirm the prim triple links + boots**

Run the Task 1 build recipe but with the existing `zephyr/HttpServerNode.nsl` (unmodified so far) → it must still compile and boot (the new prims are present but unused yet):

```bash
cd ~/workspace/tuvm
NSL="$HOME/workspace/nsl-tuvm/tools/ns_compile_oevm.py"
gcc -std=c99 -Isrc tools/dump_layout.c -o /tmp/dl && /tmp/dl > /tmp/layout.json
python3 "$NSL" zephyr/HttpServerNode.nsl /tmp/http_server.oimg --format=oimg --layout=/tmp/layout.json
bash tests/http_roundtrip_host.sh
```
Expected: `http_roundtrip OK` (existing behavior intact; the new prims are registered but not yet called).

- [ ] **Step 7: Run the C suite (enum/NAMES parity guard)**

Run: `bash tools/run_c_tests.sh`
Expected: all PASS/SKIP. A mismatch between the enum and `PRIM_NAMES[]` typically surfaces as a wrong-selector dispatch failure in `test_platform` — if that fails, re-check Steps 1-3 are in the identical order and count.

- [ ] **Step 8: Commit**

```bash
git add src/dispatch.c src/prim_ids.h src/platform.c
git commit -m "feat(http): httpBeginId:/httpChunkId:/httpEndId: streaming prims"
cd ~/workspace/nsl-tuvm && git add lib/kernel.nsl && git commit -m "feat(kernel): streaming http capability forwarders" && cd ~/workspace/tuvm
```

---

### Task 4: nsl render rewrite — sink + body-as-block

Rewrite `HttpServerNode.nsl` so routes produce a **renderer block** that writes fragments to a sink, and the egress reaction streams via begin/block/end. Uses contract (b) confirmed in Task 1 (or (a) if Task 1 selected the fallback — see Step 5).

**Files:**
- Modify: `zephyr/HttpServerNode.nsl`
- Modify: `zephyr/HttpNode.nsl` (keep in sync — it is the injected-request twin; same reply seam)
- Test: `tests/http_roundtrip_host.sh` (Task 5 tightens it)

**Interfaces:**
- Consumes: `platform httpBeginId:status:`, `httpChunkId:data:`, `httpEndId:` (Task 3).
- Produces: streamed `/thermal`, `/stats`, `/`, `/hello`, 404 responses — same bytes as today, chunked.

- [ ] **Step 1: Add a Sink helper and convert the egress reaction (contract b)**

In `zephyr/HttpServerNode.nsl`, replace the egress reaction (lines 38-41) with a begin/invoke/end that passes a sink to the body block:

```
    reply whenever: '' do: [ :resp |
      | id st blk |
      id::  resp at: #id     ifAbsent: [ 0 ].
      st::  resp at: #status ifAbsent: [ 500 ].
      blk:: resp at: #body   ifAbsent: [ [ :sink | sink << 'error' ] ].
      platform httpBeginId: id status: st.
      blk value: (Sink onId: id platform: platform).
      platform httpEndId: id ].
```

Add a minimal `Sink` class in the same file (after the `HttpServerNode` class, or as a nested helper following the file's existing style):

```
class Sink onId: anId platform: aPlatform = () (
  | id platform |
  public init: anId platform: aPlatform = ( id:: anId. platform:: aPlatform. ^ self )
  public << aString = ( platform httpChunkId: id data: aString. ^ self )
) : (
  public onId: anId platform: aPlatform = ( ^ self new init: anId platform: aPlatform )
)
```

(Match the actual class/instantiation idiom used elsewhere in the repo — see how `platform Record`/`platform OrderedCollection` classes are declared; if a class-side `new` + `init:` pattern is not how this codebase builds helpers, express `Sink` using the same construction idiom the other nodes use. The behavioral contract is only: `sink << aString` calls `platform httpChunkId: id data: aString` and returns the sink so `<<` chains.)

- [ ] **Step 2: Convert the router reaction to build blocks, not Strings**

Replace the router body-assignment (lines 27-37) so each route sets `body` to a block `[ :sink | … ]`:

```
    req whenever: '' do: [ :r |
      | id path status body |
      id::   r basicAt: 1.
      path:: r basicAt: 3.
      (path = '/')
        ifTrue:  [ status:: 200. body:: [ :s | self indexTo: s ] ]
        ifFalse: [ (path = '/hello')
        ifTrue:  [ status:: 200. body:: [ :s | s << 'hello' ] ]
        ifFalse: [ (path = '/thermal')
          ifTrue:  [ status:: 200. body:: [ :s | self renderFrame: (thermal peek: '' ifAbsent: [ nil ]) toSink: s ] ]
          ifFalse: [ (path = '/stats')
          ifTrue:  [ status:: 200. body:: [ :s | self statsPageOn: platform toSink: s ] ]
          ifFalse: [ status:: 404. body:: [ :s | s << 'not found' ] ] ] ] ].
      reply out: '' value: (platform Record tag: #response
                              with: #id -> id with: #status -> status with: #body -> body) ].
```

- [ ] **Step 3: Rewrite the renderers to stream into a sink**

Replace `renderFrame:`, `statsPageOn:`, `index`, and `statRow:value:` with sink-writing forms (`,` concatenation removed on the hot paths):

```
  indexTo: s = (
    s << '<html><head><title>tuvm http node</title></head>'
      << '<body style=''font-family:system-ui,sans-serif;max-width:34rem;margin:3rem auto;padding:0 1rem;line-height:1.5''>'
      << '<h1>tuvm &mdash; channel-native http node</h1>'
      << '<p>Request/reply served over the VM channel space. C owns the socket + HTTP framing; routing, the reduce, and the render are authored in nsl.</p>'
      << '<ul>'
      << '<li><a href=''/thermal''>/thermal</a> &mdash; 8&times;8 thermal heat-grid (SVG, rendered on the VM)</li>'
      << '<li><a href=''/stats''>/stats</a> &mdash; live VM heap / GC / channel metrics</li>'
      << '<li><a href=''/hello''>/hello</a> &mdash; plain text</li>'
      << '</ul></body></html>'.
    ^ self )

  renderFrame: f toSink: s = (
    | row col t k |
    (f == nil) ifTrue: [ s << '<html><body>no frame</body></html>'. ^ self ].
    s << '<html><body><svg viewBox=''0 0 8 8'' width=''320'' height=''320'' shape-rendering=''crispEdges''>'.
    row:: 0. [ row <= 7 ] whileTrue: [
      col:: 0. [ col <= 7 ] whileTrue: [
        t:: f at: (row * 8 + col + 1).
        k:: ((t - 1500) * 255) // 2000.
        (k < 0)   ifTrue: [ k:: 0 ].
        (k > 255) ifTrue: [ k:: 255 ].
        s << '<rect x=''' << col printString << ''' y=''' << row printString
          << ''' width=''1'' height=''1'' fill=''rgb(' << k printString << ',0,'
          << (255 - k) printString << ')''/>'.
        col:: col + 1 ]. row:: row + 1 ].
    s << '</svg><p>' << (self caption: f) << '</p></body></html>'.
    ^ self )

  statsPageOn: platform toSink: s = (
    | m |
    m:: platform metrics.
    s << '<html><head><title>tuvm vm stats</title></head>'
      << '<body style=''font-family:system-ui,sans-serif;max-width:34rem;margin:3rem auto;padding:0 1rem;line-height:1.5''>'
      << '<h1>tuvm &mdash; vm stats</h1>'
      << '<p>A live snapshot from <code>platform metrics</code> (the #Metrics Record), read on request &mdash; no periodic publisher (G9).</p>'
      << '<table style=''border-collapse:collapse;width:100%''>'
      << '<thead><tr><th style=''text-align:left;border-bottom:2px solid #ccc;padding:.4rem''>metric</th>'
      << '<th style=''text-align:right;border-bottom:2px solid #ccc;padding:.4rem''>value</th></tr></thead><tbody>'.
    self statRow: 'heap size (bytes)'        value: (m at: #heapSize         ifAbsent: [ -1 ]) toSink: s.
    self statRow: 'live bytes'               value: (m at: #liveBytes        ifAbsent: [ -1 ]) toSink: s.
    self statRow: 'heap high-water'          value: (m at: #heapHighWater    ifAbsent: [ -1 ]) toSink: s.
    self statRow: 'largest free block'       value: (m at: #largestFreeBlock ifAbsent: [ -1 ]) toSink: s.
    self statRow: 'gc cycles'                value: (m at: #gcCycles         ifAbsent: [ -1 ]) toSink: s.
    self statRow: 'handles live'             value: (m at: #handlesLive      ifAbsent: [ -1 ]) toSink: s.
    self statRow: 'handles high-water'       value: (m at: #handlesHighWater ifAbsent: [ -1 ]) toSink: s.
    self statRow: 'channel queue high-water' value: (m at: #queueHighWater   ifAbsent: [ -1 ]) toSink: s.
    self statRow: 'channel overruns'         value: (m at: #overruns         ifAbsent: [ -1 ]) toSink: s.
    s << '</tbody></table><p><a href=''/''>&larr; home</a></p></body></html>'.
    ^ self )

  statRow: label value: v toSink: s = (
    s << '<tr><td style=''padding:.35rem;border-bottom:1px solid #eee''>' << label
      << '</td><td style=''padding:.35rem;border-bottom:1px solid #eee;text-align:right;font-variant-numeric:tabular-nums''>'
      << v printString << '</td></tr>'.
    ^ self )
```

Leave `caption:` and `centi:` unchanged — they build tiny Strings (well under a chunk) and are consumed by `<<` inline; converting them buys nothing (YAGNI).

- [ ] **Step 4: Mirror the seam in `zephyr/HttpNode.nsl`**

`HttpNode.nsl` is the injected-request twin with the same reply reaction. Apply the equivalent egress change so the two do not drift (per the file's own "Routes match HttpNode.nsl" contract). If `HttpNode.nsl` is only exercised via `httpInject:` (fd == -1), the same begin/chunk/end path works — the injected sink writes to stdout. Keep its routes byte-identical to `HttpServerNode.nsl`.

- [ ] **Step 5: If Task 1 selected fallback (a)**

Only if the spike failed: instead of carrying a block through the Record, have the **router reaction** stream inline — call `platform httpBeginId: id status: 200`, invoke the `…ToSink:` renderer with a sink built right there, then `platform httpEndId: id`, and drop the `reply` channel hop for streamed routes. The `…ToSink:` renderers from Step 3 are reused unchanged; only the reaction wiring differs. Skip this step if (b) was confirmed.

- [ ] **Step 6: Build + run the host round-trip**

Run: `bash tests/http_roundtrip_host.sh`
Expected: `GET /thermal -> 200`, body contains `rgb(`, `GET /nope -> 404`, `http_roundtrip OK`. (curl transparently de-chunks; the visible body is unchanged.)

- [ ] **Step 7: Commit**

```bash
git add zephyr/HttpServerNode.nsl zephyr/HttpNode.nsl
git commit -m "feat(http): stream HttpServerNode renders via sink + body-as-block"
```

---

### Task 5: Integration test — assert chunked encoding over sockets

Tighten `tests/http_roundtrip_host.sh` to prove the response is actually chunked (not just a 200 with a body), locking the behavior against regression.

**Files:**
- Modify: `tests/http_roundtrip_host.sh`

**Interfaces:**
- Consumes: the streamed node (Task 4). Produces: a regression gate on chunked encoding + body integrity.

- [ ] **Step 1: Add a chunked-header assertion to the harness**

In `tests/http_roundtrip_host.sh`, in the `[5/5] drive with curl` block, after the existing `/thermal` 200 check, add a header check using `curl -D` (dump headers):

```bash
curl -s -D /tmp/http_hdrs -o /tmp/http_body "http://127.0.0.1:$PORT/thermal" >/dev/null
grep -qi '^HTTP/1.1 200' /tmp/http_hdrs || { echo "FAIL: /thermal not HTTP/1.1 200"; exit 1; }
grep -qi '^Transfer-Encoding: chunked' /tmp/http_hdrs || { echo "FAIL: /thermal not chunked"; exit 1; }
grep -q "rgb(" /tmp/http_body || { echo "FAIL: /thermal body has no rgb()"; exit 1; }
echo "  /thermal is HTTP/1.1 chunked and renders the heat-grid"
```

- [ ] **Step 2: Add a `/stats` chunked check**

After the `/nope` 404 check, add:

```bash
curl -s -D /tmp/http_hdrs2 -o /tmp/http_body2 "http://127.0.0.1:$PORT/stats" >/dev/null
grep -qi '^Transfer-Encoding: chunked' /tmp/http_hdrs2 || { echo "FAIL: /stats not chunked"; exit 1; }
grep -q "heap high-water" /tmp/http_body2 || { echo "FAIL: /stats missing metrics"; exit 1; }
echo "  /stats chunked + metrics present"
```

- [ ] **Step 3: Run the harness**

Run: `bash tests/http_roundtrip_host.sh`
Expected: the two new lines print, and `http_roundtrip OK` at the end.

- [ ] **Step 4: Commit**

```bash
git add tests/http_roundtrip_host.sh
git commit -m "test(http): assert chunked transfer-encoding on /thermal and /stats"
```

---

### Task 6: On-device measurement + promotion

Flash the streamed node to the WROOM-32, measure the reclaimed heap headroom via `/stats`, and promote the durable result. This is the design's success gate: render high-water must drop materially (target ≥ ~15 KB more arena headroom than the ~0.5 KB baseline).

**Files:**
- Modify: `review/model-capability-findings.md` (replace the "streaming would reclaim ~15 KB (theory)" note with the measured number)
- Modify: `review/program-sequence.md` (rung/close-out entry, if this closes one)
- (Possibly) Modify: `zephyr/run.sh` comment at lines 37-45 with the new observed high-water

**Interfaces:**
- Consumes: the full streamed node. Produces: the measured post-streaming high-water, promoted to docs of record.

- [ ] **Step 1: Build + flash the streamed node (Model B) to the devkit on /dev/ttyUSB0**

Run:
```bash
cd ~/workspace/tuvm/zephyr
bash run.sh --node HttpServerNode.nsl --flash --pristine
```
Expected: `[flash] esptool 0x1000` completes; the WiFi overlay is detected (`[wifi] overlay found`). If the build overflows DRAM at link, STOP — the streamed image should be *smaller* in live-heap need, not larger; investigate before lowering any knob.

- [ ] **Step 2: Capture the assigned IP from the console**

Run (read serial for the boot banner):
```bash
timeout 40 cat /dev/ttyUSB0 | grep -m1 'wifi: up, ip='
```
Expected: a line `wifi: up, ip=<A.B.C.D> -> http://<A.B.C.D>:18080/thermal`. Note the IP as `$DEV`.

- [ ] **Step 3: Drive `/thermal` repeatedly, then read `/stats`**

Run (from a machine on the same LAN as the board):
```bash
DEV=<A.B.C.D>
for i in $(seq 1 20); do curl -s -o /dev/null "http://$DEV:18080/thermal"; done
curl -s "http://$DEV:18080/stats" | grep -A1 -E 'heap high-water|gc cycles'
```
Expected: `heap high-water` well below the 53248 B arena with **≥ ~15 KB spare** (vs the ~0.5 KB baseline), and `gc cycles` far lower per render than the pre-change 300+.

- [ ] **Step 4: Verify the browser render is unchanged**

Run: `curl -s "http://$DEV:18080/thermal" | grep -c '<rect'`
Expected: `64` (the 8×8 grid is intact); and `curl -D - -o /dev/null -s "http://$DEV:18080/thermal"` shows `Transfer-Encoding: chunked`.

- [ ] **Step 5: Promote the measurement to the docs of record**

Edit `review/model-capability-findings.md`: replace the "streaming the render would reclaim ~15 KB (theory)" sentence (~line 203) with the measured before/after high-water and gc-cycle numbers from Step 3, dated 2026-07-18. Add a one-line pointer in `review/program-sequence.md` if this closes a rung. Update `zephyr/run.sh:37-45`'s comment with the new observed high-water so the `OEVM_HEAP_SIZE` rationale stays accurate (note: do NOT lower the knob unless a follow-up decision calls for it).

- [ ] **Step 6: Commit**

```bash
cd ~/workspace/tuvm
git add review/model-capability-findings.md review/program-sequence.md zephyr/run.sh
git commit -m "docs(review): streaming render on-device close-out — measured heap headroom reclaimed"
```

---

## Self-Review

**Spec coverage:** Problem (render peak) → Tasks 4+6. Rejected alternatives → design doc only (no task, correct). Chosen chunked streaming → Tasks 2-4. §1 three primitives → Task 3. §2 C refactor + send_all backpressure reuse → Task 2 (Steps 7-8). §3 body-as-block contract → Task 4. §4 feasibility gate → Task 1 (with (a) fallback in Task 4 Step 5). Measurement via /stats → Task 6. Promotion targets → Task 6 Step 5. All covered.

**Placeholder scan:** No TBD/TODO. The one soft spot is the `Sink` construction idiom in Task 4 Step 1 (the repo's exact `new`/`init:` convention may differ) — flagged explicitly with the behavioral contract stated so the implementer can match local style; not a silent placeholder.

**Type consistency:** prim names `httpBeginId:status:` / `httpChunkId:data:` / `httpEndId:` identical across dispatch `PRIM_NAMES`, platform.c comments, kernel forwarders, and the nsl call sites. C symbols `http_begin`/`http_chunk`/`http_end`, `http_write`/`http_close` consistent between `http_backend.h`, `http_core.c`, both backends, and Task 3's platform handlers. Sink protocol `<<` consistent between the helper and every renderer.
