# Rung 5 — UDP transport (Layer 1: net capability) — native_sim proof — design

**Status:** design frozen 2026-07-10; **reconciled to as-built same day** after verifying the
host net capability already ships and passes; **Bytes payload deferred to Layer 2** (David,
2026-07-10). Brainstormed with David 2026-07-10.
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

**Therefore the host half of Layer 1 is already done — this rung is almost entirely the
native_sim proof.** What is genuinely unbuilt/unproven, and what this rung delivers:

1. **native_sim functional round-trip** — the Zephyr `udp_zephyr.c` backend *links* on
   native_sim (the rung-2 pulse gate enables NSOS networking so it must) but has **never
   functionally round-tripped a datagram**. This is the rung's core (and nearly its only)
   deliverable — no C or facade change, so no risk to the 179/0/12 host suite or five gates.
2. **Close** — `net` vocabulary/docs (dialect note), program-sequence update, footprint
   confirmation (no C changed → expected unchanged).

**Deferred (David, 2026-07-10):** the **String → Bytes payload** refactor. The host capability
already works with String; switching to Bytes is now a refactor of tested code that would
force the immutable `Bytes` value to expose its raw leaf to the send prim (an immutability
seam). Layer 2's CBOR bridge needs raw bytes anyway and can keep that leaf internal, so Bytes
is introduced there, where it belongs — not in this rung. Keeps Layer 1 lean.

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
- Confirm the host suite + five named gates stay green (no code change → expected trivially).
- `net` dialect/docs note; program-sequence update; footprint confirmation.

**Out of scope (deferred, reversible)** — *keep Layer 1 lean* (David, 2026-07-10):
- **String → Bytes payload** → **Layer 2** (the CBOR bridge; see §0 and §4.1).
- The channel↔UDP bridge, `pumpEvents` inbound hook, `whenever:do:` eventing → **Layer 2**.
- CBOR at the wire → **Layer 2**.
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

### 4.1 Payload type = String for now; Bytes deferred to Layer 2
The socket path carries **String** payloads today (`42_udp.ns`: `dg data = 'ping'`) and works
on both paths. A datagram is semantically opaque bytes, and Layer 2 will carry CBOR (not valid
UTF-8), so the payload *will* become immutable `Bytes` — but **that change lands in Layer 2**,
not here (David, 2026-07-10). Rationale for deferring: the host capability is already tested
with String, and moving to Bytes would force the immutable `Bytes` value to expose its
underlying raw-bytes leaf so the send prim can read it (`basicSend:` casts `args[3]` to
`struct String *`; a `Bytes` *wrapper* is an ObjectInstance, not a same-layout leaf) — an
immutability seam. Layer 2's CBOR bridge already deals in raw bytes (`Cbor>>encode:` returns an
immutable `Bytes` over a `RawBytes` leaf) and can keep that leaf internal to the bridge,
introducing Bytes exactly where it is needed. So Layer 1 stays String; no `udp.c`/facade change
this rung.

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
| host (POSIX) | A + B | `udp_posix.c` | already green; **untouched** (no code change) |
| native_sim (Zephyr) | A only | `udp_zephyr.c` | **new: functional round-trip over NSOS loopback** |

The host suite stays both-path green (179/0/12, unchanged — no source edit) and the five named
gates (grant, debugger-integration, portability-32, boot-gc-asan, mcu-footprint) hold.

## 6. Task sketch (~2, subagent-driven; controller owns every full-suite run)

1. **native_sim functional round-trip.** Write `tuvm/zephyr/NetNode.ns` (a Model-A node
   declaring `net:` that binds a socket, sends to loopback, polls receive, prints an `OK`
   sentinel — mirroring `42_udp.ns`'s `roundTrip:` logic, self-contained in the tuvm repo like
   `PulseNode.ns`). First confirm it passes on **host** Path A (isolates node logic from the
   Zephyr backend). Then bake it into `solidified_image.c` via `ns_compile_oevm.py`,
   `west build -b native_sim -p always tuvm/zephyr`, run under `timeout`, assert the `OK` line
   (datagram actually round-tripped over NSOS — the `udp_zephyr.c` functional gate). Restore
   the clobbered `solidified_image.c`/`.stmap` before any commit. If NSOS offloaded loopback
   misbehaves, fall back to a two-socket same-process exchange on an ephemeral port (still a
   genuine backend round-trip) — §7.
2. **Close.** Confirm the host both-path suite (179/0/12) + five named gates unchanged (no
   source edit); footprint gate PASS (no C change → expected identical, no re-bank needed);
   NEWSPEAK_DIALECT `net` note; reconcile the tests README net line; program-sequence
   Phase-2/3 net line → "Layer 1 done (host both-path + native_sim); Layers 2/3 named."

## 7. Risks / watch-items

- **native_sim NSOS loopback semantics.** NSOS offloads to the host's real sockets, so
  `::1`/`127.0.0.1` loopback *should* behave like posix. If offloaded-socket loopback is
  restricted, fall back to the two-socket same-process exchange. Task 4 is the empirical gate.
- **`zsock_*` symbol drift** against the installed NCS/Zephyr SDK — the backend's own caveat.
  Task 1 flushes it out; fix symbols in the backend `.c` only (the `udp_backend.h` contract is
  fixed). This is the one place the outcome can't be predicted without running the build.
- **native_sim node output plumbing.** Confirm the baked node's `println` reaches the
  native_sim console (stdout) so the `OK` sentinel is observable under `timeout`, matching how
  the pulse gate observes `observed done`.
- **Footprint.** No engine C changes this rung, so the mcu ratchet is expected identical; if
  it somehow moves, investigate before re-banking (no silent ratchet).

## 8. Follow-on rungs (named, not designed here)

- **Rung 6 — Layer 2: reactive channel↔UDP bridge (CBOR + `pumpEvents` eventing).**
- **Rung 7 — Layer 3: real OpenThread on the nRF52840-DK** (brings up the native_sim-validated
  `udp_zephyr.c`; retires the skipped `43_udp_mesh`/`44_udp_thread`).
