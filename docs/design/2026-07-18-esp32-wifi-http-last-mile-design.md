# ESP32/WiFi HTTP last mile (#46) — design

**Date:** 2026-07-18
**Issue:** tuvm#46 — "Webserver spike: ESP32/WiFi HTTP last mile (serve /thermal over WiFi)"
**Predecessor:** the webserver channel-device spike (#1), which landed **fully on posix**
and green — see `docs/design/2026-07-18-webserver-channel-device-spike-design.md`,
plan `docs/superpowers/plans/2026-07-18-webserver-channel-device-spike.md`, findings
`review/model-capability-findings.md`. Series:
`docs/design/2026-07-18-coordination-model-spike-series-design.md`.

## Goal

Serve the existing channel-native HTTP node (`/`, `/thermal`, `/stats`, `/hello`) from a
real **ESP32-D0WDQ6 / WROOM-32** over **WiFi**, viewable in a browser at
`http://<dhcp-ip>:18080/thermal` (the node binds `httpListen: 18080` and stays frozen — see
the port note in §Risks). The model verdict is already recorded (request/reply over
channels is a clean fit); this issue is purely the **on-silicon transport**: a Zephyr HTTP
backend + a WiFi/TCP Kconfig + a WiFi-association boot sequence, then flash-and-view.

The nsl node (`zephyr/HttpServerNode.nsl`) and all routing/reduce/render logic are
**frozen** — this work is transport only, nothing above the channel seam changes.

## What already landed (posix, spike #1)

- Driver seam `src/http_backend.h` + `src/backend/posix/http_posix.c`: HTTP/1.0 framing,
  request ring, bounded in-flight table with G2 shed (503 on the (N+1)th), and a real TCP
  **accept thread** + `httpListen:`.
- Platform prims `httpListen:`, `httpRespondId:status:body:`, `httpInject:path:`;
  `http_pump()` hooked into `PRIM_pf_pumpEvents` (`src/platform.c:763`).
- Node `zephyr/HttpServerNode.nsl` (+ conformance twin `HttpNode.nsl`).
- Tests: C unit `tests/test_http.c`, conformance `nsl-tests/14_http/0{0..3}`, curl
  integration `tests/http_roundtrip_host.sh` — all green (8-way burst sheds 503 under load).

`prj_esp32.conf` today is **loopback-UDP only — no TCP, no WiFi driver**. That is the gap.

## Decisions (resolved in brainstorming)

1. **Scope:** all the way to live hardware — backend + Kconfig + associate + flash + browse.
   A device is on `/dev/ttyUSB0` (Silicon Labs CP210x bridge = the ESP32 devkit).
2. **Transport model:** **non-blocking poll**, mirroring `udp_zephyr.c`'s
   `recvfrom(ZSOCK_MSG_DONTWAIT)` idiom — **not** a `k_thread` accept loop. The reactor
   loop already busy-spins (`[ true ] whileTrue: [ platform pumpEvents. space drain ]`), so
   polling `accept` each tick is ~free, needs no thread/mutex/extra stack (ESP32 RAM is
   tight), and keeps the VM strictly single-threaded. Threads-later is a non-breaking
   backend swap (see §"Why the seam makes poll↔thread invisible").
3. **Network config:** IPv4 + **DHCPv4** (standard home-router path). WiFi driver + TCP +
   DHCPv4; drop the loopback `::1` block.
4. **WiFi credentials:** kept out of git in a **git-ignored overlay**
   `zephyr/prj_esp32_wifi.local.conf` (added to `.gitignore`), generated from the existing
   git-ignored `~/workspaceToit/nodus/docs/secrets.md`. Two Kconfig symbols
   `CONFIG_TUVM_WIFI_SSID` / `CONFIG_TUVM_WIFI_PSK`; no secret in any tracked file.

## Architecture

### Code organization — factor a shared core

Spike #1's `http_posix.c` bundles two concerns: **transport-agnostic** logic (HTTP framing,
the request ring, the bounded in-flight table + G2 shed, id minting) and **posix-specific**
transport (the accept *thread*, `write`+`close`). Split them:

- **`src/http_core.c` (new)** — transport-agnostic, shared by every target:
  - `http_frame_response(...)` (pure)
  - the request ring, the in-flight table, `next_id`
  - `http_accept_request`, `http_take_request`, `http_respond`, `http_inflight_count`,
    `http_backend_reset`
  - reply/503 bytes leave via a per-target hook `http_emit(int fd, const char *resp, int n)`.
- **`src/backend/posix/http_posix.c`** — posix transport hooks only:
  - `int http_listen(int port)` — bind + `listen`, listener set **non-blocking** (`O_NONBLOCK`)
  - `void http_poll(void)` — `accept4(...,SOCK_NONBLOCK)`/`accept`+`EWOULDBLOCK` → read the
    request line, parse `METHOD SP PATH`, `http_accept_request(fd, ...)`
  - `void http_emit(int fd, const char *resp, int n)` — `write`+`close` (or stdout when `fd==-1`)
- **`src/backend/zephyr/http_zephyr.c` (new)** — Zephyr transport hooks only (same three),
  over `zsock_*`. See §"Device transport".

**Posix converts from accept-thread → poll** as part of this split. Rationale: it unifies
host and device on **one** transport path, so `tests/http_roundtrip_host.sh` exercises the
*exact* poll code the ESP32 runs; it removes the pthread + ring mutex; and it is the model
endorsed for the device. The pump gains one line:

```c
case PRIM_pf_pumpEvents: {
    http_poll();          /* NEW: non-blocking accept/read one connection */
    int n = pulse_pump();
    http_pump();          /* ring -> oops -> space_post, at the safe point */
    *out = TAG_SMI(n);
    return 1;
}
```

`httpInject:path:` (the test seam, `fd == -1`) is unchanged — it calls `http_accept_request`
directly, independent of `http_poll`.

### Device transport (`src/backend/zephyr/http_zephyr.c`)

Mirrors `udp_zephyr.c`:

- `http_listen(port)`: `zsock_socket(AF_INET, SOCK_STREAM, 0)`, `SO_REUSEADDR`, bind
  `INADDR_ANY:port`, `zsock_listen`, set the listener non-blocking (`zsock_fcntl` O_NONBLOCK).
  IPv4 (WiFi/DHCP), not the UDP backend's IPv6-first idiom.
- `http_poll()`: `zsock_accept` on the non-blocking listener → `-1`/`EWOULDBLOCK` returns
  immediately; on a fd, `zsock_recv` the request bytes, parse `METHOD SP PATH SP`, discard
  the rest, `http_accept_request(fd, method, ml, path, pl)`. One connection per tick is fine
  (the loop spins fast); can drain to empty in a `for(;;)` break-on-EWOULDBLOCK if needed.
- `http_emit(fd, resp, n)`: `zsock_send(fd, resp, n, 0)` then `zsock_close(fd)`.

C never holds an oop across a boundary — there is no boundary; everything runs on the VM
thread, oops are built only in `http_pump` at the safe point.

### Network config (`zephyr/prj_esp32.conf`)

Remove the loopback block (`CONFIG_NET_LOOPBACK`, the `::1`/`127.0.0.1` `NET_CONFIG` lines,
the "never moves a packet" note). Add:

```
# WiFi (2.4 GHz) + TCP/IPv4 + DHCP for the HTTP node
CONFIG_WIFI=y
CONFIG_WIFI_ESP32=y
CONFIG_NET_TCP=y
CONFIG_NET_DHCPV4=y
CONFIG_NET_IPV4=y
CONFIG_NET_SOCKETS=y
CONFIG_NET_MGMT=y
CONFIG_NET_MGMT_EVENT=y
CONFIG_NET_L2_ETHERNET=y            # WiFi presents as an ethernet L2 to the IP stack
# net-buffer / context counts sized for a small TCP server (tuned at bring-up)
```

Keep as-is: `CONFIG_ESP32_USE_UNSUPPORTED_REVISION=y` (rev-1.0 bootloader opt-in), the
picolibc/FPU/`CBPRINTF_FP_SUPPORT`/heap block, and console-on-uart0. Exact WiFi/net-buf
Kconfig symbol names are validated against the installed `~/zephyr-esp32` SDK during
bring-up (the S2 build is the gate) — adjust to the SDK if a symbol differs, exactly as
`udp_zephyr.c`'s header note warns.

### WiFi association boot hook

A small C hook runs **before** the VM's `main:` binds the listener, so the node is on the
network first:

1. Wait for the WiFi interface to be up.
2. `net_mgmt(NET_REQUEST_WIFI_CONNECT, ...)` with an `struct wifi_connect_req_params` built
   from `CONFIG_TUVM_WIFI_SSID` / `CONFIG_TUVM_WIFI_PSK` (WPA2-PSK, 2.4 GHz).
3. Block on the `NET_EVENT_IPV4_ADDR_ADD` (DHCPv4 lease) with a timeout; log the acquired IP
   to the console (so the operator can point a browser at it). On timeout, log and continue
   (the listener still binds; useful for diagnosis).

Location: a `SYS_INIT`/boot path in the Zephyr entry (`main.c`) guarded on
`CONFIG_WIFI` so non-WiFi boards (DK/dongle) compile it out — same guard idiom as the
existing GPIO/OpenThread blocks. A new app-local Kconfig declares
`TUVM_WIFI_SSID`/`TUVM_WIFI_PSK` (string), default `""`; the git-ignored overlay supplies the
real values.

### Data flow

```
boot: wifi_connect(SSID,PSK) --> DHCPv4 lease (log IP)
main:  httpListen: 18080      --> http_listen: bind + non-blocking listener
loop:  pumpEvents:
         http_poll()          --> zsock_accept(nonblock); read line; parse;
                                   http_accept_request(fd,method,path)  [G2 shed if full]
         http_pump()          --> ring -> [id,method,path] Array -> space_post 'dev/http/request'
       app reaction           --> route + render (frozen nsl) -> reply Record
       egress reaction        --> httpRespondId:status:body: -> http_respond -> http_emit(fd)
browser: GET http://<dhcp-ip>:18080/thermal --> 200 + 8x8 heat-grid SVG
```

Identical to posix above the seam; only `http_listen`/`http_poll`/`http_emit` differ per target.

## Why the seam makes poll↔thread invisible (recorded rationale)

The nsl app sees only **channels** (`dev/http/request`, `dev/thermal0/sample`); never a
socket, thread, poll, or mutex. All transport lives in C below the channel boundary. Whether
bytes arrive by poll or by a future `k_thread` pushing into a ring, the app receives the
identical `space_post` at the `pumpEvents` safe point. The single invariant that makes the
swap non-breaking: **VM objects (oops) are built/touched only on the VM thread, at the safe
point.** Poll satisfies it trivially; a thread would satisfy it by moving only raw bytes and
letting `http_pump` build the oops. Hence poll now, threads-later (for a power/latency need)
is a `http_zephyr.c` rewrite + relink with **zero** nsl change.

## Cross-source scheduling (recorded design point — issue Q2)

`pumpEvents` drains sources in **fixed order, bounded per source** (`http_poll; pulse_pump;
http_pump`), not strict round-robin. Starvation is bounded structurally: each source has a
bounded queue (HTTP in-flight capped at `OEVM_HTTP_INFLIGHT = 4` → 503 beyond it), so no
source can produce unbounded work to drain in one tick. Strict round-robin (one item per
source per tick) is a future **pump-policy knob** decided in that one function, invisible to
apps. Adding a source (e.g. SPI) is adding one `spi_pump()` line — N sources cost N drains,
zero threads. This is noted in `review/model-capability-findings.md`.

## Testing / verification

1. **Build gate (self-contained):** `west build -b esp32_devkitc/esp32/procpu --
   -DCONF_FILE=prj_esp32.conf -DEXTRA_CONF_FILE=prj_esp32_wifi.local.conf` compiles clean
   with the new Kconfig + `http_zephyr.c` + associate hook. This is the project's standard
   device-build gate (matches `udp_zephyr`).
2. **Host regression:** `tests/http_roundtrip_host.sh` still green after the posix→poll
   conversion (curl `/thermal`→200+`rgb(`, `/nope`→404, 8-way burst sheds ≥1 503); C unit
   `tests/test_http.c` still `OK`; the `nsl-tests/14_http/0{0..3}` conformance suite green.
3. **Live (device on `/dev/ttyUSB0`):** `west flash`; console shows WiFi associate + the
   DHCP IP; `curl http://<ip>:18080/thermal` → 200 + SVG, `/nope` → 404, an 8-way `curl` burst
   shows a 200/503 mix; the 8×8 heat-grid renders in a browser.

The socket/framing/ring/in-flight logic is already host-proven by the posix backend (now the
*same* poll code); only **WiFi association** genuinely needs real ESP32 silicon — so no
`native_sim` standin is added (it would prove nothing posix hasn't).

## Risks

- **RAM budget.** The WiFi driver + TCP + net buffers are heavy on ESP32 (~300 KB DRAM,
  shared with the static VM heap `OEVM_HEAP_SIZE` and the float arena). May require trimming
  `OEVM_HEAP_SIZE` and/or `NET_PKT`/`NET_BUF` counts to fit. Flagged; tuned empirically at
  bring-up (link/boot will show the shortfall). Reuse the DK's proven env-export tuning as a
  starting point.
- **2.4 GHz only.** WROOM-32 has no 5 GHz radio — the SSID must be served on 2.4 GHz.
- **Port 18080 (node stays frozen).** The node binds `httpListen: 18080`; keeping it means
  the browser/curl use the explicit `:18080` (no node edit, no route/render change). Device
  has no privilege concept, so 18080 is fine; the host test already uses the same port.
- **SDK symbol drift.** Exact `CONFIG_WIFI_*` / `zsock_*` names verified against the
  installed `~/zephyr-esp32` at bring-up; the build is the gate.
- **fflush/ROM-newlib fault** — already handled (no-op `platform_console_flush` on Zephyr);
  no new stdio on the hot path.

## Out of scope

- #36's broader ESP32/WROOM-32 perfstex port (ESP-IDF/FreeRTOS). This issue shares the ESP32
  infra but its deliverable is the HTTP thermal demo, not the benchmark. Sequence with #36 TBD.
- Any change to routes, reduce, or render — the nsl node is frozen; this is transport only.
- TLS/HTTPS, keep-alive, non-GET methods (spike #1 scope stands: HTTP/1.0, `Connection: close`,
  GET only).

## File manifest

- `src/http_core.c` — **new** (extracted transport-agnostic core).
- `src/http_backend.h` — **modify** (declare `http_poll`, `http_emit`; keep the seam).
- `src/backend/posix/http_posix.c` — **modify** (thread → poll; `http_poll`/`http_emit`).
- `src/backend/zephyr/http_zephyr.c` — **new** (Zephyr `zsock_*` transport).
- `src/platform.c` — **modify** (add `http_poll()` to the pump).
- `zephyr/main.c` — **modify** (WiFi-associate boot hook, guarded on `CONFIG_WIFI`).
- `zephyr/prj_esp32.conf` — **modify** (WiFi + TCP + DHCPv4; drop loopback).
- `zephyr/Kconfig` (app) — **new/modify** (`TUVM_WIFI_SSID`/`TUVM_WIFI_PSK`).
- `zephyr/prj_esp32_wifi.local.conf` — **git-ignored** (already generated; the values).
- `.gitignore` — **modified** (already; the overlay line).
- build glue (`run_c_tests.sh`, `run_tests_oevm.py`, `bench.sh`) — **modify** (link
  `src/http_core.c`; the host link no longer needs `-lpthread` for http).
- `review/model-capability-findings.md`, `review/program-sequence.md` — **modify** (record
  the on-device close-out + the scheduling design point).
