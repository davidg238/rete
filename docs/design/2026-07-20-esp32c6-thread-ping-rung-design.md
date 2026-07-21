# ESP32-C6 end-to-end Thread ping — Rung Design

**Date:** 2026-07-20
**Status:** approved (design), unimplemented
**Tracking issue:** **#59** (relates #57)
**Predecessor:** `docs/design/2026-07-20-esp32c6-second-target-design.md` (the C6 as a second
target; OpenThread/IPv6 there is *link*-proved only — never joined a network).
**Ports from:** `~/workspace/ns-zephyr/app/` (the proven nRF52840 Thread recipe) and
`zephyr/ThreadEchoNodeDK.nsl` (the proven CBOR round-trip to this same gateway, rung 7).

---

## Goal

The ESP32-C6 joins the **porta** OpenThread mesh (OTBR leader on `gw85224-01`) as an **MTD
child** and round-trips CBOR datagrams to the gateway's `udp-echo6` service, printing
**`UDP OK`** on the C6 console. Two independent witnesses:

1. **C6 console** (always-on UART bridge): `thread: attached role=child` then `UDP OK`.
2. **Gateway soak dashboard** (`http://gw85224-01:8085`): a new ML-EID row appears as a
   **gate** node with a non-zero ping count and 0% missed.

This is the first end-to-end boot-test of **cell (d)** from #57.

## Non-goals (YAGNI)

- No continuous soak (bounded burst only — that keeps it a *gate*, not a *soak* node).
- No GPIO/BUTTON trigger (the DK node's button is dropped — see §3).
- No shared-`main.c` `#if` compat layer, and — per the decision below — no full second `main`.
- No NetBridge/CBOR *changes*: the node **reuses** the existing rung-6/7 machinery unchanged.

---

## 1. Bring-up seam — a board-selected TU (not a second main)

> **Amended 2026-07-20 (during implementation, David's call): collapsed to ONE shared
> `src/thread_bringup.c`, not a board split.** The premise below — that the C6 cannot
> compile the nRF bring-up because `openthread_init`/`openthread_get_default_instance` are
> absent upstream — was **wrong**: those symbols are absent only from
> `<zephyr/net/openthread.h>` (what the design-time probe checked); they are present in the
> OpenThread *module* header `<openthread.h>` (Zephyr 4.2 refactored OT into
> `modules/openthread/`), which is globally on the include path. Both the NCS (nRF) and
> upstream (C6) toolchains expose the **same no-arg API**, so the two board TUs were
> byte-identical duplication. Shipped: one `src/thread_bringup.c` behind the
> `thread_bringup.h` seam, `CMakeLists.txt` compiling it under `if (CONFIG_NET_L2_OPENTHREAD)`
> (still gated so the WROOM compiles it away). The seam is retained; re-split only if the
> NCS and upstream OT APIs actually diverge in a future bump. The original reasoning below is
> kept as the historical "why".

**Decision (2026-07-20, supersedes #59's "separate `esp32c6_main`" line and the predecessor
design's §B "shared-`main.c` `#if`" lean).** The divergence between the nRF and C6 OpenThread
paths is isolated to **one function**, `thread_bringup()`. The entire rest of `zephyr/src/main.c`
(VM boot, Model-A/B selection, console setup, pulse-GPIO) is board-agnostic. So neither a
fragile `#if` inside one function nor a whole duplicated `main` is warranted — the right unit is
a **board-selected translation unit** for the bring-up, which also honours this repo's own rule
that *core must not reach around `backend/<target>`*.

**Why the C6 cannot share the nRF bring-up** — verified against the C6 toolchain
(`~/zephyr-esp32/zephyr/include/zephyr/net/openthread.h`):

| symbol used by today's `thread_bringup()` | upstream Zephyr 4.2 (C6) |
|---|---|
| `openthread_init()` | **absent** |
| `openthread_get_default_instance()` | **absent** |
| `openthread_run()` | present, `__deprecated` |
| `openthread_mutex_lock()` / `openthread_api_mutex_lock()` | present, `__deprecated` |
| `openthread_get_default_context()` | **present (non-deprecated)** |

The two `absent` symbols mean today's function simply will not compile for the C6; the modern
upstream path is context-based.

**Structure:**

- `zephyr/src/main.c` keeps its single `#ifdef CONFIG_NET_L2_OPENTHREAD` call site, but calls a
  new **`tuvm_thread_bringup()`** declared in a small header (e.g. `zephyr/src/thread_bringup.h`).
  No `#if` on OT API names anywhere in `main.c`.
- Two implementations, **CMake-selected by board**:
  - **nRF / NCS** — `zephyr/src/backend/ncs/thread_bringup_ncs.c`: today's code, moved verbatim
    (`openthread_init` / `run` / `get_default_instance` / mutex).
  - **C6 / upstream 4.2** — `zephyr/src/backend/zephyr/thread_bringup_c6.c`:
    `openthread_get_default_context()` → `net_if_up(iface)` → poll `otThreadGetDeviceRole(inst)`
    under the OT API lock until `CHILD`/`ROUTER` (≤30 s) → `printk("thread: attached role=%d\n")`.
    The `otInstance*` comes from the context (`ctx->instance`); the lock uses the available
    upstream API (deprecated-but-functional `openthread_api_mutex_lock(ctx)` is acceptable — a
    warning, not an error — with the exact non-deprecated call pinned during implementation).
- `zephyr/CMakeLists.txt` selects the TU, **gated on `CONFIG_NET_L2_OPENTHREAD` first** — this
  matters because the *same* `tuvm/zephyr` app builds three targets: nRF (NCS), C6 (upstream 4.2),
  and the **WROOM-32 (upstream 4.2, WiFi, no Thread)**. Compiling the NCS impl for the WROOM would
  fail on the absent `openthread_init`/`get_default_instance` symbols. So:
  ```cmake
  if (CONFIG_NET_L2_OPENTHREAD)          # WROOM (WiFi, no OT) compiles NEITHER
    if (CONFIG_SOC_SERIES_ESP32C6)       # upstream-4.2 context API
      target_sources(app PRIVATE src/backend/zephyr/thread_bringup_c6.c)
    else()                               # only nRF/NCS reaches here
      target_sources(app PRIVATE src/backend/ncs/thread_bringup_ncs.c)
    endif()
  endif()
  ```
  This mirrors `main.c`'s own `#ifdef CONFIG_NET_L2_OPENTHREAD` guard on the call site: WROOM
  drops both the call and the TU. (Exact C6 Kconfig symbol confirmed at implementation —
  `CONFIG_SOC_SERIES_ESP32C6`, `CONFIG_SOC_ESP32C6` the fallback.)

Both implementations share the shape **run → admin-up the net_if → poll role → print**, and
crucially **block until attach before returning**, so the VM (and therefore the node's `main:`)
starts only once the mesh is joined. This is what lets the node drop the DK's yield hack (§3).

The nRF DK/dongle builds are unaffected: they compile the same code they do today, only relocated
into a named TU and reached through the seam.

## 2. Config + the porta dataset

- **`zephyr/prj_esp32c6_thread.conf`** (exists, Task 6 — already carries `OPENTHREAD_MTD=y`,
  `MANUAL_START=y`, stack 6240, NVS/SETTINGS): add the **5 dataset-seed placeholders**
  (`CONFIG_OPENTHREAD_NETWORKKEY/PANID/CHANNEL/XPANID/NETWORK_NAME`) as syntactically-valid
  OpenThread defaults, so the tree builds green **without** any secret. `NET_CONFIG_SETTINGS=n`
  is already set in the parent `prj_esp32c6.conf` (the boot-deadlock trap is pre-handled).
  - The nRF USB-coexistence knobs are **deliberately not copied** — `USBD_CDC_ACM_WORKQUEUE`,
    `SOC_FLASH_NRF_PARTIAL_ERASE`, the deferred-enable dance. They exist to keep the nRF *USB-CDC*
    console alive at boot; the C6 console is the always-on UART bridge, so none apply (resolves
    #59 open-question 1).
- **`zephyr/prj_esp32c6_thread.local.conf`** — **GITIGNORED**, never committed. The real porta
  dataset (the same 5 fields), copied from `~/workspace/ns-zephyr/app/prj_thread_local.conf`
  (same mesh). Its values override the placeholders. Read the live dataset any time with
  `ssh gw85224-01 docker exec otbr ot-ctl dataset active`.
- **`zephyr/run.sh`** auto-layers the `.local.conf` sibling, mirroring the existing
  `prj_esp32_wifi.local.conf` pattern: when the active board is `esp32c6` and
  `prj_esp32c6_thread.local.conf` is present, append it to `EXTRA_CONF_FILE` (stacked after the
  `--conf-extra` thread conf, so its dataset wins). A checkout without the overlay still builds
  (placeholders) but will not join the porta mesh.
- **`.gitignore`**: add `zephyr/prj_esp32c6_thread.local.conf` (mirror the wifi.local entry).

## 3. The node — `zephyr/ThreadPingNode.nsl`

A headless C6 gate node. **Reuses** `NetBridge` + `Cbor` + `Record` (rung-6/7 machinery, no VM
or library change) and is an *adaptation of* `ThreadEchoNodeDK.nsl`, simpler in two ways:

- **No BUTTON1/GPIO, no pulse-yield hack.** The DK node pressed a button and abused
  `pumpEvents` (blocking on the pulse msgq) to yield the CPU for OT attach. On the C6,
  `tuvm_thread_bringup()` already blocked until `role=CHILD` before the VM started, so `main:`
  runs on an attached mesh and sends **autonomously**.
- **Autonomous bounded burst.** After `net udpBindPort:` and wiring a `NetBridge` to the gw
  ML-EID `[fdc5:9109:9afe:97ea:9541:d963:f8d9:8567]:7777`, send **N** (default 5)
  `Record tag: #reading with: #seq -> i with: #milliC -> …` — the exact 32 B CBOR `#seq` shape
  the `:8085` daemon parses. For each, poll `pumpInbound`/`drain` for the reflected datagram and
  verify it decodes **value-equal**. Print **`UDP OK`** after all N pass; **`FAIL: <why>`** on any
  mismatch or per-send timeout. A bounded burst (not continuous) classifies as a **gate** row on
  the dashboard.

Model B on the C6 (the C6 default); the `.nsl` is model-agnostic.

**Receive-path risk (noted, not pre-solved):** the DK node busy-polls `bridge pumpInbound.
space drain` in a tight loop, relying on the OT/net RX thread to preempt and deliver. The C6's
scheduler is preemptive too, so this should hold; **if RX starves**, add a small timer-based
yield using the rung-8 `Timers` seam (a `k_timer` tick through the pulse mailbox) between polls.
Per-send timeout is a bounded poll count so a lost datagram fails loudly rather than hanging.

## 4. Heap

Keep the **128 KB** C6 default (`run.sh`). The node is tiny, but OpenThread reserves net-buf
pools + a 6240 B thread stack; 128 KB leaves ample room in the ~427 KB `sram0_0_seg` region and
means this rung does not have to re-pick the heap. No change; #57's default stands.

## 5. Bench gate

Flashed to the C6, with the mesh up (confirmed: 3 nRF nodes soaking OK on `:8085`) and the
dataset overlay present:

```
bash zephyr/run.sh --node ThreadPingNode.nsl --conf-extra prj_esp32c6_thread.conf --flash --capture 60
```

**Pass =** console shows `thread: attached role=child` then `UDP OK`, **and** a new ML-EID row
appears on `http://gw85224-01:8085` with pings ≥ N and 0% missed. This is a live-silicon +
live-gateway + radio gate, not a `west build` CI gate (same class as rungs 7–8).

## Deliverables (implementation checklist)

1. `zephyr/src/thread_bringup.h` + move nRF impl to `backend/ncs/thread_bringup_ncs.c`; new
   `backend/zephyr/thread_bringup_c6.c` (upstream-4.2 API). `main.c` calls the seam.
2. `zephyr/CMakeLists.txt` — board-select the bring-up TU.
3. `zephyr/prj_esp32c6_thread.conf` — add 5 dataset placeholders.
4. `zephyr/prj_esp32c6_thread.local.conf` — gitignored real dataset (bench-side, not committed);
   `.gitignore` entry.
5. `zephyr/run.sh` — auto-layer the C6 thread `.local.conf`.
6. `zephyr/ThreadPingNode.nsl` — the autonomous CBOR gate node.
7. Bench-run; capture the green log to `zephyr/esp32-debug-artifacts/`; screenshot / note the
   dashboard row.

## Close-out (promotion, per CLAUDE.md)

On green, promote the durable knowledge to `~/workspace/tuvm/review/program-sequence.md`
(Phase-2 / C6 rung) and note the bring-up seam + the "attach-before-VM removes the yield hack"
lesson. The dated design stays as the historical "why". No `CLAIMS.md` row (manual bench gate,
same as rungs 7–8) unless the round-trip is later automated.

## Open items explicitly deferred

- Exact non-deprecated OT-lock call on upstream 4.2 (pinned at implementation; deprecated form is
  an acceptable warning meanwhile).
- Continuous-soak variant (put the C6 in the soak fleet) — a follow-up if wanted; this rung is the
  one-shot gate.
- `run.sh --node` still resolves relative to `zephyr/` only (the out-of-tree papercut) — not this
  rung's concern; `ThreadPingNode.nsl` is in-tree.
