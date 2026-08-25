"""porta-lite web view (:8086). Renders the latest diag/spans/result for a node
from the store. NEVER opens a socket to a node — that is the structural defence
against tuvm#73 (design §3). Drives a run via mirror.send_ctl; the node pushes
the result asynchronously (design §5.1)."""
import argparse
import html
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

from store import Store
from mirror import Mirror

PORT = 8086
CTL_PORT = 54370
WINDOW_S = 3.0
PUSH_WAIT_S = 6.0          # how long to wait for the node's async push before rendering

# Display order and nesting. jobThermal ENCLOSES the three thermal children, so they
# are indented under it: its time is not additional to theirs, and summing the
# thermal rows double-counts.
#
# The HTTP spans (httpQueue, render, egress) are deliberately NOT shown. This page
# never opens a socket to the node, and the thermal job serves no HTTP, so they carry
# zero samples on every capture this spike takes — three permanently empty rows plus
# the paragraph explaining their emptiness cost more attention than they returned.
# They are still measured on the node and still appear on its own /stats page; if a
# capture is ever taken while real HTTP load runs, add them back here.
_ROWS = [("jobThermal", 0), ("acquire", 1), ("vmCompute", 1), ("publish", 1)]

_CSS = """
body{font:14px/1.5 system-ui,sans-serif;margin:2rem;max-width:46rem;color:#222}
h1{font-size:1.25rem;margin:0 0 .15rem}
.sub{color:#666;margin:0 0 1.2rem}
table{border-collapse:collapse;margin:.5rem 0 1rem;width:100%}
th,td{padding:.35rem .6rem;border-bottom:1px solid #eee;text-align:right}
th:first-child,td:first-child{text-align:left}
thead th{border-bottom:1px solid #bbb;font-weight:600}
td.num{font-variant-numeric:tabular-nums}
td.none{color:#bbb}
tr.child td:first-child{padding-left:1.8rem;color:#444}
tr.outer td{font-weight:600}
button{font:inherit;padding:.4rem .9rem}
dt{font-weight:600;margin-top:.5rem}
dd{margin:0 0 0 1.2rem;color:#444}
.note{color:#555;background:#f7f7f7;padding:.6rem .8rem;border-left:3px solid #ccc}
"""

_LEGEND = """
<h3>What the spans mean</h3>
<dl>
<dt>jobThermal</dt><dd><b>Outer span, nesting the three below.</b> Its time is
  <b>not</b> additional to theirs &mdash; summing the thermal rows double-counts.</dd>
<dt>acquire</dt><dd>the Grid-EYE sample fetch, through Zephyr's <code>sensor</code>
  API (I2C underneath, not raw I2C).</dd>
<dt>vmCompute</dt><dd>the 64-element raw&rarr;List copy, in nsl.</dd>
<dt>publish</dt><dd>the <code>put:</code> onto the sample cell.</dd>
</dl>
"""


def _n(v):
    """Thousands separators, tabular."""
    try:
        return "{:,}".format(int(v))
    except (TypeError, ValueError):
        return html.escape(str(v))


def render_span_table(cell, by_name):
    """cell is the diag/spans/result summary; by_name maps span name -> the
    LatencySpan read from its own sibling cell at diag/spans/<span> (tuvm#107)."""
    if not cell:
        return "<p>No capture taken yet — press <b>Run capture</b>.</p>"
    rows = []
    for name, depth in _ROWS:
        s = by_name.get(name)
        if s is None:
            continue
        n = s.get("n", 0) or 0
        fails = s.get("fails", 0) or 0
        cls = "child" if depth else ("outer" if name == "jobThermal" else "")
        if n == 0 and fails == 0:
            # NOT "0 µs" — there were no samples at all. Rendering 0 here reads as
            # "instantaneous", which is exactly the wrong conclusion.
            cells = ('<td class="num none">0</td><td class="num none">0</td>'
                     '<td class="num none">&mdash;</td><td class="num none">&mdash;</td>')
        else:
            cells = ('<td class="num">%s</td><td class="num">%s</td>'
                     '<td class="num">%s</td><td class="num">%s</td>'
                     % (_n(n), _n(fails), _n(s.get("minUs", 0)), _n(s.get("maxUs", 0))))
        rows.append('<tr class="%s"><td>%s</td>%s</tr>'
                    % (cls, html.escape(str(name)), cells))

    total = cell.get("n", 0) or 0
    ms = cell.get("ms", -1)
    job = by_name.get("jobThermal") or {}
    iters = job.get("n", 0) or 0
    lead = ("<p><b>%s</b> span samples banked over a <b>%s</b> ms window"
            % (_n(total), _n(ms)))
    if iters:
        lead += " &mdash; %s job iterations &times; 4 thermal spans" % _n(iters)
    lead += ".</p>"

    # The demo's standout, computed rather than asserted.
    acq = (by_name.get("acquire") or {}).get("minUs") or 0
    vmc = (by_name.get("vmCompute") or {}).get("minUs") or 0
    pub = (by_name.get("publish") or {}).get("minUs") or 0
    standout = ""
    if acq and (vmc or pub):
        standout = ('<p class="note">Per iteration (min): the sensor fetch costs '
                    '<b>%s&nbsp;&micro;s</b>, while all the VM-side work after it '
                    '&mdash; vmCompute + publish &mdash; costs <b>%s&nbsp;&micro;s</b>. '
                    'The I2C read, not the interpreter, is the larger half.</p>'
                    % (_n(acq), _n(vmc + pub)))

    return (lead + standout
            + "<table><thead><tr><th>span</th><th>n</th><th>fails</th>"
              "<th>min &micro;s</th><th>max &micro;s</th></tr></thead>"
            + "<tbody>" + "".join(rows) + "</tbody></table>"
            + _LEGEND)


def make_handler(store, mirror, node_default):
    class H(BaseHTTPRequestHandler):
        def _send(self, body):
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            self.wfile.write(body.encode())

        def do_GET(self):
            q = parse_qs(urlparse(self.path).query)
            node = q.get("node", [node_default])[0]
            cell = store.latest_cell(node, "diag/spans/result")
            # tuvm#107: one read per row, at the path the row was published to.
            by_name = {}
            for _name, _depth in _ROWS:
                _row = store.latest_cell(node, "diag/spans/%s" % _name)
                if _row is not None:
                    by_name[_name] = _row
            self._send(
                "<html><head><meta charset='utf-8'><title>ThermalNode spans</title>"
                "<style>%s</style></head><body>"
                "<h1>ThermalNode &mdash; AMG8833 Grid-EYE</h1>"
                "<p class='sub'>node %s &middot; porta-lite span capture</p>"
                '<form method="post" action="/run?node=%s"><button>Run capture</button></form>'
                "%s</body></html>"
                % (_CSS, html.escape(node), html.escape(node), render_span_table(cell, by_name)))

        def do_POST(self):
            q = parse_qs(urlparse(self.path).query)
            node = q.get("node", [node_default])[0]
            # The node pushes ASYNCHRONOUSLY. Redirecting the instant `stopped` is sent
            # lands the browser on the PREVIOUS capture, which reads as a stale/broken
            # page. Wait for the push to actually arrive before redirecting.
            before = store.event_count(node)
            mirror.send_ctl(node, CTL_PORT, "running")
            time.sleep(WINDOW_S)          # bracket the node's own thermal ticks
            mirror.send_ctl(node, CTL_PORT, "stopped")
            deadline = time.time() + PUSH_WAIT_S
            while time.time() < deadline:
                if store.event_count(node) > before:
                    break
                time.sleep(0.15)
            self.send_response(303)
            self.send_header("Location", "/?node=%s" % node)
            self.end_headers()

        def log_message(self, *a):
            pass
    return H


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="porta-lite.db")
    ap.add_argument("--node", default="192.168.0.240")
    args = ap.parse_args()
    store = Store(args.db)
    mirror = Mirror(store)
    srv = ThreadingHTTPServer(("0.0.0.0", PORT),
                              make_handler(store, mirror, args.node))
    print(f"[profweb] http://gw85224-01:{PORT}/")
    srv.serve_forever()


if __name__ == "__main__":
    main()
