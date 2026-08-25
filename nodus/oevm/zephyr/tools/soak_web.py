#!/usr/bin/env python3
"""soak_web — read-only soak dashboard for the udp-echo6 oracle (gw85224-01:8085).

Tails `journalctl -u udp-echo6 -f` (no sudo needed) and serves one HTML page of
per-node stats: ping count, first/last seen, and misses estimated from the 15 s
cadence. Deliberately a SIDECAR: the echo server itself (the oevm round-trip
oracle) stays untouched. State is in-memory — restart = re-backfill from journal.

Deployed on gw85224-01 as /home/david/soak_web.py, run by the `soak-web` systemd
unit (soak-web.service, alongside this file). See README.md — in particular, do
not retune WINDOW_S by taste: replay the real journal and diff the verdicts.
"""
import re
import socket
import subprocess
import threading
import time
from collections import deque
from datetime import datetime, timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = 8085
CADENCE_S = 15          # SoakNode send period
BACKFILL = "-12 hours"  # journal history to seed stats on start
# Health is judged over the last hour, not all history. 60 min is not arbitrary:
# real loss here is BURSTY, so a 15-30 min window lands between bursts and reports
# a lossy node as clean, while >90 min lets a long-dead outage re-brand a node that
# has since recovered (the very bug this window replaces). 60 min also gives the 2%
# bar usable resolution — 4.8 of 240 slots, vs 2.4 of 120 at half the width.
WINDOW_S = 3600
HIST = 400              # per-node timestamps retained (>1 window at cadence)

LINE = re.compile(
    r"^(?P<ts>\S+ \d+ [\d:]+) \S+ \S+ echo (?P<size>\d+)B <-> "
    r"\[(?P<addr>[0-9a-f:]+)%\d\]:(?P<port>\d+)"
)

nodes = {}   # addr -> dict(count, first, last, size, port)
lock = threading.Lock()


def note(ts, addr, port, size):
    if port not in ("54360", "54361"):
        return   # not a known node load (e.g. the 2026-07-12 rotating-port bursts)
    with lock:
        # keyed by (addr, port): the same board's gate history and soak run are
        # separate rows, so a load switch doesn't read as a pile of misses
        n = nodes.setdefault((addr, port),
                             {"count": 0, "first": ts, "last": ts, "size": size,
                              "hist": deque(maxlen=HIST)})
        n["count"] += 1
        n["last"] = ts
        n["size"] = size
        n["hist"].append(ts)


def parse(line, year):
    m = LINE.match(line.strip())
    if not m:
        return
    try:
        ts = datetime.strptime(f"{year} {m['ts']}", "%Y %b %d %H:%M:%S")
    except ValueError:
        return
    note(ts, m["addr"], m["port"], int(m["size"]))


def tail():
    year = datetime.now().year
    # one process does both backfill and follow, so no seam between them
    proc = subprocess.Popen(
        ["journalctl", "-u", "udp-echo6", "--since", BACKFILL, "-f",
         "--no-pager"],
        stdout=subprocess.PIPE, text=True)
    for line in proc.stdout:
        parse(line, year)


def recent(n, now):
    """(misses, seen) over the last WINDOW_S, counted from cadence gaps.

    Phase-agnostic on purpose: a miss is a gap that skipped whole cadence slots.
    A node that holds its cadence but loses packets in the mesh reads as lossy;
    a node that merely started late, or was reflashed an hour ago, does not.
    """
    ts = [t for t in n["hist"] if t >= now - timedelta(seconds=WINDOW_S)]
    if not ts:
        return 0, 0
    misses = sum(max(0, round((b - a).total_seconds() / CADENCE_S) - 1)
                 for a, b in zip(ts, ts[1:]))
    # trailing silence counts: a node degrading *now* must not be able to hide
    # behind a clean history earlier in the window
    misses += max(0, round((now - ts[-1]).total_seconds() / CADENCE_S) - 1)
    return misses, len(ts)


def fmt_age(dt):
    s = int((datetime.now() - dt).total_seconds())
    if s < 90: return f"{s}s ago"
    if s < 5400: return f"{s // 60}m ago"
    return f"{s // 3600}h{(s % 3600) // 60:02d}m ago"


class Page(BaseHTTPRequestHandler):
    def log_message(self, *a):  # journald has enough logs already
        pass

    def do_GET(self):
        rows = []
        now = datetime.now()
        with lock:
            snap = {k: dict(n) for k, n in nodes.items()}
        for (addr, port), n in sorted(snap.items(), key=lambda kv: kv[1]["last"],
                                      reverse=True):
            misses, seen = recent(n, now)
            rate = misses / max(1, misses + seen)
            silent = (now - n["last"]).total_seconds()
            state = ("dead" if silent > 4 * CADENCE_S else
                     "thin" if rate > 0.02 else "ok")
            kind = "gate" if port == "54360" else "soak"
            loss = "—" if state == "dead" else f"{misses} ({rate:.0%})"
            rows.append(
                f"<tr class={state}><td>…{addr[-19:]}</td><td>{kind}</td>"
                f"<td>{n['count']}</td><td>{loss}</td>"
                f"<td>{fmt_age(n['last'])}</td>"
                f"<td>{n['first']:%a %H:%M}</td>"
                f"<td>{n['size']}B</td><td>{state.upper()}</td></tr>")
        body = f"""<!doctype html><html><head><meta charset=utf-8>
<meta http-equiv=refresh content=15><title>soak — udp-echo6</title><style>
 body{{font:14px/1.5 system-ui;margin:2em;background:#111;color:#ddd}}
 table{{border-collapse:collapse}} td,th{{padding:.35em .9em;text-align:left}}
 th{{border-bottom:1px solid #555;color:#aaa}}
 tr.ok td:last-child{{color:#4c4}} tr.thin td:last-child{{color:#fa0}}
 tr.dead td:last-child{{color:#f55}}
 .f{{color:#777;margin-top:1.5em;font-size:12px}}</style></head><body>
<h2>Thread soak — udp-echo6 @ gw85224-01</h2>
<table><tr><th>node (ML-EID)</th><th>load</th><th>pings (all)</th>
<th>missed (last {WINDOW_S // 60}m)</th>
<th>last seen</th><th>first seen</th><th>last size</th><th>state</th></tr>
{''.join(rows) or '<tr><td colspan=8>no traffic yet</td></tr>'}</table>
<p class=f>misses = 15&thinsp;s-cadence slots skipped, counted over the last
{WINDOW_S // 60}&thinsp;min ONLY &mdash; health is current, so an old outage ages
out and a node degrading now cannot hide behind a clean history &middot;
THIN &gt;2% of recent slots &middot; DEAD after {4 * CADENCE_S}&thinsp;s of
silence &middot; sizes: 32B gate / 32&ndash;34B soak (CBOR #seq width) &middot;
refresh 15&thinsp;s &middot; backfill {BACKFILL}</p></body></html>"""
        data = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


class DualStack(ThreadingHTTPServer):
    address_family = socket.AF_INET6   # "::" accepts v4-mapped too


if __name__ == "__main__":
    threading.Thread(target=tail, daemon=True).start()
    time.sleep(2)   # let the backfill land before first render
    DualStack(("::", PORT), Page).serve_forever()
