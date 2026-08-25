from store import Store
from mirror import Mirror
from codec import Sym, record

# Fixtures are built through codec.record (rete#257): codec's own bytes are
# pinned against node-generated goldens in test_codec.py, so these are not
# self-referential.

def _result_bytes(n):
    return record("SpanCapture", {Sym("n"): n, Sym("ms"): 1000, Sym("slots"): []})

def test_datagram_lands_as_cell_and_event(tmp_path):
    s = Store(str(tmp_path / "p.db"))
    m = Mirror(s, bind_port=0)
    m.handle_datagram(_result_bytes(7), ("192.168.0.240", 54371), ts=100.0)
    assert s.latest_cell("192.168.0.240", "diag/spans/result")["n"] == 7
    assert s.event_count("192.168.0.240") == 1

def test_bad_cbor_is_dropped_not_raised(tmp_path):
    s = Store(str(tmp_path / "p.db"))
    m = Mirror(s, bind_port=0)
    m.handle_datagram(b"\xff\xff\xff", ("192.168.0.240", 54371), ts=100.0)  # garbage
    assert s.event_count("192.168.0.240") == 0

def test_truncated_cbor_is_dropped_not_raised(tmp_path):
    s = Store(str(tmp_path / "p.db"))
    m = Mirror(s, bind_port=0)
    m.handle_datagram(b"\x9b\x00\x00\x00\x00", ("192.168.0.240", 54371), ts=100.0)
    assert s.event_count("192.168.0.240") == 0


def _row_bytes(span, n):
    return record("LatencySpan", {Sym("span"): Sym(span), Sym("n"): n, Sym("minUs"): 100})


def test_latency_span_lands_at_its_own_sibling_cell(tmp_path):
    # tuvm#107: a row names its own slot, so the path comes from the record.
    s = Store(str(tmp_path / "p.db"))
    m = Mirror(s, bind_port=0)
    m.handle_datagram(_row_bytes("acquire", 3), ("192.168.0.240", 54371), ts=100.0)
    assert s.latest_cell("192.168.0.240", "diag/spans/acquire")["n"] == 3


def test_latency_span_without_a_name_is_dropped(tmp_path):
    # No slot name means no path. Storing it somewhere plausible would attribute
    # one slot's numbers to another, which is worse than losing the datagram.
    s = Store(str(tmp_path / "p.db"))
    m = Mirror(s, bind_port=0)
    raw = record("LatencySpan", {Sym("n"): 3})
    m.handle_datagram(raw, ("192.168.0.240", 54371), ts=100.0)
    assert s.event_count("192.168.0.240") == 0
