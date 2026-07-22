import cbor2
from codec import decode, encode, span_ctl, Sym

def test_decode_record_flattens_tag27():
    raw = cbor2.dumps(cbor2.CBORTag(27, [cbor2.CBORTag(39, "SpanCapture"),
                                         {cbor2.CBORTag(39, "n"): 2}]),
                      canonical=True)
    assert decode(raw) == {"_tag": "SpanCapture", "n": 2}

def test_span_ctl_roundtrips_to_a_running_record():
    obj = cbor2.loads(span_ctl("running"))
    assert obj.tag == 27
    assert obj.value[0] == cbor2.CBORTag(39, "SpanCtl")
    assert obj.value[1] == {cbor2.CBORTag(39, "state"): cbor2.CBORTag(39, "running")}
