# Rung 7 — Layer 3: NetBridge over real OpenThread (nRF52840-DK, MTD child) — design

**Status:** design frozen 2026-07-11. Brainstormed with David 2026-07-11 (gateway
recon done live over Tailscale same day).
**Phase:** program-sequence Phase 3/3 — the final layer of the three-layer UDP arc named in
the Rung 5 (L1) design. L1 proved the socket (host + native_sim); L2 made the node
network-transparent (CBOR channel↔UDP bridge, host + native_sim); **this rung runs that same
bridge over a real IEEE 802.15.4 / OpenThread radio to a live peer.**
**Proof bar (David, 2026-07-11):** a **full L2 NetBridge round-trip over real Thread** — a
`Record` posted on the node encodes to CBOR, travels out over the radio to the gateway, is
reflected, decodes back into a value-equal `Record`, and fires the node's `whenever:` reaction,
printing `BRIDGE OK` on the DK console.

## 0. Where this sits and what is already proven (READ FIRST)

The three-layer arc, from the L1/L2 designs:
- **Layer 1 (Rung 5, DONE)** — the `net` capability: confined UDP bind/send/recv/close;
  host both-path + native_sim.
- **Layer 2 (Rung 6, DONE)** — the reactive channel↔UDP bridge (`NetBridge`), CBOR on the wire,
  immutable `Bytes` payload; bidirectional host + native_sim (`45_udp_bridge`, `BRIDGE OK`).
- **Layer 3 (THIS RUNG)** — the same `NetBridge`, unchanged, over a **real OpenThread mesh**.

**What is already proven, so this rung is radio/mesh bring-up — not a socket, bridge, or CBOR
rewrite:**
1. `udp_zephyr.c` (the `zsock_*` backend) functionally round-trips on native_sim (Rung 5) and
   links + boots on the DK over in-kernel loopback UDP (Rung 4 Task 8).
2. `NetBridge` (nsl, `lib/kernel.ns`) does the full CBOR `Record` round-trip on host and
   native_sim (Rung 6). **No VM / facade / bridge / CBOR change is in scope this rung.**
3. The OpenThread *join* recipe is hard-won in `ns-zephyr/app/prj_thread.conf` (MTD child,
   dataset overlay, OT thread-stack sizing, NVS partial-erase, USB-CDC coexistence) — Model B on
   an nRF52840 dongle. It **builds, RAM-fits (~22 KB headroom at the time, FTD), and boots**; the
   one thing that recipe *never* demonstrated is the **actual UDP round-trip over the 802.15.4
   mesh** (`progress-r1-archive.md`: "STILL PENDING: Thread mesh round-trip, S3 proper"). **That
   gap is exactly what this rung closes**, now on the tuvm engine.

So the genuinely-new deliverable is narrow: **stand the tuvm NetBridge node up as an MTD child on
the live mesh and complete one CBOR round-trip against a real peer over the radio.**

## 1. Topology (all facts verified live on `gw85224-01` via `ot-ctl`, 2026-07-11)

```
  tuvm DK  ───802.15.4 (ch 14, MTD child)──►  OTBR leader  (RCP + docker "otbr", wpan0 on host)
  NetBridge                                     │  host is DIRECTLY on the mesh via wpan0:
  MTD, Model A   ◄──── CBOR echoed back ────────┘    OMR    fd77:9957:d9f3:1:2935:a821:1d56:c329
                                                     ML-EID fdc5:9109:9afe:97ea:9541:d963:f8d9:8567
                                                     porta :6969 (untouched) · echo :7777
```

- **Router/leader = the existing OTBR** (`ot-ctl state` → `leader`; RLOC16 `0x2000`; **0 children**
  today). We add **no FTD and no bench radio for routing.** The tuvm node stays **MTD** — David's
  firm constraint ("must use MTD, not FTD, respect the lessons learned": FTD's MPSL/radio/RAM weight
  faulted at boot on the dongle, and the archived FTD image sat at 91 % RAM). An MTD is a *child*
  and cannot form/lead a mesh; the OTBR is its parent router. This is exactly the shape the
  `ns-zephyr` MTD design assumed ("a leaf attaches as a child and sends datagrams through its
  parent, the gw").
- **The gateway host is on the Thread mesh directly** (`ip -6 addr`: `wpan0` carries the OMR addr
  `fd77:9957:d9f3:1:2935:a821:1d56:c329` and mesh-local addrs under `fdc5:9109:9afe:97ea::/64`;
  kernel routes for both prefixes via `wpan0`). Therefore **a plain UDP socket on the host is
  reachable from a Thread child** — no container plumbing, no NAT hop.
- **The dataset is already in hand.** `ot-ctl dataset active` on the live leader matches the
  gitignored `ns-zephyr/app/prj_thread_local.conf` **exactly**: Network Key `0d67fa154acd86b88e8e501e6c9048c0`,
  PAN ID `0x0437`, Channel 14, Ext PAN ID `9c858fa2d419e109`, Name `OpenThread-0437`, Mesh-Local
  Prefix `fdc5:9109:9afe:97ea::/64`. The node reuses that overlay verbatim.
- **porta occupies UDP `6969`** (`porta serve --help`: "UDP/TFTP port (default 6969)"; HTTP operator
  `6970`). The echo server picks a free port (`7777`) and leaves porta untouched.

## 2. The echo server (Layer-3 peer/oracle) — DONE FIRST (David, 2026-07-11)

A committed `tuvm/zephyr/tools/udp_echo6.py` (~20 lines) binds a dual-stack UDP socket on `:::7777`
and reflects every datagram's bytes **verbatim** to its sender. It **never parses the payload**, so
it physically cannot corrupt CBOR: value-equality of the *decoded* `Record` on the node then proves
the whole `encode → 802.15.4 → 802.15.4 → decode` path end to end, with a peer that is a trusted,
minimal oracle (the house pattern — keep a behavioral oracle for each new subsystem).

Deployed as an always-available systemd unit `udp-echo6.service` on `gw85224-01`
(`WantedBy=multi-user.target`, `Restart=always`, `DynamicUser=yes` + light hardening), installed to
`/usr/local/bin/udp_echo6.py` (script + unit staged 2026-07-11; enabled via David's `sudo` — the gw
has no passwordless sudo). Stood up **before** the node work (David's sequencing) so the peer is
live and stable across gateway reboots for this and every future net rung. Non-porta by design: this
rung proves *CBOR-over-Thread round-trips*, decoupled from *porta protocol works*.

## 3. The node

- **tuvm NetBridge, MTD child, Model A** (the tuvm/zephyr build is Model-A-only — the solidified
  image; `oimg_loader.c`/`kernel_boot.c` clash with the baked statics). Baked from a
  `NetBridgeNode`-style `.ns` that **reuses the Rung-6 bridge machinery unchanged** (`NetBridge`,
  `Cbor`, `Bytes`, `Socket`, `whenever:`). The node binds a socket, has its `NetBridge` send a
  `Record` to `[<gateway OMR>]:7777`, `pumpInbound`, decodes the echoed value, and its `whenever:`
  reaction prints `BRIDGE OK` + the round-tripped value on uart0/VCOM.
- **New config `tuvm/zephyr/prj_dk_thread.conf`** = today's `prj_dk.conf` (uart0/VCOM console,
  256 KB-trimmed heap/stack) **+ the OpenThread block lifted from `ns-zephyr/app/prj_thread.conf`**:
  `CONFIG_NET_L2_OPENTHREAD`, `CONFIG_OPENTHREAD_MTD=y`/`FTD=n`, `CONFIG_OPENTHREAD_THREAD_STACK_SIZE=6240`
  (the ns-zephyr lesson — OT init runs PSA/AES-CCM link crypto on this stack; too small → MPU fault
  at boot), `CONFIG_NET_CONFIG_SETTINGS=n` (net_config's blocking boot init deadlocks a manual-start
  Thread node), NVS + `CONFIG_SOC_FLASH_NRF_PARTIAL_ERASE` (time-sliced erase), and the
  `OPENTHREAD_MANUAL_START` pairing. Dataset via the same **gitignored `-DEXTRA_CONF_FILE` overlay**
  convention (`prj_dk_thread_local.conf`, never committed).
- **Key simplification vs ns-zephyr:** the DK console is **uart0 / on-board SEGGER VCOM
  (`/dev/ttyACM0`), not USB-CDC.** The hardest-won ns-zephyr lessons — USB-CDC enumeration starved
  by a blocking flash erase or by OpenThread's boot-time work — are about keeping the *USB console*
  alive. On the DK there is no USB console to starve, so that whole class of coexistence bugs (and
  the `USBD_CDC_ACM_WORKQUEUE` / deferred-enable dance) **drops away**. We keep the substantive
  lessons that are console-independent: OT thread-stack size, NVS partial-erase, no `NET_CONFIG`,
  MANUAL_START. (Whether MANUAL_START is still *needed* absent the USB constraint is a Task-time
  empirical call — default to keeping it for clean bring-up ordering; drop it only if boot is clean
  and simpler without.)
- **The `net` capability boot-threading already exists** for Path A (`ns_compile_oevm.py` threads
  `net` when the entry declares it) — reused unchanged; the node grants only `net`, so the
  serial-XOR-net single-2nd-cap-slot limit is not a blocker.

## 4. Addressing & the round-trip

- Node targets the gateway's **OMR address** `fd77:9957:d9f3:1:2935:a821:1d56:c329` on port `7777`.
  OMR (not the ML-EID) because it is stable and advertised in network data (`netdata show` →
  `fd77:9957:d9f3:1::/64 paos`); the ML-EID is the documented alternative if OMR misbehaves. The
  address is a node-side constant (baked into the `.ns`) this rung — no discovery/DNS (a named
  future, out of scope).
- **Round-trip is a single flow:** node → `[OMR]:7777` → echo → node; `pumpInbound` receives the
  reflection, `Cbor decode:` totals it, the decoded `Record` posts to the in-channel, `drain` fires
  the `whenever:` reaction → `BRIDGE OK`. Genuinely bidirectional (TX + RX over the radio) in one
  observable flow — cleaner than two stitched one-way legs.
- **Malformed-input policy** (from L2) still holds: a garbled datagram is dropped, never crashes the
  node. Not exercised as a gate this rung (the echo is trusted), but the code path is unchanged.

## 5. Proof matrix & the nature of the gate

| Target | Image | Backend | What is proven |
|---|---|---|---|
| host + native_sim | (Rung 6) | posix / NSOS | **unchanged, stays green** — no source edit to the bridge/CBOR/VM this rung |
| nRF52840-DK | A | `udp_zephyr.c` over `NET_L2_OPENTHREAD` | **NEW: one CBOR `Record` round-trips over the real 802.15.4 mesh to the gw echo → `BRIDGE OK`** |

Because the proof requires **live silicon + a live gateway echo server + a radio link**, it is a
**bench-verified manual gate** (like Rung-4 Task-8's physical button press), **not** a `west build`
CI gate. The Rung-6 host/native_sim suite (`180/0/12` + 5 named gates) is the regression floor and
must stay green untouched. This rung **retires the long-skipped `43_udp_mesh`/`44_udp_thread`** as
its future-gate placeholders.

## 6. Incremental staging (David: "get some nodes echo'ing first")

1. **Echo server up** as systemd on `gw85224-01`; verify it reflects (loopback datagram + journald).
   *(In progress — stood up first per David's sequencing.)*
2. **Node joins the mesh** — `prj_dk_thread.conf` + dataset overlay; the node **builds, RAM-fits,
   boots, and appears in the OTBR `child table`.** No app round-trip yet. This is the big de-risk
   (dataset, OT config, RAM fit, RF link). Gate: `ot-ctl child table` shows the node's RLOC16.
3. **Round-trip** — the baked NetBridge node sends one CBOR `Record`; the echo reflects it; the node
   decodes value-equal and prints `BRIDGE OK` on `/dev/ttyACM0`.
4. **Close** — bench evidence captured; NEWSPEAK_DIALECT note (Layer 3 done); DK README + tuvm
   zephyr README updated; **footprint re-measure** (OT L2 + mbedTLS is a large RAM/ROM move — bank
   the new DK figure, no silent ratchet); program-sequence Phase-3 line → "Layer 3 done (NetBridge
   over real OpenThread, MTD child)"; retire the skipped mesh tests; host/native_sim suite + 5 gates
   confirmed still green.

## 7. Risks / watch-items

- **RF link is the top risk, and it is hardware.** David observed comms trouble DK↔gw at ~4 ft
  (DK PCB antenna; 2.4 GHz coexistence with wifi; channel 14). **Fallback: an nRF52840 dongle**
  (the Model-B / USB-CDC path `ns-zephyr` already builds + boots — but note that path *reintroduces*
  the USB-CDC coexistence lessons §3 drops for the DK). If the DK attaches but round-trips are lossy,
  a dongle or shorter range / better antenna is the mitigation — not a code change. Staging step 2
  (attach) flushes this out before any app logic.
- **RAM fit on 256 KB.** Rung-4's DK baseline was 118 KB RAM / 45 % with only in-kernel *loopback*
  net. Adding `NET_L2_OPENTHREAD` + OT + mbedTLS link crypto + NVS is a large increment (ns-zephyr's
  *FTD* image hit 91 %). **MTD is materially lighter than FTD, but this is a real fit risk** —
  measure at staging step 2, and trim VM tunables (`OEVM_HEAP_SIZE`, method/prim cache sizes, float
  slab — all env-driven per the CMakeLists) if it is tight.
- **CBOR prims must link on the Thread build** — same watch-item as Rung-6 native_sim; `platform.c`
  compiles whole, so they should, but confirm (add to build-list sites if fenced out).
- **MTD receive latency / poll period.** An MTD polls its parent for downstream traffic; ensure the
  node is rx-on-when-idle (or its poll period is short enough) that the echo reflection arrives
  within the node's `pumpInbound` window. Sleepy-end-device power optimization is a named future,
  not this rung.
- **Dataset is a secret.** The real network key lives only in the gitignored
  `prj_dk_thread_local.conf`; the committed `prj_dk_thread.conf` carries syntactically-valid
  placeholders (the ns-zephyr convention). Without the overlay the node builds but will not join.

## 8. Follow-on (named, not designed here)

- **Thingy:52 sensor node** — David has an nRF52840 Thingy:52 with sensors; a later rung integrates
  a real sensor capability behind the same confined-capability pattern and publishes readings over
  this Thread transport (the demo-node north star). Not this rung.
- **FTD on the DK** — David: "with the much improved RAM requirement, maybe we can get an FTD node
  running" — a possible future once MTD is proven and RAM headroom is measured; lets a tuvm node
  route / lead rather than only attach as a child.
- **porta protocol over Thread** — this rung's node speaks to a bare echo, not porta; wiring the
  node to real `porta serve` (`:6969`, TFTP install + telemetry) is the keeper/porta track, separate
  from the transport proof.
- **Stream-transport framing** (serial/tcp) — the Rung-6 §7 finding; its own rung when a workload
  needs a stream transport.
- **Unified `pumpEvents` ingress + idle-wait/power** — fold net + pulse + timers into one pump with
  a Zephyr `k_poll` wait (the deep-sleep payoff); the rule-of-three extraction, when a second data
  source lands.
