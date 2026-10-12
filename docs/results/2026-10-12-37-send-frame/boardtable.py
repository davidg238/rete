#!/usr/bin/env python3
"""boardtable.py DIR GLOB_A GLOB_B [LABEL_A LABEL_B] — per row, A's and B's runs (min–max) and mean B / mean A
(rete#37 L1/L2 boards). Rows under 50 ms (NBody at the board count) are shown but kept out of the geomean."""
import glob, math, re, sys
d, ga, gb = sys.argv[1:4]
la, lb = (sys.argv[4:6] + ["A", "B"])[:2] if len(sys.argv) > 5 else ("A", "B")
ROW = re.compile(r"< (\w+): (\d+)\r?$", re.M)
SKIP = {"hanoi8", "Expected"}
def runs(g):
    out = {}
    for f in sorted(glob.glob(f"{d}/{g}")):
        text = open(f).read()
        # A boot that runs out of heap (the C6's AWFY at Json) reboots and runs again: keep its first pass,
        # the rows before the fault. A boot with no fault must have finished.
        cut = min([i for i in (text.find("Out of Memory"), text.find("FATAL")) if i >= 0], default=-1)
        if cut >= 0: text = text[:cut]
        elif "< done" not in text: sys.exit(f"boardtable: {f} did not finish")
        if "verified:" in text and "verified: true" not in text: sys.exit(f"boardtable: {f} did not verify")
        for k, v in ROW.findall(text):
            if k not in SKIP: out.setdefault(k, []).append(int(v))
    return out
a, b = runs(ga), runs(gb)
rng = lambda v: f"{min(v)}" if min(v) == max(v) else f"{min(v)}–{max(v)}"
print(f"| row (ms) | {la} ×{len(next(iter(a.values())))} | {lb} ×{len(next(iter(b.values())))} | {lb} ÷ {la} |\n|---|--:|--:|--:|")
rs = []
for k in a:
    if k not in b: continue
    r = (sum(b[k]) / len(b[k])) / (sum(a[k]) / len(a[k]))
    if min(a[k]) >= 50: rs.append(r)
    print(f"| {k} | {rng(a[k])} | {rng(b[k])} | {r:.3f} |")
print(f"\nGeomean {lb} ÷ {la} over {len(rs)} rows (≥ 50 ms): {math.exp(sum(map(math.log, rs)) / len(rs)):.3f}")
