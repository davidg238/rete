"""CBOR <-> algebra for the node's dialect (RFC 8949, tags 27/39).
Record = tag27+[tag39(name), map], Symbol = tag39+text (see review/lib/cbor.md).

Hand-rolled on the stdlib (rete#257). This used to import cbor2, which made
gwprofiler the only thing in the tree with a third-party codec dependency —
and the bench, where the tool actually runs, did not have it, so the profiler
and its tests were both dead there. The subset below is exactly the dialect's:
definite lengths only, tags 27/39, canonical encode (shortest-form heads, map
keys sorted bytewise over their ENCODED bytes — cbor.md rule 1). Correctness
is pinned in test_codec.py against goldens generated FROM THE NODE's own
encoder, not derived by reading either implementation."""
import struct


class Sym(str):
    """A decoded symbol (tag 39). A str subclass so it renders plainly."""


class Tag:
    """A tag this dialect does not lower (anything but 27/39). Kept so a
    foreign datagram decodes rather than raises, and shows what it was."""
    __slots__ = ("tag", "value")

    def __init__(self, tag, value):
        self.tag, self.value = tag, value

    def __repr__(self):
        return f"Tag({self.tag}, {self.value!r})"

    def __eq__(self, other):
        return isinstance(other, Tag) and (self.tag, self.value) == (other.tag, other.value)


class DecodeError(ValueError):
    pass


# -- decode ----------------------------------------------------------------

def _head(raw, i):
    if i >= len(raw):
        raise DecodeError("truncated head")
    ib = raw[i]
    major, info = ib >> 5, ib & 0x1F
    i += 1
    if info < 24:
        return major, info, info, i
    if info == 24 or info == 25 or info == 26 or info == 27:
        n = 1 << (info - 24)
        if i + n > len(raw):
            raise DecodeError("truncated argument")
        return major, info, int.from_bytes(raw[i:i + n], "big"), i + n
    raise DecodeError(f"indefinite/reserved head 0x{ib:02x} — the dialect never emits it")


def _item(raw, i):
    major, info, arg, i = _head(raw, i)
    if major == 0:
        return arg, i
    if major == 1:
        return -1 - arg, i
    if major == 2 or major == 3:
        if i + arg > len(raw):
            raise DecodeError("truncated string body")
        body = raw[i:i + arg]
        return (body if major == 2 else body.decode("utf-8")), i + arg
    if major == 4:
        out = []
        for _ in range(arg):
            v, i = _item(raw, i)
            out.append(v)
        return out, i
    if major == 5:
        out = {}
        for _ in range(arg):
            k, i = _item(raw, i)
            v, i = _item(raw, i)
            out[str(k)] = v
        return out, i
    if major == 6:
        v, i = _item(raw, i)
        if arg == 39:
            if not isinstance(v, str):
                raise DecodeError("tag 39 over a non-text item")
            return Sym(v), i
        if arg == 27:
            if not (isinstance(v, list) and len(v) == 2 and isinstance(v[0], Sym)
                    and isinstance(v[1], dict)):
                raise DecodeError("tag 27 is not [tag39(name), map]")
            out = {"_tag": str(v[0])}
            out.update(v[1])
            return out, i
        return Tag(arg, v), i
    # major 7: the argument bytes ARE the float bits, told apart by info width.
    if info == 25:
        return struct.unpack(">e", arg.to_bytes(2, "big"))[0], i
    if info == 26:
        return struct.unpack(">f", arg.to_bytes(4, "big"))[0], i
    if info == 27:
        return struct.unpack(">d", arg.to_bytes(8, "big"))[0], i
    if arg == 20:
        return False, i
    if arg == 21:
        return True, i
    if arg == 22:
        return None, i
    raise DecodeError(f"major 7 value {arg} unsupported")


def decode(raw: bytes):
    v, i = _item(raw, 0)
    if i != len(raw):
        raise DecodeError(f"{len(raw) - i} trailing byte(s)")
    return v


# -- encode (canonical) ----------------------------------------------------

def _enc_head(major, n):
    if n < 24:
        return bytes([(major << 5) | n])
    for info, width in ((24, 1), (25, 2), (26, 4), (27, 8)):
        if n < (1 << (8 * width)):
            return bytes([(major << 5) | info]) + n.to_bytes(width, "big")
    raise ValueError("argument too large")


def dumps(obj) -> bytes:
    """Canonical encode of the dialect subset: shortest-form heads, map keys
    sorted bytewise over their encoded bytes. Sym before str — Sym IS a str."""
    if isinstance(obj, Sym):
        return b"\xd8\x27" + dumps(str.__str__(obj))
    if isinstance(obj, bool):
        return b"\xf5" if obj else b"\xf4"
    if obj is None:
        return b"\xf6"
    if isinstance(obj, int):
        return _enc_head(0, obj) if obj >= 0 else _enc_head(1, -1 - obj)
    if isinstance(obj, bytes):
        return _enc_head(2, len(obj)) + obj
    if isinstance(obj, str):
        b = obj.encode("utf-8")
        return _enc_head(3, len(b)) + b
    if isinstance(obj, list):
        return _enc_head(4, len(obj)) + b"".join(dumps(v) for v in obj)
    if isinstance(obj, dict):
        pairs = sorted((dumps(k), dumps(v)) for k, v in obj.items())
        return _enc_head(5, len(pairs)) + b"".join(k + v for k, v in pairs)
    raise ValueError(f"cannot encode {type(obj).__name__}")


def record(name: str, fields: dict) -> bytes:
    """tag27 + [tag39(name), map] — the dialect's Record, canonical."""
    return b"\xd8\x1b" + _enc_head(4, 2) + dumps(Sym(name)) + dumps(fields)


def encode(obj) -> bytes:
    # only the shapes the profiler sends are supported (ctl records)
    raise NotImplementedError("use span_ctl for the one record the gw emits")


def span_ctl(state: str) -> bytes:
    """The #SpanCtl datagram the node's SpanCapture accepts. Canonical so the
    node's deterministic decoder sees shortest-form, sorted keys (cbor.md rule 1)."""
    return record("SpanCtl", {Sym("state"): Sym(state)})
