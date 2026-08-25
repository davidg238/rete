"""CBOR <-> algebra for the node's dialect (RFC 8949, tags 27/39).
See ~/workspace/tuvm/review/lib/cbor.md: Record=tag27+[tag39(name),map],
Symbol=tag39+text. cbor2 reads this with zero custom code."""
import cbor2


class Sym(str):
    """A decoded symbol (tag 39). A str subclass so it renders plainly."""


def _lower(x):
    if isinstance(x, cbor2.CBORTag) and x.tag == 39:
        return Sym(x.value)
    if isinstance(x, cbor2.CBORTag) and x.tag == 27:
        name, fields = x.value
        out = {"_tag": str(_lower(name))}
        for k, v in fields.items():
            out[str(_lower(k))] = _lower(v)
        return out
    if isinstance(x, list):
        return [_lower(v) for v in x]
    if isinstance(x, dict):
        return {str(_lower(k)): _lower(v) for k, v in x.items()}
    return x


def decode(raw: bytes):
    return _lower(cbor2.loads(raw))


def encode(obj) -> bytes:
    # only the shapes the profiler sends are supported (ctl records)
    raise NotImplementedError("use span_ctl for the one record the gw emits")


def span_ctl(state: str) -> bytes:
    """The #SpanCtl datagram the node's SpanCapture accepts. Canonical so the
    node's deterministic decoder sees shortest-form, sorted keys (cbor.md rule 1)."""
    rec = cbor2.CBORTag(27, [cbor2.CBORTag(39, "SpanCtl"),
                             {cbor2.CBORTag(39, "state"): cbor2.CBORTag(39, state)}])
    return cbor2.dumps(rec, canonical=True)
