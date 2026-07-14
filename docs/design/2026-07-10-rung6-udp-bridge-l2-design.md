# Rung 6 — Layer 2: the reactive channel↔UDP bridge (CBOR) — design

**Status:** design frozen 2026-07-10. Brainstormed with David 2026-07-10.
**Phase:** program-sequence Phase 2/3 — Layer 2 of the three-layer UDP arc named in the
Rung 5 (L1) design. L1 proved the socket (host both-path + native_sim); this rung makes the
node **network-transparent**: a `Record` posted to a channel travels out as CBOR/UDP, and an
inbound datagram becomes a decoded `Record` firing a `whenever:` reaction.
**Proof bar (David, 2026-07-10):** **bidirectional round-trip on host AND native_sim**
(mirroring L1's dual proof).

## 0. North star and where this sits

`review/demo-node.ns` is network-transparent by construction: the node posts readings to
`node7/reading` and *"porta bridges 'node7/reading' onto the LAN … CBOR wire."* The node never
touches a socket. Rung 5 delivered the socket (Layer 1). This rung delivers **the bridge** —
the reusable machinery that connects a channel to a socket in both directions with CBOR on the
wire — so the node stays socket-free while its readings reach the LAN and remote pokes reach the
node.

The three-layer arc (from the L1 design), for orientation:
- **Layer 1 (Rung 5, DONE)** — the `net` capability: confined UDP bind/send/non-blocking-recv/
  close, proven host both-path + native_sim.
- **Layer 2 (THIS RUNG)** — the reactive channel↔UDP bridge: inbound datagram → decode → post
  to a channel → `drain` fires; outbound channel post → encode → `udp_send`. Introduces the
  immutable-`Bytes` payload deferred from L1.
- **Layer 3 (later rung)** — real OpenThread on the nRF52840-DK.

## 1. The decision method (why this rung is lean, not a framework)

David asked the right meta-question: how do we choose a driver-authoring / reactor-ingress shape
that will still be clean once we have gpio, timers, udp, tcp, serial, i2c, spi, and third-party
drivers? The answer we adopted (David, 2026-07-10) is **rule-of-three + paper-validation**, NOT
"build all seven then rewrite":

1. **The sources are not one list.** Sorting by *how each crosses into the reactor* collapses the
   problem:
   - **Synchronous request/response — i2c, spi, most drivers.** NOT reactor ingress. Called
     *inside* a reaction (`i2c write:to:` is a blocking sub-ms C transaction returning bytes
     directly); their only async aspect is a data-ready/ALERT interrupt, which is already the
     **GPIO pulse**. These drop off the ingress list entirely.
   - **Signal sources — gpio, timer-expiry.** The event *value is known ahead of time* (a fixed
     symbol, a timer token). An ISR/clock stages a pre-built oop; pump injects it (the pulse
     mailbox; `host_advance` for timers).
   - **Data/transport sources — udp, tcp, serial, radio-RX.** Bytes arrive *at fire time* and
     must be decoded and routed. Decode+routing is **policy → nsl**. UDP is the **first** member.
2. **The "translate a raw external event into a semantic message" problem is already solved in
   the codebase.** Timers do exactly it: `host_advance` posts a bare `TAG_SMI(token)` onto a
   reserved `sys/timer` channel; a registered nsl reaction `whenever: 'sys/timer' do: [:e |
   timers dispatch: e]` translates it *in drain*. External source → raw event → nsl translator
   reaction. This idiom already repeats across gpio/timers, so for "raw event → nsl translator"
   we are past rule-of-three.
3. **Don't abstract from one data source.** Build UDP inbound in the simplest correct shape; the
   *second* transport (serial-rx, tcp) is what reveals whether the shape generalizes — extract
   shared scaffolding then, from two real examples, not zero. "Rewrite when it clarifies" is cheap
   precisely because each source is a thin nsl facade over a tiny fixed-shape C seam.
4. **Validate the shape on paper, not in speculative code** (the demo-node method — write the user
   program before the runtime). §7 sketches "how you'd add serial-rx" against the udp bridge shape;
   it costs a page and already surfaces a real finding (framing — §7).

## 2. The ingress model (Option 1: pump signals readiness, nsl does receive/decode)

**Key realization:** on **host there is no `select()`** — the L1 socket is caller-driven poll
(`receive` returns nil when empty). So "C signals readiness" degenerates to "poke the bridge," and
the bridge's `receive`-until-nil already tolerates a spurious poke. Therefore UDP inbound needs
**no reserved raw-event channel and no C readiness post**. The `sys/timer` reserved-channel
indirection exists only because the *clock prim* cannot do the translation; our bridge step is nsl
and decodes on the spot. Copying that indirection would cargo-cult the timer *shape* without its
*constraint*.

The minimal shape:

- **Inbound** — `bridge pumpInbound` (nsl): loop `socket receive` until nil; for each `Datagram`,
  `Cbor decode: dg data ifInvalid: […]` → post the decoded value onto the inbound `Channel`.
  Host drains the OS buffer; Layer 3 runs the same code gated by `k_poll`. Posting during pump
  *is* staging — `drain` then fires the node's reaction. **No new C.**
- **Outbound** — the bridge registers a normal drain-time reaction:
  `outChan whenever: '' do: [:v | socket sendTo: peerHost port: peerPort data: (Cbor new encode: v)]`.

**`pumpInbound` stays an explicit call this rung** (rule-of-three). The node/test loop runs
`bridge pumpInbound. space drain.` (plus `pumpEvents` if it also has pulses). Folding net into a
unified `Kernel>>pumpEvents` is **deferred** until a *second* data source forces it — a
pump-participant registry for one participant is exactly the premature machinery we are skipping.
The two-call awkwardness is the acceptable duplication rule-of-three tolerates.

**Run-to-completion** is preserved: `pumpInbound` stages all currently-pending inbound events; a
reply to a datagram *sent* inside a reaction is not seen until the next pump — correct RTC, no
synchronous request/reply.

**Capability discipline** holds: the bridge only ever posts onto the inbound `Channel` it was
granted at construction — no ambient injection, same confinement as every other capability.

## 3. The Bytes payload seam (the L1-deferred decision)

CBOR bytes are not valid UTF-8, so a datagram payload is semantically **opaque bytes, not text**.
Clean break to **Bytes-only** payloads (rejected: dual String-or-Bytes accept, which keeps a
text-shaped door open on a byte channel):

- **Inbound** — `basicReceive:` builds its data slot as a **`RawBytes` leaf** (was `StringVtable`
  via `udp_make_string`); `Socket>>receive` wraps it `Bytes overRaw: leaf`, so `Datagram>>data`
  is now an immutable `Bytes`. `Cbor>>decode:` already accepts a `Bytes` (its `rawBytesOf:size:`
  copies via `platform string:`). No decode-side change.
- **Outbound** — `Cbor>>encode:` already returns immutable `Bytes`. `Socket>>sendTo:data:` gets a
  `Bytes` and passes its raw leaf to `basicSend:` via a **module-internal `Bytes>>rawLeaf`**
  accessor. "Module-internal" is convention only, like every other non-public kernel setter — this
  VM's access modifiers are vestigial (oevm#36), not enforced. The leaf itself is mutation-capable
  (`RawBytes` understands `at:put:`), so this is not an enforced immutability boundary; it works
  because the single caller, `Socket>>sendTo:`, is relied upon to only read it (memcpy out to the
  socket) — it is the "keep the leaf internal to the bridge" the L1 spec (§4.1) asked for.

**Rejected alternative:** `basicSend:` taking the `Bytes` *wrapper* (an `ObjectInstance`) and C
reaching its ivar slot for `raw` — C depending on an nsl object's ivar layout is fragile.

This churns the one existing test that sends a String (`42_udp.ns`: `dg data = 'ping'`) — exactly
the "refactor of tested code" L1 §4.1 deferred here. Both `42_udp.ns` and `43_udp_confined.ns`
migrate to `Bytes` payloads.

## 4. The bridge API, addressing, malformed-input policy

`NetBridge` (in `lib/kernel.ns` alongside `Net`/`Timers`/`GpioPulse`) is a capability object wired
at setup, fully **bidirectional** (its own out + in + peer):

```
NetBridge socket: aSocket  out: outChan  peer: peerHost port: peerPort  in: inChan
  start        -> registers the outbound reaction (encode -> sendTo:port:data:)
  pumpInbound  -> receive-until-nil; decode; post decoded value onto inChan
```

- **Addressing** is raw **host String + port SMI**. demo-node's `resource?id=` LAN naming is
  porta's concern (a higher layer), out of scope.
- **Malformed-datagram policy** (follows `errors.md`): a garbled remote datagram is *untrusted
  network input* — an **expected** condition (noise, an attacker), **not a local bug**. So
  `Cbor decode: … ifInvalid: […]` **drops** it and **never crashes the node** (crashing would let
  any peer DoS the node). The rung drops silently; a `log`-channel post is the obvious one-line
  hook the bridge could take a block for. This untrusted-input boundary is the load-bearing reason
  the payload is opaque `Bytes` decoded *totally* (the L1 `Cbor>>decode:ifInvalid:` totality
  guarantee), never a String parsed optimistically.

## 5. Proof matrix (bidirectional, host + native_sim)

| Target | Image | What is proven |
|---|---|---|
| host (POSIX) | A + B | one process, one `Space`, two loopback sockets (ephemeral ports), two bridges A↔B: a `Record` posted on `a/out` travels encode → **real UDP loopback** → decode → a reaction on `b/in` observes it **value-equal**; then the reverse B→A. |
| native_sim (Zephyr) | A only | a self-contained Model-A node baked into `solidified_image.c` does the same bidirectional round-trip over NSOS loopback and prints an `OK` sentinel — reusing Rung 5's `NetNode.ns` build/run/restore machinery. |

The host both-path net tests (`42`/`43`, migrated to `Bytes`) stay green; the full suite and the
five named gates hold. New host test lives beside the existing net tests (`04_somns_basic/`; a
dedicated `16_net/` dir remains the cosmetic-deferral from L1).

Rung 5 already retired the NSOS-loopback-semantics and `zsock_*`-drift risks, so the native_sim
half is really "CBOR-over-UDP round-trips on native_sim."

## 6. Risks / watch-items

- **CBOR prims on native_sim.** `Cbor` drops to C leaves (`cborEncodeInt:`/`cborReadHead:`/…) in
  `platform.c`. `platform.c` compiles whole for the Zephyr build, so the leaves *should* link —
  but this is the one thing to verify empirically (§5 native_sim task). If a leaf is fenced out
  of the native_sim build, add it to the build-list sites.
- **Footprint moves this rung.** Unlike Rung 5, this rung **changes C** (`udp.c` payload type). The
  mcu footprint gate may move; **re-measure and re-bank if it does — no silent ratchet** (the
  standing discipline). A `Bytes`-vs-`String` leaf is the same size, so the delta should be near
  zero, but confirm.
- **Two-call ingress is intentional, not an oversight.** `bridge pumpInbound. space drain.` (plus
  `pumpEvents`) is the rule-of-three position; do not "fix" it into a registry this rung.
- **Malformed-input drop must be exercised.** The host test should include at least one garbage
  datagram to prove the node survives it (does not crash), not only the happy path.

## 7. Paper-validation: adding a second transport (serial-rx) — NOT built

To stress the bridge shape against a *second* example on paper (the demo-node method):

A `SerialBridge` would reuse the identical shape — a bound serial capability with a non-blocking
`receive`-leaf, a `pumpInbound` that drains-until-nil + decodes + posts, and an outbound reaction
that encodes + sends. **Where it diverges is the finding:** UDP is **message-framed** — one
datagram is exactly one CBOR item, so `Cbor decode:` consumes the whole payload and asserts full
consumption. Serial (and TCP) are **byte streams** with no message boundaries: bytes dribble in,
a CBOR item may span multiple `receive`s or share a `receive` with the next item. So a stream
transport needs a **framing layer** (length-prefix, or incremental CBOR head-parsing with a
carry-over buffer) that the datagram bridge does not.

**Conclusion the sketch buys us:** the `NetBridge` shape is correct for **framed** transports
(udp, radio-datagram) and generalizes to them for free; **stream** transports (serial, tcp) need a
framer between the transport and the bridge. That framer is a **named future concern**, not solved
here — but knowing it exists *now* means the bridge abstraction we ship is not accidentally
overfit to datagrams in a way that would have to be un-picked later. The bridge decodes a
*complete message's bytes*; supplying those bytes (whole datagram vs. de-framed stream slice) is
the transport's job.

## 8. Tasks (subagent-driven; controller owns every full-suite run)

1. **Bytes payload seam.** `udp.c` `basicSend:`/`basicReceive:` → `RawBytes` leaves; `Socket`
   wrap (`Bytes overRaw:`) / unwrap (`Bytes>>rawLeaf`, module-internal); migrate `42_udp.ns` and
   `43_udp_confined.ns` to `Bytes`. Prove host both-path net tests green in isolation before
   moving on.
2. **`NetBridge` + host bidirectional round-trip test.** Add the `NetBridge` class to `kernel.ns`;
   write the two-bridge loopback test (§5 host), including a malformed-datagram-survives case.
   Prove host.
3. **native_sim bidirectional round-trip.** A `NetNode`-style baked node doing the bidirectional
   round-trip over NSOS loopback; `west build -b native_sim -p always`, run under `timeout`, assert
   the `OK` line; restore the clobbered `solidified_image.c`/`.stmap` before any commit. First
   confirm the CBOR prims link on native_sim (watch-item §6).
4. **Close.** Full both-path suite + five named gates green; **footprint re-measure & re-bank if it
   moved** (C changed this rung); `net`/bridge `NEWSPEAK_DIALECT` note; reconcile the tests README
   net line; program-sequence Phase-2/3 line → "Layer 2 done (host bidirectional + native_sim);
   Layer 3 named"; ensure §7's serial-rx sketch is captured in this spec (it is).

## 9. Follow-on (named, not designed here)

- **Rung 7 — Layer 3: real OpenThread on the nRF52840-DK** (brings up the native_sim-validated
  backend on silicon; retires the skipped `43_udp_mesh`/`44_udp_thread`).
- **Stream-transport framing** (serial/tcp) — the §7 finding; its own rung when a workload needs a
  stream transport.
- **Unified `pumpEvents` ingress + idle-wait/power** — fold net + pulse + timers into one pump
  paired with a Zephyr `k_poll` wait (the deep-sleep payoff); the rule-of-three extraction, when a
  second data source lands.
