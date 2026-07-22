#!/usr/bin/env python3
"""Client-side RTT harness for tuvm#61.

WHY THIS EXISTS. The node cannot measure ingress wire time. `http_accept_request`
is the earliest moment the node has a clock at all; everything before it — the
client's send, the network, the TCP accept — happened somewhere the node cannot
see. So the network column of the published table is derived as

    network ~= client RTT - node service time

which needs no clock synchronisation between the two machines, because each half is
measured with a single clock and only DURATIONS are ever compared. Never subtract
two timestamps taken on different clocks.

This tool measures the CLIENT half only. The node half is read from the node's
own /stats table (the httpQueue/render/egress spans), and the subtraction happens
when the table is published — see the design's section 3. Nothing here reaches into
the node, so a run against a device costs the node exactly the requests it serves.

WHY THIS PRINTS EXACT PERCENTILES WHERE THE NODE REFUSES TO. The node keeps six
log-spaced BUCKETS, so its p95 can only honestly name a decade — inventing a figure
inside a bucket whose mass is not uniform would manufacture precision the data does
not contain. Here we hold every raw sample, so the reported p50/p95 is an actual
observed sample, not an interpolation. Same rank convention as the node's reducer
(`((n * p) + 99) // 100`, 1-based, rounded up) so the two are directly comparable,
and the decade is printed alongside so a reader can line the columns up by eye.

The n >= 200 gate is honoured here too: below it, a p95 is just the k-th sample and
one retransmit relocates it. 500 requests is the default because it clears that bar
with margin — fixed up front, not tuned after seeing a result.
"""
import argparse
import collections
import statistics
import time
import urllib.request

DECADES = [(100, "<100us"), (1_000, "<1ms"), (10_000, "<10ms"),
           (100_000, "<100ms"), (1_000_000, "<1s"), (float("inf"), ">=1s")]

MIN_SAMPLES_FOR_P95 = 200   # mirrors Spans>>minSamplesForP95 in kernel.nsl


def decade(us):
    for edge, name in DECADES:
        if us < edge:
            return name
    return ">=1s"


def rank(n, p):
    """1-based rank of the p-th percentile, rounded UP — the node's convention, so
    p95 of 200 samples is the 190th and not the 189.99th."""
    return max(0, (n * p + 99) // 100 - 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("url", help="e.g. http://192.168.1.50:18080/thermal")
    ap.add_argument("-n", type=int, default=500,
                    help="request count (default 500, clears the n>=200 p95 gate)")
    ap.add_argument("--timeout", type=float, default=10.0,
                    help="per-request timeout in seconds (default 10)")
    ap.add_argument("--delay", type=float, default=0.0,
                    help="seconds to pause between requests (default 0, "
                         "back-to-back). A paced run measures a different thing "
                         "from a burst — say which one you ran.")
    args = ap.parse_args()

    samples, failures = [], 0
    kinds = collections.Counter()
    for i in range(args.n):
        if args.delay and i:
            time.sleep(args.delay)
        t0 = time.perf_counter()
        try:
            with urllib.request.urlopen(args.url, timeout=args.timeout) as r:
                r.read()
        except Exception as e:
            # A timeout or a reset is a FAILED job, not a slow one. Counting it
            # separately is the same rule the node applies to a nil sensor frame:
            # folding a failure into the distribution as a large sample, or
            # dropping it silently, both misreport — one inflates the tail, the
            # other flatters the percentiles by removing the worst cases.
            #
            # The KIND is kept too. A bare count was actively unhelpful the first
            # time this ran against hardware: 488/500 failed and the number alone
            # could not distinguish a shedding node from a node that had fallen
            # off the network (it was the latter — EHOSTUNREACH, every time).
            failures += 1
            kinds[f"{type(e).__name__}: {e}"] += 1
            continue
        samples.append((time.perf_counter() - t0) * 1e6)

    n = len(samples)
    print(f"url        {args.url}")
    print(f"n          {n} ok, {failures} failed")
    for kind, count in kinds.most_common(5):
        print(f"  failure  {count:5d}  {kind}")
    if n == 0:
        print("no successful samples — nothing to report")
        return

    # Reported before sorting destroys the order: the first request carries ARP,
    # TCP setup and any lazy init on the node, so it is frequently the max. Shown
    # rather than discarded — dropping the sample that does not suit the story is
    # how a benchmark stops being one.
    first = samples[0]
    ordered = sorted(samples)
    p50 = ordered[rank(n, 50)]

    print(f"first      {first:.0f} us   ({decade(first)}) — includes connection setup")
    print(f"p50        {p50:.0f} us   ({decade(p50)})")
    if n >= MIN_SAMPLES_FOR_P95:
        p95 = ordered[rank(n, 95)]
        print(f"p95        {p95:.0f} us   ({decade(p95)})")
    else:
        print(f"p95        insufficient (n={n} < {MIN_SAMPLES_FOR_P95}) — "
              f"the design refuses to report one")
    print(f"max        {ordered[-1]:.0f} us   (exact)")
    print(f"mean       {statistics.mean(samples):.0f} us   "
          f"(secondary — never the headline; see the design 4.3)")

    # The client-side histogram, in the node's own bucket edges, so the two
    # distributions can be compared shape-to-shape and not just number-to-number.
    counts = {name: 0 for _, name in DECADES}
    for s in ordered:
        counts[decade(s)] += 1
    print("histogram  " + "  ".join(f"{name} {counts[name]}" for _, name in DECADES))


if __name__ == "__main__":
    main()
