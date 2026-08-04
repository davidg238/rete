#!/usr/bin/env python3
"""Is the collector actually collecting? (#56 §8, D7)

Asserts a RECENT HEARTBEAT IS PRESENT. Never asserts the absence of an error
message: a log that stops looks exactly like a board that went quiet, and only
a positive signal tells them apart.

Device traffic does not count. A chatty board proves the BOARD is alive; only
the collector's own marker proves the collector is still writing.

180 s = three missed 60 s beats, so one slow write is not a false alarm.
"""
import argparse
import datetime as dt
import sys

import logread
from logstore import LogStore, MARKER, DEFAULT_ROOT

MAX_AGE_S = 180.0


def check_health(root, board, now=None, max_age_s=MAX_AGE_S):
    now = now or dt.datetime.now(dt.timezone.utc).astimezone()
    store = LogStore(root, board)
    days = store.days()
    if not days:
        return ["no log files at all under %s — the collector has never run" % root]

    latest = None
    path = store.path_for(days[-1])
    with open(path) as fh:
        for line in fh:
            try:
                when, text = logread.parse_line(line.rstrip("\n"))
            except ValueError:
                continue
            # A heartbeat OR a startup marker. Both are the collector writing
            # about itself, which is the only thing that proves the collector.
            # Heartbeat alone leaves a 60s window after every restart where a
            # demonstrably-working collector reads as down — found by actually
            # stopping and restarting the unit, not by reading the code.
            if (text.startswith("%s heartbeat" % MARKER)
                    or text.startswith("%s collector started" % MARKER)):
                latest = when
    if latest is None:
        return ["no collector marker in %s — the collector is not writing" % path]

    age = (now - latest).total_seconds()
    if age > max_age_s:
        return ["last heartbeat is %.0fs old (limit %.0fs) — the collector is "
                "down or wedged" % (age, max_age_s)]
    return []


def main(argv=None):
    ap = argparse.ArgumentParser(description="Collector health check (#56)")
    ap.add_argument("--root", default=DEFAULT_ROOT)
    ap.add_argument("--board", default="c6")
    args = ap.parse_args(argv)
    problems = check_health(args.root, args.board)
    for p in problems:
        print("FAIL: %s" % p, file=sys.stderr)
    if not problems:
        print("ok: collector alive")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
