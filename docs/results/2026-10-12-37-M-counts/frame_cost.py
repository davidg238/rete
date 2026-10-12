#!/usr/bin/env python3
"""rete#37 M: cycles per frame on a board, from a counting run and a timing run of the same rows.

    python3 frame_cost.py COUNT_LOG TIME_LOG [MHZ]

COUNT_LOG is an OEVM_COUNT run of AWFY built with awfy_counts.py --count (a `count:` line before each `<row>: <ms>`
line); TIME_LOG is a normal build's run of the same profile (a board console log is fine: lines are matched anywhere).
The counts do not depend on the platform (one image, a deterministic VM), so the count run may be the host's.
Sieve pushes no frames, so its cycles per instruction is taken as the base cost of an instruction; what a row spends
above base x instructions is charged to its frames (which therefore carry the primitive sends too).
"""
import re
import sys

ROW = re.compile(r"(?:^|< )([A-Z]\w+): (\d+)\s*$")


def counts(path):
    out, c = {}, None
    for line in open(path, errors="replace"):
        if line.startswith("count:"):
            c = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)}
        elif c and (m := ROW.search(line)):
            out[m.group(1)], c = c, None
    return out


def times(path):
    return {m.group(1): int(m.group(2)) for line in open(path, errors="replace") if (m := ROW.search(line))}


C, T = counts(sys.argv[1]), times(sys.argv[2])
mhz = float(sys.argv[3]) if len(sys.argv) > 3 else 160.0
base = T["Sieve"] * mhz * 1e3 / C["Sieve"]["insn"]
print(f"base: {base:.1f} cycles per instruction (Sieve, {C['Sieve']['frame']} frames), {mhz:.0f} MHz\n")
print("| row | ms | instructions | sends | implicit | frames | block invokes | insn/frame | regs/frame | cycles/insn"
      " | cycles/frame | frame share |")
print("|---|" + "---:|" * 11)
for n, ms in T.items():
    if n not in C:
        continue
    c, cyc = C[n], ms * mhz * 1e3
    fr = c["frame"]
    per = (cyc - base * c["insn"]) / fr if fr > 100 else 0
    print(f"| {n} | {ms} | {c['insn']} | {c['send']} | {c['impl']} | {fr} | {c['block']} | "
          f"{c['insn'] / max(fr, 1):.1f} | {c['regs'] / max(fr, 1):.1f} | {cyc / c['insn']:.1f} | "
          + (f"{per:.0f} | {100 * per * fr / cyc:.0f} % |" if fr > 100 else "— | — |"))
