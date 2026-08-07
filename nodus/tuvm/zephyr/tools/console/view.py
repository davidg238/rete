#!/usr/bin/env python3
# Copyright (c) 2026 Ekorau LLC
"""#165 D8: the profile view, served next to the data.

The console collector already holds everything the view needs — the log
history (logstore), the offset->name join (mprof_resolve) and the symbolizer.
Serving from here rather than from porta's Go mux moves only the TRIGGER
across the seam, which is one small write; serving from porta would have
moved the bulk, and made the log file layout a contract.

Starting a capture is porta's, because porta owns the dbg/req write.
"""
import html as _html
import json
import os
import subprocess
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

# view.py lives in nodus/tuvm/zephyr/tools/console/; mprof_resolve.py lives in
# nodus/tuvm/tools/ (a sibling of zephyr/, not a child of it) — so this is
# ../../../tools, not ../../tools.
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "..", "tools"))
import mprof_resolve                                          # noqa: E402

import logstore                                               # noqa: E402


def latest_window(text, mmap, top=0):
    """Rows of the LAST mprof window in `text`, or None if there is none.

    Sliced to the text from the last `mprof[window]:` header onward before
    handing it to mprof_resolve.resolve(): a day's log can hold more than one
    profile run, and resolve() sums every `mprof:` line it is GIVEN — handing
    it the whole day would merge two windows' rows under whichever header
    happened to be last, silently. mprof_resolve.py itself is Task 2's and is
    not touched here; this is a caller-side slice, not a change to resolve().

    None rather than [] on purpose: an empty table and a node that was never
    asked to profile render identically otherwise, and the operator cannot tell
    'it reported nothing' from 'it was never asked'.
    """
    if not mprof_resolve.DUMP_RE.search(text):
        return None
    headers = list(mprof_resolve.HDR_RE.finditer(text))
    window_text = text[headers[-1].start():] if headers else text
    return mprof_resolve.resolve(mmap, window_text, top=top)


def render(rows, overflow, when=None):
    """`when` is the capture's OWN log timestamp (the collector's stamp on
    the `mprof[window]:` line), not the file's mtime and not the day key —
    round 2 widened the search to older days, and showing a stale capture
    with no indication of its age would be its own false green, the exact
    failure the search being scoped to `days()[-1]` alone was accidentally
    guarding against in round 1's report (by making every capture vanish at
    midnight instead)."""
    if rows is None:
        return ("<p id='table'>No capture in this window. Start one, then "
                "reload.</p>")
    meta = f"<p class='meta'>captured {_html.escape(when)}</p>" if when else ""
    banner = ""
    if overflow:
        banner = (f"<p class='warn'>INCOMPLETE: {overflow} activations were "
                  f"dropped — methods are missing from this table. Counts are "
                  f"exact; percentages are lower bounds.</p>")
    body = "".join(
        f"<tr><td>{n}</td><td>{pct:.1f}%</td><td>{_html.escape(name)}</td></tr>"
        for name, n, pct in rows)
    return (f"<div id='table'>{meta}{banner}<table>"
            f"<tr><th>count</th><th>%</th><th>method</th></tr>{body}"
            f"</table></div>")


# %-formatted (see do_GET), so any literal `%` in the CSS below — e.g. a
# percentage width — must be written `%%`, or PAGE % {...} raises ValueError
# on every request while /health keeps answering ok, which is the exact
# silent-outage shape this view exists to prevent (found in review round 1).
PAGE = """<!doctype html><meta charset=utf-8><title>node profile</title>
<script src="/htmx.min.js"></script>
<style>body{font-family:system-ui;margin:2rem;max-width:60rem}
table{border-collapse:collapse;width:100%%}td,th{padding:.25rem .75rem;
text-align:left;border-bottom:1px solid #ccc}td:first-child,td:nth-child(2)
{text-align:right;font-variant-numeric:tabular-nums}
.warn{background:#fee;padding:.5rem;border-left:3px solid #c00}</style>
<h1>node profile — %(board)s</h1>
<form hx-post="/capture" hx-target="#table">
  <label>window (ms) <input name="ms" value="10000" size="8"></label>
  <button type="submit">capture</button>
</form>
<p>A capture takes the window you ask for, plus one check-in for the request to
reach the node. Reload after it elapses.</p>
%(table)s
"""


def page(board, table):
    """Assemble the whole page. Split out from do_GET so it is testable on
    its own — the bug that shipped in review round 1 (a literal `%` in the
    CSS breaking `PAGE % {...}` on every request, while /health kept
    answering ok) lived entirely in this one substitution and nowhere a test
    ever exercised it."""
    return PAGE % {"board": _html.escape(board), "table": table}


class Handler(BaseHTTPRequestHandler):
    # No default board: c6bench has been off the bench since 2026-08-06 and its
    # by-id is absent, so a baked-in default would make "no capture in this
    # window" indistinguishable from "you read the wrong sink" — the exact
    # failure this view exists to avoid. --board is required at the CLI.
    board = None
    sidecar = None
    node = None
    top = 64

    def _capture(self):
        """Scan retained days NEWEST FIRST for the most recent one that
        actually holds a window, and return (rows, overflow, when) for it —
        or (None, 0, None) if no retained day has one.

        `days()[-1]` alone (today's file) was round 1's plan-inherited bug:
        Task 6's one hardware capture — 181 rows, zero UNKNOWN — sat in
        yesterday's file, today's file held nothing, and the view reported
        'No capture in this window' as if the capture had never happened.
        Every capture became invisible at midnight. `when` is what makes
        searching backward safe rather than merely quiet: it is read off the
        `mprof[window]:` line's own log timestamp, not the file's mtime and
        not the day key, so a three-day-old capture cannot be mistaken for a
        fresh one just because the search found it.
        """
        store = logstore.LogStore(board=self.board)
        mmap = json.load(open(self.sidecar)) if self.sidecar else {}
        for day in sorted(store.days(), reverse=True):
            text = open(store.path_for(day), encoding="utf-8",
                        errors="replace").read()
            rows = latest_window(text, mmap, top=self.top)
            if rows is None:
                continue
            overflow = 0
            when = None
            for m in mprof_resolve.HDR_RE.finditer(text):
                overflow = int(m.group(4))          # last window in the DAY wins
                line_start = text.rfind("\n", 0, m.start()) + 1
                when = text[line_start:m.start()].strip()
            return rows, overflow, when
        return None, 0, None

    def _table(self):
        rows, overflow, when = self._capture()
        return render(rows, overflow, when)

    def _send(self, code, body, ctype="text/html; charset=utf-8"):
        payload = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        if self.path == "/health":
            return self._send(200, "ok", "text/plain; charset=utf-8")
        if self.path == "/htmx.min.js":
            # Vendored, never a CDN: this runs on a bench network that may have
            # no route out, and a view that blanks when the internet is down is
            # useless at exactly the moment it is wanted.
            vendored = os.path.join(os.path.dirname(__file__), "htmx.min.js")
            return self._send(200, open(vendored, encoding="utf-8").read(),
                              "application/javascript")
        if self.path != "/":
            return self._send(404, "not found", "text/plain; charset=utf-8")
        self._send(200, page(self.board, self._table()))

    def do_POST(self):
        if self.path != "/capture":
            return self._send(404, "not found", "text/plain; charset=utf-8")
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n).decode("utf-8")
        ms = 10000
        for pair in body.split("&"):
            k, _, v = pair.partition("=")
            if k == "ms" and v.isdigit() and int(v) > 0:
                ms = int(v)
        # porta owns the dbg/req write, so the trigger crosses the seam here and
        # nowhere else. A failure is REPORTED, not swallowed: a capture that
        # never started and one that returned nothing look identical otherwise.
        try:
            # `put <node> <path> <value>` — the node is its own argument and the
            # path is NODE-RELATIVE (design D4.5). --kind stream is required:
            # it defaults to cell, and kind is pinned by the FIRST write, so a
            # cell write to dbg/req would fix the wrong kind permanently.
            subprocess.run(
                ["porta", "put", self.node, "dbg/req", f"profile {ms}",
                 "--kind", "stream"],
                check=True, capture_output=True, timeout=10)
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired,
                FileNotFoundError) as exc:
            return self._send(200, f"<p id='table' class='warn'>capture NOT "
                                   f"started: {_html.escape(str(exc))}</p>")
        self._send(200, f"<p id='table'>capture requested ({ms} ms). Reload "
                        f"once it elapses.</p>")


def main(argv):
    import argparse
    p = argparse.ArgumentParser()
    # No default: boards are resolved through the collector's env files, never
    # a table baked into code (see console-c6.env.example). c6bench has been
    # off the bench since 2026-08-06 and its by-id is absent.
    p.add_argument("--board", required=True, help="board name the collector "
                    "logs under, e.g. c6spare — NOT the node id")
    p.add_argument("--node", required=True, help="node id for the dbg/req "
                    "path, e.g. c6-bench — the string PortaBridgeNodeC6.nsl "
                    "hard-codes, independent of which board runs the image")
    p.add_argument("--sidecar", required=True, help="<image>.oimg.methods.json")
    p.add_argument("--port", type=int, default=8087)
    p.add_argument("--top", type=int, default=64)
    a = p.parse_args(argv[1:])
    Handler.board, Handler.node = a.board, a.node
    Handler.sidecar, Handler.top = a.sidecar, a.top
    print(f"profile view on http://localhost:{a.port} (board={a.board})")
    HTTPServer(("127.0.0.1", a.port), Handler).serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
