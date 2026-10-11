#!/usr/bin/env python3
"""table.py — STIC on c6spare: Opt #8 vs the send levers, three boots each (logs/c6-stic-{opt8,levers}-{1,2,3}.log)."""
import re, glob, os
D = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs")
def rows(tag):
    runs = []
    for f in sorted(glob.glob(f"{D}/c6-stic-{tag}-[123].log")):
        runs.append({m[1]: int(m[2]) for m in re.finditer(r"< (\w+): (\d+)$", open(f).read(), re.M)})
    return runs
a, b = rows("opt8"), rows("levers")
print(f"| test (ms, C6) | Opt #8 ×{len(a)} | levers ×{len(b)} | levers ÷ Opt #8 |\n|---|---:|---:|---:|")
for k in a[0]:
    if k in ("hanoi8", "Expected"): continue
    va, vb = [r[k] for r in a], [r[k] for r in b]
    fa = f"{min(va)}" if min(va) == max(va) else f"{min(va)}–{max(va)}"
    fb = f"{min(vb)}" if min(vb) == max(vb) else f"{min(vb)}–{max(vb)}"
    print(f"| {k} | {fa} | {fb} | {sum(vb)/len(vb)/(sum(va)/len(va)):.2f} |")
