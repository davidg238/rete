"""Bytes to lines. No interpretation of what the lines mean (design D3)."""


class LineAssembler:
    def __init__(self):
        self._buf = b""

    def feed(self, data):
        self._buf += data.replace(b"\x00", b"")
        out = []
        while True:
            idx = self._buf.find(b"\n")
            if idx < 0:
                return out
            raw, self._buf = self._buf[:idx], self._buf[idx + 1:]
            out.append(raw.rstrip(b"\r").decode("utf-8", errors="replace"))

    def flush(self):
        if not self._buf:
            return []
        raw, self._buf = self._buf, b""
        return [raw.rstrip(b"\r").decode("utf-8", errors="replace")]
