from store import Store
from mirror import Mirror
import codec, cbor2

def _result_bytes(n):
    rec = cbor2.CBORTag(27, [cbor2.CBORTag(39, "SpanCapture"),
                             {cbor2.CBORTag(39, "n"): n,
                              cbor2.CBORTag(39, "ms"): 1000,
                              cbor2.CBORTag(39, "slots"): []}])
    return cbor2.dumps(rec, canonical=True)

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
    rec = cbor2.CBORTag(27, [cbor2.CBORTag(39, "LatencySpan"),
                             {cbor2.CBORTag(39, "span"): cbor2.CBORTag(39, span),
                              cbor2.CBORTag(39, "n"): n,
                              cbor2.CBORTag(39, "minUs"): 100}])
    return cbor2.dumps(rec, canonical=True)


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
    rec = cbor2.CBORTag(27, [cbor2.CBORTag(39, "LatencySpan"),
                             {cbor2.CBORTag(39, "n"): 3}])
    m.handle_datagram(cbor2.dumps(rec, canonical=True), ("192.168.0.240", 54371), ts=100.0)
    assert s.event_count("192.168.0.240") == 0
