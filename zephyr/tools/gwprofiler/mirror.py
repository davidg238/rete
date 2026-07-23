"""porta-lite collector. Binds the diag UDP port, decodes each datagram into the
store, sends ctl records. Knows only paths, not what a 'span' is — the next thing
a node pushes is a new profweb view, not new code here. Design §5."""
import socket
import time

import codec

# The one path this slice mirrors; a record's _tag maps to its cell path.
_TAG_TO_PATH = {"SpanCapture": "diag/spans/result"}


class Mirror:
    def __init__(self, store, bind_port=54371):
        self.store = store
        self.bind_port = bind_port
        self._seq = {}   # node_id -> monotonic seq assigned on write

    def _next_seq(self, node_id):
        n = self._seq.get(node_id, 0) + 1
        self._seq[node_id] = n
        return n

    def handle_datagram(self, raw, addr, ts):
        node_id = addr[0]
        try:
            value = codec.decode(raw)
        except Exception:
            return  # closed algebra: a bad datagram is dropped, never stored
        if not isinstance(value, dict) or "_tag" not in value:
            return
        path = _TAG_TO_PATH.get(value["_tag"])
        if path is None:
            return
        seq = self._next_seq(node_id)
        self.store.write_event(node_id, path, seq, value, ts, "in")
        self.store.upsert_cell(node_id, path, seq, value, ts)

    def send_ctl(self, node_addr, node_port, state):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.sendto(codec.span_ctl(state), (node_addr, node_port))
        finally:
            s.close()

    def run(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("0.0.0.0", self.bind_port))
        print(f"[mirror] collecting on :{self.bind_port}")
        while True:
            raw, addr = s.recvfrom(2048)
            self.handle_datagram(raw, addr, time.time())
