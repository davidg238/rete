#!/usr/bin/env python3
"""bc_table.py — blockcost on c6spare, the hot path in flash (XIP) vs in IRAM, three boots each
(logs/bc-{flash,iram}-{1,2,3}.log). Per-iteration figures divide by 10M and assume 160 MHz."""
import re, glob, os
D = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs")
ROWS = ["baseLoop", "blockValue", "methodSend", "blockCapture", "closureMake", "ocDo", "ocAt"]
def runs(tag):
    return [{m[1]: int(m[2]) for m in re.finditer(r"< (\w+): (\d+)$", open(f).read(), re.M)}
            for f in sorted(glob.glob(f"{D}/bc-{tag}-[123].log"))]
a, b = runs("flash"), runs("iram")
rng = lambda v: f"{min(v)}" if min(v) == max(v) else f"{min(v)}–{max(v)}"
print(f"| row (ms, C6) | flash ×{len(a)} | IRAM ×{len(b)} | IRAM ÷ flash |\n|---|---:|---:|---:|")
for k in ROWS:
    va, vb = [r[k] for r in a if k in r], [r[k] for r in b if k in r]
    if va and vb: print(f"| {k} | {rng(va)} | {rng(vb)} | {sum(vb)/len(vb)/(sum(va)/len(va)):.3f} |")
mean = lambda rs, k: sum(r[k] for r in rs) / len(rs)
for tag, rs in (("flash", a), ("IRAM", b)):
    base, blk, snd = (mean(rs, k) for k in ("baseLoop", "blockValue", "methodSend"))
    us = lambda ms: ms * 1e3 / 10e6
    print(f"{tag}: baseLoop {us(base):.2f} µs/iter ({us(base)*160:.0f} cyc); "
          f"send {us(snd-base):.2f} µs ({us(snd-base)*160:.0f} cyc) = "
          f"lookup {us(snd-blk):.2f} ({us(snd-blk)*160:.0f}) + frame/entry/return {us(blk-base):.2f} ({us(blk-base)*160:.0f})")
