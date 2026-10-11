#!/usr/bin/env python3
"""table.py — the STIC rows from the WeAct boot logs beside this script, before (master) against after
(37-do-inline), three boots each, as a markdown table (rete#37)."""
import re, glob, os
HERE = os.path.dirname(os.path.abspath(__file__))
ROWS = ["alloc", "arrayWrite", "dictionaryWrite", "floatMath", "integerMath", "collectionIterate",
        "collectionWrite", "stringCompare", "hanoi8"]

def boots(tag):
    out = []
    for f in sorted(glob.glob(f"{HERE}/logs/weact-stic-{tag}-flash-*.log")):
        d = {}
        for line in open(f, errors="replace"):
            m = re.search(r"< (\w+): (\d+)\s*$", line)
            if m and m.group(1) in ROWS:
                d[m.group(1)] = int(m.group(2))
        out.append(d)
    return out

b, a = boots("before"), boots("after")
print("| test (ms, WeAct) | before ×%d | after ×%d | after ÷ before |" % (len(b), len(a)))
print("|---|---:|---:|---:|")
for r in ROWS:
    bv = [x[r] for x in b if r in x]; av = [x[r] for x in a if r in x]
    fmt = lambda v: "–" if not v else (str(v[0]) if min(v) == max(v) else f"{min(v)}–{max(v)}")
    ratio = f"{(sum(av)/len(av))/(sum(bv)/len(bv)):.2f}" if av and bv and sum(bv) else ""
    print(f"| {r} | {fmt(bv)} | {fmt(av)} | {ratio} |")
