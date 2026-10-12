#!/usr/bin/env python3
"""The table for run_all.sh: per row, the median of each VM's runs and nsl / Soup (rete#37)."""
import math
import re
import statistics
import sys
from pathlib import Path

logs = Path(sys.argv[1])
ROW = re.compile(r"^(\w+): (\d+)$", re.M)


def runs(vm):
    out = {}
    for p in sorted(logs.glob(f"{vm}-*.log")):
        text = p.read_text()
        if "verified: true" not in text:
            sys.exit(f"table: {p.name} did not verify")
        for name, ms in ROW.findall(text):
            out.setdefault(name, []).append(int(ms))
    return out


nsl, soup = runs("nsl"), runs("soup")
rows = sorted(((n, statistics.median(nsl[n]), statistics.median(soup[n])) for n in nsl if n in soup),
              key=lambda r: -r[1] / r[2])
print("| row | nsl ms (median) | nsl runs | Soup ms (median) | Soup runs | nsl ÷ Soup |")
print("|---|--:|--:|--:|--:|--:|")
for n, a, b in rows:
    print(f"| {n} | {a:g} | {'/'.join(map(str, nsl[n]))} | {b:g} | {'/'.join(map(str, soup[n]))} | {a / b:.2f} |")
geo = math.exp(sum(math.log(a / b) for _, a, b in rows) / len(rows))
print(f"\nGeomean nsl ÷ Soup over {len(rows)} rows: {geo:.2f}")
