#!/usr/bin/env python3
"""The remote console's receiving half (#56 §10, built 2026-08-04).

Runs ON THE GATEWAY, because the gateway's mesh-local address is the one the node
already reaches for NTP — reusing a proven path rather than opening a new one and
having to debug mesh-to-LAN routing at the same time as a logging feature.

Writes the SAME on-disk format as collect.py, through the same LogStore, so
`logread.py --root <copy>` and `check.py --root <copy>` work on it unchanged.
That is the whole reason this is not a `nc -lu >> file`: the format is the
contract the reader was written against, and a second nearly-identical format is
a second thing to keep in step.

It parses nothing, exactly as the collector parses nothing (design D3): a parser
bug in a collector costs the evidence.

    ./udpsink.py                 # bind [::]:54330, board c6
    ./udpsink.py --port 54330 --board c6

Health: same heartbeat contract as the collector (D7), so a silent log and a dead
sink are distinguishable. `port=up` here means the SOCKET is bound — a UDP
listener has no peer to lose, so it never reports down; what it cannot tell you
is whether the node is still sending, which is what the node's own lines are for.
"""
import argparse
import datetime as dt
import os
import socket
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from logstore import (LogStore, MARKER, DEFAULT_ROOT, DEFAULT_BUDGET,
                      acquire_lock, Busy)

DEFAULT_PORT = 54330


def now_local():
    return dt.datetime.now(dt.timezone.utc).astimezone()


def serve(store, port, heartbeat_s=60.0, now=now_local, stop=None, sock=None):
    own = sock is None
    if own:
        sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("::", port))
    sock.settimeout(1.0)

    store.append("%s udpsink started pid=%d port=%d" % (MARKER, os.getpid(), port), now())
    last_beat = now()
    peers = set()

    while not (stop and stop()):
        try:
            data, addr = sock.recvfrom(2048)
        except socket.timeout:
            data = None
        except OSError as exc:
            store.append("%s socket error: %s" % (MARKER, exc), now())
            break

        if data is not None:
            if addr[0] not in peers:
                peers.add(addr[0])
                store.append("%s peer %s" % (MARKER, addr[0]), now())
            # One datagram is one line by construction (the sender splits on
            # newline). Stamped on ARRIVAL, like every other line in this store —
            # so these timestamps are evidence of order, never of device timing.
            text = data.decode("utf-8", "replace").rstrip("\r\n")
            if text:
                store.append(text, now())

        t = now()
        if (t - last_beat).total_seconds() >= heartbeat_s:
            store.append("%s heartbeat port=up" % MARKER, t)
            last_beat = t

    if own:
        sock.close()
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="Remote console sink (#56)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--board", default="c6")
    ap.add_argument("--root", default=DEFAULT_ROOT)
    ap.add_argument("--budget-bytes", type=int, default=DEFAULT_BUDGET)
    args = ap.parse_args(argv)

    store = LogStore(args.root, args.board, args.budget_bytes)
    # ONE reader per store. SO_REUSEADDR lets two UDP sinks bind the same port,
    # and on 2026-08-04 two of them did — both writing into one file, with the
    # datagrams split between them. The lock is the same one collect.py takes,
    # for the same reason: two readers do not double the evidence, they halve it.
    try:
        lock = acquire_lock(args.root, args.board)
    except Busy as exc:
        print("refusing to start: %s" % exc, file=sys.stderr)
        return 3

    try:
        return serve(store, args.port)
    except KeyboardInterrupt:
        store.append("%s udpsink stopped" % MARKER, now_local())
        return 0
    finally:
        lock.close()


if __name__ == "__main__":
    sys.exit(main())
