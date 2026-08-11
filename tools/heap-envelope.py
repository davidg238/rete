#!/usr/bin/env python3
# Copyright (c) 2026 Ekorau LLC
"""Per-GC-cycle heap envelope over a porta soak store — the #153 gauge.

WHY THIS EXISTS, and why a plain time series will not do. GC on this VM fires only
on arena exhaustion, so every field of `sys/metrics` is a sawtooth whose period is
one GC cycle. Two samples minutes apart are two arbitrary phases of that sawtooth,
not a trend — reading them as one is what opened #153 (`largestFreeBlock` 64 kB ->
9 kB in nine minutes, which turned out to be the sawtooth and nothing else).

So bucket by `gcCycles` first, then trend the per-cycle statistic:

    live floor  = min(liveBytes)         post-sweep live  -> THE LEAK GAUGE
    lfb ceiling = max(largestFreeBlock)  fragmentation of a freshly swept heap
    lfb trough  = min(largestFreeBlock)  NOT A GAUGE — see below

`review/tests/CLAIMS.md` owns the mechanism: on a collect-when-full VM the sample just
before a collection reports whatever the failing allocation left behind, so the TROUGH is
the size of the request that tipped the arena over, not the state of the heap (26.1% of
3682 field samples sit below 9260 B; the global minimum is 8 B). It is printed here only
so that a reader who goes looking for it sees why it must be ignored — trending it looks
respectable and flips sign between adjacent slices of one flat boot.

Rules the tool enforces, all earned by falsifying the instrument (#153):

  * the first WARMUP cycles are discarded — they run on an arena that has never
    been swept, so they are not comparable to steady state and they dominate any
    trend fitted across them;
  * a `largestFreeBlock` trend from a window under ~5 h means nothing. The ceiling
    carries a slow oscillation of period hours: slicing a run that is FLAT over
    9.95 h into 1.5 h windows makes this same test alternate between "falling,
    7.1 sigma" and "rising, 8.5 sigma". --slice reproduces that check on demand.

Cycles, not wall-clock, are the unit: a node bridging at 2 s burns GC cycles ~14x
faster than the bell at 30 s, so 45 min there is a longer test than 10 h on the bell.

The CBOR decoder is hand-rolled and closed, because it has to be: a Record is
tag 27 -> [Symbol, Map] and the record NAME is itself a Symbol (tag 39), which no
general CBOR library speaks. `nodus/nsl/lib/kernel.nsl` class Cbor is the authority.
Validate any change against porta's own decode before trusting a number:

    ssh gw85224-01 'cd ~/porta-v2 && ./porta log c6-bell --db <db> --limit 30'

USAGE — the gateway has no sqlite3, so copy the store (WAL included) and read it here:

    scp gw85224-01:'~/porta-v2/soak-2026-08-04.db*' /tmp/
    tools/heap-envelope.py /tmp/soak-2026-08-04.db c6-bell
    tools/heap-envelope.py /tmp/soak-2026-08-04.db c6-bench --build <build-id>
    tools/heap-envelope.py /tmp/soak-2026-08-04.db c6-bell --delivery
    tools/heap-envelope.py /tmp/soak-2026-08-04.db c6-bell --slice 5

`--build` segments one space by `obs/build`, which is what #186 forces: the
`c6-bench` space is a SLOT, not a board, so rows from different devkits are
commingled and only a build id separates them.
"""
import argparse, datetime, sqlite3, struct, sys

WARMUP = 5


class Sym(str):
    __slots__ = ()
    def __repr__(self): return '#' + str(self)


class Rec:
    def __init__(self, name, fields): self.name, self.fields = name, fields


def dec(b, i=0, depth=0):
    """Closed nsl-CBOR decode. An unknown tag is an error, never pass-through."""
    if depth > 32:
        raise ValueError('depth bound exceeded')
    ib = b[i]; mt, ai = ib >> 5, ib & 0x1f; i += 1
    if ai < 24: val = ai
    elif ai == 24: val = b[i]; i += 1
    elif ai == 25: val = struct.unpack('>H', b[i:i+2])[0]; i += 2
    elif ai == 26: val = struct.unpack('>I', b[i:i+4])[0]; i += 4
    elif ai == 27: val = struct.unpack('>Q', b[i:i+8])[0]; i += 8
    else: raise ValueError('bad additional info %d' % ai)

    if mt == 0: return val, i
    if mt == 1: return -1 - val, i
    if mt == 2: return bytes(b[i:i+val]), i + val
    if mt == 3: return b[i:i+val].decode('utf-8'), i + val
    if mt == 4:
        out = []
        for _ in range(val):
            v, i = dec(b, i, depth+1); out.append(v)
        return out, i
    if mt == 5:
        out = []
        for _ in range(val):
            k, i = dec(b, i, depth+1); v, i = dec(b, i, depth+1); out.append((k, v))
        return out, i
    if mt == 6:
        if val == 39:
            s, i = dec(b, i, depth+1); return Sym(s), i
        if val == 27:
            a, i = dec(b, i, depth+1)
            if not (isinstance(a, list) and len(a) == 2):
                raise ValueError('record is not [Symbol, Map]')
            return Rec(a[0], a[1]), i
        raise ValueError('closed decode: unknown tag %d' % val)
    if mt == 7:
        if val in (20, 21): return val == 21, i
        if val == 22: return None, i
    raise ValueError('unsupported major type %d' % mt)


# rete#197 (2026-08-11) shortened every label on the push frame, so a soak db that
# spans that day holds BOTH vocabularies — the long names in its older rows and the
# short ones after. This tool reads stored history rather than the wire, so it
# normalizes onto the short scheme at the decode boundary and speaks one vocabulary
# everywhere below. That is not a dual-read of the protocol (the wire has exactly one
# encoding and no compatibility window); it is the cheapest way to keep a run that
# straddles the change readable, and it costs one dict rather than re-encoding history.
LONG_TO_SHORT = {
    'heapSize': 'hsz', 'liveBytes': 'lb', 'heapHighWater': 'hhw',
    'largestFreeBlock': 'lfb', 'gcCycles': 'gcc', 'handlesLive': 'hnl',
    'handlesHighWater': 'hnhw', 'queueHighWater': 'qhw', 'overruns': 'ovr',
    'pulseDrops': 'pdr', 'pulseCoalesced': 'pco',
    'outBytes': 'ob', 'outHigh': 'ohw', 'allocs': 'alc', 'frees': 'fre',
    'fails': 'fal',
    'uptimeMs': 'up', 'bootId': 'bid', 'wakes': 'wak',
    'pollTimeouts': 'pto', 'streamDropped': 'sdr',
}


def rec_fields(blob):
    v, n = dec(blob)
    if n != len(blob):
        raise ValueError('%d trailing bytes' % (len(blob) - n))
    if not isinstance(v, Rec):
        raise ValueError('value is not a Record')
    return v.name, {LONG_TO_SHORT.get(str(k), str(k)): val for k, val in v.fields}


def hhmm(ms):
    return datetime.datetime.fromtimestamp(ms/1000).strftime('%m-%d %H:%M')


def samples(db, node, path='sys/metrics', build=None):
    c = sqlite3.connect('file:%s?mode=ro' % db, uri=True)
    windows = None
    if build is not None:
        marks = []
        for ts, blob in c.execute("select ts_ms, value_cbor from events where "
                                  "node_id=? and path='obs/build' order by ts_ms", (node,)):
            marks.append((ts, rec_fields(blob)[1].get('build')))
        windows = []
        for idx, (ts, bid) in enumerate(marks):
            if bid != build:
                continue
            end = marks[idx+1][0] if idx + 1 < len(marks) else float('inf')
            if windows and windows[-1][1] == ts:
                windows[-1] = (windows[-1][0], end)
            else:
                windows.append((ts, end))
        if not windows:
            sys.exit('no obs/build window matches %r for node %s' % (build, node))
    out = []
    for ts, blob in c.execute("select ts_ms, value_cbor from events where node_id=? "
                              "and path=? order by ts_ms", (node, path)):
        if windows is not None and not any(lo <= ts < hi for lo, hi in windows):
            continue
        out.append((ts, rec_fields(blob)[1]))
    return out


def boots(rows):
    """Split on gcCycles going backwards — a reboot resets the counter."""
    segs, cur, prev = [], [], None
    for ts, f in rows:
        g = f['gcc']
        if prev is not None and g < prev:
            segs.append(cur); cur = []
        cur.append((ts, f)); prev = g
    if cur:
        segs.append(cur)
    return segs


def lsq(xs, ys):
    """Least-squares slope and its standard error. None if underdetermined."""
    n = len(xs)
    if n < 3:
        return None, None
    mx = sum(xs)/n; my = sum(ys)/n
    sxx = sum((x-mx)**2 for x in xs)
    if sxx == 0:
        return None, None
    slope = sum((x-mx)*(y-my) for x, y in zip(xs, ys))/sxx
    resid = [y - (my + slope*(x-mx)) for x, y in zip(xs, ys)]
    sd = (sum(r*r for r in resid)/(n-2))**0.5
    return slope, sd/(sxx**0.5)


def cycles(seg):
    by = {}
    for _, f in seg:
        by.setdefault(f['gcc'], []).append(f)
    return sorted(k for k in by if k >= WARMUP), by


GAUGES = (('live floor (leak gauge)', 'lb', min),
          ('lfb ceiling (post-sweep)', 'lfb', max),
          ('lfb trough  (NOT a gauge)', 'lfb', min))


def verdict(s, se):
    if s is None or not se:
        return '?'
    return 'FLAT' if abs(s) < 2*se else ('rising' if s > 0 else 'FALLING')


def report(label, rows, min_cycles):
    print('=' * 84)
    print(label)
    if not rows:
        print('  no samples'); return
    for si, seg in enumerate(boots(rows)):
        ks, by = cycles(seg)
        span = (seg[-1][0] - seg[0][0]) / 3.6e6
        head = '  boot %-2d %s..%s  %5.2f h  %4d cycles' % (
            si, hhmm(seg[0][0]), hhmm(seg[-1][0]), span, len(ks))
        if len(ks) < min_cycles:
            print(head + '  — SKIPPED (< %d post-warm-up cycles)' % min_cycles)
            continue
        d = sorted(seg[i+1][0]-seg[i][0] for i in range(len(seg)-1))
        cad = (d[len(d)//2] if d else 30000)/1000.0
        exp = int(span * 3600 / cad) + 1
        print('-' * 84)
        print(head + '  (%.2f min/cycle)' % (span*60/max(1, ks[-1]-ks[0])))
        print('  samples %d  (cadence %.0f s, expected ~%d -> %.1f%% delivered)'
              % (len(seg), cad, exp, 100.0*len(seg)/exp))
        if span < 5:
            print('  NOTE: window < 5 h — treat any lfb slope below as unresolved '
                  '(see --slice)')
        for nm, field, stat in GAUGES:
            ys = [stat(f[field] for f in by[k]) for k in ks]
            s, se = lsq(ks, ys)
            print('    %-26s mean %7.0f  slope %+8.2f B/cycle  over run %+8.0f B  '
                  '%4.1f sigma  %s'
                  % (nm, sum(ys)/len(ys), s or 0, (s or 0)*(ks[-1]-ks[0]),
                     abs(s/se) if s is not None and se else 0, verdict(s, se)))
    print()


def slice_check(rows, hours):
    """Falsify the instrument: re-run the same test on sub-windows of each boot."""
    print('=' * 84)
    print('SLICE CHECK — same test, %.1f h windows. Disagreeing verdicts within one'
          % hours)
    print('boot mean the window is too short to resolve a trend, not that one exists.')
    W = int(hours*3600*1000)
    for si, seg in enumerate(boots(rows)):
        if (seg[-1][0]-seg[0][0]) < 2*W:
            continue
        print('-' * 84)
        print('  boot %d (%s .. %s)' % (si, hhmm(seg[0][0]), hhmm(seg[-1][0])))
        print('  %-14s %6s  %26s %26s' % ('window', 'cycles', 'lfb ceiling', 'lfb trough'))
        t0 = seg[0][0]
        while t0 < seg[-1][0]:
            sub = [(ts, f) for ts, f in seg if t0 <= ts < t0+W]
            ks, by = cycles(sub)
            if len(ks) >= 40:
                line = '  %-14s %6d' % (hhmm(t0), len(ks))
                for _, field, stat in (GAUGES[1], GAUGES[2]):
                    ys = [stat(f[field] for f in by[k]) for k in ks]
                    s, se = lsq(ks, ys)
                    line += '  %+9.1f B/cyc %4.1f %-8s' % (
                        s or 0, abs(s/se) if s is not None and se else 0, verdict(s, se))
                print(line)
            t0 += W
    print()


def delivery(db, node, bucket_min):
    """Gateway-side loss, independent of any node console: arrivals vs cadence."""
    c = sqlite3.connect('file:%s?mode=ro' % db, uri=True)
    ts = [r[0] for r in c.execute("select ts_ms from events where node_id=? and "
                                  "path='sys/metrics' order by ts_ms", (node,))]
    if len(ts) < 3:
        print('%s: too few samples for a delivery estimate' % node); return
    d = sorted(ts[i+1]-ts[i] for i in range(len(ts)-1))
    cad = d[len(d)//2]
    print('=' * 84)
    print('%s — gateway-side delivery, inferred cadence %.1f s' % (node, cad/1000))
    print('  %-14s %7s %7s %7s   %s'
          % ('bucket', 'arrived', 'expect', 'miss%', 'gaps>2x cadence'))
    b0 = ts[0]
    while b0 < ts[-1]:
        b1 = b0 + bucket_min*60000
        inb = [t for t in ts if b0 <= t < b1]
        if len(inb) > 1:
            exp = int((min(b1, ts[-1]) - b0)/cad)
            gaps = sum(1 for i in range(len(inb)-1) if inb[i+1]-inb[i] > 2*cad)
            print('  %-14s %7d %7d %6.1f%%   %d'
                  % (hhmm(b0), len(inb), exp, max(0.0, 100*(1-len(inb)/exp)) if exp else 0,
                     gaps))
        b0 = b1
    print()


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('db')
    p.add_argument('node')
    p.add_argument('--build', help='restrict to the obs/build window naming this build id')
    p.add_argument('--min-cycles', type=int, default=40,
                   help='skip boot segments with fewer post-warm-up GC cycles (default 40)')
    p.add_argument('--slice', type=float, metavar='H',
                   help='re-run the trend on H-hour sub-windows to expose window artefacts')
    p.add_argument('--delivery', action='store_true',
                   help='report gateway-side arrival rate instead of the envelope')
    p.add_argument('--bucket', type=int, default=15, help='--delivery bucket, minutes')
    a = p.parse_args()

    if a.delivery:
        delivery(a.db, a.node, a.bucket); return
    rows = samples(a.db, a.node, build=a.build)
    label = a.node + (' (build %s)' % a.build if a.build else ' — all boots')
    report(label, rows, a.min_cycles)
    if a.slice:
        slice_check(rows, a.slice)


if __name__ == '__main__':
    main()
