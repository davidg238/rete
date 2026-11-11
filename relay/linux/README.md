# relay/linux — the BLE relay on a Pi

The Linux twin of `relay/android` (#498; contract `protocol/RELAY-BLE.md`). It opens the node's
encrypted L2CAP channel (PSM `0x80`) through BlueZ and forwards datagrams to porta over UDP.
It needs a route to porta's address (the node names it; on the bench, tailscale).

## Build (on the desk machine)

```
cd relay/linux && go test ./... && GOOS=linux GOARCH=arm64 go build -o relay-arm64 .   # GOARCH=arm GOARM=7 for a 32-bit Pi OS
```

## Bond once (on the Pi)

The node bonds with exactly one relay (pair-once, #439); a full flash erase of the node frees it.
The node's address is its porta id: `e044a5dcefbb` is `E0:44:A5:DC:EF:BB` (LE random static).

```
bluetoothctl
  agent NoInputNoOutput
  default-agent
  scan le                     # wait for E0:44:A5:DC:EF:BB "oevm-dk", then: scan off
  pair E0:44:A5:DC:EF:BB      # Just Works; the node prints "ble: paired, bonded"
  trust E0:44:A5:DC:EF:BB
  disconnect E0:44:A5:DC:EF:BB
```

## Run

```
sudo ./relay-arm64 -node E0:44:A5:DC:EF:BB
```

Raw L2CAP sockets need root (or `cap_net_raw,cap_net_admin`). Each link logs `linked … in N ms`
and `link down after U up, D down`.
