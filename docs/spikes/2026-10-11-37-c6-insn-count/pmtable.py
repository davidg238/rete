#!/usr/bin/env python3
"""pmtable.py LOG [LOG2] — the C6 perf counter per blockcost_pm row (rete#37 spike). With two logs, the second
is compared against the first. Bytecodes per row are the host's OEVM_INSN_COUNT (identical image)."""
import re, sys
BC = {"baseLoop": 9.0e6, "blockValue": 14.0e6, "methodSend": 13.0e6, "blockCapture": 16.0e6,
      "closureMake": 8.0e6, "ocDo": 3.5e6, "ocAt": 26.4e6}
EV = {0: "cyc", 1: "inst", 2: "ldhz", 5: "loads", 9: "takenbr"}
def read(path):
    rows, pend = {}, None
    for line in open(path):
        m = re.search(r"\[pm ev (\d+) n (\d+) ms (\d+)\]", line)
        if m: pend = (int(m[1]), int(m[2])); continue
        m = re.search(r"< (\w+): (\d+)\s*$", line)
        if m and pend and m[1] in BC:
            rows.setdefault(m[1], {})
            if pend[0] in EV: rows[m[1]][EV[pend[0]]] = pend[1]
            rows[m[1]].setdefault("ms", []).append(int(m[2])); pend = None
    return rows
logs = [read(p) for p in sys.argv[1:]]
hdr = "| row | bytecodes | cycles | native inst | CPI | native/bytecode | cycles/bytecode | ms (min–max) |"
for i, rows in enumerate(logs):
    print(f"\n{sys.argv[1+i]}\n\n{hdr}\n|---|--:|--:|--:|--:|--:|--:|--:|")
    for r in BC:
        d = rows[r]; c, n = d["cyc"], d["inst"]
        print(f"| {r} | {BC[r]/1e6:.1f}M | {c/1e6:.0f}M | {n/1e6:.0f}M | {c/n:.2f} | {n/BC[r]:.1f} | {c/BC[r]:.1f} | {min(d['ms'])}–{max(d['ms'])} |")
if len(logs) == 2:
    print("\n| row | cycles new/old | native inst new/old | ms new/old (min) |\n|---|--:|--:|--:|")
    for r in BC:
        a, b = logs[0][r], logs[1][r]
        print(f"| {r} | {b['cyc']/a['cyc']:.3f} | {b['inst']/a['inst']:.3f} | {min(b['ms'])/min(a['ms']):.3f} |")
