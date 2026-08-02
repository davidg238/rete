# R6 — ns-zephyr net capability on hardware Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run the R4 UDP networking capability and the Model B `.oimg` loader on Zephyr — swapping the POSIX backend for a Zephyr backend — and round-trip a UDP datagram from a Newspeak program on the resident oevm VM, on native_sim (must-have) then the nRF52840 dongle over Thread (stretch).

**Architecture:** The R4 backend seam (`udp_backend.h`, 6 functions, string addresses, backend owns the family) plus the loader's offset-based, never-mutated, XIP-safe blob make this a backend swap, not a rewrite. S0 splits a no-POSIX `oimg_load_blob` entry out of `oimg_loader.c`. S1 adds three tiny Zephyr backends (`udp/platform/serial_zephyr.c`) — Model B links the full Path B set, so platform+serial backends are needed alongside udp. S2 builds a Model B variant in ns-zephyr for native_sim and runs `42_udp.ns` over Zephyr's loopback. S3 retargets the same image to the dongle over OpenThread.

**Tech Stack:** C99, oevm VM, Zephyr RTOS (`zsock_*` BSD sockets), nRF Connect SDK / west, native_sim, OpenThread, Python (host-side checks).

## Global Constraints

- **`.oimg` format stays format_version 6** — no format/header-layout change in R6 (copied from spec: "the `.oimg` format / header layout (stays format_version 6)").
- **Facade/prims unchanged** — `udp.c`, the `Net`/`Socket`/`Datagram` facade, the capability grant, the compiler/serializer are NOT modified. Only backend `.c` files and Zephyr build wiring are new.
- **Backend seam contracts are fixed** — implement `udp_backend.h` / `platform_backend.h` / `serial_backend.h` exactly; do not change the headers.
- **Desktop suite must stay 72/0/4 on both paths** after S0 (the loader split is behavior-preserving).
- **Address-family-agnostic** — addresses are plain strings; the backend owns the family (IPv6-first on Zephyr).
- **Repos:** `oevm` (S0, S1), `ns-zephyr` (S2, S3), `ns-tests` (reuse `04_somns_basic/42_udp.ns`). `ns-oevm` unchanged.
- **Commit-per-task**, end commit messages with the Co-Authored-By trailer for Claude Opus 4.8 (1M context).

---

## File Structure

| File | Repo | Responsibility |
|---|---|---|
| `src/oimg.h` | oevm | add `oimg_load_blob` declaration (Task 1) |
| `src/oimg_loader.c` | oevm | split file-IO (`oimg_load`) from byte-parse (`oimg_load_blob`); guard POSIX includes (Task 1) |
| `tests/test_oimg_blob.sh` | oevm | desktop gate: `oimg_load_blob` (fread, no mmap) matches `oimg_load` output (Task 1) |
| `src/backend/zephyr/udp_zephyr.c` | oevm | UDP seam over `zsock_*` (Task 2) |
| `src/backend/zephyr/platform_zephyr.c` | oevm | console seam over Zephyr console (Task 3) |
| `src/backend/zephyr/serial_zephyr.c` | oevm | serial seam — loopback mock (Task 3) |
| `app/src/main_b.c` | ns-zephyr | Model B entry: `kernel_boot` → `oimg_load_blob` → `oimg_bootstrap` (Task 4) |
| `tools/gen_oimg_blob.py` | ns-zephyr | `.oimg` → `app/src/oimg_image.c` (`const uint8_t[]`) (Task 4) |
| `app/CMakeLists.txt` | ns-zephyr | Model B source list, selected by `OEVM_MODEL=B` (Task 4) |
| `app/prj_nativesim.conf` | ns-zephyr | net Kconfig for native_sim (Task 5) |
| `tools/build_ns_node.sh` | ns-zephyr | build Model B for a given board + program (Task 5) |
| `app/prj_thread.conf` + `app/boards/` | ns-zephyr | OpenThread Kconfig for the dongle (Task 6) |

---

## Task 1: Extract `oimg_load_blob` (S0, oevm, desktop-TDD)

**Files:**
- Modify: `oevm/src/oimg.h` (add declaration near the other `oimg_*` externs)
- Modify: `oevm/src/oimg_loader.c:104-171` (split `oimg_load`; guard POSIX includes lines 8-11)
- Test: `oevm/tests/test_oimg_blob.sh` (new)

**Interfaces:**
- Produces: `int oimg_load_blob(const uint8_t *base, size_t size)` — validates and sets `oimg_blob_base`/`oimg_blob_end` + the static `img_*_ref` header fields; returns 0 on success, non-zero on bad magic/version/registry/size. Does NOT own `base` (never frees/unmaps it).
- Produces: `int oimg_load(const char *path)` — unchanged signature/behavior; now a thin mmap wrapper over `oimg_load_blob`. Compiled only when `OEVM_NO_FILE_IO` is undefined.
- Consumes (Task 4): `oimg_load_blob`, `oimg_root`, `oimg_bootstrap` (existing) from the Zephyr main.

- [ ] **Step 1: Write the failing desktop gate test**

Create `oevm/tests/test_oimg_blob.sh`. It builds a tiny alternate main that loads a `.oimg` via `fread` into a heap buffer (NOT mmap) and calls `oimg_load_blob`, then compares output to the canonical `oevm_img` (mmap path) for the same image.

```bash
#!/usr/bin/env bash
# Gate: oimg_load_blob (fread, no mmap) produces identical output to oimg_load
# (mmap) for the same .oimg. Proves the S0 extraction is byte-acquisition-only.
set -euo pipefail
cd "$(dirname "$0")/.."          # oevm repo root

PROG="${1:-../ns-tests/01_expressions/01_addition.ns}"
OIMG=/tmp/test_oimg_blob.oimg
python3 ../ns-oevm/tools/ns_compile_oevm.py "$PROG" "$OIMG" \
  --format=oimg --layout=/tmp/oevm_layout.json

BACKEND="src/backend/posix"
COMMON="src/vm.c src/gc.c src/dispatch.c src/platform.c src/serial.c src/udp.c \
src/mirror.c $BACKEND/platform_posix.c $BACKEND/serial_posix.c $BACKEND/udp_posix.c \
src/oimg_loader.c src/kernel_boot.c"

# Canonical mmap path.
gcc -std=c99 -Isrc $COMMON oevm_image_main.c -o /tmp/oevm_img_mmap -lm
# Blob path: a fread-based main exercising oimg_load_blob.
cat > /tmp/oevm_img_blob_main.c <<'EOF'
#include "src/oevm.h"
#include <stdio.h>
#include <stdlib.h>
int  oimg_load_blob(const uint8_t *base, size_t size);
oop  oimg_root(void);
oop  oimg_bootstrap(oop entry_vtable);
extern void kernel_boot(void);
extern void gc_set_stack_base(void *p);
int main(int argc, char **argv) {
    int anchor; gc_set_stack_base(&anchor);
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb"); if (!f) return 1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) return 1;
    fclose(f);
    kernel_boot();
    if (oimg_load_blob(buf, (size_t)sz) != 0) { fprintf(stderr, "load_blob failed\n"); return 1; }
    oimg_bootstrap(oimg_root());
    return 0;
}
EOF
gcc -std=c99 -Isrc $COMMON /tmp/oevm_img_blob_main.c -o /tmp/oevm_img_blob -lm

OUT_MMAP=$(/tmp/oevm_img_mmap "$OIMG")
OUT_BLOB=$(/tmp/oevm_img_blob "$OIMG")
if [ "$OUT_MMAP" != "$OUT_BLOB" ]; then
  echo "FAIL: mmap='$OUT_MMAP' blob='$OUT_BLOB'"; exit 1
fi
echo "PASS: oimg_load_blob matches oimg_load ('$OUT_BLOB')"
```

- [ ] **Step 2: Run to verify it fails**

```bash
chmod +x oevm/tests/test_oimg_blob.sh && oevm/tests/test_oimg_blob.sh
```
Expected: link error — `undefined reference to 'oimg_load_blob'` (the symbol does not exist yet).

- [ ] **Step 3: Add the declaration to `oimg.h`**

In `oevm/src/oimg.h`, after the `extern oop oimg_singleton[3];` line (line 10), add:

```c
/* S0 (R6): no-POSIX byte-array loader for embedded targets. Validates and
   sets oimg_blob_base/end + the header refs from an in-memory image; does not
   own `base`. oimg_load(path) is a mmap wrapper over this (desktop only). */
int oimg_load_blob(const uint8_t *base, size_t size);
```

- [ ] **Step 4: Split `oimg_loader.c`**

Guard the POSIX includes (lines 8-11) so the TU compiles for Zephyr:

```c
#ifndef OEVM_NO_FILE_IO
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif
```

Replace the whole `int oimg_load(const char *path) { ... }` function (lines 104-171) with the split below. `oimg_load_blob` holds everything from the magic check onward; `oimg_load` is the mmap wrapper, compiled only when file IO is available:

```c
int oimg_load_blob(const uint8_t *base, size_t size) {
    if (size < OIMG_PREFIX_FIXED) {
        fprintf(stderr, "oimg_load_blob: image too small (%zu bytes)\n", size);
        return 1;
    }
    if (memcmp(base, "OIMG", 4) != 0) {
        fprintf(stderr, "oimg_load_blob: bad magic\n"); return 1;
    }
    uint16_t format_version   = read_u16(base + 4);
    uint32_t registry_version = read_u32(base + 8);
    uint32_t blob_size        = read_u32(base + 12);
    if (format_version < 3) {
        fprintf(stderr, "oimg_load_blob: unsupported format_version %u (expected >= 3)\n", format_version);
        return 1;
    }
    if (registry_version != BUILTIN_REGISTRY_VERSION) {
        fprintf(stderr, "oimg_load_blob: registry_version %u != firmware %u\n",
                registry_version, BUILTIN_REGISTRY_VERSION);
        return 1;
    }
    if (blob_size != size) {
        fprintf(stderr, "oimg_load_blob: blob_size %u != image size %zu\n", blob_size, size);
        return 1;
    }

    size_t W = sizeof(oop);
    size_t a = ALIGN_UP(OIMG_PREFIX_FIXED, W);
    img_root_ref          = read_word(base + a);
    img_init_sel_ref      = read_word(base + a + W);
    img_main_sel_ref      = read_word(base + a + 2 * W);
    img_init_argc         = read_u16(base + a + 3 * W);
    img_entry_nslots      = read_u16(base + a + 3 * W + 2);
    img_kernel_vtable_ref   = (format_version >= 3) ? read_word(base + a + 4 * W) : 0;
    img_kernel_init_sel_ref = (format_version >= 3) ? read_word(base + a + 5 * W) : 0;
    img_kernel_nslots       = (format_version >= 3) ? read_u16 (base + a + 6 * W) : 0;
    img_serial_on_sel_ref   = (format_version >= 4) ? read_word(base + a + 7 * W) : 0;
    img_supervisor_vtable_ref   = (format_version >= 5) ? read_word(base + a + 8 * W) : 0;
    img_supervisor_init_sel_ref = (format_version >= 5) ? read_word(base + a + 9 * W) : 0;
    img_supervisor_run_sel_ref  = (format_version >= 5) ? read_word(base + a + 10 * W) : 0;
    img_supervisor_nslots       = (format_version >= 5) ? read_u16 (base + a + 11 * W) : 0;
    img_net_on_sel_ref = (format_version >= 6) ? read_word(base + a + 12 * W) : 0;

    oimg_blob_base = base;
    oimg_blob_end  = base + blob_size;
    return 0;
}

#ifndef OEVM_NO_FILE_IO
int oimg_load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("oimg_load: open"); return 1; }
    struct stat st;
    if (fstat(fd, &st) != 0) { perror("oimg_load: fstat"); close(fd); return 1; }
    size_t sz = (size_t)st.st_size;
    void *map = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) { perror("oimg_load: mmap"); return 1; }
    int rc = oimg_load_blob((const uint8_t *)map, sz);
    if (rc != 0) munmap(map, sz);   /* keep the mapping for the process lifetime on success */
    return rc;
}
#endif
```

Note: `read_word`/`read_u32`/`read_u16`, the `img_*` statics, `OIMG_PREFIX_FIXED`, and `ALIGN_UP` already exist above this point in the file — no change to them.

- [ ] **Step 5: Run the blob gate — verify it passes**

```bash
oevm/tests/test_oimg_blob.sh
```
Expected: `PASS: oimg_load_blob matches oimg_load ('7')`

- [ ] **Step 6: Run the full desktop suite — verify no regression**

```bash
cd ns-oevm && python3 tools/run_tests_oevm.py
```
Expected: `72/0/4` on both Path A and Path B (the loader split is behavior-preserving).

- [ ] **Step 7: Commit**

```bash
cd oevm && git add src/oimg.h src/oimg_loader.c tests/test_oimg_blob.sh
git commit -m "feat(oimg): split oimg_load_blob (no-POSIX byte-array loader) from oimg_load

S0 of R6. oimg_load_blob validates + sets blob_base/end from an in-memory
image; oimg_load(path) becomes a mmap wrapper over it, compiled only when
OEVM_NO_FILE_IO is undefined. Embedded (Zephyr) targets call oimg_load_blob
on a flash array. Desktop suite 72/0/4 both paths; new test_oimg_blob.sh
proves fread-path parity.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: `udp_zephyr.c` — UDP seam over zsock (S1, oevm)

**Files:**
- Create: `oevm/src/backend/zephyr/udp_zephyr.c`

**Interfaces:**
- Consumes: `udp_backend.h` (`udp_open`, `udp_socket_bind`, `udp_send`, `udp_receive`, `udp_close`).
- Produces: the linkable Zephyr impl of those 6 functions (selected when `OEVM_TARGET=zephyr`).

This file cannot be compiled on the desktop (no Zephyr headers); its gate is the S2 build (Task 5). Match the `udp_posix.c` contract exactly: AF_INET6, non-blocking receive, plain-string numeric addresses, v4-dotted mapped to v4-mapped IPv6.

- [ ] **Step 1: Write the backend**

```c
/* oevm/src/backend/zephyr/udp_zephyr.c
   Zephyr UDP backend: real datagram sockets over the Zephyr net stack. AF_INET6
   (IPv6-first, matching the Thread/6LoWPAN mesh); one socket, dual-use where the
   L2 supports v4-mapped. Non-blocking via ZSOCK_MSG_DONTWAIT on recv. On
   native_sim this talks to the host/loopback; on the nRF52840 it rides
   OpenThread. Replaces udp_posix.c on device — one file, both Zephyr targets.
   (API names target Zephyr/NCS BSD sockets; adjust to the installed SDK if a
   symbol differs — the S2 build is the gate.) */
#include "udp_backend.h"
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <string.h>

void udp_open(void) { /* net stack is brought up by Kconfig/boot */ }

int udp_socket_bind(int port) {
    int fd = zsock_socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof a);
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_any;
    a.sin6_port = htons((unsigned short)port);
    if (zsock_bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { zsock_close(fd); return -1; }
    return fd;
}

int udp_send(int sock, const char *host, int port, const uint8_t *data, int len) {
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof a);
    a.sin6_family = AF_INET6;
    a.sin6_port = htons((unsigned short)port);
    if (zsock_inet_pton(AF_INET6, host, &a.sin6_addr) != 1) {
        struct in_addr v4;                       /* accept a v4 dotted literal */
        if (zsock_inet_pton(AF_INET, host, &v4) != 1) return -1;
        memset(&a.sin6_addr, 0, sizeof a.sin6_addr);   /* build ::ffff:a.b.c.d */
        a.sin6_addr.s6_addr[10] = 0xff;
        a.sin6_addr.s6_addr[11] = 0xff;
        memcpy(&a.sin6_addr.s6_addr[12], &v4, 4);
    }
    ssize_t n = zsock_sendto(sock, data, (size_t)len, 0,
                             (struct sockaddr *)&a, sizeof a);
    return (int)n;
}

int udp_receive(int sock, uint8_t *buf, int buf_cap,
                char *host_out, int host_cap, int *sender_port) {
    struct sockaddr_in6 src;
    socklen_t slen = sizeof src;
    ssize_t n = zsock_recvfrom(sock, buf, (size_t)buf_cap, ZSOCK_MSG_DONTWAIT,
                               (struct sockaddr *)&src, &slen);
    if (n < 0) return -1;                          /* EWOULDBLOCK -> nothing queued */
    *sender_port = ntohs(src.sin6_port);
    if (net_addr_ntop(AF_INET6, &src.sin6_addr, host_out, host_cap) == NULL)
        host_out[0] = '\0';
    /* normalize a v4-mapped sender ::ffff:1.2.3.4 -> 1.2.3.4 (best-effort) */
    if (host_out[0] && IN6_IS_ADDR_V4MAPPED(&src.sin6_addr)) {
        const char *p = strrchr(host_out, ':');
        if (p) memmove(host_out, p + 1, strlen(p + 1) + 1);
    }
    return (int)n;
}

void udp_close(int sock) { if (sock >= 0) zsock_close(sock); }
```

- [ ] **Step 2: Commit** (compile gate deferred to S2 / Task 5)

```bash
cd oevm && git add src/backend/zephyr/udp_zephyr.c
git commit -m "feat(udp): Zephyr UDP backend (udp_zephyr.c) over zsock sockets

S1 of R6. Implements the udp_backend.h seam against Zephyr BSD sockets;
AF_INET6 IPv6-first, ZSOCK_MSG_DONTWAIT non-blocking receive, v4-dotted
mapped to v4-mapped IPv6 — same contract as udp_posix.c. Selected when
OEVM_TARGET=zephyr; one file serves native_sim and the dongle. Compile
gate is the S2 ns-zephyr build.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: `platform_zephyr.c` + `serial_zephyr.c` (S1, oevm)

**Files:**
- Create: `oevm/src/backend/zephyr/platform_zephyr.c`
- Create: `oevm/src/backend/zephyr/serial_zephyr.c`

**Interfaces:**
- Consumes: `platform_backend.h` (`platform_console_write`, `platform_console_flush`, `platform_debug_readline`), `serial_backend.h` (`serial_open`, `serial_read_byte`, `serial_write_byte`, `serial_at_end`).
- Produces: the Zephyr impls — needed because Model B links the full Path B set (`platform.c`/`serial.c`), unlike Model A.

- [ ] **Step 1: Write `platform_zephyr.c`**

```c
/* oevm/src/backend/zephyr/platform_zephyr.c
   Zephyr console backend for the platform print:/printNewline: prims. Writes to
   the Zephyr console (printk-backed; the USB-CDC ACM console on the dongle, the
   native_sim stdout otherwise). printf is fine — newlib stdout is wired to the
   console by CONFIG_STDOUT_CONSOLE; ns-zephyr's main sets _IONBF so each line
   appears as produced. platform_debug_readline is a stub: the R3 debug transport
   is out of scope for R6 (no keeper on device yet). */
#include "platform_backend.h"
#include <stdio.h>

void platform_console_write(const char *bytes, size_t n) {
    if (n) fwrite(bytes, 1, n, stdout);
}
void platform_console_flush(void) {
    fflush(stdout);
}
int platform_debug_readline(char *buf, int cap) {
    (void)buf; (void)cap;
    return -1;   /* no interactive debug transport on the R6 node */
}
```

- [ ] **Step 2: Write `serial_zephyr.c`**

```c
/* oevm/src/backend/zephyr/serial_zephyr.c
   Zephyr serial backend. R6's net-capability program never exercises serial, but
   serial.c is in Model B's link set, so this satisfies the link. Same loopback
   byte FIFO as serial_posix.c (target-neutral pure C). A real UART backend can
   replace this in a later rung when a serial capability is demoed on device. */
#include "serial_backend.h"
#include <stddef.h>
#define SERIAL_FIFO_CAP 4096
static unsigned char fifo[SERIAL_FIFO_CAP];
static size_t head = 0, tail = 0, count = 0;
void serial_open(void) { head = tail = count = 0; }
void serial_write_byte(int b) {
    if (count == SERIAL_FIFO_CAP) { head = (head + 1) % SERIAL_FIFO_CAP; count--; }
    fifo[tail] = (unsigned char)(b & 0xff);
    tail = (tail + 1) % SERIAL_FIFO_CAP; count++;
}
int serial_read_byte(void) {
    if (count == 0) return -1;
    int b = fifo[head];
    head = (head + 1) % SERIAL_FIFO_CAP; count--;
    return b;
}
bool serial_at_end(void) { return count == 0; }
```

- [ ] **Step 3: Commit** (compile gate deferred to S2 / Task 5)

```bash
cd oevm && git add src/backend/zephyr/platform_zephyr.c src/backend/zephyr/serial_zephyr.c
git commit -m "feat(backend): Zephyr platform + serial backends for Model B link

S1 of R6. Model B links the full Path B set, so platform.c/serial.c need
zephyr backends alongside udp. platform_zephyr.c writes the print prims to
the Zephyr console (debug_readline stubbed -1, no on-device debug transport
yet); serial_zephyr.c is the loopback mock (net program never uses serial).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Model B build wiring in ns-zephyr (S2 part 1)

**Files:**
- Create: `ns-zephyr/tools/gen_oimg_blob.py`
- Create: `ns-zephyr/app/src/main_b.c`
- Modify: `ns-zephyr/app/CMakeLists.txt` (add an `OEVM_MODEL=B` branch)

**Interfaces:**
- Consumes: `oimg_load_blob`, `oimg_root`, `oimg_bootstrap`, `kernel_boot`, `gc_set_stack_base` (Tasks 1 + existing); the three zephyr backends (Tasks 2-3).
- Produces: a `west`-buildable Model B app where `app/src/oimg_image.c` defines `const uint8_t oimg_image[]` + `const size_t oimg_image_len`.

- [ ] **Step 1: Write the `.oimg` → C embedder**

```python
#!/usr/bin/env python3
# ns-zephyr/tools/gen_oimg_blob.py — embed a .oimg as a C const array.
# Usage: gen_oimg_blob.py <in.oimg> <out.c>
import sys

def main():
    if len(sys.argv) != 3:
        sys.exit("usage: gen_oimg_blob.py <in.oimg> <out.c>")
    data = open(sys.argv[1], "rb").read()
    with open(sys.argv[2], "w") as f:
        f.write("/* generated by tools/gen_oimg_blob.py — do not edit */\n")
        f.write("#include <stdint.h>\n#include <stddef.h>\n")
        # 8-byte aligned so the loader's W-strided word reads are aligned on the
        # nRF52840 (Cortex-M4 tolerates unaligned, but keep it clean / XIP-safe).
        f.write("const uint8_t oimg_image[] __attribute__((aligned(8))) = {\n")
        for i in range(0, len(data), 12):
            row = ", ".join("0x%02x" % b for b in data[i:i+12])
            f.write("  " + row + ",\n")
        f.write("};\n")
        f.write("const size_t oimg_image_len = sizeof(oimg_image);\n")

if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Write the Model B main**

```c
/* ns-zephyr/app/src/main_b.c — Model B entry: load the embedded .oimg blob and
   bootstrap it on the resident oevm VM. Mirrors oevm/oevm_image_main.c, but the
   image is a flash array (XIP, deserialized in place) instead of a mmap'd file. */
#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdio.h>
#include "oevm.h"
#include "oimg.h"

extern const uint8_t oimg_image[];
extern const size_t  oimg_image_len;

int  oimg_load_blob(const uint8_t *base, size_t size);
oop  oimg_root(void);
oop  oimg_bootstrap(oop entry_vtable);
extern void kernel_boot(void);
extern void gc_set_stack_base(void *p);
extern uint64_t (*oevm_millis)(void);
extern void (*gc_additional_roots_hook)(void);
static uint64_t zephyr_millis(void) { return (uint64_t)k_uptime_get(); }
static void roots_cb(void) {}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    k_sleep(K_SECONDS(2));            /* USB enumeration + line-ctrl settle */
    printf("=== ns-zephyr: OEVM Model B node ===\n");
    fflush(stdout);

    int stack_anchor;
    gc_set_stack_base(&stack_anchor);
    oevm_millis = zephyr_millis;
    gc_additional_roots_hook = roots_cb;

    kernel_boot();
    if (oimg_load_blob(oimg_image, oimg_image_len) != 0) {
        printf("=== FATAL: oimg_load_blob failed ===\n");
        fflush(stdout);
        for (;;) k_sleep(K_FOREVER);
    }
    oimg_bootstrap(oimg_root());

    printf("=== OEVM done ===\n");
    fflush(stdout);
    for (;;) k_sleep(K_FOREVER);
}
```

- [ ] **Step 3: Add the Model B branch to `app/CMakeLists.txt`**

After the existing `target_sources(app PRIVATE ... )` block (line 12-18), wrap source selection in a model switch. Replace lines 12-19 with:

```cmake
if(DEFINED ENV{OEVM_MODEL})
  set(OEVM_MODEL $ENV{OEVM_MODEL})
elseif(NOT DEFINED OEVM_MODEL)
  set(OEVM_MODEL "A")
endif()

set(OEVM_BACKEND "${OEVM_SRC}/backend/zephyr")

if(OEVM_MODEL STREQUAL "B")
  # Model B: resident VM loads an embedded .oimg blob (the primary deployment path).
  target_sources(app PRIVATE
    src/main_b.c
    src/oimg_image.c
    ${OEVM_SRC}/vm.c
    ${OEVM_SRC}/gc.c
    ${OEVM_SRC}/dispatch.c
    ${OEVM_SRC}/platform.c
    ${OEVM_SRC}/serial.c
    ${OEVM_SRC}/udp.c
    ${OEVM_SRC}/mirror.c
    ${OEVM_SRC}/oimg_loader.c
    ${OEVM_SRC}/kernel_boot.c
    ${OEVM_BACKEND}/platform_zephyr.c
    ${OEVM_BACKEND}/serial_zephyr.c
    ${OEVM_BACKEND}/udp_zephyr.c
  )
else()
  # Model A: baked solidified image appliance.
  target_sources(app PRIVATE
    src/main.c
    src/solidified_image.c
    ${OEVM_SRC}/vm.c
    ${OEVM_SRC}/gc.c
    ${OEVM_SRC}/dispatch.c
  )
endif()
target_include_directories(app PRIVATE ${OEVM_SRC})
```

Then make `OEVM_NO_FILE_IO` always defined for the Zephyr build, and use `OEVM_APPLIANCE` only for Model A. Replace the `set(OEVM_DEFS OEVM_APPLIANCE OEVM_HEAP_SIZE=...)` line (line 31) with:

```cmake
set(OEVM_DEFS OEVM_HEAP_SIZE=${OEVM_HEAP_SIZE} OEVM_NO_FILE_IO)
if(OEVM_MODEL STREQUAL "A")
  list(APPEND OEVM_DEFS OEVM_APPLIANCE)
endif()
```

- [ ] **Step 4: Commit**

```bash
cd ns-zephyr && git add tools/gen_oimg_blob.py app/src/main_b.c app/CMakeLists.txt
git commit -m "build(node): Model B build wiring (OEVM_MODEL=B) + .oimg embedder

S2 of R6. gen_oimg_blob.py embeds a .oimg as a const flash array; main_b.c
loads it via oimg_load_blob (XIP, in place) and bootstraps the resident VM;
CMakeLists gains an OEVM_MODEL=B branch linking the full Path B set + the
three zephyr backends, with OEVM_NO_FILE_IO defined for Zephyr.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: native_sim Model B run — the de-risked proof (S2 part 2)

**Files:**
- Create: `ns-zephyr/app/prj_nativesim.conf`
- Create: `ns-zephyr/tools/build_ns_node.sh`

**Interfaces:**
- Consumes: Tasks 1-4. Test program: `../ns-tests/04_somns_basic/42_udp.ns` (existing R4 loopback round-trip — runs unmodified).

This is R6's primary acceptance test: real Zephyr net stack + real Model B loader + real net cap, no hardware.

- [ ] **Step 1: Write the native_sim net Kconfig**

```ini
# ns-zephyr/app/prj_nativesim.conf — Model B + UDP on native_sim.
# Console: native_sim uses the posix console (stdout); no USB stack needed.
CONFIG_CONSOLE=y
CONFIG_POSIX_API=y

# libc with malloc/realloc/free, setjmp, snprintf, FP printf for OEVM.
CONFIG_NEWLIB_LIBC=y
CONFIG_STDOUT_CONSOLE=y
CONFIG_NEWLIB_LIBC_FLOAT_PRINTF=y
CONFIG_CBPRINTF_FP_SUPPORT=y
CONFIG_HEAP_MEM_POOL_SIZE=65536

CONFIG_MAIN_STACK_SIZE=16384

# Networking: IPv6 UDP sockets + a loopback interface so 42_udp.ns
# (sends to 127.0.0.1 and ::1, receives locally) round-trips on-device.
CONFIG_NETWORKING=y
CONFIG_NET_IPV6=y
CONFIG_NET_IPV4=y
CONFIG_NET_UDP=y
CONFIG_NET_SOCKETS=y
CONFIG_NET_LOOPBACK=y
CONFIG_NET_CONFIG_SETTINGS=y
CONFIG_NET_CONFIG_NEED_IPV6=y
CONFIG_NET_CONFIG_MY_IPV6_ADDR="::1"
CONFIG_NET_MAX_CONTEXTS=8
CONFIG_NET_PKT_RX_COUNT=16
CONFIG_NET_PKT_TX_COUNT=16
CONFIG_NET_BUF_RX_COUNT=32
CONFIG_NET_BUF_TX_COUNT=32
```

- [ ] **Step 2: Write the node build script**

```bash
#!/usr/bin/env bash
# ns-zephyr/tools/build_ns_node.sh — build a Model B node for <board> running
# <prog.ns>. Usage: build_ns_node.sh <prog-rel-to-ns-tests> <board> <conf> [heap]
set -euo pipefail
PROG="${1:?usage: build_ns_node.sh <prog.ns> <board> <conf-file> [heap]}"
BOARD="${2:?board required, e.g. native_sim or nrf52840dongle/nrf52840}"
CONF="${3:?conf file required, e.g. prj_nativesim.conf}"
HEAP="${4:-65536}"

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$SCRIPT_DIR/.." && pwd)
WS=$(cd "$REPO/.." && pwd)
OEVM_SRC="${OEVM_SRC:-$WS/oevm/src}"
NS_COMPILER="${NS_COMPILER:-$WS/ns-oevm/tools/ns_compile_oevm.py}"
NS_TESTS="${NS_TESTS:-$WS/ns-tests}"
source "$SCRIPT_DIR/env.sh"

PROG_PATH="$NS_TESTS/$PROG"
[ -f "$PROG_PATH" ] || { echo "ERROR: program not found: $PROG_PATH" >&2; exit 1; }

echo ">> compiling $PROG -> .oimg"
python3 "$NS_COMPILER" "$PROG_PATH" /tmp/ns_node.oimg \
  --format=oimg --layout=/tmp/ns_node_layout.json
echo ">> embedding .oimg -> app/src/oimg_image.c"
python3 "$SCRIPT_DIR/gen_oimg_blob.py" /tmp/ns_node.oimg "$REPO/app/src/oimg_image.c"

export OEVM_SRC OEVM_MODEL=B OEVM_HEAP_SIZE="$HEAP"
export METHOD_CACHE_SIZE="${METHOD_CACHE_SIZE:-512}"
export PRIM_CACHE_SIZE="${PRIM_CACHE_SIZE:-512}"

echo ">> west build -b $BOARD (Model B, $CONF)"
cd "$REPO/app"
west build -b "$BOARD" -p always -- -DCONF_FILE="$CONF"
```

- [ ] **Step 3: Build for native_sim — verify it compiles**

```bash
cd ns-zephyr && tools/build_ns_node.sh 04_somns_basic/42_udp.ns native_sim prj_nativesim.conf
```
Expected: `west build` completes; `app/build/.../zephyr/zephyr.exe` produced. (This is the compile gate for Tasks 2 and 3 — if a `zsock_*`/console symbol name differs on the installed SDK, fix it here and amend the relevant Task 2/3 commit.)

- [ ] **Step 4: Run native_sim — verify the round-trip**

```bash
cd ns-zephyr && ./app/build/*/zephyr/zephyr.exe
```
Expected output includes:
```
=== ns-zephyr: OEVM Model B node ===
UDP OK
=== OEVM done ===
```
`UDP OK` is the proof: `42_udp.ns` bound a socket, sent to `127.0.0.1` and `::1`, received its own datagram back, and confirmed `data = 'ping'` — through Zephyr's real UDP stack, the Model B loader, and the net capability.

- [ ] **Step 5: Commit**

```bash
cd ns-zephyr && git add app/prj_nativesim.conf tools/build_ns_node.sh
git commit -m "feat(node): native_sim Model B node — UDP round-trip on Zephyr net stack

S2 of R6 (the de-risked proof). prj_nativesim.conf enables IPv6 UDP sockets +
loopback; build_ns_node.sh compiles 42_udp.ns to .oimg, embeds it, and builds
Model B for native_sim. Running the exe prints 'UDP OK' — the R4 net
capability + Model B loader proven on Zephyr's real net stack, no hardware.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: nRF52840 dongle over Thread (S3, stretch)

**Files:**
- Create: `ns-zephyr/app/prj_thread.conf`
- Create: `ns-zephyr/docs/r6-thread-bringup.md` (rig + results log)
- Modify: `ns-zephyr/docs/results.md` (add the R6 row)

**Interfaces:**
- Consumes: Tasks 1-5. Counterpart: a 2nd Thread node or border router echoing UDP datagrams (rig documented in the new doc).

> **Stretch — RAM-gated.** OpenThread is ~tens of KB RAM/flash on a board already ~79% full with the bare VM. If it does not fit, S2 (native_sim) stands as R6's proof and this task is recorded as documented future work — see the fallback step.

- [ ] **Step 1: Write the Thread Kconfig**

```ini
# ns-zephyr/app/prj_thread.conf — Model B + UDP over OpenThread on nRF52840.
# USB-CDC ACM console (verbatim from the Model A appliance config).
CONFIG_USB_DEVICE_STACK_NEXT=y
CONFIG_USBD_CDC_ACM_CLASS=y
CONFIG_CONSOLE=y
CONFIG_UART_CONSOLE=y
CONFIG_UART_LINE_CTRL=y

CONFIG_NEWLIB_LIBC=y
CONFIG_STDOUT_CONSOLE=y
CONFIG_NEWLIB_LIBC_FLOAT_PRINTF=y
CONFIG_CBPRINTF_FP_SUPPORT=y
CONFIG_HEAP_MEM_POOL_SIZE=16384
CONFIG_MAIN_STACK_SIZE=8192
CONFIG_FPU=y

# Networking: IPv6 UDP sockets over OpenThread (IEEE 802.15.4 mesh).
CONFIG_NETWORKING=y
CONFIG_NET_IPV6=y
CONFIG_NET_UDP=y
CONFIG_NET_SOCKETS=y
CONFIG_NET_L2_OPENTHREAD=y
CONFIG_OPENTHREAD_FTD=y
CONFIG_OPENTHREAD_MANUAL_START=n
CONFIG_NET_CONFIG_SETTINGS=y
CONFIG_NET_MAX_CONTEXTS=6
CONFIG_NET_PKT_RX_COUNT=8
CONFIG_NET_PKT_TX_COUNT=8
```

- [ ] **Step 2: Add a Thread echo program to ns-tests**

The loopback test (`42_udp.ns`) proves the stack but not radio traffic. Add `ns-tests/04_somns_basic/44_udp_thread.ns` that sends to the counterpart's mesh-local IPv6 address and awaits the echo. Replace `MESH_PEER_ADDR` with the counterpart's address before building (documented in the bring-up doc).

```smalltalk
(*
UDP over Thread — sends a datagram to a mesh peer and awaits its echo. R6 S3.
expect:
THREAD UDP OK
*)
class UdpThreadTest usingPlatform: platform net: net = () (
  public main: platform2 args: args = (
    | rx tx got |
    rx:: net udpBindPort: 54331.
    rx == nil ifTrue: [ ^ self fail: 'bind' ].
    tx:: net udp.
    tx sendTo: 'MESH_PEER_ADDR' port: 54331 data: 'ping'.
    got:: nil.
    [ got == nil ] whileTrue: [ got:: rx receive ].
    rx close. tx close.
    (got data = 'ping') ifFalse: [ ^ self fail: 'echo mismatch' ].
    'THREAD UDP OK' println.
    ^ 0
  )
  fail: msg = ( msg println. ^ 1 )
) : (
  public packageUsing: manifest = ( ^ self new )
)
```

- [ ] **Step 3: Build for the dongle — verify it links and fits RAM**

```bash
cd ns-zephyr && tools/build_ns_node.sh 04_somns_basic/44_udp_thread.ns \
  nrf52840dongle/nrf52840 prj_thread.conf 32768
```
Expected: build completes and the RAM report fits 256 KB. **If it overflows RAM/flash:** go to Step 6 (fallback). Try first: lower `OEVM_HEAP_SIZE` (e.g. 24576), keep caches at 512, drop `CONFIG_OPENTHREAD_FTD` to MTD (`CONFIG_OPENTHREAD_MTD=y`).

- [ ] **Step 4: Flash and run the round-trip**

Bring up the counterpart (border router or 2nd Thread node on the same channel/PAN — see the bring-up doc), then:

```bash
cd ns-zephyr/app && \
nrfutil pkg generate --hw-version 52 --sd-req 0x00 \
  --application build/*/zephyr/zephyr.hex --application-version 1 /tmp/ns_node_dfu.zip && \
nrfutil nrf5sdk-tools dfu usb-serial -pkg /tmp/ns_node_dfu.zip -p /dev/ttyACM0 && \
cat /dev/ttyACM0
```
Expected on the console:
```
=== ns-zephyr: OEVM Model B node ===
THREAD UDP OK
=== OEVM done ===
```

- [ ] **Step 5: Record results + commit (success path)**

Write `docs/r6-thread-bringup.md` (the rig: counterpart device, channel/PAN, peer address, RAM/flash numbers) and add an R6 row to `docs/results.md`:

```markdown
| R6 | 04_somns_basic/44_udp_thread.ns | 32 KB | `THREAD UDP OK` | PASS (YYYY-MM-DD) | <RAM> / 256 KB |
```

```bash
cd ns-zephyr && git add app/prj_thread.conf docs/r6-thread-bringup.md docs/results.md
git add ../ns-tests/04_somns_basic/44_udp_thread.ns
git commit -m "feat(node): UDP over Thread on nRF52840 — the thesis on hardware (R6 S3)

Model B node + net capability over OpenThread IPv6 mesh on the dongle;
44_udp_thread.ns round-trips a datagram to a mesh peer. Same image/backend
seam as native_sim — only the L2/Kconfig changed. Rig + RAM in
docs/r6-thread-bringup.md.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

- [ ] **Step 6: Fallback (only if Step 3/4 cannot fit or no counterpart available)**

Record the outcome honestly rather than forcing it. Write `docs/r6-thread-bringup.md` documenting what was attempted, the RAM/flash overflow numbers (or missing-counterpart blocker), and what a roomier board would need. Add to `docs/results.md`:

```markdown
| R6 | 04_somns_basic/44_udp_thread.ns | — | `THREAD UDP OK` | DEFERRED (RAM/rig) — native_sim S2 is the R6 proof | — |
```

```bash
cd ns-zephyr && git add app/prj_thread.conf docs/r6-thread-bringup.md docs/results.md
git add ../ns-tests/04_somns_basic/44_udp_thread.ns
git commit -m "docs(node): Thread bring-up attempted; native_sim S2 stands as R6 proof

S3 stretch. Records the OpenThread-on-dongle attempt and the RAM/rig
blocker; the artifacts (prj_thread.conf, 44_udp_thread.ns) are kept for a
roomier board. R6 is satisfied by S2 (native_sim UDP round-trip).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review

**Spec coverage:**
- S0 loader seam → Task 1 ✅ (incl. desktop 72/0/4 gate + fread-parity test)
- S1 three zephyr backends → Task 2 (udp) ✅, Task 3 (platform+serial) ✅
- S2 native_sim Model B node → Task 4 (wiring) + Task 5 (run, `UDP OK`) ✅
- S3 dongle over Thread (stretch + fallback) → Task 6 ✅
- "Flash blob, deserialize in place" → Task 4 `gen_oimg_blob.py` (aligned `const` array) + `main_b.c` (`oimg_load_blob`, no copy) ✅
- "Facade/prims/format unchanged" → no task touches `udp.c`/facade/serializer; Global Constraints ✅
- Error handling (loader returns non-zero → console + halt) → `main_b.c` FATAL branch ✅; backend -1 contract → Tasks 2-3 ✅

**Placeholder scan:** `MESH_PEER_ADDR` in Task 6 is an intentional, documented substitution (the peer address is rig-specific, set at bring-up) — not a plan gap. No TBD/TODO/"add error handling" elsewhere.

**Type consistency:** `oimg_load_blob(const uint8_t*, size_t)` consistent across Task 1 (def), Task 1 test, Task 4 `main_b.c`. `oimg_image[]` / `oimg_image_len` consistent across `gen_oimg_blob.py`, `main_b.c`, CMakeLists. Backend function names match the three `*_backend.h` headers verbatim. `OEVM_MODEL`/`OEVM_NO_FILE_IO`/`OEVM_APPLIANCE` used consistently in CMakeLists + the `oimg_loader.c` guard.
