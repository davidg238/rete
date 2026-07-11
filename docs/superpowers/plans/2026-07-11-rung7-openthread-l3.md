# Rung 7 — Layer 3: NetBridge over real OpenThread (MTD child) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run the Rung-6 `NetBridge` (CBOR channel↔UDP, unchanged) over a real IEEE 802.15.4 / OpenThread mesh — a tuvm MTD child on the nRF52840-DK round-trips one CBOR `Record` against a live UDP echo server on the porta gateway, printing `BRIDGE OK`.

**Architecture:** No VM / bridge / CBOR change — this rung is radio/mesh bring-up. The tuvm node is a **Model-A solidified image**, an **MTD child** that auto-joins the existing porta OTBR mesh using a baked (gitignored) dataset. Its `NetBridge` sends a `Record` to the gateway's OMR address; a non-porta `udp-echo6` systemd service (already deployed) reflects the bytes verbatim; the node decodes the reflection value-equal and its `whenever:` reaction prints `BRIDGE OK`. A **host pre-flight** (Task 1) proves the identical client stack against the same echo over Tailscale IPv6 first — radio excluded.

**Tech Stack:** Newspeak dialect (`nsl-tuvm/lib/kernel.ns`, unchanged), tuvm Model-A solidifier (`ns_compile_oevm.py`), Zephyr + NCS SDK OpenThread L2 (`CONFIG_NET_L2_OPENTHREAD`, MTD), west + on-board J-Link, the tuvm `udp_zephyr.c` `zsock_*` backend (unchanged), a Python UDP echo on `gw85224-01`.

## Global Constraints

- **No VM / bridge / CBOR / facade source change.** `lib/kernel.ns`, `tuvm/src/udp.c`, `Cbor`, `NetBridge` are all frozen this rung. Only new Zephyr config + node `.ns` + docs. If any of them *needs* editing, stop and escalate — it means a premise broke.
- **MTD only, never FTD** (David, firm — RAM/MPSL boot-fault lesson from `ns-zephyr`). `CONFIG_OPENTHREAD_MTD=y` / `CONFIG_OPENTHREAD_FTD=n`.
- **The Thread network key is a SECRET.** It lives ONLY in the gitignored `tuvm/zephyr/prj_dk_thread_local.conf` (copied from `ns-zephyr/app/prj_thread_local.conf`). The committed `prj_dk_thread.conf` carries OpenThread-default PLACEHOLDER dataset values. **Never write the real key into any committed file, this plan, a commit message, or a log paste.**
- **Three repos, separate commits.** `tuvm` (Zephyr config + node `.ns` + tuvm docs + footprint baseline), `nsl-tuvm` (docs/plan/spec only this rung), `nsl-tests` (only a *transient, uncommitted* pre-flight copy in Task 1). Never cross-commit; `cd` into the right repo before `git add`.
- **`solidified_image.c` is a generated artifact** shared by other tests. Baking a node CLOBBERS `tuvm/src/solidified_image.c` + `.stmap`; ALWAYS restore before committing anything in tuvm:
  ```bash
  git -C /home/david/workspace/tuvm checkout -- src/solidified_image.c src/solidified_image.stmap
  ```
- **The proof is a bench-verified MANUAL gate** (live silicon + live gateway + radio), like Rung-4 Task-8's button press — NOT a `west build` CI gate. Tasks 3–4 are executed by the controller **with David at the bench** (flashing, pressing BUTTON1, reading `/dev/ttyACM0` and `ot-ctl`). A subagent runs Task 1 and the Task-2 *build*; it cannot flash or observe hardware.
- **The Rung-6 host + native_sim floor stays green untouched** — full both-path suite `180/0/12` + the five named gates. No source edit should move them; confirm at close (Task 5).
- **Zephyr build needs the SDK env** sourced each shell: `source /home/david/workspace/ns-zephyr/tools/env.sh`.
- **DK build config-D env** (RAM-tuned, from `tuvm/zephyr/FOOTPRINT_DK.md`), exported before any DK bake/build:
  ```bash
  export OEVM_HEAP_SIZE=49152 METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256
  export VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512
  ```
  (OpenThread + mbedTLS eat SRAM the loopback build did not — if Task 2 does not RAM-fit, **shrink `OEVM_HEAP_SIZE` first**, e.g. 40960/32768, since it is the largest single knob.)
- **Gateway facts** (verified 2026-07-11; see memory `rung7-gateway-thread-infra`): `gw85224-01` (Tailscale `100.87.217.33`, IPv6 `fd7a:115c:a1e0::bf01:d921`), OTBR **leader**, dataset == `ns-zephyr` overlay. Gateway **OMR** `fd77:9957:d9f3:1:2935:a821:1d56:c329`; **ML-EID** `fdc5:9109:9afe:97ea:9541:d963:f8d9:8567`. `udp-echo6.service` on **:7777** (opaque byte-verbatim reflect); porta on `:6969` (leave alone).
- **Commit-message trailer** on every commit:
  ```
  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
  ```

---

## File Structure

- `tuvm/zephyr/EchoClientHost.ns` — CREATE (Task 1). Host pre-flight: a single `NetBridge` client targeting the live gw echo over Tailscale IPv6. Reusable reference; run on host via the conformance harness (transient copy). NOT a suite member.
- `tuvm/zephyr/ThreadEchoNodeDK.ns` — CREATE (Task 2). The DK Thread node: net + pulse-triggered `NetBridge` send to the gw OMR echo; prints `BRIDGE OK`. Baked into `solidified_image.c`.
- `tuvm/zephyr/prj_dk_thread.conf` — CREATE (Task 2). `prj_dk.conf`'s console/libc/heap + the OpenThread MTD L2 block lifted from `ns-zephyr/app/prj_thread.conf` (uart0 console, so the USB-CDC coexistence config drops away); PLACEHOLDER dataset.
- `tuvm/zephyr/prj_dk_thread_local.conf` — CREATE (Task 2), **gitignored**. The real dataset overlay, copied from `ns-zephyr/app/prj_thread_local.conf`. Never committed.
- `tuvm/.gitignore` — MODIFY (Task 2). Add `zephyr/prj_dk_thread_local.conf`.
- `tuvm/zephyr/README.md` — MODIFY (Task 5). Add the Rung-7 Thread gate section.
- `tuvm/review/program-sequence.md` — MODIFY (Task 5). Phase-3 line → Layer 3 done.
- `tuvm/tools/footprint-*` / `tuvm/zephyr/FOOTPRINT_DK.md` — MODIFY (Task 5, if footprint moved — it will). Re-bank the DK figure.
- dialect/vocabulary doc (grep to locate; do NOT invent) — MODIFY (Task 5). One-line Layer-3 note.

---

## Task 1: Pre-flight — host NetBridge client → live gw echo over Tailscale IPv6

Prove the exact Layer-2 client round-trip (`Record → CBOR → sendTo: → real UDP/IPv6 → gw udp-echo6 → reflect → receive → decode → whenever:`) against the **live** echo server, with **no radio**. This de-risks the client stack and confirms the deployed echo is a valid peer for our NetBridge before any hardware. Runnable by a subagent (this machine is on Tailscale; raw-UDP reachability to `[fd7a:115c:a1e0::bf01:d921]:7777` already confirmed byte-exact).

**Files:**
- Create: `tuvm/zephyr/EchoClientHost.ns`
- (Transient, never committed): `nsl-tests/04_somns_basic/46_echo_client_live.ns`

**Interfaces:**
- Consumes: `platform space` / `platform NetBridge` / `platform Record` / `net udpBindPort:` (all from the frozen `kernel.ns`), the `run_tests_oevm.py` net-granting single-file harness.
- Produces: `tuvm/zephyr/EchoClientHost.ns` — a reusable host echo client (`ECHO OK` sentinel). No new nsl API.

- [ ] **Step 1: Create `tuvm/zephyr/EchoClientHost.ns`**

```
(*
EchoClientHost — Rung 7 pre-flight. A single NetBridge client against the LIVE
udp-echo6 server on gw85224-01 over Tailscale IPv6 (NO radio). Proves the whole
Layer-2 client round-trip: a Record is CBOR-encoded, sent over real UDP/IPv6 to
the gw echo (:7777), reflected byte-verbatim, received, decoded value-equal, and
a whenever: reaction observes it. Depends on the live gw + Tailscale, so it is
NOT a conformance-suite member — run it manually (Task 1 harness copy).
expect:
ECHO OK
*)
class EchoClientHost usingPlatform: platform net: net = () (
  gwAddr = ( ^ 'fd7a:115c:a1e0::bf01:d921' )   (* gw85224-01 Tailscale IPv6 *)
  echoPort = ( ^ 7777 )
  public main: platform2 args: args = (
    | space sock bridge got rec |
    space:: platform space.
    sock:: net udpBindPort: 54350.
    sock == nil ifTrue: [ ^ self fail: 'bind' ].
    bridge:: platform NetBridge
      socket: sock out: (space channel: 'echo/out') peer: gwAddr port: echoPort in: (space channel: 'echo/in').
    bridge start.
    got:: nil.
    (space channel: 'echo/in') whenever: '' do: [ :v | got:: v ].
    rec:: platform Record tag: #reading with: #seq -> 1 with: #milliC -> 235.
    (space channel: 'echo/out') out: '' value: rec. space drain.
    (* busy-poll until the reflection arrives; a lost datagram => the outer
       `timeout`/harness kills the run (a genuine FAIL), never a silent pass *)
    [ got == nil ] whileTrue: [ bridge pumpInbound. space drain ].
    (got = rec) ifTrue: [ 'ECHO OK' println ] ifFalse: [ ^ self fail: 'echo mismatch' ].
    ^ 0
  )
  fail: msg = ( msg println. ^ 1 )
) : (
  public packageUsing: manifest = ( ^ self new )
)
```

- [ ] **Step 2: Make the transient harness copy and run it (verify it PASSES against the live echo)**

The conformance harness threads the `net` grant when the entry declares `net:` and does the host build+run+expect-compare. Copy the node in transiently (it depends on the live gw, so it must NOT stay in the suite):

```bash
cp /home/david/workspace/tuvm/zephyr/EchoClientHost.ns \
   /home/david/workspace/nsl-tests/04_somns_basic/46_echo_client_live.ns
cd /home/david/workspace/nsl-tuvm
OEVM_PATH=AB python3 tools/run_tests_oevm.py 46_echo_client_live 2>&1 | tee /tmp/rung7_preflight.log
```

Expected: PASS on Path A and Path B — `ECHO OK`. On the gw, `journalctl -u udp-echo6.service -n 2 -o cat` shows a fresh `echo NB <-> [fd7a:115c:a1e0::8733:894a%0]:54350` line (the reflection this run triggered). If it FAILS with a hang→timeout, the datagram did not round-trip: confirm `udp-echo6.service` is active and `ssh david@gw85224-01 'ss -uln | grep 7777'` shows the listener; confirm `python3 -c "import socket; s=socket.socket(10,2); s.settimeout(3); s.sendto(b'x',('fd7a:115c:a1e0::bf01:d921',7777)); print(s.recvfrom(64))"` still round-trips from this host.

- [ ] **Step 3: Remove the transient harness copy (it must never be committed to the suite)**

```bash
rm /home/david/workspace/nsl-tests/04_somns_basic/46_echo_client_live.ns
git -C /home/david/workspace/nsl-tests status --short   # expect: clean (nothing to commit)
```

Expected: `nsl-tests` is clean — the pre-flight copy is gone, nothing staged.

- [ ] **Step 4: Commit the reusable host client (tuvm only)**

```bash
cd /home/david/workspace/tuvm
git add zephyr/EchoClientHost.ns
git commit -m "feat(rung7): host NetBridge echo client vs live gw (pre-flight)

A single NetBridge client that round-trips a CBOR Record against the live
udp-echo6 server on gw85224-01 over Tailscale IPv6 (ECHO OK) — proves the
Layer-2 client stack against the real echo peer with no radio. Not a suite
member (depends on the live gw); run via a transient conformance-harness copy.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: DK Thread node + config; build + RAM-fit

Create the OpenThread MTD config (with the secret dataset gitignored) and the DK Thread node, and prove it **builds and RAM-fits** on the nRF52840 — no flash yet. This isolates the config/RAM risk from the radio risk.

**Files:**
- Create: `tuvm/zephyr/ThreadEchoNodeDK.ns`
- Create: `tuvm/zephyr/prj_dk_thread.conf` (placeholder dataset)
- Create: `tuvm/zephyr/prj_dk_thread_local.conf` (real dataset, gitignored)
- Modify: `tuvm/.gitignore`

**Interfaces:**
- Consumes: the frozen `kernel.ns` (`platform space`/`NetBridge`/`Record`/`gpio`/`pumpEvents`, `net udpBindPort:`), the existing DK pulse-GPIO plumbing (`tuvm/zephyr/boards/nrf52840dk_nrf52840.overlay` `pulsegpio` alias + `src/main.c` `wire_pulse_gpio()`, from Task 8 — unchanged), `ns_compile_oevm.py` net boot-threading.
- Produces: `ThreadEchoNodeDK.ns` (`BRIDGE OK` sentinel, BUTTON1-triggered send to the gw OMR echo); `prj_dk_thread.conf` (the DK OpenThread MTD Kconfig, selected via `-DCONF_FILE`).

- [ ] **Step 1: Create `tuvm/zephyr/ThreadEchoNodeDK.ns`**

The send is triggered by a **physical BUTTON1 press** (reusing the proven Task-8 pulse-GPIO seam) so it happens *after* the MTD has had a few seconds to attach — sidestepping join-latency with a human-timed single clean TX (no packet storm on a flaky radio).

```
(*
ThreadEchoNodeDK — Rung 7 Layer-3 gate. Model-A DK node (baked into
src/solidified_image.c). Declares net:, binds a socket, wires one NetBridge to
the gw OMR echo (:7777) over the OpenThread mesh. Reuses the Task-8 pulse-GPIO
seam: a PHYSICAL BUTTON1 press (after the MTD attaches) posts the Record, whose
outbound reaction CBOR-encodes + sends it over 802.15.4; the gw echo reflects it;
pumpInbound receives + decodes value-equal; the whenever: reaction prints
BRIDGE OK on uart0/VCOM (/dev/ttyACM0). Dialect note (as PulseNodeDK): use
`[ ... ] whileTrue:`, not `repeat`.
expect (on the DK console, after boot + attach + a BUTTON1 press):
BRIDGE OK
*)
class ThreadEchoNodeDK usingPlatform: platform net: net = () (
  gwAddr = ( ^ 'fd77:9957:d9f3:1:2935:a821:1d56:c329' )   (* gw OMR (mesh-routable) *)
  echoPort = ( ^ 7777 )
  public main: platform2 args: args = (
    | space sock bridge gpio trigger got rec |
    space:: platform space.
    sock:: net udpBindPort: 54360.
    sock == nil ifTrue: [ ^ self fail: 'bind' ].
    bridge:: platform NetBridge
      socket: sock out: (space channel: 'echo/out') peer: gwAddr port: echoPort in: (space channel: 'echo/in').
    bridge start.
    got:: nil.
    (space channel: 'echo/in') whenever: '' do: [ :v | got:: v ].
    rec:: platform Record tag: #reading with: #seq -> 1 with: #milliC -> 235.
    (* BUTTON1 (P0.11, `pulsegpio` alias) edge -> post the Record to echo/out *)
    trigger:: space channel: 'sensor/alert'.
    trigger whenever: '' do: [ :e | (space channel: 'echo/out') out: '' value: rec ].
    gpio:: platform gpio.
    gpio pin: 13 edge: #rising post: 'go' on: trigger.
    'THREAD NODE READY — press BUTTON1 after attach' println.
    (* pump pulses + inbound each turn; exit once the echo round-trips back *)
    [ got == nil ] whileTrue: [ platform pumpEvents. bridge pumpInbound. space drain ].
    (got = rec) ifTrue: [ 'BRIDGE OK' println ] ifFalse: [ ^ self fail: 'echo mismatch' ].
    ^ 0
  )
  fail: msg = ( msg println. ^ 1 )
) : (
  public packageUsing: manifest = ( ^ self new )
)
```

- [ ] **Step 2: Create the gitignored real-dataset overlay (never committed)**

Copy the real dataset from the existing `ns-zephyr` overlay (it already holds the live values and matches the gw's active dataset) and gitignore it BEFORE it can be staged:

```bash
cd /home/david/workspace/tuvm
cp /home/david/workspace/ns-zephyr/app/prj_thread_local.conf zephyr/prj_dk_thread_local.conf
printf '\nzephyr/prj_dk_thread_local.conf\n' >> .gitignore
git check-ignore zephyr/prj_dk_thread_local.conf   # MUST print the path (proves it is ignored)
```

Expected: `git check-ignore` echoes `zephyr/prj_dk_thread_local.conf` (ignored). If it prints nothing, STOP — the secret is not yet ignored; fix `.gitignore` before continuing. (If `ns-zephyr/app/prj_thread_local.conf` is absent, read the live dataset with `ssh david@gw85224-01 'docker exec otbr ot-ctl dataset active'` and hand-write the five `CONFIG_OPENTHREAD_*` lines — but never into a committed file.)

- [ ] **Step 3: Create `tuvm/zephyr/prj_dk_thread.conf` (committed; PLACEHOLDER dataset)**

`prj_dk.conf`'s console/libc/GPIO + the OpenThread MTD L2 block from `ns-zephyr/app/prj_thread.conf`, minus everything USB-CDC (the DK console is uart0/VCOM, so the ns-zephyr USB-enumeration lessons do not apply). Networking switches from loopback to OpenThread. Heap bumped to 32768 (the ns-zephyr lesson: net/OT/mbedTLS boot allocs draw on the k_malloc pool).

```
# tuvm/zephyr/prj_dk_thread.conf — Rung 7 (Layer 3): NetBridge over real
# OpenThread on the nRF52840-DK, MTD child. Selected with
# `-DCONF_FILE=prj_dk_thread.conf`; the gitignored prj_dk_thread_local.conf is
# layered via `-DEXTRA_CONF_FILE` and overrides the placeholder dataset below.
#
# Console on uart0 -> on-board SEGGER VCOM (/dev/ttyACM0). No USB-CDC: the DK
# routes its console to uart0, so the ns-zephyr USB-coexistence config
# (USBD_CDC_ACM_WORKQUEUE, deferred enable, MANUAL_START) is NOT needed here.
CONFIG_CONSOLE=y
CONFIG_UART_CONSOLE=y
CONFIG_STDOUT_CONSOLE=y

# libc with malloc/realloc/free, setjmp, snprintf, FP printf for the VM arena.
CONFIG_NEWLIB_LIBC=y
CONFIG_NEWLIB_LIBC_FLOAT_PRINTF=y
CONFIG_CBPRINTF_FP_SUPPORT=y
CONFIG_FPU=y
# System heap: OT + mbedTLS + net boot-time allocs draw on this pool (NOT the VM
# arena, which is the static oevm_heap_store sized by OEVM_HEAP_SIZE). ns-zephyr
# learned 16384 is too small for the OT boot path; use 32768.
CONFIG_HEAP_MEM_POOL_SIZE=32768
CONFIG_MAIN_STACK_SIZE=8192

# GPIO: the on-board BUTTON1 (P0.11) is the physical send trigger (Task-8 seam).
CONFIG_GPIO=y

# --- Networking: IPv6 UDP over OpenThread (IEEE 802.15.4 mesh), MTD child ------
CONFIG_NETWORKING=y
CONFIG_NET_IPV6=y
CONFIG_NET_UDP=y
CONFIG_NET_SOCKETS=y
CONFIG_NET_L2_OPENTHREAD=y
# MTD, never FTD (David, firm — FTD's MPSL/radio/RAM weight faulted at boot and
# sat at 91% RAM). A leaf attaches as a child of the OTBR and sends through it.
CONFIG_OPENTHREAD_MTD=y
CONFIG_OPENTHREAD_FTD=n
# OT init runs PSA/AES-CCM link crypto on this stack at boot; too small -> MPU
# fault before the console comes up (ns-zephyr lesson). 6240 is the proven size.
CONFIG_OPENTHREAD_THREAD_STACK_SIZE=6240
# NET_CONFIG OFF: its SYS_INIT blocks boot waiting for an IPv6 address and
# deadlocks a Thread node (ns-zephyr lesson). The node binds its own sockets.
CONFIG_NET_CONFIG_SETTINGS=n
CONFIG_NET_MAX_CONTEXTS=6
CONFIG_NET_PKT_RX_COUNT=8
CONFIG_NET_PKT_TX_COUNT=8
CONFIG_NET_BUF_RX_COUNT=16
CONFIG_NET_BUF_TX_COUNT=16

# --- Thread dataset persistence + the flash-erase lesson -----------------------
# OT persists its active dataset to NVS. A blocking flash erase at boot must be
# time-sliced (PARTIAL_ERASE) so it does not stall the system for long stretches
# (ns-zephyr's hard-won line; kept even though the DK has no USB to starve).
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_NVS=y
CONFIG_SETTINGS=y
CONFIG_SETTINGS_NVS=y
CONFIG_SOC_FLASH_NRF_PARTIAL_ERASE=y
CONFIG_SOC_FLASH_NRF_PARTIAL_ERASE_MS=2
CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=2048

# --- Thread dataset SEED — PLACEHOLDER (OpenThread defaults) --------------------
# SECRET: the REAL porta dataset (incl. network key) lives ONLY in the gitignored
# prj_dk_thread_local.conf, layered via -DEXTRA_CONF_FILE, overriding these. The
# node builds without the overlay but will NOT join the porta mesh.
CONFIG_OPENTHREAD_NETWORKKEY="00:11:22:33:44:55:66:77:88:99:aa:bb:cc:dd:ee:ff"
CONFIG_OPENTHREAD_PANID=43981
CONFIG_OPENTHREAD_CHANNEL=11
CONFIG_OPENTHREAD_XPANID="de:ad:00:be:ef:00:ca:fe"
CONFIG_OPENTHREAD_NETWORK_NAME="ot_zephyr"
```

- [ ] **Step 4: Bake the node and build for the DK (do NOT flash yet)**

```bash
source /home/david/workspace/ns-zephyr/tools/env.sh
export OEVM_HEAP_SIZE=49152 METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256
export VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512
cd /home/david/workspace/nsl-tuvm
python3 tools/ns_compile_oevm.py \
  /home/david/workspace/tuvm/zephyr/ThreadEchoNodeDK.ns \
  /home/david/workspace/tuvm/src/solidified_image.c
cd /home/david/workspace/tuvm/zephyr
west build -b nrf52840dk/nrf52840 -p always . -- \
  -DCONF_FILE=prj_dk_thread.conf -DEXTRA_CONF_FILE=prj_dk_thread_local.conf \
  2>&1 | tee /tmp/rung7_dk_build.log | tail -25
```

Expected: `west build` completes and the linker's memory-region summary prints `FLASH` and `RAM` well under 100%. If it fails on a `zsock_*` / OpenThread symbol, fix `src/backend/zephyr/udp_zephyr.c` ONLY (the backend contract is fixed). If a CBOR/space symbol is missing, add that `.c` to `zephyr/CMakeLists.txt`.

- [ ] **Step 5: Confirm the RAM report fits (the config/RAM gate)**

```bash
cd /home/david/workspace/tuvm/zephyr
west build -t ram_report 2>&1 | tail -5
grep -E "Used (RAM|FLASH)|region .* is used|Memory region" /tmp/rung7_dk_build.log | tail -6
```

Expected: RAM usage under the 256 KB part (the linker summary line `RAM: NNNNN B / 262144 B (xx.xx%)`) with headroom. If RAM is **over 100% or has < ~8 KB headroom**, shrink the VM arena and rebuild: re-export `OEVM_HEAP_SIZE=40960` (then `32768` if still tight) and repeat Step 4. Record the final RAM/ROM figures for the Task-5 re-bank. (Rung-4 loopback baseline was 118 KB/45%; OT L2 + mbedTLS is a large increment — expect a substantially higher number, but MTD should fit on the 256 KB part with a trimmed arena.)

- [ ] **Step 6: Restore the clobbered image, then commit (config + node; NOT the local overlay)**

```bash
cd /home/david/workspace/tuvm
git checkout -- src/solidified_image.c src/solidified_image.stmap
git status --short   # expect: zephyr/ThreadEchoNodeDK.ns, zephyr/prj_dk_thread.conf, .gitignore
                     # (+ maybe udp_zephyr.c / CMakeLists.txt if a build fix was needed)
                     # prj_dk_thread_local.conf MUST NOT appear (it is gitignored)
git add zephyr/ThreadEchoNodeDK.ns zephyr/prj_dk_thread.conf .gitignore
git commit -m "feat(rung7): DK OpenThread MTD config + Thread echo node (builds + RAM-fits)

prj_dk_thread.conf = prj_dk.conf console/libc/GPIO + the OpenThread MTD L2 block
from ns-zephyr (uart0 console, so the USB-CDC coexistence config drops away);
placeholder dataset (real key stays in the gitignored prj_dk_thread_local.conf
via -DEXTRA_CONF_FILE). ThreadEchoNodeDK.ns: a BUTTON1-triggered NetBridge send
to the gw OMR echo. Builds for nrf52840dk and RAM-fits with the config-D arena.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Flash + mesh attach (bench — controller + David)

Flash the built image and prove the node **joins the mesh as an MTD child** — the big radio de-risk, before any round-trip. Gate = the node's RLOC16 appears in the OTBR `child table`.

**Files:** none (bench operation on the Task-2 build).

**Interfaces:** consumes the Task-2 `west build` artifact; produces mesh attachment (an entry in `ot-ctl child table` on `gw85224-01`).

- [ ] **Step 1: Re-bake + flash the DK (skip-rebuild so the flash uses the built image)**

```bash
source /home/david/workspace/ns-zephyr/tools/env.sh
export OEVM_HEAP_SIZE=49152 METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256   # or the trimmed value from Task 2 Step 5
export VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512
cd /home/david/workspace/nsl-tuvm
python3 tools/ns_compile_oevm.py \
  /home/david/workspace/tuvm/zephyr/ThreadEchoNodeDK.ns \
  /home/david/workspace/tuvm/src/solidified_image.c
cd /home/david/workspace/tuvm/zephyr
west build -b nrf52840dk/nrf52840 -p always . -- \
  -DCONF_FILE=prj_dk_thread.conf -DEXTRA_CONF_FILE=prj_dk_thread_local.conf
west flash --skip-rebuild --runner jlink
cd /home/david/workspace/tuvm && git checkout -- src/solidified_image.c src/solidified_image.stmap
```

Expected: `west flash` programs the DK. (J-Link note from the DK README: if `/dev/ttyACM0` is missing / connect times out, power-cycle the on-board-debug USB; healthy it enumerates as `1366:1025 SEGGER J-Link`.)

- [ ] **Step 2: Open the console and confirm clean boot (no MPU fault)**

```bash
timeout 15 sh -c 'cat /dev/ttyACM0' &   # or: minicom/screen /dev/ttyACM0 115200
```

Expected: the nRF Connect SDK boot banner followed by `THREAD NODE READY — press BUTTON1 after attach`. **If the node boots silent** (no banner), it faulted at boot — most likely the OT thread stack (confirm `CONFIG_OPENTHREAD_THREAD_STACK_SIZE=6240`) or a heap alloc returning NULL (bump `CONFIG_HEAP_MEM_POOL_SIZE`). This is the ns-zephyr silent-boot failure mode.

- [ ] **Step 3: Confirm the node attaches to the mesh (the join gate)**

Wait ~10–30 s after boot for the MTD to discover a parent and attach, then on the gateway:

```bash
ssh david@gw85224-01 'docker exec otbr ot-ctl child table; echo "---"; docker exec otbr ot-ctl childip'
```

Expected: **a row in `child table`** (a new RLOC16 / extended MAC = our node) and its mesh-local/OMR address under `childip`. That is the join gate — the tuvm MTD node is a live child of the porta mesh. **If no child appears** after ~1 min: (a) confirm the overlay actually applied — the built `zephyr/.config` should show the REAL `CONFIG_OPENTHREAD_CHANNEL=14` / `PANID=0x437`, not the placeholders (`grep OPENTHREAD_CHANNEL build/zephyr/.config`); (b) **RF link** — David observed trouble at ~4 ft: move the DK closer / re-orient, or fall back to a **dongle** (§Task-3 note). This is the top risk; attachment is where it surfaces.

- [ ] **Step 4: Record the attach result (no commit — bench evidence)**

Capture the `child table` row (RLOC16 + ext MAC) and the boot banner to the SDD ledger / a bench note. No repo change this task; it gates Task 4.

---

## Task 4: On-radio round-trip — the rung's proof (bench — controller + David)

With the node attached, trigger one CBOR `Record` round-trip over the radio and observe `BRIDGE OK`.

**Files:** none (bench operation).

**Interfaces:** consumes the attached node (Task 3) + the live `udp-echo6.service`; produces the `BRIDGE OK` bench gate.

- [ ] **Step 1: Watch both ends, then press BUTTON1**

```bash
# terminal 1 — the DK console
cat /dev/ttyACM0    # (or screen /dev/ttyACM0 115200)
# terminal 2 — the gw echo log
ssh david@gw85224-01 'journalctl -u udp-echo6.service -f -o cat'
```

With the node showing `THREAD NODE READY` and attached (Task 3), **press BUTTON1 (P0.11) on the DK once.**

Expected sequence:
1. gw `journalctl`: a fresh `echo NB <-> [fdc5:9109:9afe:97ea:...]:54360` (or an `fd77:...` OMR sender) line — the node's CBOR datagram arrived over 802.15.4 and was reflected.
2. DK console: `BRIDGE OK` — the reflection was received, CBOR-decoded value-equal to the sent `Record`, and the `whenever:` reaction fired.

- [ ] **Step 2: Interpret / troubleshoot**

- **`BRIDGE OK` printed** → the rung's proof is met: a full Layer-2 NetBridge round-trip (`Record → CBOR → 802.15.4 → echo → 802.15.4 → decode → whenever:`) over real OpenThread. Proceed to Task 5.
- **gw shows the echo line but the DK never prints `BRIDGE OK`** → the outbound leg works; the reflection was lost or not received. MTD downstream latency — the node polls its parent; confirm it is rx-on-when-idle (default for a non-sleepy MTD) and press again. If reproducible, the reflected datagram is being dropped inbound (buffer counts) — bump `CONFIG_NET_BUF_RX_COUNT`/`CONFIG_NET_PKT_RX_COUNT` and re-flash.
- **gw shows nothing on press** → the outbound datagram did not traverse the mesh: re-confirm attach (`child table`), the OMR target address, and RF proximity. Dongle fallback if the link is marginal.

- [ ] **Step 3: Capture the pass evidence (bench)**

Save the DK console `BRIDGE OK` line + the gw `echo` log line (with timestamps) to the SDD ledger / bench note as the manual-gate evidence. No repo change this task.

---

## Task 5: Close — footprint re-bank, docs, retire skipped tests, floor green

**Files:**
- Modify: `tuvm/zephyr/FOOTPRINT_DK.md` and/or `tuvm/tools/footprint-*` (the DK figure moved — OT L2 is a large increment; re-bank).
- Modify: `tuvm/zephyr/README.md` (Rung-7 Thread gate section).
- Modify: `tuvm/review/program-sequence.md` (Phase-3 line → Layer 3 done).
- Modify: dialect/vocabulary doc (grep to locate).
- Modify: the skipped-mesh-test placeholders (`43_udp_mesh` / `44_udp_thread`) — retire.

**Interfaces:** none new — closes the rung.

- [ ] **Step 1: Confirm the host + native_sim floor is still green (controller-owned)**

```bash
cd /home/david/workspace/nsl-tuvm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | tail -5
```

Expected: `180/0/12` (unchanged from Rung 6 — no source edit this rung), both paths. Then run the five named gates (grant / debugger-integration / portability-32 / boot-gc-asan / mcu-footprint) per the standard recipe (`.superpowers/sdd/progress.md` gate lines); all PASS. The **mcu-footprint** gate measures the *host/native_sim* engine, which did not change — it should be unmoved; the **DK** footprint (a separate figure) is what moved and is re-banked in Step 2.

- [ ] **Step 2: Re-measure + re-bank the DK footprint (no silent ratchet)**

```bash
cd /home/david/workspace/tuvm/zephyr
source /home/david/workspace/ns-zephyr/tools/env.sh
export OEVM_HEAP_SIZE=49152 METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256 VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512
# (use the same OEVM_HEAP_SIZE the round-trip actually ran with)
west build -t ram_report 2>&1 | tail -3 ; west build -t rom_report 2>&1 | tail -3
```

Update `tuvm/zephyr/FOOTPRINT_DK.md` with the new ROM/RAM (the OpenThread MTD figure) beside the Rung-4 loopback figure, noting the delta is OpenThread L2 + mbedTLS. This is a deliberate re-bank with the delta explained in the commit message — not a silent ratchet.

- [ ] **Step 3: Add the Rung-7 Thread gate section to `tuvm/zephyr/README.md`**

```markdown
# Rung 7 — real OpenThread round-trip gate (ThreadEchoNodeDK.ns)

Proves the Layer-2 NetBridge round-trips a CBOR `Record` over a **real
802.15.4 / OpenThread mesh**. The DK is an MTD child of the porta OTBR
(`gw85224-01`); a BUTTON1 press posts a `Record` whose outbound reaction
CBOR-encodes + sends it over the radio to the gw `udp-echo6` server (:7777),
which reflects it; the node receives + decodes value-equal and prints
`BRIDGE OK` on uart0/VCOM. Model A; MTD only. This is a **bench-verified manual
gate** (live silicon + live gateway + radio), not a `west build` CI gate.

## Build + flash + run

    source /home/david/workspace/ns-zephyr/tools/env.sh
    export OEVM_HEAP_SIZE=49152 METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256
    export VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512
    cd /home/david/workspace/nsl-tuvm
    python3 tools/ns_compile_oevm.py \
      /home/david/workspace/tuvm/zephyr/ThreadEchoNodeDK.ns \
      /home/david/workspace/tuvm/src/solidified_image.c
    cd /home/david/workspace/tuvm/zephyr
    west build -b nrf52840dk/nrf52840 -p always . -- \
      -DCONF_FILE=prj_dk_thread.conf -DEXTRA_CONF_FILE=prj_dk_thread_local.conf
    west flash --skip-rebuild --runner jlink
    cd /home/david/workspace/tuvm && git checkout -- src/solidified_image.c src/solidified_image.stmap
    # console /dev/ttyACM0 @115200: wait for attach (ot-ctl child table on the gw),
    # then press BUTTON1 -> expect: BRIDGE OK

The gitignored `prj_dk_thread_local.conf` (copied from
`ns-zephyr/app/prj_thread_local.conf`) supplies the real dataset; without it the
node builds but will not join.
```

- [ ] **Step 4: Update `tuvm/review/program-sequence.md` — Phase-3 net line**

Grep for the Rung-6 "Layer 2 done" line and update it:

```
Layer 3 done (NetBridge over real OpenThread; nRF52840-DK MTD child joins the
porta mesh, CBOR Record round-trips to a gw echo over 802.15.4 -> BRIDGE OK,
bench-verified). The three-layer UDP arc is complete.
```

- [ ] **Step 5: Dialect note + retire the skipped mesh tests**

```bash
grep -rln 'net' /home/david/workspace/nsl-tuvm/docs/NEWSPEAK_DIALECT.md 2>/dev/null
grep -rln '43_udp_mesh\|44_udp_thread' /home/david/workspace/nsl-tests 2>/dev/null
```

- If `NEWSPEAK_DIALECT.md` owns the `net`/`NetBridge` vocabulary, add a one-line "Layer 3: NetBridge runs over real OpenThread (MTD child), bench-verified" note. Otherwise the tests README net line. Do NOT invent a doc.
- Retire the skipped `43_udp_mesh` / `44_udp_thread` placeholders (the ones this rung's real gate supersedes): either delete them or convert the skip note to "superseded by the Rung-7 bench gate (ThreadEchoNodeDK)". Confirm the suite skip count drops accordingly and re-note the expected `180/0/N` at close.

- [ ] **Step 6: Update memory + commit the close**

Update the `milestone-status` memory (Rung-7 done: MTD child on porta mesh, BRIDGE OK bench-verified, DK footprint re-banked to <new>) and `rung7-gateway-thread-infra` if any address/port changed.

```bash
cd /home/david/workspace/tuvm
git add zephyr/README.md zephyr/FOOTPRINT_DK.md review/program-sequence.md
# add tools/footprint-* only if that baseline moved
git commit -m "docs(rung7): close — Layer 3 done (NetBridge over real OpenThread, MTD child)

BRIDGE OK bench-verified: nRF52840-DK MTD child joined the porta mesh and
round-tripped a CBOR Record over 802.15.4 to the gw udp-echo6 server. DK
footprint re-banked (OpenThread L2 + mbedTLS delta). Host/native_sim floor
(180/0/12 + 5 gates) unchanged. Three-layer UDP arc complete.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"

# nsl-tests, only if the skipped mesh tests were retired there:
cd /home/david/workspace/nsl-tests && git add -A && git commit -m "test(rung7): retire skipped 43_udp_mesh/44_udp_thread — superseded by the DK bench gate

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review

**Spec coverage** (each design § → a plan task):
- §2 echo server (done first) → deployed pre-plan; Task 1 exercises it as a real peer, Task 4 as the radio peer. ✓
- §3 node (MTD, Model A, `prj_dk_thread.conf` lineage, USB-CDC lessons drop, dataset overlay) → Task 2. ✓
- §3 MANUAL_START call → resolved: MTD auto-start (MANUAL_START unset), since the DK has no USB console to starve; the send is instead human-timed via BUTTON1 (Task 2 node + Task 4). ✓
- §4 addressing (gw OMR target) + single-flow round-trip + malformed-drop (code path unchanged) → Task 2 node (`gwAddr` = OMR) + Task 4. ✓
- §5 proof matrix + bench-gate nature → Tasks 3–4 (manual) + Task 5 Step 1 (floor green). ✓
- §6 staging 1–4 → echo (pre-plan/Task 1), attach (Task 3), round-trip (Task 4), close (Task 5). ✓
- §7 risks (RF→dongle, RAM fit, CBOR link, MTD latency, secret dataset) → Task 2 Step 5 (RAM), Task 2 Step 2 (secret), Task 3 Step 3 + Task 4 Step 2 (RF/latency), Task 2 Step 4 (CBOR link). ✓
- §8 follow-ons (Thingy:52, FTD, porta proto, framing, unified pump) → named in the spec; not this plan. ✓

**Placeholder scan:** no TBD/TODO. Every code/config step shows full content. The one deliberate placeholder is the *dataset* in the committed `prj_dk_thread.conf` (OpenThread defaults) — that is a security requirement (real key gitignored), not a plan gap, and is called out in Task 2 Steps 2–3. The dialect-doc target (Task 5 Step 5) is grep-resolved with a "do not invent a doc" guard. Hardware Tasks 3–4 are steps of observation, not code — exact commands + gate criteria + troubleshooting are given.

**Type consistency:** `NetBridge socket:out:peer:port:in:` + `start` + `pumpInbound` match the frozen Rung-6 API and the `NetBridgeNode.ns` call sites. `gpio pin:edge:post:on:` + `platform pumpEvents` + `space channel: 'sensor/alert'` match `PulseNodeDK.ns` exactly. `platform Record tag:with:with:` matches `NetBridgeNode.ns`. `EchoClientHost.ns` (Task 1, Tailscale addr, no pulse) and `ThreadEchoNodeDK.ns` (Task 2, OMR addr, BUTTON1) differ only in target address + trigger, as intended. ✓
