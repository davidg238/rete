# porta beyond the LAN: federation, and the transports a public deployment would use

**Conceptual record — 2026-09-14. NOT BUILT, nothing scheduled.** Written at David's
request before the day's work, from the question: *we defined porta as a gateway for a
local area network — could some portas hold nodus nodes over Meshtastic or cellular, and
could those portas then federate?* The answer is yes, and most of it is already true of
the system as built. This document records **why** it is already true, what each transport
family would cost, the one claim in the question that needs correcting, and the
dependencies — so that #253's frozen-surface enumeration and #79's go-to-market both see
it. It is an input to #253, not a rung.

Tracker: the federation issue is **#292**; the auth gap it exposed is **#291** (G5, which
until today had no number).

---

## 1. porta was never a LAN gateway; it is a gateway for a datagram path

Three properties of the system as built carry the whole of the claim. None of them was
designed for federation; each was designed for a sleepy radio node, and it turns out that
is the same design.

1. **The exchange is node-initiated and idempotent.** A node `pull`s the writes porta has
   for it and `push`es its own entries (`protocol/PROTOCOL-NSL.md` §7). A node needs a
   route *to* its porta and nothing else: no listener, no inbound reachability, no session
   to keep alive. `porta/internal/wire/server.go`'s loop is "one datagram in, one datagram
   out; no connection state, so a sleepy node costs the gateway nothing between check-ins."
   That sentence is true of a node behind carrier-grade NAT, a node on a LoRa mesh with a
   ten-minute duty cycle, and a node on the bench.

2. **The protocol is a pure function.** `HandleFrom(req []byte, peer string) []byte`
   (`server.go:120`) turns one request into one response with no socket in sight; `Serve`
   takes any `net.PacketConn` (`server.go:299`). A transport is therefore a loop that
   wraps one function. The UDP loop is thirty lines. A Meshtastic loop, a serial loop or a
   cellular loop is the same thirty lines around a different reader.

3. **Paths are node-relative and `node/<id>/**` is a view, not storage** (rewrite design
   D4.5). The node id is the factory link address and **a LAN id only** — decided
   2026-09-11 (#118/#242, `docs/design/2026-09-11-node-identity-hardware-rooted-design.md`).
   A node on a distant porta needs no globally unique name because the *pair*
   (porta, node) is the global name, and `site/<porta>/node/<id>/**` is one more view over
   the same rows.

The question's premise — that a remote node "would not need a globally unique id" — is not
a new decision. It is the one made four days ago.

---

## 2. Federation: a porta is a node

The tempting design is a porta-to-porta synchronisation API. The one that adds **no
mechanism** is this:

> A downstream porta checks in with an upstream porta using the same `pull`/`push`,
> presenting its whole store as its tree.

- The upstream sees one node whose tree is `node/<porta-id>/…`, and the downstream's
  nodes appear beneath it. Rendered, that is `site/<porta-id>/node/<id>/**` — a view.
- **Goals flow down as ordinary outbound entries.** An operator writes
  `site/X/node/Y/goal/name` at the upstream; the downstream porta pulls it, stores it
  under `node/Y/goal/name`, and node Y pulls it on its next check-in.
- **Observations flow up as ordinary pushes.** `obs/**`, `tel/**`, `sys/**` and porta's
  own `gw/**` verdicts ride the same push the node used, one hop later.
- The `goal/X` versus `obs/X` diff is still the whole truth, at every level. There is no
  second consistency model.
- Values are raw deterministic CBOR (D4.1), so mirroring is a byte copy; timestamps are
  milliseconds everywhere (D4.2); one monotonic seq per space per side (D5) composes,
  because the downstream porta's outbound-to-upstream is one space.

This is the shape custos §8 already named — "aggregation and federation, not a
godzilla-sized database in the sky" (David, 2026-08-22,
`porta/docs/design/2026-08-22-gatekeeper-first-cut-design.md`) — with the snapshot file
replaced by the live exchange. Custos's snapshot path stays as the *read-only* federation
for dashboards; the porta-as-node path is what carries goals downward. Whether both are
needed is a measurement, not a decision to make now.

Two things to **record, not solve**:

- **The stream ack becomes a two-hop ack.** The upstream's `cursor_hw` for a downstream
  porta means *delivered to the downstream porta*, not *applied on the node*. The node's
  own ack lives one hop down, and `obs/**` is the evidence that a goal took effect —
  exactly as today, one level up. This is a semantic to write into `PROTOCOL-NSL.md` §3
  when the rung is cut, not a defect.
- **The 512 B batch is wasteful porta-to-porta over IP.** Ship it anyway. The manifest's
  size law (`PROTOCOL-NSL-RECORDS.md` §2.1/§2.2) frames 512/256 as *radio* constraints,
  which makes a per-transport instance of the law the natural generalisation — but that
  is a tunable, and the posture says do not invent one before there is data. A porta with
  fifty nodes syncing at 512 B a datagram is the degenerate case whose cost can be
  measured.

**What joins the frozen surface (#253):** a *porta id*. Today porta has no identity of its
own; a federation gives it one, and it is a wire-visible name. This is the one new item
this document adds to the freeze list.

---

## 3. Transport families

The law is one line: **a node holds a datagram path to its porta; porta wraps
`HandleFrom`.** What differs per family is the perimeter, the payload bound, the cadence,
and which side has to change. The node's own transport seam is `PortaBridge socket:` — a
UDP `Socket` today (`nodus/nsl/lib/kernel.nsl:1517`, `:2876`), and since #274 the bridge
is built with `socket: nil` when there is no radio, so a second backend answering
`sendTo:port:data:` plus a receive path is the whole node-side change for any non-IP
transport.

| family | perimeter without G5 | payload vs the 512/256 law | cadence | node side | porta side |
|---|---|---|---|---|---|
| **Thread (802.15.4 / 6LoWPAN)** | network key (#288) | 512 B = 5–6 fragments, the law's origin | seconds | as built | as built |
| **Wi‑Fi** | WPA2 / the LAN | no bound | seconds | Kconfig: the C6 has the radio | as built |
| **Ethernet / host** | the LAN | no bound | seconds | as built (`tutorial-porta` on loopback) | as built |
| **Cellular (LTE‑M / NB‑IoT)** | **none** | no bound (1280 B MTU typical); cost per byte | minutes–hours (PSM / eDRX) | a board port: nRF9160 is Zephyr under the NCS toolchain already in `~/ncs`, same UDP socket | as built; G5 first |
| **Meshtastic (LoRa)** | channel PSK ≈ a Thread network key | **tighter: ~233 B data payload, NO fragmentation** → batch ≤ 233, value ≤ ~200 | minutes (duty cycle 1 % EU) | MCU + Meshtastic radio over UART: a `socket:` backend speaking the serial client API | a radio on the host's USB and a loop calling `HandleFrom` |
| **LoRaWAN** | the LNS's keys | 51 B at DR0 | minutes; class A downlink only after uplink | as Meshtastic, different modem | an LNS integration (MQTT from ChirpStack / TTN) — a **third party in the loop** |
| **Satellite SBD** (Swarm, Iridium) | the operator's | 192–340 B; cost per message | hours | as Meshtastic | the operator's delivery API — a third party |
| **BLE** | pairing | 6LoBLE exists in Zephyr but is fading | seconds | — | realistically a phone acting as the transport adapter |
| **Serial / USB tether** | physical | no bound, but the dongle console DISCARDS under burst (#286) | seconds | the console cable | a serial loop |

Observations that fall out of the table:

- **Wi‑Fi is the first outside deployment.** It needs a router and nothing else — no
  border router, no dataset overlay, no mesh. The C6 has the radio and the port already
  runs it under Zephyr; the UDP socket is unchanged. If one node ever leaves the house,
  this is the transport it leaves on. (The C6's Wi‑Fi and 802.15.4 share one radio; a node
  is one or the other.)
- **Cellular changes the node side least and the security side most.** An nRF9160 is a
  Zephyr board under the toolchain this repo already builds nRF52840s with; the UDP socket
  and the bridge are unchanged; PSM/eDRX simply makes it a sleepy node with a declared
  `telCad` of minutes or hours — which is exactly why #273 H1 mattered: a gateway constant
  would have been wrong for this node, and the node's declaration is right by
  construction. Carrier-grade NAT is not an obstacle, because there is no session to time
  out: each exchange is one request and one reply. What cellular removes is the perimeter
  (§4). One useful fact: the nRF91 modem runs DTLS in modem firmware, so §7 item 7's RAM
  question does not gate that board family.
- **Meshtastic is tighter than 6LoWPAN, not looser.** The question implied a looser
  network; the numbers say the opposite. Neither today's 512 B batch nor its 256 B value
  fits a single Meshtastic packet, and the mesh does not fragment, so the size law needs a
  per-transport instance — the same generalisation §2 needed for the other reason. The
  duty cycle puts check-ins at minutes, and porta's reply is a second packet: a check-in
  costs two airtime slots, not one. The store's replication model (§1 "not a synchronous
  mount") is what makes this fine; a request/reply protocol would not survive it.
- **LoRaWAN and satellite put a third party in the loop.** The network server holds the
  keys, queues the downlink, and delivers uplinks over its own API. porta can be a client
  of that API — it is still a loop around `HandleFrom` — but the perimeter and the
  delivery guarantees belong to someone else. Named so they are not confused with
  Meshtastic, where the whole path is the operator's. Not proposed.
- **The serial tether is the degenerate transport**, and possibly a test fixture: it is
  the transport `liveness-noradio` does not have, and it would let a node with no radio at
  all complete an exchange on the bench. The #286 discard is the caution.

---

## 4. Security, honestly

The question said the remote nodes would be "protected by porta from attack." Half of
that is true everywhere, and the other half depends entirely on the transport.

**True everywhere:** there is no inbound path to a node. It initiates every exchange, it
listens on nothing the internet can reach, and behind carrier NAT it has no address at
all. That is real protection and it costs nothing.

**Not true today, anywhere:** porta itself is unprotected. `HandleFrom` decodes any
well-formed datagram from any peer; `ensureNode` (`porta/internal/space/store.go:187`)
creates the node row on first mention, so an unknown sender does not fail — it *becomes*
a node; and the node id is a name, not a credential (the 2026-09-11 identity decision
says so in its own non-goals). Anyone who can reach porta's UDP port can push `obs/**`
as any id and pull that id's `goal/**`. **porta's perimeter is the transport's:**

- Thread — the network key, baked into every image from a gitignored overlay (#288);
  the image is the credential (#289).
- Wi‑Fi / Ethernet — WPA2 and the LAN.
- Meshtastic — the channel PSK. Comparable to Thread; a public deployment on Meshtastic
  is no worse than the house.
- **Cellular — nothing.** porta's port is on a public network.

So: Meshtastic federation costs one size-law instance and a porta-as-node check-in.
**Cellular costs G5 first.** G5 — "node identity + session auth wrap the exchange at the
transport layer; prefix authority is enforced inside" (`PROTOCOL-NSL.md` §1, §3.3, §7
items 5 and 7) — had no tracker number until this document; it is **#291** now. The
protocol's stated answer is DTLS (CoAP's reason for existing in §5.3), gated on the
dongle-class RAM measurement; #291 also names the smaller alternative (a per-node key over
the existing UDP framing) and the one contract G5 changes: after it, an unauthenticated
inbound datagram must *not* create a node, while the operator door staging `goal/` for a
not-yet-seen node stays.

---

## 5. Dependencies, in order

None of this is scheduled. This is the order in which it *could* be, and what each step
is gated on.

1. **#253 — the frozen 1.0 surface.** Two additions from this document: a **porta id**,
   and the size law stated **per transport** rather than as two constants. Both are
   wire-visible; both belong in the enumeration before the first external user.
2. **#200 — wire versioning.** Today's posture is "both ends are reflashed together." A
   downstream porta on someone else's site **cannot be reflashed with its nodes**, so a
   federation makes mixed versions structural rather than accidental. #200's
   recommendation (relax FIELD closure, keep TAG closure, name the end of the flag-day
   era) is the minimum a federation needs; it does not need more.
3. **#291 — G5.** Required for cellular; optional for Meshtastic; not required for a
   Wi‑Fi node on a LAN the operator owns. The order of transports in §3 is the order in
   which this becomes load-bearing.
4. **#289 — onboarding.** "Which porta does this node belong to" is the credential
   question seen from the other side; the two want one answer, and the porta id from
   step 1 is the thing a node is onboarded *to*.
5. **#233 — OTA is inert.** A node on a transport the operator cannot walk to must update
   itself, or it is a brick in waiting; and #200's flag day is impossible without it.
   Note the interaction with §3: a 200 kB image over a 233 B / 1 %-duty-cycle Meshtastic
   link is on the order of a day of airtime. Bulk transfer is deliberately off the space
   and deferred to a CoAP blockwise plane (rewrite design); on LoRa-class transports that
   plane may not be the answer either, and the honest option is *no OTA on that family*.

---

## 6. Other gaps seen from here, before anything goes public

Not transports. Found while walking the seams above; each has a home or gets one.

- **The operator surface is the sqlite file.** Every CLI verb calls
  `space.OpenExisting(dbPath)` (`porta/internal/portacli/{get,put,log,snapshot}.go`), so
  remote operation is ssh, and v1's HTTP and MCP surfaces were given the fate "deferred"
  in the rewrite's §4.2 audit. For one porta in a house that is fine. For an operator
  writing goals into an upstream porta it is not — though §2 answers *most* of it: the
  upstream porta's store is the one door, and the downstream nodes need nothing. What is
  missing is a human's door into the upstream that is not a shell. This is #266's `doit`
  question and the §4.2 deferral, seen together; no new issue — it belongs on #253's list.
- **Gateway health has no verdict.** #276 (a porta with a broken uplink reads `#silent`
  for every node; a porta refusing everything reads `#live`), #282 (gateway services
  unsupervised, logs do not survive the restart) and #290 (`gw/liveness` is recorded, never
  actioned) are one problem. A federation multiplies gateways, and a downstream porta *is
  a node* to the upstream — which means porta-as-node gives gateway health the same
  verdict machinery nodes have, for free. That is an argument for §2 that was not in the
  question.
- **Event time is arrival time.** `cells.ts_ms` and the event log are stamped by porta
  at arrival; nodes have no wall clock and do not need one. On a store-and-forward
  transport (satellite, a Meshtastic store-and-forward relay) arrival can lag the event by
  hours, and `last_seen_ms` — which the liveness sweep judges on
  (`porta/internal/space/sweep.go:60`) — measures the transport, not the node. The node's
  own monotonic counters (`wak`, `bid`) are the order; the sweep's `bud = telCad × misses`
  absorbs the lag only if the declared `telCad` includes it. Recorded, not fixed: it is
  only wrong on transports this document does not propose.
- **No `LICENSE` file in the tree.** Not architecture; a publication gate all the same.
- **Hardware compliance if boards ship** — no hobbyist exemption, a certified module does
  not cover a different antenna, USB or mains adds a conducted test. Out of scope here;
  the memory holds the list.

---

## 7. What this document does not decide

- Whether custos's snapshot federation and porta-as-node both survive, or one absorbs the
  other. Measure.
- The per-transport size law's numbers. Measure on the first non-6LoWPAN transport.
- G5's mechanism. #291.
- Any of §3's node-side backends. The first one to build is Wi‑Fi, and it is a Kconfig.
