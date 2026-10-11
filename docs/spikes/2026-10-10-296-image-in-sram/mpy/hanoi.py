# hanoi.py — the 2026-10-10 RP2350 row, in Python: a METHOD (self.hanoi), depth 18,
# 262143 calls per run, 5 runs, self-timed. The nsl body is MethodProfileNodeC6.nsl's.
import time
try:
    _ticks = time.ticks_ms
    _diff = time.ticks_diff
except AttributeError:
    _ticks = lambda: int(time.monotonic() * 1000)
    _diff = lambda a, b: a - b

class H:
    def hanoi(self, n, src, dst, tmp):
        if n > 1:
            self.hanoi(n - 1, src, tmp, dst)
            self.hanoi(n - 1, tmp, dst, src)
        return n

h = H()
for i in range(5):
    t0 = _ticks()
    h.hanoi(18, 1, 2, 3)
    t1 = _ticks()
    print("hanoi %d: %d" % (i + 1, _diff(t1, t0)))
print("done")
