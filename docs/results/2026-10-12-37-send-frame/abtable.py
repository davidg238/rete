#!/usr/bin/env python3
"""The table for ab.sh: per AWFY row, the median of A's runs and of B's, and B / A (rete#37)."""
import math
import re
import statistics
import sys
from pathlib import Path

logs = Path(sys.argv[1])
ROW = re.compile(r"^(\w+): (\d+)$", re.M)


def runs(tag):
    out = {}
    for p in sorted(logs.glob(f"{tag}-*.log")):
        text = p.read_text()
        if "verified: true" not in text:
            sys.exit(f"abtable: {p.name} did not verify")
        for name, ms in ROW.findall(text):
            out.setdefault(name, []).append(int(ms))
    return out


a, b = runs("a"), runs("b")
rows = [(n, statistics.median(a[n]), statistics.median(b[n]), min(a[n]), min(b[n])) for n in a if n in b]
print("| row | A ms (median) | B ms (median) | B ÷ A | A min | B min |")
print("|---|--:|--:|--:|--:|--:|")
for n, ma, mb, na, nb in rows:
    print(f"| {n} | {ma:g} | {mb:g} | {mb / ma:.3f} | {na} | {nb} |")
geo = math.exp(sum(math.log(mb / ma) for _, ma, mb, _, _ in rows) / len(rows))
print(f"\nGeomean B ÷ A over {len(rows)} rows (medians): {geo:.3f}; runs per side: {len(next(iter(a.values())))}")
