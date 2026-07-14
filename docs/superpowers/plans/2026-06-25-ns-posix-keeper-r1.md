# R1 — `ns-posix` keeper on the resident oevm VM — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up a new `ns-posix` repo — a nodus-style keeper (host C) that polls a real `porta serve`, downloads an OTA'd `.oimg`, runs it on the resident oevm VM, and reports observed state — proving the capability node end-to-end on the desktop.

**Architecture:** Reuse `nodus-st`'s VM-neutral keeper core (`keeper`/`cmd`/`tftp`/`report`/`crc32`/`seam`) and posix seam impls (`udp_transport`/`file_store`) unchanged; the only genuinely new code is a `struct vm` seam (`oimg_vm.c`) that runs `.oimg` by `popen`-ing the `oevm_img` binary (run-once, fault-isolated). `ns-posix` owns the keeper core as a strictly-VM-neutral shared component (primary); a later follow-up rewires `nodus-st` to depend on it.

**Tech Stack:** C11 (keeper + seams, `cc`/`gcc`, `make`); Python 3 + `curl` (deploy tool); the oevm Model B build (`oevm_img`); `porta` (Go gateway, `porta serve`); `ns_compile_oevm.py` + `dump_layout` (host-ABI `.oimg`).

**Design spec:** `ns-oevm/docs/design/2026-06-25-ns-posix-keeper-r1-design.md`

## Global Constraints

- **Strictly VM-neutral keeper core.** Files under `ns-posix/keeper/` must contain no oevm/Berry/format-specific code — only the seam interfaces. The reuse discipline is the point.
- **Run-once only (R1).** `oevm_img <image>` runs `main:args:` to completion and exits; the vm-seam `stop` is a no-op. `run-loop`/triggers/`reboot`-apply are parsed-and-stored but not acted on.
- **Host vm-seam = subprocess.** `oimg_vm.c` uses `popen("$OEVM_IMG <tmp.oimg>")`. A VM crash must NOT take down the keeper (fault isolation).
- **Real porta in R1.** Reuse `udp_transport.c` verbatim against `porta serve` on localhost; no mock transport.
- **Node identity (host):** `device_id` = fixed synthetic 16-hex EUI-64 `686f73740000f0a1`; `chip = "host"`; `loop_sleep_s = 2`.
- **CRC32-IEEE:** polynomial `0xEDB88320`, init/xor-out `0xFFFFFFFF`, test vector `"123456789" → 0xCBF43926`. Porta computes it server-side on upload; the keeper re-verifies on download (existing `keeper.c` code).
- **Image size cap:** the reused keeper buffers images in a static array sized for tiny Berry `.bec` (8 KB); a real `.oimg` is ~34–47 KB (R0 measurements). Raise the cap to `131072` (128 KB).
- **Repos / paths (absolute):**
  - new: `/home/david/workspace/ns-posix`
  - reuse source: `/home/david/workspace/nodus-st`
  - VM build: `/home/david/workspace/oevm`
  - compiler tool: `/home/david/workspace/ns-oevm/tools/ns_compile_oevm.py`
  - test program: `/home/david/workspace/ns-tests/04_somns_basic/01_method_call.ns` (prints `42`)
  - gateway: `/home/david/workspace/porta`
- **oevm Model B build line** (produces `oevm_img`; run from `/home/david/workspace/oevm`):
  ```bash
  TARGET=posix
  gcc -Wall -std=c99 -Isrc \
      src/vm.c src/gc.c src/dispatch.c \
      src/platform.c src/serial.c src/mirror.c \
      "src/backend/$TARGET/platform_$TARGET.c" "src/backend/$TARGET/serial_$TARGET.c" \
      src/oimg_loader.c src/kernel_boot.c oevm_image_main.c \
      -o oevm_img -lm
  ```
- **Commit discipline:** commit after each task; do NOT push (push only on explicit say-so).

---

## File Structure

**New repo `ns-posix/`:**
- `keeper/keeper.{c,h}` `keeper/cmd.{c,h}` `keeper/tftp.{c,h}` `keeper/report.{c,h}` `keeper/crc32.{c,h}` `keeper/seam.h` — vendored VM-neutral core (from `nodus-st/src/`).
- `posix/udp_transport.c` `posix/file_store.c` — vendored posix seams (from `nodus-st/host/`).
- `posix/oimg_vm.c` — **new**: `struct vm` over `popen(oevm_img)`.
- `posix/keeperd.c` — harness/main (adapted from `nodus-st/host/keeperd.c`).
- `tool/deploy.py` — **new**: compile `.ns`→`.oimg` + push to porta.
- `tests/fake_seam.{c,h}` `tests/test_*.c` — vendored unit suite (from `nodus-st/tests/`).
- `tests/test_oimg_vm.sh` — **new**: smoke-test the vm-seam against a real `.oimg`.
- `tests/porta_integration.sh` — **new**: the R1 acceptance gate.
- `Makefile` `README.md`.

---

## Task 1: Scaffold `ns-posix` + vendor the VM-neutral keeper core

**Files:**
- Create: `/home/david/workspace/ns-posix/keeper/{keeper,cmd,tftp,report,crc32}.{c,h}`, `keeper/seam.h` (copied)
- Create: `/home/david/workspace/ns-posix/tests/{fake_seam.c,fake_seam.h,test_cmd.c,test_crc32.c,test_keeper.c,test_report.c,test_tftp.c}` (copied)
- Create: `/home/david/workspace/ns-posix/Makefile`
- Create: `/home/david/workspace/ns-posix/README.md`

**Interfaces:**
- Produces: the seam contract `seam.h` (`struct transport`/`struct store`/`struct vm`/`struct keeper_config`) and `keeper.h` (`keeper_init`, `keeper_poll_once`) that all later tasks consume.

- [ ] **Step 1: Create the repo and vendor the core + tests**

```bash
mkdir -p /home/david/workspace/ns-posix/keeper /home/david/workspace/ns-posix/posix \
         /home/david/workspace/ns-posix/tool /home/david/workspace/ns-posix/tests
cd /home/david/workspace/ns-posix
git init -q
cp /home/david/workspace/nodus-st/src/keeper.c   keeper/
cp /home/david/workspace/nodus-st/src/keeper.h   keeper/
cp /home/david/workspace/nodus-st/src/cmd.c      keeper/
cp /home/david/workspace/nodus-st/src/cmd.h      keeper/
cp /home/david/workspace/nodus-st/src/tftp.c     keeper/
cp /home/david/workspace/nodus-st/src/tftp.h     keeper/
cp /home/david/workspace/nodus-st/src/report.c   keeper/
cp /home/david/workspace/nodus-st/src/report.h   keeper/
cp /home/david/workspace/nodus-st/src/crc32.c    keeper/
cp /home/david/workspace/nodus-st/src/crc32.h    keeper/
cp /home/david/workspace/nodus-st/src/seam.h     keeper/
cp /home/david/workspace/nodus-st/tests/*.c      tests/
cp /home/david/workspace/nodus-st/tests/*.h      tests/
# (only *.c/*.h are vendored; porta_integration.sh is rewritten fresh in Task 5)
```

- [ ] **Step 2: Write the Makefile (test target only for now)**

Create `/home/david/workspace/ns-posix/Makefile`:

```make
CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -g
CORE     = keeper/crc32.c keeper/tftp.c keeper/cmd.c keeper/report.c keeper/keeper.c
TESTS    = $(wildcard tests/test_*.c)
TESTBINS = $(TESTS:tests/%.c=build/%)

build/%: tests/%.c $(CORE) tests/fake_seam.c
	@mkdir -p build
	$(CC) $(CFLAGS) -Ikeeper $^ -o $@

test: $(TESTBINS)
	@set -e; for t in $(TESTBINS); do echo "== $$t"; $$t; done; echo "ALL TESTS PASSED"

clean:
	rm -rf build
.PHONY: test clean
```

- [ ] **Step 3: Write a README stub**

Create `/home/david/workspace/ns-posix/README.md`:

```markdown
# ns-posix

The Newspeak **node** on the resident oevm VM, developed desktop-first. A
nodus-style **keeper** (host C) supervises the resident oevm VM and joins the
**porta** fleet plane.

`ns-posix` owns the **strictly VM-neutral keeper core** (`keeper/`) — the porta
protocol + lifecycle, depending only on the 3-interface seam (`keeper/seam.h`).
`posix/` provides the host seam impls; `oimg_vm.c` runs `.oimg` on `oevm_img`.

    make test     # VM-neutral unit suite (crc/tftp/cmd/report/keeper)
    make keeperd  # host daemon -> build/keeperd  (needs $OEVM_IMG; see below)

History: the keeper core is seeded from `nodus-st/src/` (the Berry/ST node).
ns-posix is now canonical; rewiring nodus-st to depend on `ns-posix/keeper/` is
a tracked follow-up.
```

- [ ] **Step 4: Run the vendored unit suite to verify it builds + passes unchanged**

Run: `cd /home/david/workspace/ns-posix && make test`
Expected: each `== build/test_*` line runs, ending with `ALL TESTS PASSED`. (Proves the vendored VM-neutral core compiles and its unit suite passes verbatim under the new layout.)

- [ ] **Step 5: Commit**

```bash
cd /home/david/workspace/ns-posix
git add -A
git commit -q -m "feat(ns-posix): scaffold repo + vendor VM-neutral keeper core (from nodus-st)"
```

---

## Task 2: Raise the image-size caps for `.oimg`

**Files:**
- Modify: `/home/david/workspace/ns-posix/keeper/keeper.h` (add `KEEPER_MAX_IMAGE`)
- Modify: `/home/david/workspace/ns-posix/keeper/keeper.c` (two `img[8192]` sites)
- Modify: `/home/david/workspace/ns-posix/keeper/tftp.c` (block size)
- Test: `/home/david/workspace/ns-posix/tests/test_keeper.c` (add a static-assert)

**Interfaces:**
- Produces: `KEEPER_MAX_IMAGE` (128 KB) — the per-image buffer/transfer ceiling consumed by `keeper.c`. Real-capacity proof is deferred to Task 5's integration gate (the unit fakes only carry tiny images).

- [ ] **Step 1: Add the cap constant to `keeper.h`**

In `/home/david/workspace/ns-posix/keeper/keeper.h`, after `#define KEEPER_MAX_APPS 4`, add:

```c
/* Max .oimg image the keeper buffers/transfers in one shot. Berry .bec were
 * tiny (8 KB sufficed); oevm .oimg are ~34-47 KB (R0), so size for headroom. */
#define KEEPER_MAX_IMAGE (128 * 1024)
```

- [ ] **Step 2: Use the cap in `keeper.c`**

In `/home/david/workspace/ns-posix/keeper/keeper.c`, replace BOTH occurrences of:

```c
    static uint8_t img[8192];
```

with:

```c
    static uint8_t img[KEEPER_MAX_IMAGE];
```

(One is in `ensure_image`, one in `do_run`. The matching `sizeof img` calls need no change.)

- [ ] **Step 3: Widen the TFTP block size**

In `/home/david/workspace/ns-posix/keeper/tftp.c`, change:

```c
#define TFTP_BLOCK_SIZE 64
```

to:

```c
#define TFTP_BLOCK_SIZE 512
```

(The packet buffers are already `s_pkt_buf[4 + 512]` / `s_recv_buf[4 + 512 + 64]`, so 512-byte blocks fit. 64-byte blocks would mean ~730 sync round-trips for a 47 KB image; 512 cuts that to ~92.)

- [ ] **Step 4: Add a compile-time guard in the keeper test**

At the top of `/home/david/workspace/ns-posix/tests/test_keeper.c`, after the `#include` lines, add:

```c
/* R1: the keeper must buffer a full .oimg (~34-47 KB), not just a Berry .bec. */
_Static_assert(KEEPER_MAX_IMAGE >= 64 * 1024, "image cap too small for .oimg");
```

- [ ] **Step 5: Verify no regression**

Run: `cd /home/david/workspace/ns-posix && make clean && make test`
Expected: `ALL TESTS PASSED` (the `_Static_assert` compiles; existing unit tests still pass — they carry small images, so the larger buffer changes nothing behaviorally).

- [ ] **Step 6: Commit**

```bash
cd /home/david/workspace/ns-posix
git add -A
git commit -q -m "feat(ns-posix): raise image cap to 128KB + TFTP blksize 512 for .oimg"
```

---

## Task 3: Posix seams — vendor transport/store, write the oevm vm-seam + daemon

**Files:**
- Create: `/home/david/workspace/ns-posix/posix/udp_transport.c` (copied verbatim)
- Create: `/home/david/workspace/ns-posix/posix/file_store.c` (copied; image filename `.bec`→`.oimg`)
- Create: `/home/david/workspace/ns-posix/posix/oimg_vm.c` (**new**)
- Create: `/home/david/workspace/ns-posix/posix/keeperd.c` (adapted)
- Modify: `/home/david/workspace/ns-posix/Makefile` (add `keeperd` target)
- Test: `/home/david/workspace/ns-posix/tests/test_oimg_vm.sh` (**new** smoke test)

**Interfaces:**
- Consumes: `seam.h` (`struct transport`/`struct store`/`struct vm`), `keeper.h`.
- Produces: `struct transport host_transport(const char *host, int port)`, `struct store host_store(const char *dir)`, `struct vm host_vm(void)` — the three host seam constructors `keeperd.c` wires.

- [ ] **Step 1: Vendor the posix transport + store**

```bash
cp /home/david/workspace/nodus-st/host/udp_transport.c /home/david/workspace/ns-posix/posix/
cp /home/david/workspace/nodus-st/host/file_store.c    /home/david/workspace/ns-posix/posix/
```

Then in `/home/david/workspace/ns-posix/posix/file_store.c`, change the image cache filename from Berry's `.bec` to `.oimg` (cosmetic; the cache is crc-keyed). Replace all THREE occurrences of:

```c
    snprintf(path, sizeof(path), "%s/img-%08x.bec", s->dir, crc);
```

with:

```c
    snprintf(path, sizeof(path), "%s/img-%08x.oimg", s->dir, crc);
```

(in `fs_img_has`, `fs_img_get`, `fs_img_put`) and update the header comment `//   images:    <dir>/img-<crc>.bec` to `.oimg`.

- [ ] **Step 2: Write the oevm vm-seam `posix/oimg_vm.c`**

Create `/home/david/workspace/ns-posix/posix/oimg_vm.c`:

```c
// posix/oimg_vm.c — struct vm that execs the resident oevm VM (`oevm_img
// <file.oimg>`) and captures its stdout. Run-once: stop is a no-op. A VM crash
// is contained in the child process, so the keeper survives (fault isolation).
#define _POSIX_C_SOURCE 200809L  // expose popen/pclose under -std=c11
#include "seam.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

struct oimg_ctx { char bin[256]; };

static int oimg_run(void *c, const char *name, const uint8_t *img, size_t len,
                    char *out, size_t out_size) {
    struct oimg_ctx *b = c;

    const char *tmp = "/tmp/ns-posix-keeperd-run.oimg";
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    if (fwrite(img, 1, len, f) != len) { fclose(f); unlink(tmp); return -1; }
    fclose(f);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s %s", b->bin, tmp);
    FILE *p = popen(cmd, "r");
    if (!p) { unlink(tmp); return -1; }

    size_t off = 0;
    if (out && out_size > 0) {
        size_t cap = out_size - 1;
        while (off < cap) {
            size_t r = fread(out + off, 1, cap - off, p);
            if (r == 0) break;
            off += r;
        }
        out[off] = '\0';
    } else {
        char sink[256];
        while (fread(sink, 1, sizeof(sink), p) > 0) { }
    }

    int status = pclose(p);
    unlink(tmp);
    // Surface what the VM printed so the R1 integration gate can verify the run
    // (the keeper core discards `out`; telemetry forwarding is R2).
    fprintf(stderr, "[oimg_vm] %s ran -> %.*s\n", name, (int)off, out ? out : "");
    if (status == -1) return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
    return -1;
}

static int oimg_stop(void *c, const char *name) {
    (void)c; (void)name;
    return 0;  // run-once: nothing to stop
}

struct vm oimg_vm_make(struct oimg_ctx *c) {
    struct vm v = { oimg_run, oimg_stop, c };
    return v;
}

// Convenience wrapper: owns a static ctx, resolves the oevm_img binary path.
struct vm host_vm(void) {
    static struct oimg_ctx c;
    const char *env = getenv("OEVM_IMG");
    if (env && env[0]) {
        snprintf(c.bin, sizeof(c.bin), "%s", env);
    } else {
        const char *home = getenv("HOME");
        snprintf(c.bin, sizeof(c.bin), "%s/workspace/oevm/oevm_img", home ? home : "");
    }
    return oimg_vm_make(&c);
}
```

- [ ] **Step 3: Write the daemon `posix/keeperd.c`**

Create `/home/david/workspace/ns-posix/posix/keeperd.c`:

```c
// posix/keeperd.c — main() wiring the host seam and running the keeper loop.
#include "seam.h"
#include "keeper.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// host seam constructors (posix/*.c)
struct transport host_transport(const char *host, int port);
struct store     host_store(const char *dir);
struct vm        host_vm(void);

int main(int argc, char **argv) {
    const char *id   = getenv("NS_POSIX_ID");      // 16-hex EUI-64; default below
    const char *host = getenv("PORTA_HOST");        // default 127.0.0.1
    if (!id || !id[0]) id = "686f73740000f0a1";     // fixed synthetic host id
    if (!host) host = "127.0.0.1";
    int once = (argc > 1 && !strcmp(argv[1], "--once"));

    struct transport t = host_transport(host, 6969);
    struct store     s = host_store("/tmp/ns-posix-keeperd");
    struct vm        v = host_vm();
    struct keeper_config cfg = { .device_id = id, .chip = "host", .loop_sleep_s = 2 };

    struct keeper k; keeper_init(&k, &t, &s, &v, &cfg);
    do {
        keeper_poll_once(&k);
        if (!once) sleep((unsigned)cfg.loop_sleep_s);
    } while (!once);
    return 0;
}
```

- [ ] **Step 4: Add the `keeperd` target to the Makefile**

In `/home/david/workspace/ns-posix/Makefile`, add after the `test:` rule (and add `keeperd` to `.PHONY`):

```make
keeperd: posix/keeperd.c posix/udp_transport.c posix/file_store.c posix/oimg_vm.c $(CORE)
	@mkdir -p build
	$(CC) $(CFLAGS) -Ikeeper $^ -o build/keeperd
```

Change the last line to: `.PHONY: test clean keeperd`

- [ ] **Step 5: Write the vm-seam smoke test `tests/test_oimg_vm.sh`**

Create `/home/david/workspace/ns-posix/tests/test_oimg_vm.sh`:

```bash
#!/usr/bin/env bash
# Smoke-test the oevm vm-seam: build oevm_img, compile a known .ns to .oimg,
# call host_vm().run() on its bytes, assert captured stdout == "42".
set -euo pipefail
cd "$(dirname "$0")/.."
OEVM=/home/david/workspace/oevm
NSOEVM=/home/david/workspace/ns-oevm
SRC=/home/david/workspace/ns-tests/04_somns_basic/01_method_call.ns

# 1) Build oevm_img (Model B) if absent.
if [ ! -x "$OEVM/oevm_img" ]; then
  ( cd "$OEVM" && TARGET=posix gcc -Wall -std=c99 -Isrc \
      src/vm.c src/gc.c src/dispatch.c src/platform.c src/serial.c src/mirror.c \
      "src/backend/$TARGET/platform_$TARGET.c" "src/backend/$TARGET/serial_$TARGET.c" \
      src/oimg_loader.c src/kernel_boot.c oevm_image_main.c -o oevm_img -lm )
fi
export OEVM_IMG="$OEVM/oevm_img"

# 2) Compile the .ns to a host-ABI .oimg.
gcc -std=c99 -I"$OEVM/src" "$OEVM/tools/dump_layout.c" -o /tmp/ns-posix-dl
/tmp/ns-posix-dl > /tmp/ns-posix-layout.json
python3 "$NSOEVM/tools/ns_compile_oevm.py" "$SRC" /tmp/ns-posix-mcall.oimg \
    --format=oimg --layout=/tmp/ns-posix-layout.json

# 3) Build a tiny C harness that loads the .oimg bytes and calls the seam.
cat > /tmp/ns-posix-vmharness.c <<'EOF'
#include "seam.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct vm host_vm(void);
int main(void) {
    FILE *f = fopen("/tmp/ns-posix-mcall.oimg", "rb");
    if (!f) return 2;
    static unsigned char buf[256*1024];
    size_t n = fread(buf, 1, sizeof buf, f); fclose(f);
    struct vm v = host_vm();
    char out[256];
    int rc = v.run(v.ctx, "mcall", buf, n, out, sizeof out);
    printf("rc=%d out=%s\n", rc, out);
    return (rc == 0 && strstr(out, "42")) ? 0 : 1;
}
EOF
cc -std=c11 -Ikeeper /tmp/ns-posix-vmharness.c posix/oimg_vm.c -o /tmp/ns-posix-vmharness
/tmp/ns-posix-vmharness
echo "PASS: vm-seam ran .oimg and captured 42"
```

Make it executable: `chmod +x /home/david/workspace/ns-posix/tests/test_oimg_vm.sh`

- [ ] **Step 6: Build the daemon and run the smoke test**

Run: `cd /home/david/workspace/ns-posix && make keeperd && bash tests/test_oimg_vm.sh`
Expected: `make` links `build/keeperd`; the smoke test prints `rc=0 out=42 ...` then `PASS: vm-seam ran .oimg and captured 42`.

- [ ] **Step 7: Commit**

```bash
cd /home/david/workspace/ns-posix
git add -A
git commit -q -m "feat(ns-posix): posix seams + oevm vm-seam (popen oevm_img) + keeperd"
```

---

## Task 4: `deploy.py` — compile `.ns`→`.oimg` and push to porta

**Files:**
- Create: `/home/david/workspace/ns-posix/tool/deploy.py`

**Interfaces:**
- Produces: `python3 tool/deploy.py <app.ns> --to <id> --name <app> [--server URL] [--dry-run]` — compiles a host-ABI `.oimg` and (unless `--dry-run`) `POST`s it multipart to porta's `/api/nodes/<id>/containers`, which stores the payload, computes its CRC32-IEEE, and enqueues a `run` command.

- [ ] **Step 1: Write `tool/deploy.py`**

Create `/home/david/workspace/ns-posix/tool/deploy.py`:

```python
#!/usr/bin/env python3
"""ns-posix deploy: compile a .ns to a host-ABI .oimg and push it to porta.

porta's POST /api/nodes/<id>/containers takes a multipart 'image' file (+ name,
lifecycle, runlevel), computes CRC32-IEEE server-side, stores the payload, and
auto-enqueues a `run` command. The node's keeper then polls, downloads by crc,
verifies, and runs it on the resident oevm VM.
"""
import argparse, subprocess, sys, os

OEVM = "/home/david/workspace/oevm"
NSOEVM = "/home/david/workspace/ns-oevm"


def compile_oimg(src, out):
    """Compile <src.ns> -> <out.oimg> for the host ABI."""
    dl = "/tmp/ns-posix-dl"
    layout = "/tmp/ns-posix-layout.json"
    subprocess.run(["gcc", "-std=c99", "-I%s/src" % OEVM,
                    "%s/tools/dump_layout.c" % OEVM, "-o", dl], check=True)
    with open(layout, "w") as f:
        f.write(subprocess.run([dl], capture_output=True, text=True, check=True).stdout)
    subprocess.run([sys.executable, "%s/tools/ns_compile_oevm.py" % NSOEVM,
                    src, out, "--format=oimg", "--layout=%s" % layout], check=True)


def push(server, node_id, name, oimg):
    """Multipart POST the .oimg to porta (upload + auto-enqueue run)."""
    url = "%s/api/nodes/%s/containers" % (server.rstrip("/"), node_id)
    r = subprocess.run(["curl", "-fsS",
                        "-F", "image=@%s" % oimg,
                        "-F", "name=%s" % name,
                        "-F", "lifecycle=run-once",
                        "-F", "runlevel=3",
                        url], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("deploy: porta upload failed: %s" % (r.stderr or r.stdout))
    print("deployed %s -> %s: %s" % (name, node_id, r.stdout.strip()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("--to", required=True, help="node id (16-hex EUI-64)")
    ap.add_argument("--name", required=True, help="app name")
    ap.add_argument("--server", default="http://localhost:6970")
    ap.add_argument("--dry-run", action="store_true", help="compile only, no push")
    a = ap.parse_args()
    out = "/tmp/ns-posix-deploy-%s.oimg" % a.name
    compile_oimg(a.src, out)
    sz = os.path.getsize(out)
    with open(out, "rb") as f:
        assert f.read(4) == b"OIMG", "compiled output is not a .oimg"
    print("compiled %s -> %s (%d bytes)" % (a.src, out, sz))
    if a.dry_run:
        return
    push(a.server, a.to, a.name, out)


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Verify the compile path with `--dry-run` (no porta needed)**

Run:
```bash
cd /home/david/workspace/ns-posix
python3 tool/deploy.py /home/david/workspace/ns-tests/04_somns_basic/01_method_call.ns \
    --to 686f73740000f0a1 --name mcall --dry-run
```
Expected: prints `compiled .../01_method_call.ns -> /tmp/ns-posix-deploy-mcall.oimg (NNNNN bytes)` with no error (the `assert ... == b"OIMG"` passes).

- [ ] **Step 3: Commit**

```bash
cd /home/david/workspace/ns-posix
git add -A
git commit -q -m "feat(ns-posix): deploy.py — compile .ns to .oimg + push to porta"
```

---

## Task 5: `porta_integration.sh` — the R1 acceptance gate

**Files:**
- Create: `/home/david/workspace/ns-posix/tests/porta_integration.sh`
- Modify: `/home/david/workspace/ns-posix/README.md` (document the gate + deps)

**Interfaces:**
- Consumes: `build/keeperd`, `tool/deploy.py`, `oevm_img`, a `porta` build.
- Produces: the end-to-end proof — deploy `01_method_call.ns` via real porta → keeper downloads + runs it on oevm → porta report shows the app installed AND the captured VM stdout is `42`.

- [ ] **Step 1: Write the integration gate `tests/porta_integration.sh`**

Create `/home/david/workspace/ns-posix/tests/porta_integration.sh`:

```bash
#!/usr/bin/env bash
# R1 acceptance gate. End-to-end: deploy a .ns via real porta, run the keeper,
# assert porta saw the node install the app AND the VM printed "42".
# Skips (exit 0) unless NS_POSIX_IT=1 is set.
set -euo pipefail
[ "${NS_POSIX_IT:-}" = "1" ] || { echo "SKIP: set NS_POSIX_IT=1 to run"; exit 0; }

cd "$(dirname "$0")/.."
ID="686f73740000f0a1"
NAME="mcall"
PORTA_DIR=/home/david/workspace/porta
SERVER="http://localhost:6970"
SRC=/home/david/workspace/ns-tests/04_somns_basic/01_method_call.ns
OEVM=/home/david/workspace/oevm
export OEVM_IMG="$OEVM/oevm_img"

# 1) Build oevm_img (Model B) + keeperd.
if [ ! -x "$OEVM/oevm_img" ]; then
  ( cd "$OEVM" && TARGET=posix gcc -Wall -std=c99 -Isrc \
      src/vm.c src/gc.c src/dispatch.c src/platform.c src/serial.c src/mirror.c \
      "src/backend/$TARGET/platform_$TARGET.c" "src/backend/$TARGET/serial_$TARGET.c" \
      src/oimg_loader.c src/kernel_boot.c oevm_image_main.c -o oevm_img -lm )
fi
make keeperd >/dev/null

# 2) Start a fresh porta serve on localhost.
( cd "$PORTA_DIR" && go build -o /tmp/ns-posix-porta ./cmd/porta )
rm -f /tmp/ns-posix-porta.db
/tmp/ns-posix-porta serve --db /tmp/ns-posix-porta.db >/tmp/ns-posix-porta.log 2>&1 &
PORTA_PID=$!
trap 'kill $PORTA_PID 2>/dev/null || true' EXIT
# wait for the HTTP port to answer
for i in $(seq 1 50); do curl -fsS "$SERVER/api/nodes" >/dev/null 2>&1 && break; sleep 0.1; done

# 3) Register the node with one empty poll (no commands queued yet).
NS_POSIX_ID="$ID" PORTA_HOST="127.0.0.1" build/keeperd --once 2>/dev/null

# 4) Deploy: compile .ns -> .oimg, upload to porta (auto-enqueues run).
python3 tool/deploy.py "$SRC" --to "$ID" --name "$NAME" --server "$SERVER"

# 5) Poll once more: keeper pulls the run command, downloads payload by crc,
#    verifies size+crc, runs it on oevm, uploads a report. Capture keeperd stderr.
NS_POSIX_ID="$ID" PORTA_HOST="127.0.0.1" build/keeperd --once 2>/tmp/ns-posix-keeperd.log

# 6) Assertions: (a) porta report shows the app installed; (b) the VM printed 42.
curl -fsS "$SERVER/api/nodes/$ID" | tee /tmp/ns-posix-nodedetail.json | grep -q "\"$NAME\"" \
  || { echo "FAIL: porta does not show $NAME installed"; cat /tmp/ns-posix-nodedetail.json; exit 1; }
grep -q "42" /tmp/ns-posix-keeperd.log \
  || { echo "FAIL: VM did not print 42"; cat /tmp/ns-posix-keeperd.log; exit 1; }

echo "PASS: deployed $NAME via porta; keeper ran it on oevm; report + stdout(42) verified"
```

Make it executable: `chmod +x /home/david/workspace/ns-posix/tests/porta_integration.sh`

- [ ] **Step 2: Run the acceptance gate**

Run: `cd /home/david/workspace/ns-posix && NS_POSIX_IT=1 bash tests/porta_integration.sh`
Expected: ends with `PASS: deployed mcall via porta; keeper ran it on oevm; report + stdout(42) verified`.

(Prereqs: Go toolchain for `porta`; `$OEVM_IMG` builds; `ns_compile_oevm.py` works; ports 6969/6970 free. If `/api/nodes/$ID` returns a different shape, adjust the grep in step 6 to the node-detail route of the current porta build — confirm against `curl $SERVER/api/nodes/$ID`.)

- [ ] **Step 3: Finalize the README (gate + deps + follow-up)**

Append to `/home/david/workspace/ns-posix/README.md`:

```markdown
## Integration (R1 acceptance gate)

    NS_POSIX_IT=1 bash tests/porta_integration.sh

Deploys `01_method_call.ns` via a real `porta serve`, runs the keeper for a
cycle, and asserts porta shows the app installed and the resident oevm VM
printed `42`. Requires: a Go toolchain (`porta`), `$OEVM_IMG` (oevm Model B
build), and `ns_compile_oevm.py` (`ns-oevm/tools`).

## Follow-ups (not R1)

- Rewire `nodus-st` to depend on `ns-posix/keeper/` (delete its `src/`,
  submodule ns-posix) — ns-posix is now the canonical owner of the VM-neutral
  keeper core.
- Surface VM run-failure + captured stdout into porta telemetry (`data?id=`) — R2.
- run-loop lifecycle / triggers / reboot-apply — deferred.
```

- [ ] **Step 4: Commit**

```bash
cd /home/david/workspace/ns-posix
git add -A
git commit -q -m "test(ns-posix): porta integration gate (deploy->run->report, asserts 42)"
```

- [ ] **Step 5: File the deferred follow-up issues**

```bash
gh issue create --repo davidg238/nodus-st \
  --title "Depend on ns-posix/keeper for the VM-neutral keeper core" \
  --body "ns-posix now owns the canonical strictly-VM-neutral keeper core (keeper/cmd/tftp/report/crc32/seam), seeded from nodus-st/src. Rewire nodus-st to depend on ns-posix/keeper (submodule + build include) and delete its own src/ copy, so the core has one home. Keep nodus-st's Berry vm-seam (berryhost_vm.c) + firmware seams. Tracked from R1 of the ns-zephyr node roadmap."
```

(If `ns-posix` gets its own GitHub remote later, also mirror the R2 telemetry item there. Record the issue URL in the roadmap.)

---

## Self-Review (coverage map)

| Spec section | Task(s) |
|---|---|
| ns-posix owns VM-neutral keeper core (vendored) | 1 |
| Image cap raised for `.oimg` (keeper.c/tftp.c) | 2 |
| Posix transport/store reuse | 3 |
| New oevm vm-seam (subprocess, fault-isolated, run-once) | 3 |
| Adapted daemon (host id/chip/cadence) | 3 |
| Deploy path (compile .ns→.oimg + push to porta) | 4 |
| Real porta in R1, end-to-end | 5 |
| Acceptance: deploy 01_method_call → report + stdout "42" | 5 |
| nodus-st→ns-posix rewiring deferred (follow-up) | 5 |
| Telemetry / run-loop / triggers deferred | 5 (README) |

**Out of scope (per spec):** nodus-st migration; run-loop/triggers/reboot-apply; telemetry stream; ESP32/Zephyr backend + in-process device vm-seam; multi-node host identity; OTA-delta/partial-graph messages.
