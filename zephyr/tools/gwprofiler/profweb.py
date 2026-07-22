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
_COLS = [("span", "span"), ("lang", "lang"), ("n", "n"),
         ("fails", "fails"), ("minUs", "min µs"), ("maxUs", "max µs")]


def render_span_table(cell):
    if not cell or not cell.get("slots"):
        return "<p>No capture taken.</p>"
    head = "".join(f"<th>{h}</th>" for _, h in _COLS)
    rows = []
    for slot in cell["slots"]:
        tds = "".join(f"<td>{html.escape(str(slot.get(k, '')))}</td>"
                      for k, _ in _COLS)
        rows.append(f"<tr>{tds}</tr>")
    return (f"<p><b>{cell.get('n', 0)}</b> samples over "
            f"<b>{cell.get('ms', -1)}</b> ms.</p>"
            f"<table><thead><tr>{head}</tr></thead>"
            f"<tbody>{''.join(rows)}</tbody></table>")


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
            self._send(f"<html><body><h1>node {html.escape(node)}</h1>"
                       f'<form method="post" action="/run?node={html.escape(node)}">'
                       f'<button>Run capture</button></form>'
                       f"{render_span_table(cell)}</body></html>")

        def do_POST(self):
            q = parse_qs(urlparse(self.path).query)
            node = q.get("node", [node_default])[0]
            mirror.send_ctl(node, 54370, "running")
            time.sleep(3.0)              # bracket the node's own thermal ticks
            mirror.send_ctl(node, 54370, "stopped")
            self.send_response(303)
            self.send_header("Location", f"/?node={node}")
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
