# Security levels: who may talk to whom, per node

**Status: DRAFT FOR REVIEW, 2026-10-02. Nothing here is decided or built. It records David's direction of 2026-10-02 and widens rete#291. It has had no review by a security specialist; treat it as a structured starting point, not as assurance.**

## 0. Summary

- Today nothing on the wire proves who sent it. A node id is a name anyone can say; the only protection is the transport's own wall (§3).
- David's direction: networks are mixed, and protection is decided per node. The outdoor thermometer need not prove itself; the robot in the house must.
- Proposed model (§5): three levels (dumb node, keyed node, gateway), set by what a node can be told to do, not by what it measures.
- The gateway keeps a rule per node id. An unknown id is refused, not created (§8).
- Above the wall, one rule for every transport (§7). The per-message mechanism is open and is decided by measurement (§9).
- Only a few items touch the wire envelope and so belong in surface 1.0 (§10). The rest is gateway policy and can change later.

## 1. How we got here

| Date | What happened |
|---|---|
| (earlier) | "G5 — The OUTER security boundary: who may push code?" is named in [reliability-gaps.md](../../review/reliability-gaps.md) with "Decide: before ANY over-the-network push path exists". |
| (earlier) | [PROTOCOL-NSL.md](../../protocol/PROTOCOL-NSL.md) §4.3 says "Auth / identity (G5) — the grant machinery cannot ride the thing it guards"; §7 open questions 5 (auth handshake) and 7 (DTLS memory footprint) are open. Deferred each time. |
| 2026-08-08 | rete#25: app images are signed (§3, item 8). |
| 2026-09-11 | rete#288: the premise "the mesh runs on stock sample credentials" was refuted; the real Thread dataset lives in gitignored overlays. What survived: "the image is the credential", the same key on every node. Same day, rete#289: no onboarding process past the house. |
| 2026-09-14 | [The federation design](2026-09-14-porta-federation-and-transports-design.md) §3–§4 walks the transports. rete#291 filed: no auth on the wire. |
| 2026-10-02 | David, reviewing the [frozen surface draft](2026-10-02-frozen-1.0-surface.md) (its §0a row 4), recorded on rete#291 and rete#307. |

David's words, 2026-10-02:

- "we need to be able to enable mixed networks ... I don't care if the outside temperature sensors are authenticated, but the robot in my house must be"
- "maybe we have to be thinking in terms of levels of protection"
- "your point of per device key seems critical, that way we can quarantine a stolen device and not impact the existing network"
- The security document "must address multiple comms ... wifi, thread, cellular, meshtastic".

## 2. Terms

### Who and what

| Term | Meaning |
|---|---|
| node | A small device running the VM: the outdoor thermometer, the robot, the ship's-bell clock. |
| gateway (porta) | The Go program that nodes check in with. It stores what nodes report and hands them instructions. |
| operator | The person running the gateway (David). |
| transport | The radio or network a node uses to reach its gateway. |
| mesh | A network where nodes relay for each other, so far-away nodes reach the gateway through near ones. |
| Thread | A low-power mesh radio standard (802.15.4) used by our boards. |
| border router | The box that joins the Thread mesh to the ordinary home network. |
| Wi-Fi | The home wireless network. |
| cellular (LTE-M / NB-IoT) | Mobile-phone-network data for small devices. The gateway's port is then on the public internet. |
| Meshtastic (LoRa) | A long-range, very low-rate radio mesh. Packets are tiny and minutes apart. |

### Ideas

| Term | Meaning |
|---|---|
| authentication | Proving who sent a message. |
| authorization | What a proven sender is then allowed to do. |
| identity vs credential | A node id is a name anyone can say. A credential is something only the real node can show. |
| secret key | A string of bits only its holder knows. |
| public key and private key | A matched pair. The private half signs; the public half only checks, so it can be handed out safely. |
| signature | A value made with a private key over some data. Anyone with the public key can check it; nobody without the private key can make one. |
| keyed checksum | A short tag computed from the message and a shared secret, so only a holder of the secret could have produced it. Technical name: message authentication code (HMAC is one kind). |
| encryption | Hiding the content. Different from authentication: a message can be proven genuine and still readable by everyone, or hidden and still forged. |
| DTLS (Datagram Transport Layer Security) | An encrypted, authenticated session over datagrams. It needs a handshake of several round trips and memory for the session. |
| pre-shared key | A secret both ends were given in advance. |
| the wall (perimeter) | Protection that comes from the transport itself, such as needing the mesh's network key to join. |
| network key | The shared secret that is the wall on one transport. |
| impersonation | Posing as another node, or as the gateway. |
| eavesdropping | Listening to messages not meant for you. |
| replay | Re-sending a recorded genuine message. |
| commissioning / enrolment | Adding a node to a network on purpose. |
| enrolment list | The gateway's list of node ids it knows, with what each may do. |
| revocation / quarantine | Striking one node from the list, or invalidating its key, so it is no longer trusted. |
| read-out protection | A chip setting that stops the stored program and keys being copied out through the debug port. |
| level | A class of node, defined in §5. |
| capability | rete's inner boundary: inside a node, a job can only reach the paths it was handed ([programming_with_nsl.md](../../nodus/nsl/docs/programming_with_nsl.md) §4, "Capabilities only narrow"). This document is about the outer boundary. |

## 3. What is true today

| # | Fact | Source |
|---|---|---|
| 1 | Nothing on the wire is authenticated. porta decodes any well-formed datagram from any sender and creates a node row on first mention. | `ensureNode`, [store.go](../../porta/internal/space/store.go) line 185; rete#291 |
| 2 | A node id is a name, not a credential. | [identity design](2026-09-11-node-identity-hardware-rooted-design.md), non-goals (line 86) |
| 3 | So porta's only protection is the transport's own wall. | [federation design](2026-09-14-porta-federation-and-transports-design.md) §4 |
| 4 | On Thread the wall is one network key, the same on every node, baked into each image from a gitignored overlay. A stolen node of any kind yields it. Rotating it means reflashing every node and the border router. | rete#288 (last comment), rete#289 |
| 5 | No read-out protection setting was found in the board configs (a search of `.conf`, `.overlay` and `Kconfig*` files under `nodus/oevm/zephyr` for `READOUT`, `FLASH_ENCRYPT`, `SECURE_BOOT`, `APPROTECT` found nothing). "None found", not "none exists". | search re-run 2026-10-02 |
| 6 | A node always starts the exchange; nothing can call a node. | federation design §4, "True everywhere" |
| 7 | A node sends to one configured gateway address and does not check where a reply came from. In [kernel.nsl](../../nodus/nsl/lib/kernel.nsl), `senderHost` appears only in the `Datagram` class (lines 1531–1543); no comparison on it was found in `PortaBridge` (class starts line 1621). | `grep senderHost` |
| 8 | App images are signed. porta signs the image's SHA-256 with its ed25519 key at `porta blob put`; `porta key gen` makes `porta.key` and `porta.pub`. The signature rides in the `App` record (`sig`). The one place that checks it is the runner, the host process that launches the VM, not the VM; the node's C staging code checks only the content hash. The node-side bridge refuses an `App` record with no `sig`. | [RECORDS](../../protocol/PROTOCOL-NSL-RECORDS.md) §4.1; [CLI.md](../../porta/docs/CLI.md) "key gen"; [ota.c](../../nodus/oevm/src/ota.c) lines 268–270 (`ota_stage`), 347 (whole-image verify), 364 (`ota_activate`) |
| 9 | On the Zephyr boards the over-the-air backend is inert (rete#233), so today no board accepts new code over the air. | rete#233 |
| 10 | What a gateway can tell a node today: install an app (signed), set the sleep mode, set its name, set a forwarding policy, set config values, attach a debugger or profiler, reboot it, send debug request lines. There is no "drive this actuator now" record. | RECORDS §4.1, §4.5 |

## 4. Who we are protecting against

| Scenario | What they have | What they can do today | In scope? |
|---|---|---|---|
| A stranger outside the wall (drive-by) | No key | Thread, Wi-Fi, Meshtastic: nothing. Cellular: reaches the gateway's port directly. | Yes |
| Someone who steals a dumb node | The wall key (§3 item 4) | Inside the wall: pose as any node to the gateway, pose as the gateway to any node, listen to everything. | Yes |
| Someone who steals a keyed node (none exist today) | The wall key and that node's own key | Everything above, plus act as that one node with proof. | Yes |
| A fake gateway inside the wall | The wall key | Instruct nodes; a node does not check who answered (§3 item 7). | Yes |
| A fake node inside the wall | The wall key | Report as any id; the gateway creates it. | Yes |
| A listener inside the wall | The wall key | Read all traffic. | Yes, see §11 |
| Radio jamming and flooding (denial of service) | Radio | Stop the network | No |
| An attacker who controls the gateway machine | The machine | Everything | No |
| A laboratory attacker extracting keys from a chip that has read-out protection | Lab equipment | Keys | No |
| The developer workstation that holds the keys | n/a | n/a | No |

## 5. The levels — PROPOSED — not decided

Rule in one sentence: **a node's level is set by what it can be told to do, not by what it measures.**

| Level | Example | What it holds | What it can prove | What it checks | If stolen |
|---|---|---|---|---|---|
| Dumb node | Outdoor thermometer; the ship's bell (fixed function: it rings the time; no record tells it to strike) | The wall key, its hardware id, its gateways' addresses, and the gateway's public key (not a secret) | Nothing about itself | A signature on the few things that matter, new code first of all | The thief learns nothing that is the node's own; the wall key is the loss (§6, rete#307) |
| Keyed node | The robot | All of the above plus a secret key of its own | Every message is its own | Every reply is its own gateway's | Revoke one row at the gateway; nothing else on the network changes |
| Gateway | porta | Its own private key, the enrolment list, and whatever it needs to check each keyed node (which depends on §7) | Every reply is its own, to any node that can check | Every keyed node's proof, and every other gateway's | Replace its key, and give every node the new public half (an open question in §8) |

A gateway is always keyed, toward keyed nodes and toward other gateways. A downstream gateway is a keyed node to its upstream (rete#292).

Three consequences:

**(a)** The gateway holds a rule per node id saying which level it is. Without it an unauthenticated sender simply claims the robot's id.

**(b)** Honest limit: a dumb node cannot tell a fake gateway from the real one for anything that is not signed. So what an unsigned message may make a dumb node do must be harmless by design. From §3 item 10:

| Instruction | If forged |
|---|---|
| Set its name | Harmless |
| Set sleep mode, set config values, set forwarding policy, attach debugger or profiler, send debug lines, reboot | Nuisance (not assessed per item; each needs review) |
| Install an app | Must be signed. Already carries a signature. |

**(c)** Anything that can be told to move, switch or unlock something is a keyed node. The bell qualifies as dumb only while no record can command its striker.

## 6. The outer wall, per transport (source: federation design §3 table and §4)

| Transport | The wall | Shared by | If one node is stolen | Can the wall be rebuilt without touching every node? | Per-message room |
|---|---|---|---|---|---|
| Thread | Network key | Every node on the mesh | Wall breached | Unknown. Thread defines a way to change the key across a live mesh; unverified on our boards (rete#307). | A 512-byte batch is 5–6 radio fragments |
| Wi-Fi / Ethernet | The LAN password (WPA2) | Every device on the LAN | Wall breached | Change the password and touch every device | No practical bound |
| Meshtastic (LoRa) | Channel key | Every radio on the channel | Wall breached | Touch every radio | About 233 bytes per packet, no fragmentation; a 32-byte tag would be about a seventh of it |
| Cellular | None; the gateway's port is on a public network | | | | No practical bound; cost per byte. The nRF91 modem runs DTLS in modem firmware. |

On a transport with no wall, a dumb node is exposed to anyone who can reach the gateway's port. This bears on decision 6.

## 7. One rule above the wall — PROPOSED — not decided

**The wall differs per transport; the levels above it are the same on every transport.**

The open choice rete#291 names, with no winner declared:

| | Keyed checksum on each datagram | DTLS session |
|---|---|---|
| Handshake needed | No | Yes, several round trips |
| Memory on the node | Small, unmeasured | The open measurement of PROTOCOL-NSL §7 question 7 |
| Hides content | No | Yes |
| Fits a sleepy node that sends one datagram and sleeps | Yes | Session must be resumed or redone |
| Fits Meshtastic's minutes-apart packets | Yes | Poorly |
| Already in a modem | No | Yes, on nRF91 |
| A second mechanism if CoAP+DTLS arrives anyway | Yes (rete#291 says so) | No |

Mechanism minimization says pick one for every transport (rete#291 question 1). The measurements in §9 decide.

## 8. Commissioning and theft — PROPOSED — not decided

| Step | Dumb node | Keyed node |
|---|---|---|
| Enrol | Operator adds id, name, level and what it may do to the gateway's list | Same |
| The node is given | The wall key; primary and secondary gateway address; the gateway's public key | All of those, plus its own key |
| Unknown id arrives | Today: it becomes a node. Proposed: refused and shown to the operator as an unknown sender. | Same |
| Stolen | Strike it from the list; consider the wall breached (rete#307) | Revoke its key; one row |

David: "knowing which ids are named what, their capabilities on my network, seems like a fair assumption to initialize/commission and operate a private network".

A gateway address is where to send, not proof of who answered. The public key is the proof.

Open questions, not answered here:

- How a node's own key gets onto it and where it is stored (rete#289; the node's NVS, its small non-volatile settings store).
- Primary and secondary gateway: one shared key pair, or two public keys on each node.
- What happens if the gateway's private key is lost or stolen, since every node holds its public half.

## 9. What it costs a weak board — measurements to make

No measurement exists for any mechanism here.

| Measurement | Board | What it decides |
|---|---|---|
| Signature check (ed25519): time and code size | nRF52840, ESP32-C6 | Whether a dumb node can check code signatures itself (today only the host runner does) |
| Keyed checksum per datagram: time, code size, tag bytes against the size caps ([RECORDS](../../protocol/PROTOCOL-NSL-RECORDS.md) §2.2) | Same | Whether the keyed checksum fits every transport |
| DTLS memory | The most constrained board (PROTOCOL-NSL §7 q7) | Whether DTLS is possible there at all |
| Read-out protection: available? side effects on the debug workflow? | Each board family | Decision 7 |
| Thread key change across a live mesh | The bench mesh (rete#307) | Whether the wall can be rebuilt without reflashing |

## 10. What rides the wire — the part the frozen surface needs

Per the [frozen surface draft](2026-10-02-frozen-1.0-surface.md) §0a row 4, authentication is inside surface 1.0, per node.

What changes the bytes on the wire:

- An optional proof on each check-in (absent for a dumb node), and a proof on the reply that a keyed node can check.
- The code signature (exists: `App.sig`).
- Possibly a signature on other instructions that matter.

What changes the gateway's behaviour as a node sees it:

- A per-node-id level rule: a check-in without proof, for an id that requires one, is refused.
- An unknown id is refused, not created.

These belong inside surface 1.0. Everything else in this document is gateway policy or node storage and can change later without a wire change.

## 11. What this does not cover

- Secrecy. Nothing here hides content; a keyed checksum proves the sender and hides nothing. Whether the robot's traffic must be hidden is decision 5.
- Replay. It needs a counter; the envelope already carries `bid` and sequence numbers that could serve. Unverified; a design question.
- Denial of service.
- A compromised gateway host.
- Key extraction from a stolen keyed node before it is revoked.
- The security of the developer workstation.
- Regulatory compliance.
- No security specialist has reviewed this document.

## 12. Decisions asked of David

| # | Question | Context | Recommended |
|---|---|---|---|
| 1 | Adopt levels (dumb / keyed / gateway) as the model? | §5 | Yes |
| 2 | The rule "level is set by what a node can be told to do; anything that can be commanded to act on the world is keyed"? | §5 (c) | Yes |
| 3 | Unknown ids: refused by default and shown to the operator for enrolment (today: auto-created)? | §8; rete#305 is the related id-validation gap | Yes, with a bench switch to keep today's behaviour for development |
| 4 | One per-message mechanism for every transport, chosen by the §9 measurements? | §7 | Yes. Lean toward the keyed checksum (no handshake; suits sleepy and LoRa nodes), but do not decide before measuring. |
| 5 | Does any node's traffic need to be hidden, not just proven (the robot's)? | §11 | David's call. If yes, it pushes keyed nodes toward an encrypted session. |
| 6 | On a transport with no wall (cellular), are dumb nodes allowed? | §6 | No, keyed only |
| 7 | Turn on read-out protection for keyed nodes, after measuring what it does to the debug workflow? | §3 item 5, §9 | Yes |
| 8 | Must there be a recovery plan for a lost or stolen gateway key before the first node leaves the house? | §8 | Yes |
| 9 | Where should the code-signature check run on a board, decided together with rete#233 (over-the-air on Zephyr)? | §3 items 8–9 | Yes, one piece of work |

## 13. Sources

| Document | Used for |
|---|---|
| [reliability-gaps.md](../../review/reliability-gaps.md) (G5, line 187) | §1 |
| [PROTOCOL-NSL.md](../../protocol/PROTOCOL-NSL.md) §4.3, §7 q5 and q7 | §1, §7, §9 |
| [PROTOCOL-NSL-RECORDS.md](../../protocol/PROTOCOL-NSL-RECORDS.md) §2.2, §4.1, §4.5 | §3, §9 |
| [Federation and transports design](2026-09-14-porta-federation-and-transports-design.md) §3, §4 | §3, §6 |
| [Node identity design](2026-09-11-node-identity-hardware-rooted-design.md) | §3 |
| [Frozen 1.0 surface draft](2026-10-02-frozen-1.0-surface.md) §0a row 4 | §1, §10 |
| [programming_with_nsl.md](../../nodus/nsl/docs/programming_with_nsl.md) §4 | §2 |
| [CLI.md](../../porta/docs/CLI.md) "key gen" | §3 |
| [store.go](../../porta/internal/space/store.go), [kernel.nsl](../../nodus/nsl/lib/kernel.nsl), [ota.c](../../nodus/oevm/src/ota.c) | §3 |

Issues: rete#25 (code-push and signing), rete#233 (Zephyr OTA inert), rete#288 (Thread credentials), rete#289 (onboarding), rete#291 (no auth on the wire), rete#292 (federation), rete#305 (node id validation), rete#307 (rolling the Thread key), rete#253 (frozen surface).
