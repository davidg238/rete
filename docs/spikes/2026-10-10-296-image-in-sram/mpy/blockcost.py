# blockcost.py — 00_perf/blockcost.nsl's rows at the Spike296Node scale (1M inline,
# 100k outer), in the Python shape each row would naturally take. Where the two
# languages differ in what the row IS, both shapes are given:
#   baseLoop     for i in range(N): i + 1             nsl: inlined 1 to: N do:
#   blockValue   repeat(N, lambda i: i + 1)           nsl: aBlock value: i  (one call/elem)
#   methodSend   self.bump(i)                         nsl: self bump: i
#   blockCapture closure writing a nonlocal           nsl: block writes a captured temp
#   closureMake  a fresh lambda per iteration         nsl: fresh [ 42 ] per iteration
#   ocDo         for e in oc: e   (no call per elem)  nsl: oc do: [ :e | e ] = a call/elem
#   ocDoCall     for e in oc: f(e)                    the nsl shape, honestly
#   ocAt         for j in range(10): oc[j]            nsl: oc at: j
import time
try:
    _ticks = time.ticks_ms
    _diff = time.ticks_diff
except AttributeError:
    _ticks = lambda: int(time.monotonic() * 1000)
    _diff = lambda a, b: a - b

N = 1000000
M = 100000

class BC:
    def bump(self, i):
        return i + 1
    def repeat(self, n, f):
        for i in range(n):
            f(i)
    def baseLoop(self):
        for i in range(N):
            i + 1
    def blockValue(self):
        self.repeat(N, lambda i: i + 1)
    def methodSend(self):
        for i in range(N):
            self.bump(i)
    def blockCapture(self):
        s = 0
        def f(i):
            nonlocal s
            s = s + 1
        self.repeat(N, f)
    def closureMake(self):
        for i in range(N):
            f = lambda: 42
    def ocDo(self, oc):
        for i in range(M):
            for e in oc:
                e
    def ocDoCall(self, oc):
        f = lambda e: e
        for i in range(M):
            for e in oc:
                f(e)
    def ocAt(self, oc):
        for i in range(M):
            for j in range(10):
                oc[j]

def timed(name, fn, *a):
    t0 = _ticks()
    fn(*a)
    print("%s: %d" % (name, _diff(_ticks(), t0)))

p = BC()
oc = [k for k in range(1, 11)]
timed("baseLoop", p.baseLoop)
timed("blockValue", p.blockValue)
timed("methodSend", p.methodSend)
timed("blockCapture", p.blockCapture)
timed("closureMake", p.closureMake)
timed("ocDo", p.ocDo, oc)
timed("ocDoCall", p.ocDoCall, oc)
timed("ocAt", p.ocAt, oc)
print("done")
