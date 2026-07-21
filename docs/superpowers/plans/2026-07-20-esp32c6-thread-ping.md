# ESP32-C6 end-to-end Thread ping — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The ESP32-C6 joins the porta OpenThread mesh as an MTD child and round-trips CBOR datagrams to the gateway `udp-echo6`, printing `UDP OK` on its console and appearing as a gate row on the `gw85224-01:8085` soak dashboard.

**Architecture:** Split the board-divergent OpenThread bring-up out of `zephyr/src/main.c` into a board-selected translation unit (`tuvm_thread_bringup()` — NCS impl for nRF, upstream-4.2 impl for the C6), gated on `CONFIG_NET_L2_OPENTHREAD` so the WROOM (WiFi, no Thread) compiles neither. Add the C6 Thread config + gitignored porta dataset overlay, an autonomous CBOR gate node reusing the rung-6/7 NetBridge machinery, and a device bench gate.

**Tech Stack:** Zephyr 4.2 (upstream, `~/zephyr-esp32` west workspace) for the C6/WROOM; NCS (`~/ncs`) for the nRF; OpenThread MTD; nsl (`ns_compile_oevm.py`) → Model B `.oimg`; `zephyr/run.sh` dev loop.

## Global Constraints

- **Design of record:** `docs/design/2026-07-20-esp32c6-thread-ping-rung-design.md`. This plan implements it; on conflict the design wins.
- **The same `tuvm/zephyr` app builds THREE targets:** nRF (NCS), C6 (upstream 4.2), WROOM-32 (upstream 4.2). Any change to shared `main.c`/`CMakeLists.txt` must keep all three building.
- **Bring-up TU selection is gated on `CONFIG_NET_L2_OPENTHREAD` first**, then board — never compile the NCS impl for an upstream-4.2 target (`openthread_init`/`openthread_get_default_instance` are absent upstream → link failure).
- **Secrets never committed:** the real porta dataset lives ONLY in the gitignored `zephyr/prj_esp32c6_thread.local.conf`. The committed `prj_esp32c6_thread.conf` carries syntactically-valid placeholders.
- **Echo target (durable):** `[fdc5:9109:9afe:97ea:9541:d963:f8d9:8567]:7777` (gw ML-EID + `udp-echo6`).
- **C6 heap default stays 131072** (`run.sh`, #57) — do not change it in this rung.
- **Build env (C6/WROOM):** `source ~/zephyr-esp32/.venv/bin/activate && export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.17.4 && source ~/zephyr-esp32/zephyr/zephyr-env.sh`
- **nsl dialect:** node classes are single-class `.nsl`; use `[ ... ] whileTrue:`, never `repeat` (per `ThreadEchoNodeDK.nsl`).
- Commit after each task. Branch off `master` (do not commit rung work directly to a shared branch without David's ok).

---

### Task 1: Extract `tuvm_thread_bringup()` seam (nRF behavior-preserving)

Move the existing OpenThread bring-up out of `main.c` into a named TU reached through a header, so the C6 can supply its own impl later without a second `main`. nRF behavior must be byte-identical.

**Files:**
- Create: `zephyr/src/thread_bringup.h`
- Create: `zephyr/src/backend/ncs/thread_bringup_ncs.c`
- Modify: `zephyr/src/main.c:28-63` (remove the inline `thread_bringup()`), and its call site (`~line 245`)
- Modify: `zephyr/CMakeLists.txt` (add the TU under an `if (CONFIG_NET_L2_OPENTHREAD)` gate)

**Interfaces:**
- Produces: `void tuvm_thread_bringup(void);` — declared in `thread_bringup.h`, called from `main.c` inside its existing `#ifdef CONFIG_NET_L2_OPENTHREAD` block. Blocks until MTD attach (role CHILD/ROUTER) or ~30 s timeout, then returns.

- [ ] **Step 1: Create the seam header**

`zephyr/src/thread_bringup.h`:
```c
#ifndef TUVM_THREAD_BRINGUP_H
#define TUVM_THREAD_BRINGUP_H

/* Bring the OpenThread mesh up for the Zephyr socket layer and BLOCK until the
   MTD attaches (role CHILD/ROUTER) or a ~30 s timeout elapses. Board-selected:
   NCS impl (backend/ncs) for nRF, upstream-4.2 impl (backend/zephyr) for the C6.
   Only compiled/linked when CONFIG_NET_L2_OPENTHREAD=y. */
void tuvm_thread_bringup(void);

#endif /* TUVM_THREAD_BRINGUP_H */
```

- [ ] **Step 2: Move the nRF impl into its own TU**

`zephyr/src/backend/ncs/thread_bringup_ncs.c` — the current `main.c` body verbatim, renamed and made non-static:
```c
/* NCS (nRF52840) OpenThread bring-up. Moved verbatim from zephyr/src/main.c
   (rung 7). NCS API: openthread_init/run/get_default_instance + api mutex. */
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/openthread.h>
#include <openthread.h>
#include <openthread/thread.h>
#include "../../thread_bringup.h"

void tuvm_thread_bringup(void) {
    (void)openthread_init();
    int rr = openthread_run();
    struct openthread_context *ctx = openthread_get_default_context();
    struct net_if *iface = ctx ? ctx->iface : net_if_get_default();
    int ur = iface ? net_if_up(iface) : -1;
    printk("thread: run=%d net_if_up=%d, joining...\n", rr, ur);

    otInstance *inst = openthread_get_default_instance();
    for (int i = 0; i < 300 && inst; i++) {
        openthread_mutex_lock();
        otDeviceRole role = otThreadGetDeviceRole(inst);
        openthread_mutex_unlock();
        if (role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER) {
            printk("thread: attached role=%d\n", role);
            return;
        }
        k_msleep(100);
    }
    printk("thread: attach timed out, continuing\n");
}
```

- [ ] **Step 3: Gut `main.c` — delete the inline function, keep the includes it still needs, call the seam**

In `zephyr/src/main.c`, delete lines 34-62 (the comment + `static void thread_bringup(void) {...}`), keeping the `#ifdef CONFIG_NET_L2_OPENTHREAD` / `#include` lines 28-33 ONLY if still referenced elsewhere in `main.c`; if not, move them into the TU (they already are) and delete from `main.c`. Add near the top includes:
```c
#include "thread_bringup.h"
```
At the call site (currently `thread_bringup();` ~line 245), change to:
```c
    tuvm_thread_bringup();   /* board-selected seam (backend/{ncs,zephyr}) */
```

- [ ] **Step 4: Wire the TU into CMake, OPENTHREAD-gated (ncs branch only for now)**

In `zephyr/CMakeLists.txt`, after the `target_sources(app PRIVATE src/main.c ...)` block add:
```cmake
# Board-selected OpenThread bring-up TU. Gated on OPENTHREAD so the WROOM
# (WiFi, upstream 4.2) compiles NEITHER impl — the NCS symbols are absent there.
if (CONFIG_NET_L2_OPENTHREAD)
  target_sources(app PRIVATE src/backend/ncs/thread_bringup_ncs.c)
endif()
```
(The C6 branch is added in Task 2.)

- [ ] **Step 5: WROOM regression build — proves the gate excludes the TU**

Run:
```bash
source ~/zephyr-esp32/.venv/bin/activate && export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.17.4 && source ~/zephyr-esp32/zephyr/zephyr-env.sh
cd ~/workspace/tuvm && bash zephyr/run.sh --board esp32 --node TimerNode.nsl --pristine
```
Expected: build succeeds (the WROOM has no `CONFIG_NET_L2_OPENTHREAD`, so `thread_bringup_ncs.c` is not compiled and the NCS symbols never resolve). Look for `Successfully created esp32 image.` and no linker errors about `openthread_*`.

- [ ] **Step 6: nRF DK build — proves the moved NCS TU still compiles (needs the NCS workspace)**

Run (recipe from `zephyr/docs/BOARD_PORTS.md:55`):
```bash
source ~/ncs/.venv/bin/activate && source ~/ncs/zephyr/zephyr-env.sh
cd ~/workspace/tuvm/zephyr && python3 ~/workspace/nsl-tuvm/tools/ns_compile_oevm.py ThreadEchoNodeDK.nsl /tmp/dk.oimg --format=oimg --layout=layout-dk.json 2>/dev/null; \
west build -b nrf52840dk/nrf52840 --build-dir build-dk . -- -DCONF_FILE=prj_dk_thread.conf -DTUVM_MODEL=B -DTUVM_BLOB=/tmp/dk.oimg
```
Expected: build succeeds; `tuvm_thread_bringup` links from `backend/ncs/thread_bringup_ncs.c`. If the NCS workspace/toolchain is not set up in this session, record that Step 6 is deferred to a machine with `~/ncs` and rely on Step 5 + Task 2's C6 build; do NOT block the rung on it.

- [ ] **Step 7: Commit**

```bash
cd ~/workspace/tuvm
git add zephyr/src/thread_bringup.h zephyr/src/backend/ncs/thread_bringup_ncs.c zephyr/src/main.c zephyr/CMakeLists.txt
git commit -m "refactor(zephyr): extract tuvm_thread_bringup() into a board-selected TU (#59)"
```

---

### Task 2: C6 OpenThread bring-up TU (upstream 4.2) + board-select

Add the C6's own bring-up using the upstream context API, and make CMake choose it for the C6.

**Files:**
- Create: `zephyr/src/backend/zephyr/thread_bringup_c6.c`
- Modify: `zephyr/CMakeLists.txt` (turn the ncs branch into a board select)

**Interfaces:**
- Consumes: `zephyr/src/thread_bringup.h` (`void tuvm_thread_bringup(void)`).
- Produces: the same symbol, C6 flavor.

- [ ] **Step 1: Pin the exact non-deprecated OT calls from the C6 toolchain**

The upstream header (`~/zephyr-esp32/zephyr/include/zephyr/net/openthread.h`) marks `openthread_start`/`openthread_api_mutex_*` deprecated and points to `openthread_run`/`openthread_mutex_lock`; `openthread_init`/`openthread_get_default_instance` are ABSENT. Confirm the replacements' signatures before writing code:
```bash
grep -rnE "openthread_run|openthread_mutex_lock|openthread_mutex_unlock" \
  ~/zephyr-esp32/zephyr/include/zephyr/net/openthread.h \
  ~/zephyr-esp32/zephyr/subsys/net/l2/openthread/*.h 2>/dev/null
```
Expected: exact prototypes (arg = `struct openthread_context *` or void). Use them verbatim in Step 2. The `otInstance*` comes from `ctx->instance` (a deprecated struct field — a warning, acceptable).

- [ ] **Step 2: Write the C6 bring-up**

`zephyr/src/backend/zephyr/thread_bringup_c6.c`:
```c
/* ESP32-C6 OpenThread bring-up against upstream Zephyr 4.2. openthread_init and
   openthread_get_default_instance are ABSENT upstream; the API is context-based.
   Same shape as the NCS impl: enable -> admin-up the net_if -> poll role -> print,
   BLOCKING until the MTD attaches so the VM starts on a joined mesh. */
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/openthread.h>
#include <openthread/thread.h>
#include "../../thread_bringup.h"

void tuvm_thread_bringup(void) {
    struct openthread_context *ctx = openthread_get_default_context();
    if (ctx == NULL) { printk("thread: no OT context\n"); return; }

    int rr = openthread_run(ctx);                 /* non-deprecated replacement for openthread_start */
    int ur = ctx->iface ? net_if_up(ctx->iface) : -1;
    printk("thread: run=%d net_if_up=%d, joining...\n", rr, ur);

    otInstance *inst = ctx->instance;             /* deprecated struct field (warning) */
    for (int i = 0; i < 300 && inst; i++) {
        openthread_mutex_lock(ctx);
        otDeviceRole role = otThreadGetDeviceRole(inst);
        openthread_mutex_unlock(ctx);
        if (role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER) {
            printk("thread: attached role=%d\n", role);
            return;
        }
        k_msleep(100);
    }
    printk("thread: attach timed out, continuing\n");
}
```
If Step 1 showed `openthread_run`/`openthread_mutex_lock` take no args or different args, adjust the calls to match — the build in Step 4 is the arbiter (absent = error, deprecated = warning).

- [ ] **Step 3: Board-select in CMake**

Replace the Task-1 ncs-only block in `zephyr/CMakeLists.txt` with:
```cmake
if (CONFIG_NET_L2_OPENTHREAD)
  if (CONFIG_SOC_SERIES_ESP32C6)          # upstream-4.2 context API
    target_sources(app PRIVATE src/backend/zephyr/thread_bringup_c6.c)
  else()                                   # nRF / NCS
    target_sources(app PRIVATE src/backend/ncs/thread_bringup_ncs.c)
  endif()
endif()
```
(If Step 1 of Task 3 shows the C6 Kconfig symbol is `CONFIG_SOC_ESP32C6` not `..._SERIES_...`, use that — confirm with `grep -r "config SOC_SERIES_ESP32C6\|config SOC_ESP32C6" ~/zephyr-esp32/soc/espressif`.)

- [ ] **Step 4: C6 Thread build gate (hardware-free) — the real arbiter**

Run:
```bash
source ~/zephyr-esp32/.venv/bin/activate && export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.17.4 && source ~/zephyr-esp32/zephyr/zephyr-env.sh
cd ~/workspace/tuvm && bash zephyr/run.sh --node TimerNode.nsl --conf-extra prj_esp32c6_thread.conf --pristine
```
Expected: `Successfully created esp32c6 image.`, `tuvm_thread_bringup` links from `thread_bringup_c6.c`, no errors about absent `openthread_*` symbols (deprecation warnings on `ctx->instance` / mutex are fine). (TimerNode here just exercises the build; the ping node lands in Task 4.)

- [ ] **Step 5: Commit**

```bash
cd ~/workspace/tuvm
git add zephyr/src/backend/zephyr/thread_bringup_c6.c zephyr/CMakeLists.txt
git commit -m "feat(zephyr): C6 upstream-4.2 OpenThread bring-up TU + board select (#59)"
```

---

### Task 3: C6 Thread config placeholders + dataset overlay wiring

Make the committed Thread conf build without secrets, gitignore the real dataset overlay, and have `run.sh` auto-layer it.

**Files:**
- Modify: `zephyr/prj_esp32c6_thread.conf` (append 5 dataset placeholders)
- Modify: `.gitignore` (add the local overlay)
- Modify: `zephyr/run.sh` (auto-layer `prj_esp32c6_thread.local.conf`)

**Interfaces:**
- Produces: a C6 Thread build that is green with placeholders, and that picks up `prj_esp32c6_thread.local.conf` (overriding the placeholders) when present.

- [ ] **Step 1: Append dataset placeholders to the committed Thread conf**

Append to `zephyr/prj_esp32c6_thread.conf`:
```
# --- Thread dataset SEED (first-boot defaults; the persisted NVS copy wins after) ---
# SECRET REDACTED: the real porta dataset (incl. network key) is NOT committed. It
# lives in the gitignored prj_esp32c6_thread.local.conf, auto-layered by run.sh, whose
# values OVERRIDE these placeholders. Read the live dataset:
#   ssh gw85224-01 docker exec otbr ot-ctl dataset active
# Without the overlay the node builds but will NOT join the porta mesh.
CONFIG_OPENTHREAD_NETWORKKEY="00:11:22:33:44:55:66:77:88:99:aa:bb:cc:dd:ee:ff"
CONFIG_OPENTHREAD_PANID=43981
CONFIG_OPENTHREAD_CHANNEL=11
CONFIG_OPENTHREAD_XPANID="de:ad:00:be:ef:00:ca:fe"
CONFIG_OPENTHREAD_NETWORK_NAME="ot_zephyr"
```

- [ ] **Step 2: Gitignore the real overlay**

Add to `.gitignore` (mirror the existing `prj_esp32_wifi.local.conf` entry — confirm its exact form with `grep wifi.local .gitignore`):
```
zephyr/prj_esp32c6_thread.local.conf
```

- [ ] **Step 3: Auto-layer the overlay in `run.sh`**

In `zephyr/run.sh`, after the WiFi-overlay block (`if [ "$BOARD_ID" = esp32 ] && [ -f "$WIFI_CONF" ]; ...`), add a sibling block:
```bash
# C6 Thread dataset overlay (#59): gitignored real porta dataset, auto-layered like
# the WiFi overlay so its 5 fields override prj_esp32c6_thread.conf's placeholders.
THREAD_LOCAL="$ZDIR/prj_esp32c6_thread.local.conf"
if [ "$BOARD_ID" = esp32c6 ] && [ -f "$THREAD_LOCAL" ]; then
  EXTRA_CONF_FILES+=(prj_esp32c6_thread.local.conf)
  echo "[thread] dataset overlay found -> $THREAD_LOCAL"
fi
```
(It stacks after any `--conf-extra prj_esp32c6_thread.conf`, so the local values win.)

- [ ] **Step 4: Build gate — placeholders build clean, no secret required**

Run:
```bash
cd ~/workspace/tuvm && bash zephyr/run.sh --node TimerNode.nsl --conf-extra prj_esp32c6_thread.conf --pristine
```
Expected: `Successfully created esp32c6 image.` with NO `[thread] dataset overlay found` line (the local.conf does not exist yet), proving the committed tree builds green without secrets.

- [ ] **Step 5: Commit**

```bash
cd ~/workspace/tuvm
git add zephyr/prj_esp32c6_thread.conf .gitignore zephyr/run.sh
git commit -m "feat(zephyr): C6 Thread dataset placeholders + run.sh auto-layer overlay (#59)"
```

---

### Task 4: `ThreadPingNode.nsl` — the autonomous CBOR gate node

An in-tree headless node that, on an already-attached mesh, sends a bounded burst of CBOR `#seq` Records to the gw echo, verifies each round-trip, and prints `UDP OK`.

**Files:**
- Create: `zephyr/ThreadPingNode.nsl`

**Interfaces:**
- Consumes: the platform's `NetBridge`, `Record`, `space`, and the `net:` capability (rung-6/7, unchanged). Bring-up (Task 2) guarantees the mesh is attached before `main:` runs.
- Produces: console output `UDP OK` on success, `FAIL: <why>` otherwise; exit 0/1.

- [ ] **Step 1: Write the node** (adapted from `zephyr/ThreadEchoNodeDK.nsl`, button + pulse-yield removed, autonomous bounded burst)

`zephyr/ThreadPingNode.nsl`:
```
(*
ThreadPingNode — #59 C6 end-to-end Thread gate. Headless: on an already-attached
mesh (tuvm_thread_bringup blocks until role=CHILD before the VM starts), bind a
socket, wire a NetBridge to the gw ML-EID echo (:7777), send a bounded burst of
CBOR #reading Records (#seq 1..5), verify each reflected datagram decodes
value-equal, and print UDP OK. Shows as a gate row on gw85224-01:8085. Dialect:
use `[ ... ] whileTrue:`, never `repeat`.
expect (C6 console, after boot + attach):
UDP OK
*)
class ThreadPingNode usingPlatform: platform net: net = () (
  gwAddr = ( ^ 'fdc5:9109:9afe:97ea:9541:d963:f8d9:8567' )   (* gw ML-EID *)
  echoPort = ( ^ 7777 )
  pings = ( ^ 5 )
  public main: platform2 args: args = (
    | space sock bridge got i tries rec |
    space:: platform space.
    sock:: net udpBindPort: 54360.
    sock == nil ifTrue: [ ^ self fail: 'bind' ].
    bridge:: platform NetBridge
      socket: sock out: (space channel: 'echo/out') peer: gwAddr port: echoPort in: (space channel: 'echo/in').
    bridge start.
    (space channel: 'echo/in') whenever: '' do: [ :v | got:: v ].
    i:: 1.
    [ i <= pings ] whileTrue: [
      rec:: platform Record tag: #reading with: #seq -> i with: #milliC -> 235.
      got:: nil.
      (space channel: 'echo/out') out: '' value: rec.
      tries:: 0.
      [ (got == nil) and: [ tries < 2000 ] ] whileTrue: [
        bridge pumpInbound. space drain. tries:: tries + 1 ].
      (got = rec) ifFalse: [ ^ self fail: 'no echo seq' ].
      i:: i + 1 ].
    'UDP OK' println.
    ^ 0
  )
  fail: msg = ( 'FAIL: ' print. msg println. ^ 1 )
) : (
  public packageUsing: manifest = ( ^ self new )
)
```

- [ ] **Step 2: Bake gate — the node compiles to a `.oimg` against the C6 layout**

Run:
```bash
cd ~/workspace/tuvm && source ~/zephyr-esp32/.venv/bin/activate
python3 ~/workspace/nsl-tuvm/tools/ns_compile_oevm.py \
  zephyr/ThreadPingNode.nsl /tmp/ping.oimg --format=oimg --layout=zephyr/layout-esp32c6.json
```
Expected: `OIMG written to /tmp/ping.oimg (... bytes, entry=ThreadPingNode)`, no compile error (DNU / parse). If a selector like `udpBindPort:` / `Record tag:with:with:` DNUs, cross-check the exact selectors against `zephyr/ThreadEchoNodeDK.nsl` and `zephyr/NetNode.nsl` and fix.

- [ ] **Step 3: Full C6 Thread build with the node (hardware-free)**

Run:
```bash
source ~/zephyr-esp32/zephyr/zephyr-env.sh
cd ~/workspace/tuvm && bash zephyr/run.sh --node ThreadPingNode.nsl --conf-extra prj_esp32c6_thread.conf --pristine
```
Expected: `Successfully created esp32c6 image.`; the bake line shows `entry=ThreadPingNode`.

- [ ] **Step 4: Commit**

```bash
cd ~/workspace/tuvm
git add zephyr/ThreadPingNode.nsl
git commit -m "feat(zephyr): ThreadPingNode.nsl — autonomous C6 CBOR gate node (#59)"
```

---

### Task 5: Device bench gate (live silicon + live gateway + radio)

Flash the C6 with the real dataset and witness the round-trip two ways. Requires the C6 on the bench (Espressif native port + CP210x console bridge) and the mesh up (confirmed: 3 nRF nodes soaking OK on `:8085`).

**Files:**
- Create (bench-side, NOT committed): `zephyr/prj_esp32c6_thread.local.conf`
- Create: `zephyr/esp32-debug-artifacts/tuvm-esp32c6-threadping-GREEN-run1.log` (the captured evidence)

- [ ] **Step 1: Materialize the real dataset overlay (gitignored)**

```bash
cp ~/workspace/ns-zephyr/app/prj_thread_local.conf ~/workspace/tuvm/zephyr/prj_esp32c6_thread.local.conf
cd ~/workspace/tuvm && git check-ignore zephyr/prj_esp32c6_thread.local.conf
```
Expected: the path prints (confirming it is gitignored). If it does NOT print, STOP — do not proceed (secret would be committable); fix `.gitignore` first.

- [ ] **Step 2: Confirm the mesh + echo are live**

```bash
ssh gw85224-01 docker exec otbr ot-ctl dataset active | head
```
Expected: an active dataset with `Network Key`, PAN ID etc. (leader up). Optionally open `http://gw85224-01:8085` and note the current node rows.

- [ ] **Step 3: Build + flash + capture**

Run (the overlay now auto-layers; watch for `[thread] dataset overlay found`):
```bash
source ~/zephyr-esp32/.venv/bin/activate && export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.17.4 && source ~/zephyr-esp32/zephyr/zephyr-env.sh
cd ~/workspace/tuvm && bash zephyr/run.sh --node ThreadPingNode.nsl --conf-extra prj_esp32c6_thread.conf --pristine --flash --capture 90 \
  > zephyr/esp32-debug-artifacts/tuvm-esp32c6-threadping-GREEN-run1.log 2>&1
grep -nE "\[thread\] dataset|thread: run=|attached role|UDP OK|FAIL|EXCCAUSE|oops" zephyr/esp32-debug-artifacts/tuvm-esp32c6-threadping-GREEN-run1.log
```
Expected in the log: `[thread] dataset overlay found`, `thread: attached role=3` (CHILD) or `=2` (ROUTER), then `UDP OK`. (ONLY ONE `capture_coredump.py` may hold `/dev/ttyUSB0` at a time — the 90 s window keeps the port until it elapses; do not launch a second capture meanwhile.) If it prints `FAIL:` or attach times out, that is a genuine finding — do NOT paper over it; diagnose (dataset mismatch, OMR-vs-ML-EID route, RX starvation → add the rung-8 timer yield per the design §3).

- [ ] **Step 4: Confirm the dashboard witness**

Open `http://gw85224-01:8085`. Expected: a new ML-EID row (the C6's mesh-local address) as a **gate** node with pings ≥ 5, 0% missed. Note the ML-EID in the commit/close-out.

- [ ] **Step 5: Commit the green log (only the log — never the local.conf)**

```bash
cd ~/workspace/tuvm
git status --short   # verify prj_esp32c6_thread.local.conf is NOT listed
git add zephyr/esp32-debug-artifacts/tuvm-esp32c6-threadping-GREEN-run1.log
git commit -m "test(zephyr): C6 end-to-end Thread ping GREEN — UDP OK + dashboard gate row (#59)"
```

---

### Task 6: Close-out — promote knowledge + update the tracker

Per CLAUDE.md's promotion rule, a rung is not closed until its durable knowledge reaches a mainline doc of record.

**Files:**
- Modify: `~/workspace/tuvm/review/program-sequence.md` (the ladder — the C6 rung entry)

- [ ] **Step 1: Promote to the ladder**

Add a dated entry to `review/program-sequence.md` under the C6 rung recording: the board-selected bring-up seam (nRF NCS vs C6 upstream-4.2, gated on `CONFIG_NET_L2_OPENTHREAD`); the "attach-before-VM start removes the DK's button + pulse-yield hack" lesson; the two-witness gate (console `UDP OK` + `:8085` gate row); and the observed ML-EID. Cite the design doc as the historical "why". (Editing `review/` here is explicitly for this promotion — do not sweep anything else.)

- [ ] **Step 2: Update issue #59**

```bash
gh issue comment 59 --body "Landed: board-selected tuvm_thread_bringup seam (C6 upstream-4.2 + nRF NCS, OPENTHREAD-gated), prj_esp32c6_thread placeholders + gitignored dataset overlay auto-layered by run.sh, and ThreadPingNode.nsl. Bench GREEN: console 'thread: attached role=child' then 'UDP OK', and a new gate row on :8085 (ML-EID <fill>). Log banked. Open-question 1 resolved: no USB-coexistence dance needed on the C6 (UART-bridge console)."
gh issue close 59
```

- [ ] **Step 3: Commit the ladder promotion**

```bash
cd ~/workspace/tuvm
git add review/program-sequence.md
git commit -m "docs(ladder): promote the C6 end-to-end Thread ping rung (#59)"
```

---

## Self-Review

**Spec coverage** (design §1–§5 + deliverables 1–7):
- Bring-up seam / board-select (§1, deliverables 1-2) → Tasks 1, 2. ✓
- Config placeholders + gitignored overlay + run.sh auto-layer (§2, deliverables 3-5) → Task 3 + Task 5 Step 1. ✓
- ThreadPingNode autonomous CBOR gate node (§3, deliverable 6) → Task 4. ✓
- Heap unchanged (§4) → Global Constraints (no task needed — explicitly "do not change"). ✓
- Bench gate, two witnesses (§5, deliverable 7) → Task 5. ✓
- Close-out/promotion (design close-out) → Task 6. ✓

**Placeholder scan:** The only deferred specifics are the exact upstream OT `run`/`mutex` signatures and the C6 Kconfig symbol — both are pinned by concrete grep steps (Task 2 Steps 1/3) with the build as arbiter, not left as prose "TBD". Acceptable and honest for a version-divergent vendor API.

**Type/name consistency:** `tuvm_thread_bringup(void)` is defined identically in `thread_bringup.h` (Task 1 Step 1), the NCS TU (Task 1 Step 2), and the C6 TU (Task 2 Step 2), and called that way in `main.c` (Task 1 Step 3). The node's selectors (`udpBindPort:`, `NetBridge socket:out:peer:port:in:`, `Record tag:with:with:`, `pumpInbound`, `space drain`, `channel:`, `out:value:`, `whenever:do:`) are copied from the proven `ThreadEchoNodeDK.nsl`; Task 4 Step 2 re-verifies them by baking. Echo address/port match the design and `ThreadEchoNodeDK.nsl`.
