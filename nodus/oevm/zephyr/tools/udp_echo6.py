#!/usr/bin/env python3
# udp_echo6.py — opaque UDP echo for Thread-node round-trip testing (Rung 7 / Layer 3).
#
# Binds a dual-stack UDP socket on :: and reflects every datagram's bytes
# VERBATIM back to its sender. It never parses the payload, so it physically
# cannot corrupt CBOR: value-equality of the *decoded* Record on the node then
# proves the whole encode -> 802.15.4 -> 802.15.4 -> decode path end to end.
#
# The gateway host sits directly on the Thread mesh via wpan0, so a plain
# socket here is reachable from a Thread MTD child. Leaves porta's :6969 alone.
#
# DEPLOYED AT /usr/local/bin/udp_echo6.py ON gw85224-01, under the `udp-echo6`
# systemd unit. It lived ONLY there, untracked, for seven weeks while being the
# entire instrument behind the program's longest reliability record — 720,878
# round trips, one `journalctl --vacuum` from gone (rete#281). This is now the
# copy of record; deploying means scp'ing it over that path and restarting the
# unit, which costs the soak a ~2 s gap (well under the 600 s that the analysis
# in docs/results/2026-08-29-thread-echo-soak.md counts as a break).
#
# THE HEARTBEAT (rete#281). A log of successes cannot report its own deafness.
# On 2026-08-18 three nodes went silent and the record could not say whether the
# nodes had stopped or this server had — `NRestarts=0`, so a wedged socket would
# never self-clear and would look EXACTLY like a dead mesh (soak doc §3, and the
# same shape as rete#269: an instrument reporting a failure that was not
# happening). So the loop now says so itself, once a minute, whether or not
# anyone spoke.
#
# What the heartbeat settles, precisely: that this process was alive and polling
# its socket across a silence. That is the whole of §3's open question. What it
# does NOT settle is loss — an echo the mesh dropped is invisible here in both
# directions, and `seen=0` reads identically for a deaf server and a quiet mesh.
# The line is evidence about THIS process, not about the radio.
import socket
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 7777
HEARTBEAT_S = 60.0

s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
# Dual-stack: also accept v4-mapped senders. Reuse so a restart rebinds fast.
s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("::", PORT))
# The timeout is what makes the heartbeat possible at all: the old loop blocked
# in recvfrom() forever, so a silent mesh and a wedged process were the same
# observation. Short enough that a heartbeat is never late by more than this.
s.settimeout(5.0)
print(f"udp_echo6 listening on [::]:{PORT}", flush=True)

last_beat = time.monotonic()
seen = 0

while True:
    try:
        data, addr = s.recvfrom(2048)
    except socket.timeout:
        data = None
    if data is not None:
        s.sendto(data, addr)
        host, port = addr[0], addr[1]
        scope = addr[3] if len(addr) > 3 else 0
        # Format is load-bearing: the soak extraction greps this exact line.
        print(f"echo {len(data)}B <-> [{host}%{scope}]:{port}", flush=True)
        seen += 1
    now = time.monotonic()
    if now - last_beat >= HEARTBEAT_S:
        # Emitted on the SAME schedule under traffic and under silence, so its
        # absence means the loop stopped — not that the mesh went quiet.
        print(f"@@udp-echo6@@ heartbeat port=up seen={seen}", flush=True)
        last_beat = now
        seen = 0
