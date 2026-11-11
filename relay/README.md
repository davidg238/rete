# relay — a bridge from a Bluetooth LE node to porta

A node whose datagram seam rides Bluetooth (`nodus/oevm/src/backend/zephyr/udp_ble.c`, built with
`nodus/oevm/zephyr/prj_dk_ble.conf`) cannot reach porta by itself. A relay holds the node's one
encrypted L2CAP channel and forwards each datagram as UDP, both ways. It keeps no node state and does
not read the payload, so any project's node works through either relay.

| Relay | Runs on | Build and test | State |
|---|---|---|---|
| [`android/`](android/) | a phone (the Pixel, Tailscale) | `cd relay/android && ./gradlew test assembleDebug` | in use since #439 |
| [`linux/`](linux/README.md) | a Pi or any BlueZ host | `cd relay/linux && go test ./...` | frame codec tested; not yet run against a node (#498) |

- **The contract** is `protocol/RELAY-BLE.md`: the link (service UUID, PSM `0x0080`, bonded) and the frame.
  Both relays and the node's codec are held to one golden vector, `protocol/fixtures/relay/frame.hex`.
- **The two relays share no code**, only the contract (Kotlin and Go). A change to the frame is a change to
  the contract, the vector, and all three codecs.
- **A node bonds with exactly one relay** (pair once, #439). Moving a node to the other relay means a full
  flash erase of the node, then bonding again.
- The relay needs a route to the address the node names for porta (on the bench, Tailscale).
- `relay/linux` is its own Go module; the root gate loop (`protocol porta custos`) does not run its tests.

Design: `docs/design/2026-10-23-439-ble-relay-design.md`.
