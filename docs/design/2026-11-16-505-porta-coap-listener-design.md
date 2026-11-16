# porta's CoAP listener — the first transport a Toit node speaks

**Status:** design, decided with David 2026-11-16 in conversation; tracked as rete#505. The node side is
davidg238/ztoit#10 (`~/workspace/ztoit/docs/design/2026-11-15-nodus-toit-design.md`, §11 step 3).
Follow-ups split out of #505: rete#506 (the apps' telemetry endpoint), rete#507 (nsl nodes move to CoAP),
rete#508 (`sys/trace` and `goal/mode` in the manifest).

## 1. Intent

A ztoit node's C agent is a porta client. It checks in and fetches images over CoAP, the direction
`protocol/PROTOCOL-NSL.md` §5.3 ratified on 2026-07-15 and never built. porta has no CoAP listener, so the
agent's first network step, and everything after it up to the dev tool faber, waits on this.

## 2. Goals, non-goals, anti-goals

- **Goals.** A CoAP listener in `porta serve` that answers a check-in (pull and push) and serves image blobs
  blockwise, over IPv6 and IPv4, proven against a CoAP client that shares no code with it. The protocol
  documents agree with what is built.
- **Non-goals.**
  - DTLS. Not initially (David 2026-11-16): the same security as jag has today. §7 says exactly what that is.
  - nsl nodes on CoAP (rete#507). They keep the UDP exchange and the binary block plane, untouched.
  - The apps' own telemetry endpoint (rete#506).
  - Any `MANIFEST.cddl` change (rete#508, and §6 for why).
  - Observe, proxies, caching, separate responses.
  - Deploying to the production porta on gw. That is a rebuild David starts.
- **Anti-goals.**
  - A second check-in protocol. The CoAP listener carries the same records to the same handler; if a rule
    about cursors, boot ids or refusals has to be written twice, the design has failed.
  - A change to any nsl gate's result. The nsl suite and `porta_sync_host.sh` must read as they did before.

## 3. How porta works today (what this touches)

- **Check-in** is `internal/wire`: one CBOR datagram in, one out, on UDP `:6969`. A request is a `pull` record
  (`id`, `cursor`, `bid`, `hw`, `wak`) or a `push` record (`id`, `bid`, `ent`); a reply is a batch, an ack or an
  `#err` record. `wire.Server.HandleFrom(req []byte, peer string) []byte` holds all of it and knows nothing
  about UDP. A batch is capped at `space.MaxBatchBytes`.
- **Images** are `internal/blockserv`: a fixed binary frame on the exchange port + 1, blocks of
  `blockserv.BlockSize`, stateless, so a client resumes by asking for any block number. Blobs live in
  `internal/blob` by SHA-256; `porta blob put` signs the hash (ed25519) and, with `--node`, writes `goal/apps/main`.
- **porta never looks inside an image and has no node-kind column.** #505's "a Toit image kind beside `nsl`"
  therefore needs no code: the `app` cell in `MANIFEST.cddl` already carries `image`, `sig`, `lifecycle`,
  `triggers`, `runlevel` and `arguments`. What is wrong is `PROTOCOL-NSL.md` §0, which still describes v1 Toit
  nodes that were deleted.
- The node-facing UDP listener has no admission rule; only the operator HTTP listener has a CIDR allowlist.

## 4. Approaches considered

| Question | Options | Picked (David, 2026-11-16) |
|---|---|---|
| Do nsl nodes move now? | **A: CoAP beside UDP**; B: CoAP replaces UDP | **A.** B puts the nsl kernel bridge, the C fetch seam and both BLE relays in the path of ztoit's first step. Two transports exist until rete#507, whose job is deleting the UDP planes |
| How a check-in maps onto CoAP | **Envelope:** `POST /space`, body is today's record; Resources: `GET /space?id=&cursor=…`, refusals as 4.xx | **Envelope.** A pull reports five fields and porta records them, so it is a POST either way; the resource form only moves the fields into a query string and adds a second request decoder. One path per URI (the truly resource-shaped design) costs a round trip per path, which the batch-by-cursor exchange exists to avoid |
| Library or hand-written | hand-written subset (~250 lines, estimated); **`github.com/plgd-dev/go-coap/v3`** | **The library.** DTLS is coming, and starting on the library gives it running time before then. Cost, measured at v3.8.1: 10 modules, 75 packages (pion's DTLS among them) in a binary with three direct dependencies |

The envelope changes one line of the ztoit record: ztoit#10 and its design say `GET /space?id=&cursor=`. That
becomes `POST /space`. *(Claude's call: note it on ztoit#10; the ztoit design is edited by a ztoit session.)*

## 5. The design

### 5.1 The surface

One UDP port for both resources. *(Claude's call: 5683, CoAP's registered port, is the one to deploy on.)*

| Request | Body | Reply |
|---|---|---|
| `POST /space` | exactly the bytes a UDP check-in datagram carries today: a `pull` or a `push` record | 2.04 Changed, Content-Format `application/cbor`, body exactly the bytes `HandleFrom` returns: a batch, an ack, or an `#err` record |
| `GET /blob/<hash>` | none. `<hash>` is 64 lowercase hex characters, the SHA-256 in the `blobref` | 2.05 Content, `application/octet-stream`, the blob by Block2 (RFC 7959). Unknown hash: 4.04. Malformed hash: 4.00 |

- **A refusal of a check-in stays an `#err` record in a 2.04 reply.** *(Claude's call.)* The node counts
  refusals in one place whatever the transport, and porta keeps one refusal path. CoAP error codes are used
  only for what the record layer never sees: an unknown path (4.04), a wrong method (4.05).
- **Only `/blob` uses blockwise, and `coapsrv` does it itself.** *(Claude's call, after review.)* The
  library's blockwise layer is one setting for the whole server (`BlockwiseSZX`, default 1024) and splits any
  reply at least one block long, so capping it at `blockserv.BlockSize` would split a `/space` batch, which may
  reach `space.MaxBatchBytes`. So the library's blockwise is switched off. A check-in is then one datagram
  each way, always. The `/blob` handler reads the request's Block2 option and answers that one block with the
  option set: offset = block number × block size, `more` while bytes remain. Nothing is kept per transfer.
- **Block size.** `blockserv.BlockSize`, or a smaller size the client asks for. A first request with no Block2
  option gets block 0 at `blockserv.BlockSize`.
- **Resume is the client asking for any block number, with no earlier request.** This is the property the
  binary plane has and ztoit's power-cut test (its design §9, test 3) depends on. With the handler stateless
  it holds by construction, and it is still an acceptance test (§8).
- **The peer address** goes to `HandleFrom` as it does for UDP, so the log lines read the same.

### 5.2 The code

- A new package, `porta/internal/coapsrv`, is the whole adapter: it takes a `*wire.Server` and a `*blob.Store`
  and owns the library. `internal/blockserv` and `internal/space` do not change.
- **`internal/wire` gains one mutex.** *(Claude's call, after review.)* `wire.Server` is documented as not
  concurrency-safe because the UDP loop is its only caller. The library calls from its own goroutines while
  the UDP loop runs, and two overlapping check-ins can collide in the store's read-then-write
  (`Store.NoteCheckIn`), which would reach a node as a refusal. `HandleFrom` takes a lock for its whole body,
  so check-ins are handled one at a time whichever listener they came in on. `/blob` needs no lock.
- **The transport is not recorded per check-in.** *(Claude's call, after review.)* The peer address is stored
  as it is for UDP; `coapsrv` logs its own lines as CoAP. A column for it is earned when someone has to ask
  which transport a node used and `obs/build` cannot answer.
- `porta serve` gains `--coap <addr>`, listening on IPv6 and IPv4. **The default is empty: no CoAP listener.**
  *(Claude's call, after the plan.)* Every harness starts its own `porta serve` (the nsl suite,
  `porta_sync_host.sh`, the tutorials), and a listener that is on by default would have them all bind 5683 and
  collide with each other and with a running porta, which breaks the anti-goal in §2. A deployment that serves
  Toit nodes passes `--coap :5683`. A port already in use is a start-up error.
- `--test-refuse-all` applies to CoAP for free, because it lives in `HandleFrom`.
- `--test-drop-all` and `--test-drop-ack` are **not** offered on CoAP. They work by sending nothing, and the
  library answers a confirmable request the handler leaves unanswered with an empty ACK
  (`udp/client/conn.go`, `processResponse`), so true silence has to be made below the library. Earned when a
  ztoit test needs porta to go silent; until then `serve` says at start-up that the two switches cover UDP
  only. *(Claude's call.)*

### 5.3 The documents (part of the work, not after it)

| Document | Change |
|---|---|
| `protocol/PROTOCOL-NSL.md` §0 | rewritten: one protocol; porta does not distinguish node kinds; a Toit-on-Zephyr node is a second implementation of the node end |
| `protocol/PROTOCOL-NSL.md` §5.3 | CoAP is built for check-in and images as in §5.1 here, beside UDP until rete#507. The DTLS measurement no longer gates CoAP |
| `protocol/PROTOCOL-NSL.md` §7 q7 | reworded: the DTLS RAM measurement gates DTLS only |
| `protocol/PROTOCOL-NSL.md` §8 | a dated entry: CoAP without DTLS, David 2026-11-16; envelope; library |
| `porta/docs/ARCHITECTURE.md` | the listener in the picture and the listener table; the port; the new dependency |

## 6. Deferred, with what would earn each back

- **`sys/trace` and `goal/mode`'s `reportS` (rete#508).** *(Claude's call, changing what was said in
  conversation, where `sys/trace` was in this work.)* Two reasons. No agent produces a trace yet, so its shape
  would be a guess. And any `MANIFEST.cddl` edit changes the manifest pin that every nsl image carries and
  `protocol/tools/check_provision.py` compares, which breaks the anti-goal in §2. Earned when ztoit reaches
  its design §11 step 5.
- **Silence switches on CoAP** (§5.2).
- **DTLS.** Earned by the node-side RAM measurement on the nRF52840 (`PROTOCOL-NSL.md` §7 q7).
- **Admission by address.** The UDP listener has none; CoAP matches it.

## 7. Security, stated plainly

With no DTLS, anyone who can send a UDP datagram to porta's CoAP port can read any node's goals, write
observations under any node id, and download any blob whose hash they know. Nothing is encrypted. What is
protected: a node installs only an image whose hash porta signed. This is the UDP listener's position today,
and jag's.

## 8. Proof

1. **Go tests in `coapsrv`**, with the library's own client: a pull returns the same bytes `HandleFrom`
   returns for the same record; a push is ingested and acked; a blob fetched whole has the stored SHA-256;
   unknown and malformed hashes are refused; IPv4 and IPv6 both answer.
2. **Cold block:** a single `GET /blob/<hash>` asking for block N, with no earlier request on that socket,
   returns block N.
3. **Interop gate**, `porta/tests/coap_interop.sh`: libcoap's `coap-client` (package `libcoap3-bin`, not
   installed on msi; needs David's `sudo apt install`) does a check-in from a fixture record and fetches a
   blob, against a `porta serve` on a temporary store. A missing `coap-client` fails the gate; it does not
   skip. The Go tests share the library with the server, so this is the check that does not share a cause.
4. **The guard is shown to fail:** break the block offset by one in the adapter and record which of 1–3 go
   red.
5. **Check-ins do not collide:** a test sends overlapping check-ins over UDP and CoAP for the same node and
   none is refused; it is run under `go test -race`.
6. **Nothing nsl moved:** `go test ./...` in `protocol`, `porta` and `custos`; `porta_sync_host.sh` prints
   `PORTA SYNC OK`.

The real peer, Zephyr's `coap_client`, is ztoit's step 3 and is proven there.

## 9. Left to the builder

- The library's API: read its documentation and examples at the pinned version; do not write from memory.
- Whether `coapsrv` reads the blob whole per request or by range.
- The library's message-size and inactivity options.
- The fixture bytes for the interop gate (an existing `protocol/fixtures` record where one fits).

## 10. Close-out

Promotion: `protocol/PROTOCOL-NSL.md` (§5.3 table above) and `porta/docs/ARCHITECTURE.md` absorb this. A
comment on ztoit#10 records the `POST /space` change. #505 closes when §8 is demonstrated; rete#506–#508
stay open.
