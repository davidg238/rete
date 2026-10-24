# Datagrams over Bluetooth LE to a phone relay — from spike to mainline

2026-10-23 · #439 (P0) · consumer: ttgm's ACC01 (#437, #438). Spike commits `87d085aa`, `25f24566`.

## Why

A node reaches porta through David's phone: Bluetooth LE L2CAP to a relay app on the Pixel, then UDP over
Tailscale to gw85224-01. The premise is that where David is, his phone is. First light 2026-10-23 17:09 MST
(ACC01 → Pixel → test porta :6979, `#live`, acks flat for hours with the phone asleep). This record turns
the spike into the mainline mechanism.

## Decided by David (2026-10-23, #439 "Mainline decided")

- **Mainline.** A Bluetooth build is a rete build like any other, not a spike branch.
- **Pair once, then refuse every other phone.** LE Secure Connections, Just Works, bonded.
- **Production porta `:6969`.** Bluetooth nodes are ordinary porta nodes.
- **Power bar: months on 2× AA**, measured on the ACC01 with a meter, idle-linked and per event.
- Overnight Doze survival is not a gate; the ACC01 is on Bluetooth, not Thread (#437).

## The mechanism (my calls — one rule each)

1. **The radio picks the datagram backend; nothing above the seam knows.** `CONFIG_BT` links
   `udp_ble.c` instead of `udp_zephyr.c` (as the spike does). nsl, the kernel and porta are unchanged.
2. **No IP stack in a Bluetooth build.** `prj_dk_ble.conf` drops `CONFIG_NETWORKING`. `http_zephyr.c`
   gains the `#ifndef CONFIG_NET_SOCKETS` branch `log_udp.c` already has: `http_listen` answers -1.
   The spike measured 11.9 KB of RAM in the unused stack.
3. **The remote console tees through the datagram seam, on every build.** `log_udp.c` sends with
   `udp_socket_bind`/`udp_send` (`udp_backend.h`) instead of `zsock_*`, so the address stays text
   end to end and the same code reaches porta over Thread, Wi-Fi or Bluetooth. This is what lets
   profile (#415) reach porta over Bluetooth. Its busy-vs-dead back-off (#415) reads `errno`: the
   socket backend leaves `sendto`'s, and `udp_ble.c` sets `EAGAIN` (no buffer) or `ENOTCONN` (no
   channel). The seam must take two callers at once, the VM thread and the tee thread; `udp_ble.c`'s
   socket table gets a lock.
4. **The frame is a `protocol/` contract: `protocol/RELAY-BLE.md`, one golden vector in
   `protocol/fixtures/relay/`, tested in C (`nodus/oevm/tests/test_relay_frame.c`) and Kotlin
   (`FrameTest.kt`) from the same file.** The codec moves into a header (`relay_frame.h`), so the
   host test needs no Zephyr.
   - **The version byte is deleted.** Both ends are reflashed together (design posture); a byte that only
     ever holds one value is a versioned field. The header is `u16 local_port | u16 remote_port |
     u8 host_len | host | payload`.
5. **Pairing.** The L2CAP server asks for `BT_SECURITY_L2` (encrypted; Just Works cannot authenticate).
   Bond keys persist in the settings/NVS already mounted (`CONFIG_BT_SETTINGS`); `CONFIG_BT_MAX_PAIRED=1`.
   Once bonded, the node refuses any new pairing (`pairing_accept` → not allowed), so a phone without
   the keys can connect but never open the channel. The relay pairs with `createBond()` and dials
   `createL2capChannel` (secure), not the insecure variant.
   - **Degenerate case shipped: no filter accept list.** A stranger in range can still hold the node's
     one connection and keep the bonded phone out (denial of service, not access). The accept list
     fixes that but depends on resolving the phone's private address; it is earned the first time a
     node is seen held by a stranger.
   - **Re-pairing (a lost phone) is a full flash erase**, which drops the bond with the program. No button,
     no prim, until a board needs one.
   - Security terms: *Just Works* = pairing with no PIN, so it cannot tell a real phone from an attacker
     in range at that moment. *Bonded* = the keys are kept, so later connections are encrypted and need
     no new pairing. The exposure is the window between the flash and the first pairing; after that, a
     stranger's phone is refused.
6. **One node per relay** (David). Multi-node is filed, not built.
7. **The relay app moves to `relay/android/`** — already there; it joins the tree map in `CLAUDE.md`.
8. **The address is porta's Tailscale IPv6, as text, in the nsl program** — the relay forwards to
   whatever the node names. `PortaBridgeNodeBle.nsl` loses its SPIKE header and points at `:6969`.

## Left open, to be earned by data

- **Connection interval and slave latency** (the power knob): ship Zephyr's defaults; the meter sets them
  against the bar.
- **Who notices a silent node** (phone out of range): porta's `#live`/`#unknown` already records it; an
  alert waits for a missed reading that mattered.
- CompanionDeviceManager association on Android: the foreground service survives without it today.

## Gates (done means demonstrated)

| Gate | Evidence |
|---|---|
| Frame contract | `test_relay_frame` and `FrameTest` pass on the same golden vector; break one byte, both fail |
| Bonded link | DK ↔ Pixel ↔ production porta `:6969`, `#live`; a second central (msi `bluetoothctl`) is refused |
| No IP stack | DK image links without `CONFIG_NETWORKING`; RAM delta vs the spike's 251,776 B recorded |
| Console over Bluetooth | profile output reaches porta from the DK |
| Thread unbroken | `prj_dk_thread.conf` still builds; nsl suite, `porta_sync_host.sh` green (the tee change is shared) |
| Power | ACC01 average current, idle-linked and per event, against the bar: ~2,500 mAh (2× AA alkaline, low drain) lasts 3 months at ≤ 1.1 mA, 6 months at ≤ 0.57 mA, 12 months at ≤ 0.29 mA |

## Order

Frame contract (4) → pairing (5) on the DK and Pixel → no IP stack (2) + tee through the seam (3) →
production porta (8) → power on the ACC01. Promotion at close-out: `protocol/RELAY-BLE.md` is the
mainline record; `porta/docs/ARCHITECTURE.md` gains one line on the relay path.

## Addendum — pairing measured on the ACC01 (2026-10-23 evening)

Bonded link works: ACC01 ↔ Pixel ↔ test porta :6979, encrypted, paired once, bond survives a reflash that
skips the settings partition (`9f9c62d4`, `c5296ee6`). Three corrections to rule 5, each from the console:

- **`CONFIG_BT_SMP_ENFORCE_MITM=n`.** Zephyr's default demands MITM protection; Just Works cannot give it,
  and every pairing failed with error 9.
- **`CONFIG_BT_LONG_WQ_PRIO=0`** (the Kconfig floor). The pairing key agreement runs on that queue; at 10
  it starved behind the VM, which busy-polls in `awaitReply:` (#440), missed SMP's 30 s timeout, and stayed
  pending so every later pairing failed. A workaround: #440 removes the cause, then this line goes.
- **The link drops with its channel.** A link left up without a channel holds the node's one connection and
  stops advertising, so the relay scans forever.

## Addendum — step 3 measured (2026-10-23, late)

No IP stack + the tee through the seam (rules 2–3). DK image (`PortaBridgeNodeBle`): RAM 252,800 → 240,512 B
(−12,288), FLASH 447,700 → 405,764 B. On the ACC01 via the Pixel to test porta :6979: the bond survives the
reflash, the held boot reaches custos whole, and a `goal/profile` window (`OEVM_METHOD_PROFILE=1`, +6,144 B,
247,936 B total) comes back as 220 methods + 75 alloc rows. Costs found:

- **The tee's socket is bound, so it drains it** each pass: on `udp_ble.c` an unread datagram would sit at the head
  of the one receive ring, in front of the VM's replies.
- **An address is no longer refused at `goal/console`**: it is text to the seam; a bad one shows as send errors.
- **The dump costs send errors**: `tx_pool` holds 2 SDUs, so the tee hits `EAGAIN` and backs off `BUSY_MS`; one dump
  logged 420 send errors and 71 dropped chars with every row arriving. A third buffer is earned if rows go missing.
- Profile on the nRF is a build knob (`OEVM_METHOD_PROFILE`, run.sh's C6-only default); whether the ACC01 image
  carries it by default is open.

## Addendum — rule 8 done (2026-10-23)

The ACC01 reports to production porta :6969 via the Pixel; `PortaBridgeNodeBle.nsl` points there. Production and
test porta run the same binary and manifest, so the move needed no gateway restart. Each check-in pushes at most
`maxRows`/`maxBatchBytes`, so after a boot an event row waits behind the span cells for a few ticks.
