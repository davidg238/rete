# The relay frame — datagrams over Bluetooth LE to a phone relay

The contract between a node whose datagram seam rides Bluetooth (`nodus/oevm/src/backend/zephyr/udp_ble.c`)
and the relay that forwards each datagram as UDP: `relay/android/` (a phone) or `relay/linux/` (BlueZ, #498);
see `relay/README.md`. Design:
`docs/design/2026-10-23-439-ble-relay-design.md` (#439).

## The link

- The node is a Bluetooth LE peripheral advertising service UUID `6f657673-6d00-4000-8000-00000000439a`.
- The relay connects, bonds (LE Secure Connections, Just Works) and opens an L2CAP connection-oriented
  channel on PSM `0x0080`. The node accepts one channel, and once bonded, only from the bonded relay.

## The frame

One L2CAP SDU is one datagram, in both directions. Integers are big-endian.

| Bytes | Field | Node → relay | Relay → node |
|---|---|---|---|
| 2 | `local_port` | the node socket's bound port | the node socket it is for |
| 2 | `remote_port` | destination port | sender's port |
| 1 | `host_len` | | |
| `host_len` | `host` | destination address, as text | sender's address, as text |
| rest | payload | | |

- `host` is the text the node's seam carries (IPv6 or IPv4), never a binary address, so the relay never
  re-formats an address the node will compare. A reply carries the text the node sent to.
- The relay keeps one UDP socket per `local_port`, so porta sees a distinct source port per node socket.
- A frame shorter than 5 bytes, or whose `host` runs past its end, is dropped, as UDP would drop it.
  No version field: both ends are rebuilt together.

## The golden vector

`fixtures/relay/frame.hex` is `local_port` 16970, `remote_port` 6969, `host` `fd7a:115c:a1e0::1`, payload
`hi`. Every codec tests against it: `nodus/oevm/tests/test_relay_frame.c` (codec `src/relay_frame.h`) and
`relay/android/app/src/test/java/net/rete/relay/FrameTest.kt` and `relay/linux/frame_test.go`.
