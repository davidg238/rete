#!/usr/bin/env python3
# Copyright (c) 2026 Ekorau LLC
"""Read ProfileCostNodeC6's console log and price an OPEN attribution window.

rete#203 (design 2026-08-12 §9.7). The node prints one span row per slot per
block, in rounds of three blocks:

    round R arm 1 off-a span <name> n N sumLo L sumHi H min MIN max MAX
    round R arm 2 on    span ...
    round R arm 3 off-b span ...

A ROUND IS THE UNIT, never a block. `off-a` and `off-b` bracket `on` in time, so
their spread is this bench's drift *while the measurement was running* — the only
honest yardstick for the ON delta. A block from one round compared against a
block from another measures the mesh.

The headline is the SIGN TEST, not the mean. `on` being the peak of its own
triple in k of n rounds is a claim a single lucky triple cannot manufacture;
"+1.6%" from one round is not. Under the null (drift only, no hook cost) `on` --
the middle block in time -- is the peak of its triple with probability 1/3.

Usage:
    profile_cost.py <console.log> [--span NAME]... [--min-round N]
"""

import argparse
import re
import sys
from collections import defaultdict

ROW = re.compile(
    r"round (\d+) arm (\d) (\S+) span (\S+) "
    r"n (\d+) sumLo (\d+) sumHi (\d+) min (\d+) max (\d+)"
)
BLOCK = re.compile(
    r"round (\d+) arm (\d) (\S+) block ms (\d+) ticks (\d+) "
    r"misses (\d+) dropped (\d+)"
)
# The window dump the ON arm emits when it closes. `misses`/`dropped` on the
# block line are the bridge's CUMULATIVE counters, so a block's own share is the
# DIFFERENCE from the previous block -- testing them for nonzero flags every
# block after the first miss and reads as "the whole run is dirty".
WINDOW = re.compile(
    r"mprof\[window\]: methods=(\d+) activations=(\d+) overflow=(\d+) cap=(\d+)"
)
AROW = re.compile(r"mprofa: \S+ (\d+) (\d+) (\d+) (\d+)")

# The spans that are LOCAL CPU: everything inside checkIn that is not a radio
# wait, plus publish. Deliberately NOT jobTick or checkIn -- those are dominated
# by pullWait/pushWait, where one retransmit is larger than the whole effect, and
# summing them would bury the signal under the radio. pushCollect/pushEncode/
# pushTrim are NESTED inside pushBuild (rete#161) and would double-count.
LOCAL = ["pullSend", "applyIn", "pushBuild", "pushSend", "publish"]


def parse(path):
    """-> rounds[r][arm][span] = sum_us,
          blocks[r][arm] = (ms, own_misses, own_drops),
          windows[r] = dict of the ON arm's dump totals."""
    rounds = defaultdict(lambda: defaultdict(dict))
    blocks = defaultdict(dict)
    windows = {}
    prev = None           # (misses, dropped) of the previous block, for the diff
    open_win = None       # the round whose dump we are inside
    for line in open(path, errors="replace"):
        m = ROW.search(line)
        if m:
            r, arm, _armname, span, _n, lo, hi, _mn, _mx = m.groups()
            rounds[int(r)][int(arm)][span] = int(lo) + int(hi) * 1_000_000_000
            continue
        m = BLOCK.search(line)
        if m:
            r, arm, _name, ms, _ticks, misses, dropped = m.groups()
            cum = (int(misses), int(dropped))
            own = cum if prev is None else (cum[0] - prev[0], cum[1] - prev[1])
            prev = cum
            blocks[int(r)][int(arm)] = (int(ms),) + own
            # The dump follows the ON block's rows, so the next mprof[window]
            # belongs to this round.
            open_win = int(r) if int(arm) == 2 else None
            continue
        m = WINDOW.search(line)
        if m and open_win is not None:
            methods, acts, overflow, cap = (int(x) for x in m.groups())
            windows[open_win] = dict(methods=methods, activations=acts,
                                     overflow=overflow, cap=cap,
                                     arena_bytes=0, arena_allocs=0,
                                     cheap_bytes=0, cheap_allocs=0, rows=0)
            continue
        m = AROW.search(line)
        if m and open_win is not None and open_win in windows:
            ab, ac, cb, cc = (int(x) for x in m.groups())
            w = windows[open_win]
            w["arena_bytes"] += ab
            w["arena_allocs"] += ac
            w["cheap_bytes"] += cb
            w["cheap_allocs"] += cc
            w["rows"] += 1
    return rounds, blocks, windows


def local_us(spans):
    """Sum of the local-CPU spans. None if any is missing -- a partial sum would
    be a smaller number that looks like a faster arm."""
    if any(s not in spans for s in LOCAL):
        return None
    return sum(spans[s] for s in LOCAL)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--span", action="append", default=[],
                    help="report this span too (repeatable)")
    ap.add_argument("--min-round", type=int, default=1)
    args = ap.parse_args()

    rounds, blocks, windows = parse(args.log)
    complete = sorted(r for r in rounds
                      if r >= args.min_round and all(a in rounds[r] for a in (1, 2, 3)))
    if not complete:
        sys.exit("no complete rounds (need arms 1, 2 and 3 for the same round)")

    # A round with a missed check-in is REPORTED, not dropped: a miss costs the
    # arm that suffered it real time, so silently excluding rounds would be
    # choosing the data after seeing it. What matters for the claim is whether
    # the ON arm caught MORE misses than its own OFF arms -- if the effect
    # survives on the rounds where it caught none, the misses are not the cause.
    print(f"complete rounds: {len(complete)}  ({complete[0]}..{complete[-1]})")
    clean = [r for r in complete if blocks.get(r, {}).get(2, (0, 0, 0))[1] == 0]
    print(f"per-block misses (own, not cumulative):  " +
          "  ".join(f"r{r}[{'/'.join(str(blocks[r][a][1]) for a in (1, 2, 3))}]"
                    for r in complete))
    print(f"rounds where the ON block caught NO miss: {clean}")

    if windows:
        w = [windows[r] for r in complete if r in windows]
        if w:
            print(f"\nON-window dumps ({len(w)} of {len(complete)} rounds):")
            print(f"  methods      {min(x['methods'] for x in w)}"
                  f"..{max(x['methods'] for x in w)}  of cap {w[0]['cap']}"
                  f"  (load {min(x['methods'] for x in w)/w[0]['cap']:.2f}"
                  f"..{max(x['methods'] for x in w)/w[0]['cap']:.2f})")
            acts = sum(x["activations"] for x in w) / len(w)
            alloc = sum(x["arena_allocs"] for x in w) / len(w)
            byts = sum(x["arena_bytes"] for x in w) / len(w)
            cheap = sum(x["cheap_allocs"] for x in w)
            print(f"  activations  {acts:,.0f} per window (mean)")
            print(f"  arena allocs {alloc:,.0f} per window ({byts:,.0f} B), "
                  f"1 per {acts/alloc:.2f} activations")
            print(f"  C-heap allocs {cheap} across every window   "
                  f"overflow {sum(x['overflow'] for x in w)}"
                  f"/{sum(x['rows'] for x in w)} allocating rows")
    print()

    names = ["LOCAL"] + args.span
    for name in names:
        def val(r, arm):
            s = rounds[r][arm]
            return local_us(s) if name == "LOCAL" else s.get(name)

        rows, peaks = [], 0
        for r in complete:
            a1, on, a2 = val(r, 1), val(r, 2), val(r, 3)
            if None in (a1, on, a2):
                continue
            off = (a1 + a2) / 2.0
            rows.append((r, a1, on, a2, on - off, 100.0 * (on - off) / off,
                         100.0 * abs(a2 - a1) / off))
            if on > a1 and on > a2:
                peaks += 1
        if not rows:
            print(f"{name}: no data\n")
            continue

        print(f"=== {name} "
              f"({'+'.join(LOCAL) if name == 'LOCAL' else 'single span'}) ===")
        print(f"{'rnd':>4} {'off-a':>10} {'on':>10} {'off-b':>10} "
              f"{'on-off':>9} {'on-off %':>9} {'drift %':>8}")
        for r, a1, on, a2, d, pct, drift in rows:
            print(f"{r:>4} {a1:>10} {on:>10} {a2:>10} {d:>9.0f} {pct:>9.3f} {drift:>8.3f}")

        pcts = [x[5] for x in rows]
        drifts = [x[6] for x in rows]
        mean = sum(pcts) / len(pcts)
        n = len(rows)
        # Population spread, not an inferential interval: n is small and the
        # samples are not independent of the bench's own slow drift.
        sd = (sum((p - mean) ** 2 for p in pcts) / n) ** 0.5 if n > 1 else 0.0
        print(f"\n  mean on-off: {mean:+.3f}%   spread(sd): {sd:.3f}%   "
              f"n={n}")
        cl = [x[5] for x in rows if x[0] in clean]
        if cl and len(cl) != n:
            print(f"  mean on-off, ON-block-caught-no-miss rounds only: "
                  f"{sum(cl)/len(cl):+.3f}%   n={len(cl)}")
        print(f"  mean |off-b - off-a| (drift): {sum(drifts)/len(drifts):.3f}%")
        print(f"  SIGN TEST: `on` is the peak of its own triple in "
              f"{peaks}/{n} rounds (null = 1/3)")
        print()


if __name__ == "__main__":
    main()
