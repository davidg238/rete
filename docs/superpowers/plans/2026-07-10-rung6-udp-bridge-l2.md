# Rung 6 — Layer 2: reactive channel↔UDP bridge (CBOR) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the node network-transparent — a `Record` posted to a channel travels out as CBOR/UDP, and an inbound datagram becomes a decoded `Record` firing a `whenever:` reaction — proven bidirectionally on host and native_sim.

**Architecture:** A pure-nsl `NetBridge` capability (in `kernel.ns`) reuses the Layer-1 `Socket` send/receive prims and the existing `Cbor` codec. Outbound is a normal drain-time reaction (`whenever:` → encode → `sendTo:`); inbound is an explicit `pumpInbound` step (receive-until-nil → decode → post) that follows the reactor's pump-then-drain idiom. The datagram payload moves from `String` to immutable `Bytes` (the L1-deferred seam) via a module-internal `Bytes>>rawLeaf`.

**Tech Stack:** Newspeak dialect (`nsl-tuvm/lib/kernel.ns`), the tuvm C VM (`tuvm/src/udp.c`), the `run_tests_oevm.py` both-path conformance harness, Zephyr `native_sim` + west/NCS SDK (`ns_compile_oevm.py` Model-A solidifier).

## Global Constraints

- **Three repos, separate commits.** `tuvm` (C + native_sim node + tuvm docs), `nsl-tuvm` (`lib/kernel.ns` + nsl-tuvm docs), `nsl-tests` (`.ns` test files). Never cross-commit; `cd` into the right repo before `git add`/`commit`.
- **Clean break to Bytes-only payload.** After Task 1, `Socket>>sendTo:port:data:` takes an immutable `Bytes` and `Datagram>>data` answers a `Bytes`. No dual String-or-Bytes accept.
- **Malformed datagram = drop, never crash.** Inbound decode uses `Cbor decode:ifInvalid:` and drops on failure. A garbled remote datagram must never crash the node (untrusted-input boundary — errors.md: expected condition, not a local bug).
- **`pumpInbound` stays an explicit call this rung** (rule-of-three). Do NOT fold net into a unified `Kernel>>pumpEvents` or build a pump-participant registry — deferred until a second data source lands.
- **Controller owns every full-suite run.** Implementers run single-file / host checks foreground only; the controller runs `OEVM_PATH=AB` and the five named gates.
- **`solidified_image.c` is a generated artifact** shared by other tests. Baking a native_sim node CLOBBERS `tuvm/src/solidified_image.c` + `.stmap`; ALWAYS restore before committing anything in tuvm:
  ```bash
  git -C /home/david/workspace/tuvm checkout -- src/solidified_image.c src/solidified_image.stmap
  ```
- **Zephyr build needs the SDK env** sourced each shell: `source /home/david/workspace/ns-zephyr/tools/env.sh`. native_sim exe path is nested: `build/zephyr/zephyr.exe`.
- **Footprint moves this rung** (C changes — `udp.c`). Re-measure and **re-bank if it moved — no silent ratchet**. Baseline: `tuvm/tools/footprint-mcu-baseline.txt` (text 29182 / data_bss 69036).
- **Commit-message trailer** on every commit:
  ```
  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
  ```

---

## File Structure

- `tuvm/src/udp.c` — MODIFY. `basicReceive:` data slot → `RawBytes` leaf (was `String`); host slot stays `String`. `basicSend:` unchanged (a `RawBytes` leaf is struct-String-layout); comment updated.
- `nsl-tuvm/lib/kernel.ns` — MODIFY. Add `Bytes>>rawLeaf` (module-internal); `Socket>>sendTo:port:data:` passes `aBytes rawLeaf`; `Socket>>receive` wraps data in `Bytes overRaw:`; add `class NetBridge`; add `public NetBridge` accessor.
- `nsl-tests/04_somns_basic/42_udp.ns` — MODIFY. Migrate payload `'ping'` (String) → a 4-byte `Bytes`.
- `nsl-tests/04_somns_basic/45_udp_bridge.ns` — CREATE. Host bidirectional bridge round-trip + malformed-datagram-survives.
- `tuvm/zephyr/NetBridgeNode.ns` — CREATE. native_sim baked node doing the bidirectional bridge round-trip, prints `BRIDGE OK`.
- `tuvm/zephyr/NetNode.ns` — MODIFY (Task 3). Migrate its L1 payload `'ping'` → `Bytes` so the L1 native_sim gate still builds under the new seam.
- `tuvm/zephyr/README.md`, `tuvm/review/program-sequence.md`, dialect doc — MODIFY (Task 4). Close-out docs.

---

## Task 1: Bytes payload seam

Move the UDP datagram payload from `String` to immutable `Bytes` on both directions, in isolation from CBOR. The L1 host net tests must stay both-path green with the new type.

**Files:**
- Modify: `tuvm/src/udp.c` (add `udp_make_bytes`; `basicReceive:` data slot; `basicSend:` comment)
- Modify: `nsl-tuvm/lib/kernel.ns` (`Bytes>>rawLeaf`, `Socket>>sendTo:port:data:`, `Socket>>receive`)
- Modify: `nsl-tests/04_somns_basic/42_udp.ns` (payload → `Bytes`)

**Interfaces:**
- Consumes: L1 `Socket`/`Datagram` (`kernel.ns:980-1000`), `Bytes`/`ByteArray` (`kernel.ns:428-499`), `RawBytesVtable` (extern, set in `solidified_image.c` for Model A and `kernel_boot.c` for host).
- Produces:
  - `Bytes>>rawLeaf` — module-internal (non-public); answers the underlying `RawBytes` leaf (struct-String layout). Only `Socket` calls it.
  - `Socket>>sendTo: host port: aPort data: aBytes` — `aBytes` is an immutable `Bytes`.
  - `Socket>>receive` — answers a `Datagram` whose `data` is an immutable `Bytes` (or nil).
  - C `basicReceive:` — slot 1 (data) is a `RawBytes` leaf; slots 2/3 (host String, port SMI) unchanged.

- [ ] **Step 1: Migrate `42_udp.ns` to a `Bytes` payload (the failing test)**

Rewrite `nsl-tests/04_somns_basic/42_udp.ns` to build a 4-byte `Bytes` (`'ping'` = 112,105,110,103), send it, and compare the received `Bytes` value-equal:

```
(*
UDP — confined native capability, datagram socket over a real dual-stack POSIX
backend. R4. Entry declares the second capability `net:`. Payload is an immutable
Bytes (Rung 6 Layer 2 seam: a datagram is opaque bytes, not text).
expect:
UDP OK
*)
class UdpTest usingPlatform: platform net: net = () (
  public main: platform2 args: args = (
    (roundTrip: '127.0.0.1') ifFalse: [ ^ self fail: 'v4 loopback' ].
    (roundTrip: '::1')       ifFalse: [ ^ self fail: 'v6 loopback' ].
    'UDP OK' println.
    ^ 0
  )
  ping = (
    | b |
    b:: (platform ByteArray) new: 4.
    b u8At: 1 put: 112. b u8At: 2 put: 105. b u8At: 3 put: 110. b u8At: 4 put: 103.
    ^ b freeze
  )
  roundTrip: host = (
    | rx tx dg got |
    rx:: net udpBindPort: 54331.
    rx == nil ifTrue: [ ^ false ].
    tx:: net udp.
    tx sendTo: host port: 54331 data: ping.
    got:: nil.
    [ got == nil ] whileTrue: [ got:: rx receive ].   (* loopback is ~instant *)
    dg:: got.
    rx close. tx close.
    ^ dg data = ping
  )
  fail: msg = ( msg println. ^ 1 )
) : (
  public packageUsing: manifest = ( ^ self new )
)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd /home/david/workspace/nsl-tuvm && OEVM_PATH=A python3 tools/run_tests_oevm.py 42_udp`
Expected: FAIL — the send prim currently reads `args[3]` as bytes fine, but `Socket>>sendTo:` has not yet been changed to unwrap a `Bytes` (it forwards the `Bytes` *wrapper* to `basicSend:`, which mis-reads an `ObjectInstance` as `struct String`), and `dg data` is still a `String` from `basicReceive:` so `dg data = ping` (Bytes) is false. (Either symptom is an acceptable pre-implementation failure.)

- [ ] **Step 3: Add `udp_make_bytes` and switch the data slot in `tuvm/src/udp.c`**

Add the extern + ctor near the existing `udp_make_string` (after line 34):

```c
extern oop RawBytesVtable;   /* Milestone B RawBytes leaf; set by kernel_boot.c (host) / solidified_image.c (Model A) */

/* Datagram payloads are opaque bytes, not text (Rung 6 Layer 2). Same {length,
   inline bytes} layout as udp_make_string, distinct vtable so the leaf never
   gains String methods and reads back as an immutable Bytes in nsl. */
static oop udp_make_bytes(const uint8_t *bytes, size_t len) {
    oop s = gc_allocate(RawBytesVtable, sizeof(struct String) + len);
    ((struct String *)s)->length = len;
    if (len > 0) memcpy(((struct String *)s)->bytes, bytes, len);
    return s;
}
```

Change the data-slot construction in `basicReceive:` (line 79) from `udp_make_string` to `udp_make_bytes`:

```c
        oop dataS = udp_make_bytes(buf, (size_t)n);
```

(Leave line 81 `hostS = udp_make_string(...)` — the sender host IS text.) Update the `basicSend:` comment (line 56) for accuracy:

```c
    if (strcmp(sel, "basicSend:to:port:data:") == 0) { /* fd, hostStr, portSMI, dataBytesLeaf */
```

(`basicSend:` reads `data->bytes`/`data->length`; a `RawBytes` leaf has the identical layout, so no code change there.)

- [ ] **Step 4: Add `Bytes>>rawLeaf` and rewire `Socket` in `nsl-tuvm/lib/kernel.ns`**

In `class Bytes` (after `setRaw: r` at line 496), add the module-internal accessor:

```
    (* Module-internal (Rung 6 Layer 2): answer the underlying RawBytes leaf for
       the send prim to read. Non-public — same confinement as Channel>>rootPath;
       only the sibling Socket reaches it. Handing the leaf to basicSend: is a
       READ (memcpy out to the socket), so it does not breach immutability. *)
    rawLeaf = ( ^ raw )
```

In `class Socket` (lines 980-991), change `sendTo:` to unwrap and `receive` to wrap:

```
    public sendTo: host port: aPort data: aBytes = (
      port basicSend: handle to: host port: aPort data: aBytes rawLeaf. ^ self )
    public receive = ( | t |
      t:: port basicReceive: handle.
      t == nil ifTrue: [ ^ nil ].
      ^ Datagram data: (Bytes overRaw: (t basicAt: 1)) senderHost: (t basicAt: 2) senderPort: (t basicAt: 3) )
```

- [ ] **Step 5: Run the migrated test to verify it passes (both paths)**

Run: `cd /home/david/workspace/nsl-tuvm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 42_udp`
Expected: PASS on Path A and Path B — `UDP OK`, both v4 and v6 loopback, `dg data = ping` true (Bytes value-equal). Also confirm `43_udp_confined.ns` still passes (it never reaches the send, so the new type is irrelevant):
Run: `OEVM_PATH=AB python3 tools/run_tests_oevm.py 43_udp_confined`
Expected: PASS both paths (runtime DNU on `net` as before).

- [ ] **Step 6: Commit (three repos)**

```bash
cd /home/david/workspace/tuvm && git add src/udp.c && git commit -m "feat(rung6): UDP datagram payload = RawBytes leaf, not String

basicReceive: builds its data slot as a RawBytes leaf (distinct vtable, same
{length,bytes} layout) so a datagram reads back as an immutable Bytes in nsl —
a datagram is opaque bytes, not text. basicSend: is unchanged (reads the same
layout). Host slot stays a String (the sender host IS text).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"

cd /home/david/workspace/nsl-tuvm && git add lib/kernel.ns && git commit -m "feat(rung6): Socket payload = immutable Bytes; Bytes>>rawLeaf send seam

Socket>>sendTo:port:data: takes a Bytes and passes its raw leaf to basicSend:
via a module-internal Bytes>>rawLeaf (non-public, same confinement as
Channel>>rootPath — only the sibling Socket reaches it; the leaf is READ, not
mutated). Socket>>receive wraps the data slot as Bytes overRaw:, so
Datagram>>data answers an immutable Bytes. The L1-deferred immutability seam.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"

cd /home/david/workspace/nsl-tests && git add 04_somns_basic/42_udp.ns && git commit -m "test(rung6): 42_udp payload migrated String 'ping' -> 4-byte Bytes

The L1 net round-trip now carries an immutable Bytes payload (Rung 6 seam).
43_udp_confined unchanged — it DNUs on net before the send, so payload type is
irrelevant there.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: `NetBridge` + host bidirectional round-trip test

Add the `NetBridge` capability and prove a `Record` round-trips through real UDP loopback in both directions, with a malformed datagram dropped (node survives).

**Files:**
- Modify: `nsl-tuvm/lib/kernel.ns` (add `class NetBridge`; add `public NetBridge` accessor)
- Create: `nsl-tests/04_somns_basic/45_udp_bridge.ns`

**Interfaces:**
- Consumes: `Socket>>sendTo:port:data:` / `Socket>>receive` / `Datagram>>data` (Task 1), `Cbor new encode:` / `Cbor new decode:ifInvalid:` (`kernel.ns:521`, `:689`), `Channel>>whenever:do:` / `out:value:` (`kernel.ns:1050-1057`), `Space>>drain` (`kernel.ns:1098`), `platform space` / `platform Cbor` / `platform NetBridge` / `net udp` / `net udpBindPort:`.
- Produces:
  - `class NetBridge` with class-side `socket: s out: o peer: ph port: pp in: i` and instance `start` / `pumpInbound`.
  - `public NetBridge = ( ^ NetBridge )` accessor.

- [ ] **Step 1: Write the failing host test `nsl-tests/04_somns_basic/45_udp_bridge.ns`**

```
(*
Rung 6 Layer 2 — the reactive channel<->UDP bridge, bidirectional, over REAL
UDP loopback. Two NetBridges A<->B in one Space: a Record posted to a bridge's
`out` channel is CBOR-encoded and sent; the peer's pumpInbound receives it,
CBOR-decodes, and posts the decoded Record onto its `in` channel, where a
reaction observes it value-equal. Then the reverse direction. Finally a garbage
(non-CBOR) datagram is delivered: the bridge DROPS it (decode ifInvalid) and the
node survives — no crash, no spurious reaction.

R4 (declares net:). Payload is CBOR bytes (Rung 6).
expect:
a->b ok
b->a ok
survived garbage
BRIDGE OK
*)
class UdpBridgeTest usingPlatform: platform net: net = () (
  public main: platform2 args: args = (
    | space sockA sockB portA portB bridgeA bridgeB gotB gotA rec |
    space:: platform space.

    (* two receivers on fixed loopback ports; a sender socket each *)
    portA:: 54341. portB:: 54342.
    sockA:: net udpBindPort: portA.
    sockB:: net udpBindPort: portB.
    (sockA == nil or: [ sockB == nil ]) ifTrue: [ ^ self fail: 'bind' ].

    bridgeA:: platform NetBridge
      socket: sockA out: (space channel: 'a/out') peer: '127.0.0.1' port: portB in: (space channel: 'a/in').
    bridgeB:: platform NetBridge
      socket: sockB out: (space channel: 'b/out') peer: '127.0.0.1' port: portA in: (space channel: 'b/in').
    bridgeA start. bridgeB start.

    gotB:: nil. gotA:: nil.
    (space channel: 'b/in') whenever: '' do: [ :v | gotB:: v ].
    (space channel: 'a/in') whenever: '' do: [ :v | gotA:: v ].

    (* A -> B *)
    rec:: platform Record tag: #reading with: #seq -> 1 with: #milliC -> 235.
    (space channel: 'a/out') out: '' value: rec. space drain.
    [ gotB == nil ] whileTrue: [ bridgeB pumpInbound. space drain ].
    (gotB = rec) ifTrue: [ 'a->b ok' println ] ifFalse: [ ^ self fail: 'a->b mismatch' ].

    (* B -> A *)
    (space channel: 'b/out') out: '' value: rec. space drain.
    [ gotA == nil ] whileTrue: [ bridgeA pumpInbound. space drain ].
    (gotA = rec) ifTrue: [ 'b->a ok' println ] ifFalse: [ ^ self fail: 'b->a mismatch' ].

    (* garbage datagram -> B: a raw non-CBOR byte the bridge must drop *)
    gotB:: nil.
    self sendGarbageTo: portB.
    bridgeB pumpInbound. space drain.        (* one pump: decode fails -> dropped *)
    gotB == nil ifTrue: [ 'survived garbage' println ] ifFalse: [ ^ self fail: 'garbage leaked' ].

    'BRIDGE OK' println.
    ^ 0
  )

  (* 0xFF is a reserved/invalid CBOR initial byte -> Cbor decode fails -> drop *)
  sendGarbageTo: aPort = (
    | tx b |
    tx:: net udp.
    b:: (platform ByteArray) new: 1.
    b u8At: 1 put: 255.
    tx sendTo: '127.0.0.1' port: aPort data: b freeze.
    tx close.
    ^ self
  )
  fail: msg = ( msg println. ^ 1 )
) : (
  public packageUsing: manifest = ( ^ self new )
)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd /home/david/workspace/nsl-tuvm && OEVM_PATH=A python3 tools/run_tests_oevm.py 45_udp_bridge`
Expected: FAIL — `platform NetBridge` is `doesNotUnderstand` (class + accessor not yet added).

- [ ] **Step 3: Add `class NetBridge` to `nsl-tuvm/lib/kernel.ns`**

Insert after `class Datagram` (after line 1000, before `serialOn:`):

```
  (* Rung 6 Layer 2 — the reactive channel<->UDP bridge. Holds a bound Socket, an
     `out` Channel it subscribes to (posts there are CBOR-encoded and sent to the
     peer), and an `in` Channel it posts decoded inbound Records onto. Reuses the
     L1 Socket send/receive prims and the Cbor codec; no new C. Outbound is a
     normal drain-time reaction; inbound is an explicit pumpInbound step (the
     reactor's pump-then-drain idiom — pumpInbound stages, drain fires). Peer is a
     raw host String + port SMI. A malformed inbound datagram is DROPPED (untrusted
     network input, errors.md expected condition) — it must never crash the node. *)
  class NetBridge = ( | socket out peerHost peerPort in | ) (
    setSocket: s out: o peerHost: ph peerPort: pp in: i = (
      socket:: s. out:: o. peerHost:: ph. peerPort:: pp. in:: i. ^ self )
    public start = (
      out whenever: '' do: [ :v | socket sendTo: peerHost port: peerPort data: (Cbor new encode: v) ].
      ^ self )
    (* Receive-until-empty; decode each datagram; drop on decode failure (bad `set`
       true), post the decoded value onto `in` otherwise (a legitimately-nil value
       still posts — only a decode FAILURE drops). *)
    public pumpInbound = (
      | dg bad v |
      dg:: socket receive.
      [ dg == nil ] whileFalse: [
        bad:: false.
        v:: Cbor new decode: dg data ifInvalid: [ :msg | bad:: true. nil ].
        bad ifFalse: [ in out: '' value: v ].
        dg:: socket receive ].
      ^ self )
  ) : (
    public socket: s out: o peer: ph port: pp in: i = (
      ^ self new setSocket: s out: o peerHost: ph peerPort: pp in: i )
  )
```

- [ ] **Step 4: Add the `public NetBridge` accessor**

In the accessor block, after `public ByteArray` (line 1736):

```
  public NetBridge         = ( ^ NetBridge )
```

- [ ] **Step 5: Run the test to verify it passes (both paths)**

Run: `cd /home/david/workspace/nsl-tuvm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 45_udp_bridge`
Expected: PASS on Path A and Path B — prints `a->b ok` / `b->a ok` / `survived garbage` / `BRIDGE OK`.

- [ ] **Step 6: Commit (two repos)**

```bash
cd /home/david/workspace/nsl-tuvm && git add lib/kernel.ns && git commit -m "feat(rung6): NetBridge — reactive channel<->UDP bridge (CBOR)

Outbound: a drain-time reaction encodes each `out`-channel post to CBOR and
sendTo:s the peer. Inbound: pumpInbound receives-until-empty, CBOR-decodes, and
posts the decoded value onto `in` (drop on decode failure — untrusted input
never crashes the node). Reuses L1 Socket + the Cbor codec; no new C. pumpInbound
stays an explicit call (rule-of-three; no unified-pump registry this rung).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"

cd /home/david/workspace/nsl-tests && git add 04_somns_basic/45_udp_bridge.ns && git commit -m "test(rung6): bidirectional NetBridge round-trip over UDP loopback + garbage drop

Two bridges A<->B in one Space: a Record round-trips encode->UDP->decode->
reaction both directions (value-equal), and a non-CBOR garbage datagram is
dropped with the node surviving.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: native_sim bidirectional round-trip

Prove the same bridge round-trip on Zephyr `native_sim` (Model A), and keep the L1 `NetNode.ns` gate building under the new Bytes seam.

**Files:**
- Create: `tuvm/zephyr/NetBridgeNode.ns`
- Modify: `tuvm/zephyr/NetNode.ns` (migrate its L1 `'ping'` payload → `Bytes`)

**Interfaces:**
- Consumes: `platform space` / `platform NetBridge` / `platform Record` / `net udpBindPort:` / `net udp` (all baked into the Model-A image from `kernel.ns`), `ns_compile_oevm.py` net boot-threading (`:2389-2396`), the node-agnostic `zephyr/main.c`/`CMakeLists.txt`/`prj.conf` (unchanged).
- Produces: a console sentinel `BRIDGE OK` observable under `timeout` — the native_sim gate.

- [ ] **Step 1: Migrate `tuvm/zephyr/NetNode.ns` to a `Bytes` payload**

The L1 native_sim gate sends `'ping'` (String) and compares `got data = 'ping'`; both break under the Bytes seam. Rewrite its `roundTrip:` to build/compare a `Bytes` (mirror the `ping` helper from `42_udp.ns`):

```
  ping = (
    | b |
    b:: (platform ByteArray) new: 4.
    b u8At: 1 put: 112. b u8At: 2 put: 105. b u8At: 3 put: 110. b u8At: 4 put: 103.
    ^ b freeze
  )
  roundTrip: host = (
    | rx tx got |
    rx:: net udpBindPort: 54332.
    rx == nil ifTrue: [ ^ false ].
    tx:: net udp.
    tx sendTo: host port: 54332 data: ping.
    got:: nil.
    [ got == nil ] whileTrue: [ got:: rx receive ].
    rx close. tx close.
    ^ got data = ping
  )
```

(Add the `ping` method; the `usingPlatform: platform net: net` header already binds `platform`.)

- [ ] **Step 2: Create `tuvm/zephyr/NetBridgeNode.ns`**

```
(*
NetBridgeNode — native_sim bidirectional NetBridge round-trip gate. Model A
(baked into src/solidified_image.c). Declares net:, binds two loopback
receivers, wires two NetBridges A<->B in one Space, round-trips a CBOR Record
each direction over Zephyr native_sim NSOS loopback, and prints BRIDGE OK.
Mirrors nsl-tests 45_udp_bridge.ns; proves the Layer-2 bridge (Socket + Cbor +
Space drain) functionally on the target-shaped backend. main: returns after
printing; zephyr/src/main.c then idles (K_FOREVER) — the gate is console output
under `timeout`, not process exit.
expect:
BRIDGE OK
*)
class NetBridgeNode usingPlatform: platform net: net = () (
  public main: platform2 args: args = (
    | space sockA sockB portA portB bridgeA bridgeB gotB rec |
    space:: platform space.
    portA:: 54343. portB:: 54344.
    sockA:: net udpBindPort: portA.
    sockB:: net udpBindPort: portB.
    (sockA == nil or: [ sockB == nil ]) ifTrue: [ ^ self fail: 'bind' ].
    bridgeA:: platform NetBridge
      socket: sockA out: (space channel: 'a/out') peer: '127.0.0.1' port: portB in: (space channel: 'a/in').
    bridgeB:: platform NetBridge
      socket: sockB out: (space channel: 'b/out') peer: '127.0.0.1' port: portA in: (space channel: 'b/in').
    bridgeA start. bridgeB start.
    gotB:: nil.
    (space channel: 'b/in') whenever: '' do: [ :v | gotB:: v ].
    rec:: platform Record tag: #reading with: #seq -> 1 with: #milliC -> 235.
    (space channel: 'a/out') out: '' value: rec. space drain.
    [ gotB == nil ] whileTrue: [ bridgeB pumpInbound. space drain ].
    (gotB = rec) ifFalse: [ ^ self fail: 'a->b mismatch' ].
    'BRIDGE OK' println.
    ^ 0
  )
  fail: msg = ( msg println. ^ 1 )
) : (
  public packageUsing: manifest = ( ^ self new )
)
```

- [ ] **Step 3: Confirm both nodes pass on host Path A first (isolates node logic from the Zephyr backend)**

Copy each node into the nsl-tests tree transiently OR run via the solidifier + host VM per the project's Model-A single-file recipe. The lightweight check: bake and run on host Path A.

```bash
cd /home/david/workspace/nsl-tuvm
python3 tools/ns_compile_oevm.py /home/david/workspace/tuvm/zephyr/NetBridgeNode.ns /home/david/workspace/tuvm/src/solidified_image.c
cd /home/david/workspace/tuvm && cc -I src src/*.c src/backend/posix/*.c -o /tmp/rung6_host 2>/tmp/rung6_host_build.log && /tmp/rung6_host 2>&1 | tee /tmp/rung6_host.log
grep -q 'BRIDGE OK' /tmp/rung6_host.log && echo HOST_PASS || echo HOST_FAIL
git checkout -- src/solidified_image.c src/solidified_image.stmap
```

Expected: `HOST_PASS`. (If the compile line differs from this repo's Model-A host build, use the same host-build invocation `run_tests_oevm.py` uses for Path A — the point is to run the baked node against the posix backend before the Zephyr build. If it fails, the bug is in the node, not the Zephyr backend — fix `NetBridgeNode.ns` before proceeding.)

- [ ] **Step 4: Verify the CBOR prims are in the native_sim build list (watch-item §6)**

```bash
cd /home/david/workspace/tuvm
grep -n 'platform.c' zephyr/CMakeLists.txt
```

Expected: `zephyr/CMakeLists.txt` compiles `src/platform.c` (whole file → the `cborEncodeInt:`/`cborReadHead:`/… leaves are included). If `platform.c` is NOT listed or CBOR symbols are fenced, add the CBOR source to the Zephyr build list before building. (`platform.c` is a single translation unit, so this is expected to already be satisfied.)

- [ ] **Step 5: Bake `NetBridgeNode.ns` and build native_sim**

```bash
source /home/david/workspace/ns-zephyr/tools/env.sh
cd /home/david/workspace/nsl-tuvm
python3 tools/ns_compile_oevm.py /home/david/workspace/tuvm/zephyr/NetBridgeNode.ns /home/david/workspace/tuvm/src/solidified_image.c
cd /home/david/workspace/tuvm/zephyr
west build -b native_sim -p always . 2>&1 | tee /tmp/rung6_build.log | tail -20
```

Expected: `west build` completes. If it fails on a missing CBOR/space symbol, add that `.c` to `zephyr/CMakeLists.txt` and rebuild. If it fails on a `zsock_*` mismatch (backend caveat), fix the symbol in `src/backend/zephyr/udp_zephyr.c` only.

- [ ] **Step 6: Run native_sim and observe the round-trip**

```bash
cd /home/david/workspace/tuvm/zephyr
timeout 8 ./build/zephyr/zephyr.exe 2>&1 | tee /tmp/rung6_run.log
grep -q 'BRIDGE OK' /tmp/rung6_run.log && echo GATE_PASS || echo GATE_FAIL
```

Expected: `GATE_PASS` — a CBOR `Record` round-tripped over NSOS loopback through the bridge. (Fallback if NSOS loopback drops the datagram: the `[ gotB == nil ] whileTrue:` spins to the timeout — add `CONFIG_NET_LOOPBACK=y` to `zephyr/prj.conf` (native_sim-only) and rebuild, exactly as the Rung 5 fallback notes.)

- [ ] **Step 7: Restore the clobbered generated image (MANDATORY before any commit)**

```bash
cd /home/david/workspace/tuvm
git checkout -- src/solidified_image.c src/solidified_image.stmap
git status --short   # expect ONLY zephyr/NetBridgeNode.ns (new) + zephyr/NetNode.ns (modified); maybe prj.conf/udp_zephyr.c if a native_sim fix was needed
```

Expected: `src/solidified_image.c`/`.stmap` clean; only the intended `zephyr/` changes present.

- [ ] **Step 8: Commit (tuvm)**

```bash
cd /home/david/workspace/tuvm
git add zephyr/NetBridgeNode.ns zephyr/NetNode.ns
# add ONLY if a native_sim fix was needed: git add zephyr/prj.conf src/backend/zephyr/udp_zephyr.c
git commit -m "feat(rung6): native_sim bidirectional NetBridge round-trip gate

Bakes a Model-A node that wires two NetBridges A<->B in one Space and round-trips
a CBOR Record over Zephyr native_sim NSOS loopback (BRIDGE OK) — proves the
Layer-2 bridge (Socket + Cbor + Space drain) on the target-shaped backend. Also
migrates the L1 NetNode.ns gate to a Bytes payload under the Rung 6 seam.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Close — suite, gates, footprint re-bank, docs

**Files:**
- Modify (only if the footprint moved): `tuvm/tools/footprint-mcu-baseline.txt`
- Modify: `tuvm/zephyr/README.md` (native_sim bridge gate section)
- Modify: `tuvm/review/program-sequence.md` (Phase-2/3 line → Layer 2 done)
- Modify: the dialect/vocabulary doc owning `net` (`nsl-tuvm/docs/NEWSPEAK_DIALECT.md` if it has the net/serial entry; else the tests README net line — confirm by grep, do NOT invent a doc)

**Interfaces:** none new — closes the rung.

- [ ] **Step 1: Run the full both-path suite (controller-owned)**

Run: `cd /home/david/workspace/nsl-tuvm && OEVM_PATH=AB python3 tools/run_tests_oevm.py`
Expected: the prior baseline count **+1 test** (the new `45_udp_bridge.ns`), 0 failed, skips unchanged — both paths. Record the exact passed/failed/skipped from the run (do not assume; the baseline was 179/0/12 at rung 5 — expect **180/0/12** if no other tests changed).

- [ ] **Step 2: Run the five named gates (controller-owned)**

Run the project's standard gate recipe (grant / debugger-integration / portability-32 / boot-gc-asan / mcu-footprint), same commands the rung-4/rung-5 close used (see `.superpowers/sdd/progress.md` gate lines).
Expected: grant / debugger-integration / portability-32 / boot-gc-asan PASS. The **mcu-footprint** gate is the one that may move this rung (C changed).

- [ ] **Step 3: Re-measure and re-bank the footprint if it moved (no silent ratchet)**

```bash
cd /home/david/workspace/tuvm
source /home/david/workspace/ns-zephyr/tools/env.sh
FOOTPRINT_CONFIG=mcu ./tools/footprint.sh 2>&1 | tail -5
git diff tools/footprint-mcu-baseline.txt
```

Expected: `udp.c` added ~one small function (`udp_make_bytes`); a `RawBytes` leaf is the same size as a `String` leaf, so the delta should be near-zero. If the measured text/data moved, update `tools/footprint-mcu-baseline.txt` to the new numbers **in the same commit as the docs**, with the delta noted in the commit message (this is a deliberate re-bank, not a silent ratchet). If it did not move, leave the baseline untouched.

- [ ] **Step 4: Document the native_sim bridge gate in `tuvm/zephyr/README.md`**

Add a section after the Rung 5 net gate section:

```markdown
# Rung 6 — native_sim NetBridge round-trip gate (NetBridgeNode.ns)

Proves the Layer-2 reactive channel<->UDP bridge (`NetBridge` in
`nsl-tuvm/lib/kernel.ns`) functionally round-trips a CBOR `Record` on
`native_sim`. The baked node wires two `NetBridge`s A<->B in one `Space`, posts a
`Record` to A's `out` channel, and observes it decoded on B's `in` channel,
printing `BRIDGE OK`. Model A; reuses this directory's `main.c`/`CMakeLists.txt`/
`prj.conf` unchanged.

## Build + run

    source /home/david/workspace/ns-zephyr/tools/env.sh
    cd /home/david/workspace/nsl-tuvm
    python3 tools/ns_compile_oevm.py \
      /home/david/workspace/tuvm/zephyr/NetBridgeNode.ns \
      /home/david/workspace/tuvm/src/solidified_image.c
    cd /home/david/workspace/tuvm/zephyr
    west build -b native_sim -p always .
    timeout 8 ./build/zephyr/zephyr.exe   # expect a line: BRIDGE OK
    cd /home/david/workspace/tuvm
    git checkout -- src/solidified_image.c src/solidified_image.stmap
```

- [ ] **Step 5: Update `tuvm/review/program-sequence.md` — Phase-2/3 net line**

Find the net/transport line (the Rung 5 "Layer 1 done" line) and update it to reflect Layer 2:

```
Layer 2 done (host bidirectional NetBridge round-trip + native_sim; CBOR on the
wire, immutable Bytes payload). Layer 3 (real OpenThread on the nRF52840-DK)
named. Stream-transport framing (serial/tcp) named (see rung-6 design §7).
```

(Match the surrounding prose/format of that file; grep for "Layer 1 done" to locate it.)

- [ ] **Step 6: Add the dialect/vocabulary note**

```bash
grep -rln 'net' /home/david/workspace/nsl-tuvm/docs/NEWSPEAK_DIALECT.md 2>/dev/null
```

If `NEWSPEAK_DIALECT.md` owns the `net`/`serial` capability vocabulary, add a one-line `NetBridge` note beside the `net` entry (the reactive channel↔UDP bridge; CBOR wire; drop-on-malformed). Otherwise add the line to the tests README net section. Do NOT invent a new doc.

- [ ] **Step 7: Confirm the spec's §7 serial-rx paper-sketch is captured**

Verify `docs/superpowers/specs/2026-07-10-rung6-udp-bridge-l2-design.md` §7 is present and complete (it is — no action beyond confirmation). This is the rule-of-three paper-validation artifact; no code.

- [ ] **Step 8: Commit the close (tuvm + nsl-tuvm as touched)**

```bash
cd /home/david/workspace/tuvm
git add zephyr/README.md review/program-sequence.md
# add tools/footprint-mcu-baseline.txt ONLY if it moved (Step 3)
git commit -m "docs(rung6): close — native_sim bridge gate README, program-sequence Layer 2 done$(: 'append: + footprint re-bank if moved')

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"

# if the dialect doc lives in nsl-tuvm and was edited:
cd /home/david/workspace/nsl-tuvm
git add docs/NEWSPEAK_DIALECT.md 2>/dev/null && git commit -m "docs(rung6): NetBridge vocabulary note

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>" || true
```

---

## Self-Review

**Spec coverage** (each spec §8 task + design decision → a plan task):
- §3 Bytes payload seam → Task 1 (C `udp_make_bytes` + `Bytes>>rawLeaf` + Socket wrap/unwrap + 42_udp migration). ✓
- §2 ingress model (Option 1, `pumpInbound` explicit, no reserved channel) → Task 2 `NetBridge>>pumpInbound`. ✓
- §4 bridge API + malformed-drop-never-crash → Task 2 `NetBridge` + the garbage-datagram case in `45_udp_bridge.ns`. ✓
- §5 proof (bidirectional host + native_sim) → Task 2 (host A↔B) + Task 3 (native_sim A↔B). ✓
- §6 watch-items (CBOR prims on native_sim; footprint re-bank) → Task 3 Step 4 + Task 4 Step 3. ✓
- §7 serial-rx paper-sketch → in the spec; Task 4 Step 7 confirms. ✓
- §8 tasks 1–4 → Tasks 1–4. ✓

**Placeholder scan:** no TBD/TODO; every code step shows full code; every run step shows the exact command + expected output. The dialect-doc target (Task 4 Step 6) is resolved by grep at execution time with an explicit "do not invent a doc" guard — not a placeholder but a documented conditional. ✓

**Type consistency:** `NetBridge` class-side `socket:out:peer:port:in:` ↔ instance ivars `socket/out/peerHost/peerPort/in` (via `setSocket:out:peerHost:peerPort:in:`) — consistent across Tasks 2 and 3 call sites. `Socket>>sendTo:port:data:` takes `Bytes` and `Datagram>>data` answers `Bytes` — consistent Task 1 → Tasks 2/3. `ping` helper identical in `42_udp.ns` (Task 1) and `NetNode.ns` (Task 3). `Bytes>>rawLeaf` (Task 1) consumed only by `Socket>>sendTo:` (Task 1). ✓
