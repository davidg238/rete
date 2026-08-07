#!/usr/bin/env python3
# Copyright (c) 2026 Ekorau LLC
"""#165 D8: the profile view, served next to the data.

The console collector already holds everything the view needs — the log
history (logstore), the offset->name join (mprof_resolve) and the symbolizer.
Serving from here rather than from porta's Go mux moves only the TRIGGER
across the seam, which is one small write; serving from porta would have
moved the bulk, and made the log file layout a contract.

Starting a capture is porta's, because porta owns the dbg/req write.

That write is a LOCAL sqlite write, not a network call — every porta verb opens
the store file directly. So this process must run on the host holding the store
`porta serve` is serving, and `--porta-db` must name that file. Neither is
inferable from here, which is why the flag is required and why nothing in this
file guesses a path or a hostname.
"""
import html as _html
import json
import os
import subprocess
import sys
import traceback
from http.server import BaseHTTPRequestHandler, HTTPServer

# view.py lives in nodus/tuvm/zephyr/tools/console/; mprof_resolve.py lives in
# nodus/tuvm/tools/ (a sibling of zephyr/, not a child of it) — so this is
# ../../../tools, not ../../tools.
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "..", "tools"))
import mprof_resolve                                          # noqa: E402

import logstore                                               # noqa: E402


def latest_window(text, mmap, top=0):
    """(rows, total) for the LAST mprof window in `text`, or (None, 0).

    `total` is the row count BEFORE `top` truncates, and it is returned rather
    than left implicit because a table that silently drops rows reads as a
    complete one — mprof_resolve.py's own docstring makes that argument and its
    CLI prints the sentence; the page was showing 64 of 177 and saying nothing.

    Sliced to the text from the last `mprof[window]:` header onward before
    handing it to mprof_resolve.resolve(): a day's log can hold more than one
    profile run, and resolve() sums every `mprof:` line it is GIVEN — handing
    it the whole day would merge two windows' rows under whichever header
    happened to be last, silently. mprof_resolve.py itself is Task 2's and is
    not touched here; this is a caller-side slice, not a change to resolve().

    The "is there a window at all" gate is checked against the SLICE, not the
    unsliced `text` (round 3 fix — checking the whole text reopened the very
    None-vs-[] conflation this function exists to prevent: a re-capture that
    legitimately counts zero activations is ordinary, and if it is the LAST
    window, the earlier window's dump lines would pass a whole-text gate while
    the slice resolve() actually sees is empty, drawing a headers-only table
    that looks identical to "it reported nothing" rather than "it was never
    asked" — the exact pair this function's return value exists to keep apart).

    None rather than [] on purpose: an empty table and a node that was never
    asked to profile render identically otherwise, and the operator cannot tell
    'it reported nothing' from 'it was never asked'.
    """
    headers = list(mprof_resolve.HDR_RE.finditer(text))
    window_text = text[headers[-1].start():] if headers else text
    if not mprof_resolve.DUMP_RE.search(window_text):
        return None, 0
    # Resolved ONCE, untruncated, and sliced here: calling resolve() a second
    # time with top=0 just to count would be a second denominator to keep in
    # step with the first.
    rows = mprof_resolve.resolve(mmap, window_text)
    return (rows[:top] if top else rows), len(rows)


def render(rows, overflow, when=None, total=0):
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
    # An ABSENT capture time is stated, never omitted. Showing the time is the
    # condition under which this view is allowed to search backward into older
    # days at all, so a table that quietly drops the line is a table whose age
    # the operator has no way to question — the same false green as showing a
    # three-day-old capture with today's date.
    meta = (f"<p class='meta'>captured {_html.escape(when)}</p>" if when
            else "<p class='warn'>capture time unknown — this table may be "
                 "from an older day; the window header carried no readable "
                 "timestamp.</p>")
    banner = ""
    if overflow:
        banner = (f"<p class='warn'>INCOMPLETE: {overflow} activations were "
                  f"dropped — methods are missing from this table. Counts are "
                  f"exact; percentages are lower bounds.</p>")
    # The truncation notice mprof_resolve.py's CLI already prints. A profile
    # showing 64 of 177 rows with no such line reads as the whole profile.
    trunc = ""
    if total > len(rows):
        trunc = (f"<p class='meta'>showing top {len(rows)} of {total} rows — "
                 f"percentages are of ALL activations, not of the shown "
                 f"rows.</p>")
    body = "".join(
        f"<tr><td>{n}</td><td>{pct:.1f}%</td><td>{_html.escape(name)}</td></tr>"
        for name, n, pct in rows)
    return (f"<div id='table'>{meta}{banner}{trunc}<table>"
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
    porta_db = None
    top = 64

    def _capture(self):
        """Scan retained days NEWEST FIRST for the most recent one that
        actually holds a window, and return (rows, overflow, when, total) for
        it — or (None, 0, None, 0) if no retained day has one.

        `days()[-1]` alone (today's file) was round 1's plan-inherited bug:
        Task 6's one hardware capture — 177 rows, zero UNKNOWN — sat in
        yesterday's file, today's file held nothing, and the view reported
        'No capture in this window' as if the capture had never happened.
        Every capture became invisible at midnight. `when` is what makes
        searching backward safe rather than merely quiet: it is read off the
        `mprof[window]:` line's own log timestamp, not the file's mtime and
        not the day key, so a three-day-old capture cannot be mistaken for a
        fresh one just because the search found it.
        """
        store = logstore.LogStore(board=self.board)
        if self.sidecar:
            with open(self.sidecar) as fh:
                mmap = json.load(fh)
        else:
            mmap = {}
        for day in sorted(store.days(), reverse=True):
            with open(store.path_for(day), encoding="utf-8",
                      errors="replace") as fh:
                text = fh.read()
            rows, total = latest_window(text, mmap, top=self.top)
            # KNOWN AND DELIBERATE: the fall-through is per FILE, not per
            # window. If a day's LAST window is headers-only, latest_window
            # answers None for the whole file and this loop moves to an OLDER
            # day — skipping an earlier, complete window in the SAME file. The
            # alternative is walking a file's windows backward, which makes
            # "the last capture" mean something different from what the header
            # says, and the capture time is shown either way so the operator
            # can see the table is not the one just requested.
            if rows is None:
                continue
            overflow = 0
            when = None
            for m in mprof_resolve.HDR_RE.finditer(text):
                overflow = int(m.group(4))          # last window in the DAY wins
                line_start = text.rfind("\n", 0, m.start()) + 1
                when = text[line_start:m.start()].strip()
            return rows, overflow, when, total
        return None, 0, None, 0

    def _table(self):
        rows, overflow, when, total = self._capture()
        return render(rows, overflow, when, total=total)

    def _render_page(self):
        """(healthy, html) for GET / — and for GET /health, which builds the
        same page and throws it away.

        `healthy` is False when the page assembled only an error banner. Round
        1 shipped a view where every GET / raised while /health answered `ok`
        throughout; a health check that does not build the thing it is
        certifying can only ever report that the process is running, which is
        the one fact nobody was in doubt about."""
        try:
            table = self._table()
            healthy = True
        except (OSError, json.JSONDecodeError) as exc:
            # REPORTED, not swallowed — the same principle POST /capture
            # already gets. Caught narrowly (OSError covers a missing or
            # unreadable sidecar/log file; JSONDecodeError covers a sidecar
            # that isn't valid JSON) rather than a bare `except Exception`,
            # which would also have hidden round 1's ValueError from ever
            # being found. The banner tells the operator something broke;
            # the traceback in the server log tells whoever fixes it what.
            traceback.print_exc()
            healthy = False
            table = (f"<p id='table' class='warn'>error reading the capture: "
                     f"{_html.escape(str(exc))} — see the server log for the "
                     f"traceback</p>")
        return healthy, page(self.board, table)

    def _send(self, code, body, ctype="text/html; charset=utf-8"):
        payload = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        if self.path == "/health":
            # Builds the WHOLE page and discards it. `except Exception` is
            # correct here and nowhere else in this file: this route's entire
            # job is to convert any failure of GET / into a red answer, so a
            # narrow catch would be a probe that stays green for exactly the
            # class of fault nobody anticipated — which is how round 1's
            # ValueError survived.
            try:
                healthy, _ = self._render_page()
            except Exception as exc:                      # noqa: BLE001
                traceback.print_exc()
                return self._send(500, f"not ok: GET / raises: {exc}",
                                  "text/plain; charset=utf-8")
            if not healthy:
                return self._send(
                    500, "not ok: the page assembles only an error banner — "
                         "see the server log for the traceback",
                    "text/plain; charset=utf-8")
            return self._send(200, "ok", "text/plain; charset=utf-8")
        if self.path == "/htmx.min.js":
            # Vendored, never a CDN: this runs on a bench network that may have
            # no route out, and a view that blanks when the internet is down is
            # useless at exactly the moment it is wanted.
            vendored = os.path.join(os.path.dirname(__file__), "htmx.min.js")
            with open(vendored, encoding="utf-8") as fh:
                return self._send(200, fh.read(), "application/javascript")
        if self.path != "/":
            return self._send(404, "not found", "text/plain; charset=utf-8")
        _, body = self._render_page()
        self._send(200, body)

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
            #
            # --db IS NOT OPTIONAL, and leaving it off is what this route
            # shipped with. `porta` is not a network client: every verb opens
            # the sqlite file directly (portacli/root.go:56 defaults --db to a
            # RELATIVE "porta.db"), and space.Open() runs the schema DDL on
            # whatever it opens — so a missing --db makes sqlite CREATE an
            # empty store in this process's cwd, the write succeeds, exit 0,
            # and the page says "capture requested" while no served store and
            # no node ever heard of it. The operator then reloads, sees an
            # earlier capture with its own date, and reads it as this one.
            put = subprocess.run(
                ["porta", "put", self.node, "dbg/req", f"profile {ms}",
                 "--kind", "stream", "--db", self.porta_db],
                check=True, capture_output=True, timeout=10)
            # READ IT BACK. `porta put` prints `wrote <path> seq=<n>`; the
            # write is a STREAM, so it never reaches the `cells` table and
            # `porta get` (a cell-tree read, store.go:342) cannot see it at
            # all — the log is the only place a stream write lands.
            #
            # What this proves and what it does not: it proves the row is in
            # the store this view was pointed at. It CANNOT prove that store
            # is the one `porta serve` is serving — nothing local can, since
            # the CLI never speaks to the server. That is what makes
            # --porta-db required rather than defaulted.
            seq = None
            for tok in put.stdout.decode("utf-8", "replace").split():
                if tok.startswith("seq="):
                    seq = tok
            log = subprocess.run(
                ["porta", "log", self.node, "--limit", "20",
                 "--db", self.porta_db],
                check=True, capture_output=True, timeout=10)
            landed = seq is not None and any(
                seq in line.split() and "dbg/req" in line.split()
                for line in log.stdout.decode("utf-8", "replace").splitlines())
            if not landed:
                which = seq or "(porta put printed no seq)"
                return self._send(
                    200, f"<p id='table' class='warn'>capture NOT started: "
                         f"porta put exited 0 but the write is not in "
                         f"{_html.escape(self.porta_db)} — porta log has no "
                         f"dbg/req row at {_html.escape(which)}</p>")
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired,
                FileNotFoundError) as exc:
            detail = str(exc)
            stderr = getattr(exc, "stderr", None)   # FileNotFoundError has none
            if stderr:
                # stderr is usually the ONLY useful diagnostic for why
                # `porta put` refused — str(exc) alone is often just the
                # exit code, and porta's own message ("no such table",
                # "value is N bytes, over the cap") is the one that names
                # the fault.
                detail += f" — {stderr.decode('utf-8', 'replace').strip()}"
            return self._send(200, f"<p id='table' class='warn'>capture NOT "
                                   f"started: {_html.escape(detail)}</p>")
        self._send(200, f"<p id='table'>capture requested ({ms} ms, {seq}). "
                        f"Reload once it elapses.</p>")


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
    # REQUIRED, not defaulted, and this is the whole fix: `porta` is a local
    # sqlite writer, so an unqualified `porta put` silently CREATES an empty
    # store in the cwd and reports success. Which file the gateway serves is a
    # fact about the deployment, and code cannot guess it — so view.py must run
    # on the host holding the served store and be told where it is.
    p.add_argument("--porta-db", required=True,
                   help="path to the sqlite store `porta serve` is serving — "
                        "this process must run on that host")
    p.add_argument("--port", type=int, default=8087)
    p.add_argument("--top", type=int, default=64)
    a = p.parse_args(argv[1:])
    Handler.board, Handler.node = a.board, a.node
    Handler.sidecar, Handler.top = a.sidecar, a.top
    Handler.porta_db = a.porta_db
    print(f"profile view on http://localhost:{a.port} (board={a.board})")
    HTTPServer(("127.0.0.1", a.port), Handler).serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
