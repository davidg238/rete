#!/usr/bin/env python3
"""table.py RUN A B — per-row min over passes, B ÷ A, geomean over rows ≥ 50 ms, from run_ab.sh's logs."""
import collections, glob, math, re, sys
d, A, B = sys.argv[1:4]
T = collections.defaultdict(dict)
for f in glob.glob(d + "/*.log"):
    m = re.match(r".*/(\w+)-(awfy|performance)-(\d)\.log$", f)
    if not m: continue
    for l in open(f):
        r = re.match(r"\S+ < (\w+): (\d+)\s*$", l)
        if r: T[(m[2], r[1])][(m[1], m[3])] = int(r[2])
for suite in ("awfy", "performance"):
    print(f"### {suite}: {B} ÷ {A}\n\n| row (ms) | {A} 1 | {A} 2 | {B} 1 | {B} 2 | {B} ÷ {A} |\n|---|--:|--:|--:|--:|--:|")
    g = []
    for (s, row), v in sorted(T.items()):
        if s != suite: continue
        a = min(v.get((A, k), math.inf) for k in "12"); b = min(v.get((B, k), math.inf) for k in "12")
        if a >= 50: g.append(b / a)
        print(f"| {row} | {v.get((A,'1'))} | {v.get((A,'2'))} | {v.get((B,'1'))} | {v.get((B,'2'))} | {b/a:.3f} |")
    print(f"\nGeomean over {len(g)} rows (≥ 50 ms): {math.exp(sum(map(math.log, g)) / len(g)):.3f}\n")
