# R4 — UDP networking capability (design)

**Status:** DESIGNED 2026-06-26 (brainstorm w/ David, all decisions ratified via
AskUserQuestion + the IPv6 follow-up). NOT yet planned. Next = writing-plans → SDD.

**Rung:** R4 of the ns-zephyr-capability-node roadmap. The first **native confined
capability built on the active node**, replacing the deferred host-mock GPIO
(GPIO → R6/Zephyr; blueprint in `2026-06-26-gpio-capability-deferred-blueprint.md`).
Chosen because **the host backend is genuine** — POSIX gives the desktop real UDP
sockets, so there is no mock-fidelity gap, and the device path (R6) is UDP-centric
(Thread / 6LoWPAN, IPv6).

## 1. Why this shape (ratified decisions)

| Decision | Choice | Rationale |
|---|---|---|
| Transport | **UDP datagrams** | Connectionless; clean POSIX `SOCK_DGRAM` map; natural fit for Thread/6LoWPAN device path. |
| Facade | **Socket-vends, bound endpoint** | `net` cap is a factory minting confined `Socket` objects; mirrors how `Serial` mints its wrapper. Multiplexing + explicit lifecycle. |
| `receive` semantics | **Non-blocking, nil when empty** | Single-threaded VM, no fibers yet (R7); blocking would freeze the whole image (keeper, debugger). Matches Serial's nil-at-empty (dialect #13). |
| `receive` return | **`Datagram` value object** | Bundles payload + sender (UDP servers always need "who sent this"); trivial wrapper. |
| Addressing | **String host + SMI port, address-family-agnostic (IPv4 + IPv6)** | Facade is family-blind; backend uses `getaddrinfo`/`getnameinfo` + `sockaddr_storage`. De-risks the IPv6-only Thread device target — R6 swaps backend, not seam/facade. |
| Payload type | **immutable `String` byte-container** | Same mechanism as Serial `read:`/`write:` (`platform string: anArray`). |

## 2. Architecture — the proven native-cap spine (mirror `Serial`, step 3c-3b)

One capability = one C module + one receiver-identity hook + one thin `.ns`
wrapper, granted by boot injection. NOT ambient like `platform`.

- **C module** `oevm/src/udp.c`
  - `oop udp_object(void)` — static singleton (`&UdpObj`), lazily `udp_open()`s the
    backend, sets `UdpVtable.base.vtable = VtableVtable`.
  - `oop udp_dispatch(oop selector, int argc, oop *args)` — receiver-identity
    dispatch; `symbol_name((struct Symbol*)selector)` string-match; reads `oop *args`;
    returns `oop` (`TAG_SMI` / `NULL`=nil / `&true_sym_obj`/`&false_sym_obj` / heap obj).
    Adds nothing to `PRIM_NAMES` (no numbered prim id).
- **Link-time seam** `oevm/src/udp_backend.h` (mirror `serial_backend.h`):
  ```c
  void udp_open(void);                                   /* idempotent boot init */
  int  udp_socket_bind(int port);                        /* >=0 handle, or -1; port 0 = ephemeral */
  int  udp_send(int sock, const char *host, int port,
                const uint8_t *data, int len);           /* bytes sent, or -1 */
  /* receive: fills payload (caller buffer + cap), sender host (caller buffer),
     and *sender_port. Returns payload length, or -1 when no datagram waiting. */
  int  udp_receive(int sock, uint8_t *buf, int buf_cap,
                   char *host_out, int host_cap, int *sender_port);
  void udp_close(int sock);
  ```
- **Host backend** `oevm/src/backend/posix/udp_posix.c`:
  - Bind: `AF_INET6` socket, `setsockopt(IPV6_V6ONLY, 0)` → **dual-stack**, one
    `Socket` receives both v4-mapped and v6. `O_NONBLOCK` so `recvfrom` returns
    `EWOULDBLOCK` (→ -1 → nil) when empty.
  - Send: `getaddrinfo(host, port, hints{AF_UNSPEC|AF_INET6 w/ AI_V4MAPPED|AI_ALL})`
    → `sockaddr_storage` → `sendto`.
  - Receive: `recvfrom` → `sockaddr_storage`; `getnameinfo(..., NI_NUMERICHOST)` →
    host string; **normalize `::ffff:1.2.3.4` → `1.2.3.4`** so apps/tests see plain
    dotted-quad for v4 senders.
  - Datagram cap = 2 KB buffer (single-datagram).
- **Dispatch hook** `oevm/src/dispatch.c` (mirror serial ~line 619-621):
  ```c
  if (receiver == udp_object()) return udp_dispatch(selector, argc, args);
  ```
  `&UdpObj` is static BSS → `mark()` skips it (`gc.c` `is_valid_heap_object`) → no
  new GC root.

## 3. Newspeak facade — `ns-oevm/lib/kernel.ns` (three confined classes)

```newspeak
class Net = ( | port | )  (* port slot holds the native udp_object() *)
  setPort: aPort = ( port:: aPort. ^ self )
  public udp = ( ^ self udpBindPort: 0 )                      (* ephemeral *)
  public udpBindPort: aPort = ( | h |
    h:: port basicBind: aPort.
    h < 0 ifTrue: [ ^ nil ].                                  (* bind failed *)
    ^ Socket onPort: port handle: h ) ) : (
  public onPort: aPort = ( ^ self new setPort: aPort ) )

class Socket = ( | port handle | )
  setPort: aPort handle: h = ( port:: aPort. handle:: h. ^ self )
  public sendTo: host port: aPort data: aString = (
    port basicSend: handle to: host port: aPort data: aString. ^ self )
  public receive = ( | dg |
    dg:: port basicReceive: handle.                           (* nil if none *)
    ^ dg )                                                     (* basicReceive builds the Datagram *)
  public close = ( port basicClose: handle. ^ self ) ) : (
  public onPort: aPort handle: h = ( ^ self new setPort: aPort handle: h ) )

class Datagram = ( | data senderHost senderPort | )
  public data = ( ^ data )
  public senderHost = ( ^ senderHost )
  public senderPort = ( ^ senderPort ) ) : ( ... )
```

**Note on `basicReceive:`**: the native `udp_dispatch` does the recvfrom and must
return a fully-built `Datagram` (payload String + sender String + port SMI). Two
viable mechanisms — decide at plan time with a verify-first probe:
- (a) C builds the `Datagram` by sending `Kernel>>datagramData:host:port:` back into
  the image (like serial's `serialOn:` callback), OR
- (b) C returns a small Array `{payloadString, hostString, portSmi}` and the `.ns`
  `Socket>>receive` wraps it into a `Datagram`. **(b) is simpler** — keeps C from
  needing a kernel selector ref; payload String via the existing `platform string:`
  path. Lean (b).

`Net`/`Socket`/`Datagram` are minted by a **non-public** `Kernel>>netOn:` (returns a
`Net`); no public accessor → confined.

## 4. Protocol surface

- `net udp` → ephemeral-port `Socket`; `net udpBindPort: n` → `Socket` bound to n
  (nil on bind failure).
- `socket sendTo: aHostString port: aPortSmi data: aString` → self.
- `socket receive` → `Datagram` (with `data`/`senderHost`/`senderPort`) or **nil**.
- `socket close` → self.

## 5. Grant, confinement, both image paths (mirror serial threading)

- Entry signature: `class Main usingPlatform: platform net: net = (...)`.
- **Path A** (`ns-oevm/tools/ns_compile_oevm.py`, boot region ~1940-1970): thread
  `net` as Nth cap by entry signature, guarded by `"net" in params`; 1-cap /
  serial-only entries untouched. Mint via `send(kernel_inst, netOn:, {udp_object()})`.
- **Path B** (`.oimg`): serializer (`ns-oevm/tools/oimg_serialize.py`) emits a
  `net_on_sel_ref` header word for net-declaring entries. **format_version 5 → 6**
  (current max is 5 = the Supervisor/in-image-debugger descriptor, NOT 4). The new
  word appends at the header **tail, offset A+12W** (byte 120 on 64-bit), AFTER the
  supervisor block (A+8W..A+11W) — so all existing offsets are untouched and non-net
  images stay byte-identical at their version (3/4/5). When net is present, the
  serial slot (A+7W) and supervisor block (A+8W..A+11W) are emitted as zeros so
  `net_on_sel_ref` lands at the fixed A+12W. Loader (`oevm/src/oimg_loader.c`) reads
  `img_net_on_sel_ref = (format_version >= 6) ? read_word(base + a + 12*W) : 0` and
  mints the same wrapper. (registry_version stays 4 — UDP adds no builtins.)
- **Confinement = runtime DNU**: an entry without `net:` that names `net` compiles
  (bare unary self-send) but aborts at runtime ("Message 'net' not understood").

## 6. Testing

- `oevm/tests/test_udp.c` — backend-only C smoke: bind two sockets on `127.0.0.1`
  and on `::1`, `udp_send`/`udp_receive` round-trip both families; assert nonblock
  returns -1 when empty; assert v4-mapped→dotted-quad normalization.
- `ns-tests/04_somns_basic/42_udp.ns` — capability round-trip over loopback: bind
  two sockets, sendTo one → poll receive → assert payload String + senderHost +
  senderPort; do it for **both `127.0.0.1` and `::1`**.
- `ns-tests/04_somns_basic/43_udp_confined.ns` — `expect-runtime-fail:` DNU when
  `net` undeclared.
- Suite target **63/0/3 → 65/0/3** (+42, +43).

## 7. Dialect divergence

**#14** = UDP native capability tier. Non-blocking `receive` (nil when no datagram,
like serial dialect #13). `Datagram` value object (data/senderHost/senderPort).
Address-family-agnostic (IPv4 + IPv6) String host + SMI port; host backend
dual-stack. Payloads are immutable String byte-containers (dialect #10).
DEFERRED (additive, reversible): blocking/timeout receive (needs fibers, R7);
multicast; TCP; DNS beyond `getaddrinfo` literals.

## 8. Source decomposition (oevm#10, Option A)

`udp.c` + `udp_backend.h` are **language-neutral oevm core** (a future `st-oevm`
inherits them). `udp_posix.c` is a **language-neutral target backend** in
`oevm/src/backend/posix/`; `udp_zephyr.c` is its R6 sibling. Only the `.ns`
`Net`/`Socket`/`Datagram` facade + boot wiring is ns-specific.

## 9. Verify-first probes (carry serial's lessons)

- NS parser rejects inline `"..."` comments — use `(* ... *)`.
- `platform print:` dispatches only on the native C platform object, not the
  Kernel-wrapper instance the entry gets as `platform` — use `println`.
- Trace serializer/loader header offsets independently; confirm Path-A/Path-B parity
  before trusting it.
- Probe the `basicReceive:` mechanism (Array-return §3(b) vs C-callback §3(a))
  empirically before committing the `.ns`/C boundary.
