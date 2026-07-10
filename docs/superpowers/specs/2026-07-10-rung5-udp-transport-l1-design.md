# Rung 5 — UDP transport (Layer 1: net capability) + native_sim validation — design

**Status:** design frozen 2026-07-10. Brainstormed with David the same day.
**Phase:** program-sequence Phase 2/3 seam — the transport that the CBOR wire seam (rung 3)
and the reactive channel model (Milestone C) were built to feed. **This is Layer 1 of a
three-layer arc** (see "Layering" below); it is deliberately the smallest, de-risking bite.

## 1. North star and where this rung sits

`review/demo-node.ns` is network-transparent: the node posts readings to a `node7/reading`
channel and *"porta bridges 'node7/reading' onto the LAN (resource?id= addressing, CBOR
wire)."* The node never touches a socket. Getting there needs a real transport under the
reactor. The transport splits cleanly into three independently-shippable layers, and this
rung is Layer 1 only:

- **Layer 1 (THIS RUNG) — the `net` capability.** A confined, no-fiber-clean UDP primitive:
  bind / send / non-blocking receive / close, carrying **opaque byte payloads**. Proven on
  host POSIX (both image paths) and on native_sim (Zephyr backend, functional round-trip).
- **Layer 2 (next rung) — the reactive channel↔UDP bridge.** Inbound datagram → (during
  `pumpEvents`) CBOR-decode → post to a channel → `Space>>drain` fires the reaction;
  outbound channel post → CBOR-encode → `udp_send`. This is where CBOR and the reactor
  eventing live. It makes the node network-transparent (matches demo-node).
- **Layer 3 (later rung) — real OpenThread on silicon.** Bring the Zephyr `udp_zephyr.c`
  backend up on the nRF52840-DK over an OpenThread mesh (SDK-symbol, Kconfig,
  commissioning, DK capture). Layer 1 already validates the same backend on native_sim, so
  Layer 3 is a radio/mesh bring-up, not a socket rewrite.

Decomposition rationale (David, 2026-07-10): "1 then 2" is a good bite size; each layer is
an independent spec→plan→implement cycle, and Layer 1 de-risks Layer 2 (which de-risks
Layer 3). Host-proof-first matches the A–E pattern.

## 2. Scope (locked)

**In scope**
- Make the existing-but-inert `net` capability **live** on the current no-fiber reactive VM.
- Payload type is **immutable Bytes / ByteArray** (not String) — see §4.1.
- Proof matrix (§5): host Path A + Path B (posix backend); native_sim Path A (zephyr
  backend, **functional** round-trip over Zephyr NSOS loopback — not merely a link).
- Round-trip (v4 + v6 loopback) + confinement (runtime DNU) tests; GC temp-rooting
  re-verified against the post-conservative-scan world.
- Footprint bank + docs/vocabulary reconcile + program-sequence update.

**Out of scope (deferred, reversible)** — *keep Layer 1 lean* (David, 2026-07-10):
- The channel↔UDP bridge, `pumpEvents` inbound hook, and `whenever:do:` eventing → **Layer 2**.
- CBOR at the wire → **Layer 2** (Layer 1 carries opaque bytes; no dependency on the rung-3
  `Cbor` class).
- Real OpenThread / DK silicon → **Layer 3**.
- Blocking/timeout receive, multicast, TCP, DNS resolution → later, if a workload forces it.
- Lifting the serial-XOR-net single-2nd-cap-slot limit (§4.4) → not this rung.

## 3. Current state — this is re-validation, not a build

The full capability spine already exists in the tree (carried from the pre-pivot
`2026-06-26-ns-oevm-udp-networking-capability-design.md`, mirrored on the live `serial`
spine) and is internally consistent. It is **inert only because no entry program declares a
`net:` parameter**, so the both-path boot injection of `udp_object()` never fires.

Existing pieces (source of truth = the code; line numbers approximate, as of 2026-07-10):
- **`tuvm/src/udp.h` / `tuvm/src/udp.c`** — `udp_object()` static-BSS singleton +
  `udp_dispatch()` with its own string-compare selector matching. Four selectors:
  `basicBind:` (port SMI → fd SMI / −1), `basicSend:to:port:data:` (fd, hostStr, portSMI,
  dataStr → self), `basicReceive:` (fd → 3-slot Array `{data, host, portSMI}` or nil),
  `basicClose:` (fd → self). Reached by a **receiver-identity gate** in `dispatch.c`
  (`if (receiver == udp_object()) return udp_dispatch(...)`), **not** a PRIM_NAMES id.
  Zero fiber/NLR coupling. `basicReceive:` already pins intermediate oops with
  `gc_temp_root_push`/`gc_temp_roots_clear` (post-conservative-scan-aware).
- **`tuvm/src/udp_backend.h`** — the 6-function link-time seam (string addresses, backend
  owns family, non-blocking receive: payload len ≥0 or −1 when nothing queued).
- **`tuvm/src/backend/posix/udp_posix.c`** — real dual-stack POSIX (AF_INET6 + IPV6_V6ONLY
  off, O_NONBLOCK, getaddrinfo/recvfrom, v4-mapped normalization).
- **`tuvm/src/backend/zephyr/udp_zephyr.c`** — `zsock_*` backend. **Already compiles and
  links on native_sim today** (the rung-2 pulse gate's `tuvm/zephyr/prj.conf` enables the
  full NSOS networking block precisely because `dispatch.c`'s `send()` fast path
  unconditionally probes `udp_object()`, so `udp.c` + `udp_zephyr.c` must link regardless).
  Carries a caveat comment that its `zsock_*` symbol names are unverified against the
  running SDK — **the functional round-trip is the gate that has never been run.**
- **`nsl-tuvm/lib/kernel.ns`** — `class Net` / `class Socket` / `class Datagram` +
  non-public `Kernel>>netOn:` (confined, mirrors `serialOn:`).
- **Both-path grant** — Path A: `nsl-tuvm/tools/ns_compile_oevm.py` threads `net` when the
  entry declares it (emits `extern oop udp_object(void)` + `#netOn:` send). Path B:
  `nsl-tuvm/tools/oimg_serialize.py` emits `net_on_sel_ref` at header `A+12W`
  (format_version 6); `tuvm/src/oimg_loader.c` reads it and mints the `Net` wrapper into
  the single 2nd-cap slot (serial XOR net).

Reassuring findings for the no-fiber concern: the recv path is **already non-blocking
poll-based** (no blocking model, no fiber suspension); the only VM-internals coupling is GC
rooting, already ported. The roadmap's *"fiber-free entry points need review"* means
re-verifying exactly that rooting, not removing dead fiber calls (there are none).

The Zephyr build is **Model A (solidified image) only** — `tuvm/zephyr/` excludes
`oimg_loader.c`/`kernel_boot.c` (their static `Vtable_*` clash with the baked
`solidified_image.c`). Therefore **Path B is a host-only concern**; the native_sim proof is
Path A (node baked via `ns_compile_oevm.py`, whose `net` boot-threading already exists).

## 4. Design decisions

### 4.1 Payload = immutable Bytes / ByteArray (not String)
The existing `basicReceive:` returns `data` as a String (`make_string`). This rung switches
send + `Datagram>>data` to carry an **immutable Bytes** value. A datagram is opaque bytes,
and Layer 2 will carry **CBOR**, which is not valid UTF-8 — a String payload is semantically
wrong and would misbehave the moment the bridge lands. Change is local: `udp.c`'s
`basicReceive:` builds a Bytes (the same byte-container the rung-3 `Cbor`/`ByteArray` work
uses) instead of a String, `basicSend:to:port:data:` reads bytes from a Bytes argument, and
the `Socket`/`Datagram` facade types follow. Keeps Layer 1 free of any `Cbor` dependency.
(David, 2026-07-10: "bytes".)

### 4.2 Opaque, poll-based, standalone — keep Layer 1 lean
No CBOR, no reactor integration, no `pumpEvents` inbound hook in this rung (David:
"keep lean"). `Socket>>receive` stays a caller-driven non-blocking poll returning a
`Datagram` or nil; a receive loop is the test's responsibility. All eventing is Layer 2.

### 4.3 Address model — unchanged
Host is a String (IPv4-dotted or IPv6), port is an SMI; the backend owns the family
(IPv6-first dual-stack). v4-mapped senders normalize to dotted-quad.

### 4.4 serial XOR net single-slot limit — persists (documented, not fixed)
The loader mints at most one 2nd-capability wrapper per entry (serial OR net). Layer 1's
test node grants only `net`, so this is not a blocker; the limit is carried forward and
noted. Lifting it (an entry taking both serial and net) is out of scope.

## 5. Proof matrix

| Target | Image path | Backend | Proves |
|---|---|---|---|
| host (POSIX) | A (solidified) + B (`.oimg`) | `udp_posix.c` | both-path `net` grant; round-trip v4+v6 loopback; confinement DNU |
| native_sim (Zephyr) | A (solidified) only | `udp_zephyr.c` | **functional** round-trip over Zephyr NSOS loopback (backend works, not just links) |

The host suite must stay green on both paths (regression gate); the five named gates
(grant, debugger-integration, portability-32, boot-gc-asan, mcu-footprint) must hold.

## 6. Task sketch (~6, subagent-driven per prior rungs; controller owns every full-suite run)

1. **`udp.c` payload→Bytes + GC re-verify.** Switch `basicReceive:`/`basicSend:to:port:data:`
   to the Bytes container; re-verify temp-rooting across intermediate allocations; host C
   smoke test (`test_udp.c`, backend-only) still green.
2. **Host Path A round-trip.** New net-granting test node declaring `net:` (new
   `nsl-tests/15_net/01_roundtrip.ns` or the repo's current convention); v4+v6 loopback
   round-trip green on Path A. Verify the existing `ns_compile_oevm.py` `net` threading.
3. **Host Path B round-trip.** `.oimg` net grant (v6 header already present — verify the
   emit/read round-trips); green on BOTH paths. Confirm `.oimg` offset parity leaves the
   serial test undisturbed.
4. **Confinement pin.** `02_confined.ns`: an entry declaring only `usingPlatform:` cannot
   reach `net`; runtime DNU is the pass, both paths.
5. **native_sim functional round-trip.** Bake a net-granting node (Model A) via
   `ns_compile_oevm.py`; `west build -b native_sim -p always tuvm/zephyr`; round-trip a
   datagram over NSOS loopback; assert under `timeout`. Restore the clobbered
   `solidified_image.c`/`.stmap` before any commit. This is the `udp_zephyr.c` functional gate.
6. **Close.** Footprint bank (expected ≈ zero engine delta — `udp.c`/`udp_zephyr.c` already
   link in config-D; going live adds no C, only exercises it — verify the mcu ratchet holds);
   write `review/lib/net.md` + a NEWSPEAK_DIALECT note; reconcile `CLAIMS.md`/tests README;
   `program-sequence.md` Phase-2/3 net line updated to reflect Layer 1 done + Layer 2/3 named.

## 7. Risks / watch-items

- **native_sim NSOS loopback semantics.** NSOS offloads to the host's real sockets, so
  `::1`/`127.0.0.1` loopback should behave like posix. If offloaded-socket loopback is
  restricted, fall back to a two-socket same-process exchange on an ephemeral port (still a
  genuine backend round-trip). Task 5 is the empirical gate.
- **`zsock_*` symbol drift** against the installed NCS/Zephyr SDK — the backend's own caveat.
  Task 5 flushes it out; adjust symbols to the SDK if one differs (backend `.c` only; the
  `udp_backend.h` contract is fixed).
- **Bytes container identity.** Confirm the exact byte-container ctor/vtable the rung-3
  `Cbor`/`ByteArray` path uses so `udp.c` builds the same type the facade expects
  (verify-first against `bytes.ns` + the C helpers, mirroring how `make_string` is reached).
- **Footprint.** If the mcu ratchet *does* move (e.g. the new node pulls extra kernel
  methods into the solidified image, or the Bytes path adds C), re-bank with
  `FOOTPRINT_UPDATE_BASELINE=1` and record the delta — no silent ratchet bump.

## 8. Follow-on rungs (named, not designed here)

- **Rung 6 — Layer 2: reactive channel↔UDP bridge (CBOR + `pumpEvents` eventing).** The
  substantive net-new work; makes the node network-transparent.
- **Rung 7 — Layer 3: real OpenThread on the nRF52840-DK.** Radio/mesh bring-up of the
  already-native_sim-validated `udp_zephyr.c`.
