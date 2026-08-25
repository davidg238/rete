import pytest
from codec import decode, dumps, record, span_ctl, Sym, DecodeError

# Goldens generated FROM THE NODE's own encoder (`platform Cbor encode:` via a
# scratch .nsl through run-one.py, 2026-08-25) — never derived by reading either
# codec, which is the discipline that made the first porta<->node protocol run
# agree with no wire fixes. The Map golden is the node's own published one
# (nodus/tests/14_cbor/02_golden_hex.nsl).
GOLDEN_SPANCTL_RUNNING = "d81b82d827675370616e43746ca1d827657374617465d8276772756e6e696e67"
GOLDEN_SPANCTL_STOPPED = "d81b82d827675370616e43746ca1d827657374617465d8276773746f70706564"
GOLDEN_SPANCAPTURE_N2 = "d81b82d8276b5370616e43617074757265a1d827616e02"
GOLDEN_MAP_N42_S_HI = "a2d827616e182ad8276173626869"


def test_decode_record_flattens_tag27():
    assert decode(bytes.fromhex(GOLDEN_SPANCAPTURE_N2)) == {"_tag": "SpanCapture", "n": 2}


def test_span_ctl_matches_the_node_golden():
    assert span_ctl("running").hex() == GOLDEN_SPANCTL_RUNNING
    assert span_ctl("stopped").hex() == GOLDEN_SPANCTL_STOPPED


def test_dumps_canonical_map_matches_the_node_golden():
    # Two symbol keys — exercises the sorted-by-encoded-bytes rule.
    assert dumps({Sym("n"): 42, Sym("s"): "hi"}).hex() == GOLDEN_MAP_N42_S_HI


def test_record_roundtrips_through_decode():
    raw = record("LatencySpan", {Sym("span"): Sym("checkIn"), Sym("n"): 7, Sym("minUs"): 100})
    assert decode(raw) == {"_tag": "LatencySpan", "span": "checkIn", "n": 7, "minUs": 100}


def test_garbage_and_truncation_raise_decode_error():
    with pytest.raises(DecodeError):
        decode(b"\xff\xff\xff")
    with pytest.raises(DecodeError):
        decode(b"\x9b\x00\x00\x00\x00")  # truncated 8-byte array head
    with pytest.raises(DecodeError):
        decode(bytes.fromhex(GOLDEN_SPANCAPTURE_N2) + b"\x00")  # trailing byte
