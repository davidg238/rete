# Rung 5 — UDP transport (Layer 1: net capability) — native_sim proof + Bytes payload — design

**Status:** design frozen 2026-07-10; **reconciled to as-built same day** after verifying the
host net capability already ships and passes. Brainstormed with David 2026-07-10.
**Phase:** program-sequence Phase 2/3 seam — the transport the CBOR wire seam (rung 3) and
the reactive channel model (Milestone C) were built to feed. **This is Layer 1 of a
three-layer arc** (below); it is the smallest, de-risking bite.

## 0. As-built reconciliation (READ FIRST — the premise the plan is built on)

Investigation on 2026-07-10 found the host `net` capability is **already complete and
green**, contradicting an initial "inert" reading (which came from a stale `dispatch.c`
comment). Verified by running the both-path single-file recipe:

- `nsl-tests/04_somns_basic/42_udp.ns` (live round-trip, v4+v6 loopback) → **PASS Path A + PASS Path B**.
- `nsl-tests/04_somns_basic/43_udp_confined.ns` (runtime-DNU confinement pin) → in-suite, green.
- Full suite is **179 passed / 0 failed / 12 skipped** with all five named gates PASS
  (SDD ledger progress.md:219). `src/udp.c` is in the conformance build list (line 85) and
  the mcu footprint gate's source list.

**Therefore the host half of Layer 1 needs no build work — only a payload refactor.** What
is genuinely unbuilt/unproven, and what this rung delivers:

1. **native_sim functional round-trip** — the Zephyr `udp_zephyr.c` backend *links* on
   native_sim (the rung-2 pulse gate enables NSOS networking so it must) but has **never
   functionally round-tripped a datagram**. This is the rung's core deliverable.
2. **Payload String → immutable Bytes** — the socket path currently carries String payloads
   (`42_udp.ns` compares `dg data = 'ping'`). Layer 2 will carry CBOR (not valid UTF-8), so
   the payload must be raw Bytes. This is now a deliberate refactor of working, tested code
   plus a wrap/unwrap bridge between the nsl `Bytes` value class and the raw-bytes leaf
   (§4.1). (David, 2026-07-10: "bytes".)
3. **Close** — build-list audit (a class of omission that has bitten this project),
   footprint re-bank, `net` vocabulary/docs, program-sequence update.

## 1. North star and the three-layer arc

`review/demo-node.ns` is network-transparent: the node posts readings to a `node7/reading`
channel and *"porta bridges 'node7/reading' onto the LAN … CBOR wire."* The node never
touches a socket. The transport splits into three independently-shippable layers; this rung
is Layer 1:

- **Layer 1 (THIS RUNG) — the `net` capability.** Confined, no-fiber-clean UDP: bind / send /
  non-blocking receive / close, carrying **opaque Bytes payloads**. Host POSIX both paths is
  *already done*; this rung finishes it by proving the Zephyr backend on native_sim and
  moving the payload to Bytes.
- **Layer 2 (next rung) — the reactive channel↔UDP bridge.** Inbound datagram → (during
  `pumpEvents`) CBOR-decode → post to a channel → `Space>>drain` fires; outbound channel post
  → CBOR-encode → `udp_send`. Where CBOR and the reactor eventing live. Makes the node
  network-transparent.
- **Layer 3 (later rung) — real OpenThread on silicon.** Bring `udp_zephyr.c` up on the
  nRF52840-DK over an OpenThread mesh. Layer 1 validates the same backend on native_sim, so
  Layer 3 is radio/mesh bring-up, not a socket rewrite. (The skipped
  `44_udp_thread.ns`/`43_udp_mesh.ns` are its future gates.)

Decomposition rationale (David, 2026-07-10): "1 then 2" is a good bite size; each layer is an
independent spec→plan→implement cycle; Layer 1 de-risks Layer 2 de-risks Layer 3.
Host-proof-first matches the A–E pattern.

## 2. Scope (locked)

**In scope**
- **native_sim functional round-trip** of the `net` capability (Path A / Model A only — the
  Zephyr build excludes `oimg_loader.c`), over Zephyr NSOS loopback: proves `udp_zephyr.c`
  send/recv actually work, not just link.
- **Payload String → immutable Bytes** across `udp.c` (receive data slot), the
  `Socket`/`Datagram` facade, and the affected tests, with the `Bytes`↔raw-leaf bridge (§4.1).
- Keep the host both-path round-trip + confinement green through the refactor.
- Build-list audit for `src/udp.c` + the udp backend; footprint re-bank; `net` dialect/docs
  note; program-sequence update.

**Out of scope (deferred, reversible)** — *keep Layer 1 lean* (David, 2026-07-10):
- The channel↔UDP bridge, `pumpEvents` inbound hook, `whenever:do:` eventing → **Layer 2**.
- CBOR at the wire → **Layer 2** (Layer 1 carries opaque Bytes; no `Cbor` dependency).
- Real OpenThread / DK silicon → **Layer 3**.
- Blocking/timeout receive, multicast, TCP, DNS → later, if a workload forces it.
- Moving net tests into a dedicated `16_net/` dir (cosmetic — they work where they are).
- Lifting the serial-XOR-net single-2nd-cap-slot limit (§4.4).

## 3. Current state (source of truth = the code; line numbers approximate, 2026-07-10)

The full capability spine exists and is exercised:

- **`tuvm/src/udp.c` / `udp.h`** — `udp_object()` static-BSS singleton + `udp_dispatch()`
  with its own string-compare selector matching: `basicBind:` (port SMI → fd SMI/−1),
  `basicSend:to:port:data:` (fd, hostStr, portSMI, **dataStr**), `basicReceive:` (fd → 3-slot
  raw indexable `{data, host, portSMI}` or nil), `basicClose:`. Reached by a receiver-identity
  gate in `dispatch.c` — no PRIM_NAMES entry. Zero fiber/NLR coupling. `basicReceive:` builds
  its 3 slots via `indexable_new(3)` and pins intermediates with
  `gc_temp_root_push`/`gc_temp_roots_clear` (already post-conservative-scan-correct). The data
  slot is built with `udp_make_string(...)` (StringVtable) — **this is what moves to Bytes**.
- **`tuvm/src/udp_backend.h`** — 6-function link-time seam; string addresses; backend owns
  family; non-blocking receive (len ≥0 or −1).
- **`tuvm/src/backend/posix/udp_posix.c`** — real dual-stack AF_INET6, `IPV6_V6ONLY=0`,
  O_NONBLOCK, getaddrinfo/recvfrom, v4-mapped normalization.
- **`tuvm/src/backend/zephyr/udp_zephyr.c`** — `zsock_*` backend; **links on native_sim** (the
  rung-2 pulse `tuvm/zephyr/prj.conf` enables the NSOS networking block precisely because
  `dispatch.c`'s `send()` fast path unconditionally probes `udp_object()`). Carries a caveat
  that its `zsock_*` symbols are unverified against a running SDK — **the functional round-trip
  is the gate never run.**
- **`nsl-tuvm/lib/kernel.ns:968-1003`** — `class Net`/`class Socket`/`class Datagram` +
  non-public `Kernel>>netOn:` (confined; mirrors `serialOn:`). `Socket>>receive` reads
  `t basicAt: 1/2/3` (raw indexable, not a wrapped Array).
- **Both-path grant, already wired and green** — Path A: `ns_compile_oevm.py:2389-2396`
  threads `net` when the entry declares it. Path B: `oimg_serialize.py` emits `net_on_sel_ref`
  at header `A+12W` (v8); `tuvm/src/oimg_loader.c:216-227` reads it and mints the `Net` wrapper
  into the single 2nd-cap slot (serial XOR net).
- **Byte container** — the leaf is `RawBytesVtable`, **same `struct String` layout** (length +
  bytes), distinct vtable so it never gains String methods; GC marks it
  (`builtin_registry[18]`). Allocator prim `platform basicNewBytes: n`; read via the generic
  `at:`/`size` prims (gated on `RawBytesVtable`, 1-based). The nsl value class `Bytes`
  (kernel.ns ~470-499) wraps a `raw` RawBytes leaf and adds value `=`, `printHex`, `hash`,
  `overRaw:` (class-side); mutable sibling `ByteArray` has `fromList:`/`fromHex:`/`new:`/
  `freeze`. `Cbor>>encode:` already returns `(ByteArray fromList: acc) freeze` → immutable
  `Bytes`.

The Zephyr build is **Model A (solidified image) only** — `tuvm/zephyr/` excludes
`oimg_loader.c`/`kernel_boot.c` (static `Vtable_*` clash with the baked `solidified_image.c`).
So the native_sim proof is Path A; a net-granting node is baked via `ns_compile_oevm.py`
(whose `net` boot-threading already exists, §above).

## 4. Design decisions

### 4.1 Payload = immutable Bytes, via a `Bytes`↔raw-leaf bridge
A datagram is opaque bytes and Layer 2 carries CBOR (not UTF-8), so a String payload is
semantically wrong. The refactor:

- **C (`udp.c`):** `basicReceive:` builds the **data** slot (slot 1) with a new
  `udp_make_bytes(...)` that allocates `RawBytesVtable` instead of `StringVtable` (identical
  struct — a one-vtable change; keep `udp_make_string` for the **host** slot 2, which stays a
  textual address). `basicSend:to:port:data:` reads `args[3]` as `struct String *` today; a
  RawBytes leaf has the same layout, so **the send C path is unchanged** — it just needs the
  facade to hand it a raw leaf (below).
- **Facade (`kernel.ns`):** `Socket>>receive` wraps the raw data leaf into an immutable nsl
  `Bytes` value: `Datagram data: (Bytes overRaw: (t basicAt: 1)) senderHost: (t basicAt: 2) …`
  so `Datagram>>data` answers a first-class `Bytes` (has `=`, `printHex`).
  `Socket>>sendTo:port:data: aBytes` must hand the prim the **raw leaf**, not the `Bytes`
  wrapper — pass `aBytes asRawLeaf` (add a read accessor on `Bytes`/`ByteArray` that returns
  its `raw` ivar; name TBD-in-plan, e.g. `rawLeaf`). The param renames `aString`→`aBytes`.
- **Tests:** `42_udp.ns` builds its payload as `Bytes` (e.g. `ByteArray fromHex: '70696e67'`
  = 'ping', or `fromList:`) and asserts `dg data = payload` (value `=` on `Bytes`).

This keeps Layer 1 free of any `Cbor` dependency while making the seam carry exactly what
Layer 2 needs.

### 4.2 Opaque, poll-based, standalone — keep Layer 1 lean
No CBOR, no reactor integration, no `pumpEvents` inbound hook (David: "keep lean").
`Socket>>receive` stays a caller-driven non-blocking poll returning a `Datagram` or nil.

### 4.3 Address model — unchanged
Host is a String (IPv4-dotted or IPv6); port an SMI; backend owns family (IPv6-first
dual-stack); v4-mapped senders normalize to dotted-quad.

### 4.4 serial XOR net single-slot limit — persists (documented, not fixed)
At most one 2nd-cap wrapper per entry. The net node grants only `net`, so not a blocker.

## 5. Proof matrix

| Target | Image path | Backend | Status after this rung |
|---|---|---|---|
| host (POSIX) | A + B | `udp_posix.c` | already green; **kept green** through the Bytes refactor (42_udp updated) |
| native_sim (Zephyr) | A only | `udp_zephyr.c` | **new: functional round-trip over NSOS loopback** |

The host suite must stay both-path green (179→179; the Bytes refactor changes assertions, not
counts) and the five named gates (grant, debugger-integration, portability-32, boot-gc-asan,
mcu-footprint) must hold.

## 6. Task sketch (~5, subagent-driven; controller owns every full-suite run)

1. **Payload → Bytes (C + facade + host tests).** `udp.c` `udp_make_bytes` for the data slot
   + GC-root re-verify; `Bytes`/`ByteArray` `rawLeaf` accessor; `Socket`/`Datagram` facade to
   Bytes; update `42_udp.ns` to build/compare `Bytes`; `test_udp.c` (backend-only C) still
   green. Both-path single-file green.
2. **Host regression + build-list audit.** Full both-path suite green (179/0/12); grep every
   `platform.c`-bearing build list (`footprint.sh`, `bench.sh`, `tests/test_load_run.sh`,
   `tests/test_oimg_blob.sh`, `-m32` `tests/test_portability_32.sh`, grant/debugger gate
   runners) to confirm `src/udp.c` + the udp backend are present and compile with the Bytes
   change; five named gates PASS.
3. **native_sim net node.** Write `tuvm/zephyr/NetNode.ns` (a Model-A node declaring `net:`
   that binds a socket, sends to loopback, polls receive, prints an `OK` sentinel); bake it
   into `solidified_image.c` via `ns_compile_oevm.py`; add it beside the pulse node (its own
   `CONF_FILE`/main variant as needed).
4. **native_sim functional round-trip.** `west build -b native_sim -p always tuvm/zephyr`;
   run under `timeout`; assert the `OK` line (datagram actually round-tripped over NSOS).
   Restore the clobbered `solidified_image.c`/`.stmap` before any commit. If NSOS offloaded
   loopback misbehaves, fall back to a two-socket same-process exchange on an ephemeral port
   (still a genuine backend round-trip) — §7.
5. **Close.** Footprint re-bank (expected ≈0 engine delta — `udp.c` already in the set, Bytes
   is near-identical code; re-bank if it moves, no silent ratchet); NEWSPEAK_DIALECT `net`
   note + a short `net` vocabulary paragraph; reconcile CLAIMS/tests README; program-sequence
   Phase-2/3 net line → "Layer 1 done (host + native_sim); Layers 2/3 named."

## 7. Risks / watch-items

- **native_sim NSOS loopback semantics.** NSOS offloads to the host's real sockets, so
  `::1`/`127.0.0.1` loopback *should* behave like posix. If offloaded-socket loopback is
  restricted, fall back to the two-socket same-process exchange. Task 4 is the empirical gate.
- **`zsock_*` symbol drift** against the installed NCS/Zephyr SDK — the backend's own caveat.
  Task 4 flushes it out; fix symbols in the backend `.c` only (the `udp_backend.h` contract is
  fixed).
- **Bytes bridge correctness.** Confirm `Bytes overRaw:` and the new `rawLeaf` accessor
  against kernel.ns before wiring; ensure `basicSend:` still receives a same-layout leaf
  (a `Bytes` *wrapper* would break the C `struct String *` cast — the facade must unwrap).
- **Build-list omission (Task 2).** Historically several tuvm build scripts omitted
  `serial/udp/mirror`; a host-only single-file run won't catch a gate that fails to link the
  Bytes change. Audit explicitly.
- **Footprint.** If the mcu ratchet moves (e.g. the net node pulls extra kernel methods into
  the solidified image, or the Bytes path adds C), re-bank with `FOOTPRINT_UPDATE_BASELINE=1`
  and record the delta.

## 8. Follow-on rungs (named, not designed here)

- **Rung 6 — Layer 2: reactive channel↔UDP bridge (CBOR + `pumpEvents` eventing).**
- **Rung 7 — Layer 3: real OpenThread on the nRF52840-DK** (brings up the native_sim-validated
  `udp_zephyr.c`; retires the skipped `43_udp_mesh`/`44_udp_thread`).
