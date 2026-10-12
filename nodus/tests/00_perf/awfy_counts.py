#!/usr/bin/env python3
"""The AWFY run profiles (rete#37): awfy.nsl with its runAll counts set for where it runs.

    python3 awfy_counts.py PROFILE [--count] [source.nsl] > out.nsl

awfy.nsl's own counts are the reference sizes AWFY verifies. A profile changes a row's `inner:` count or drops the
row; nothing else in the source changes. Every driver that runs AWFY (the host baseline and A/B, the board A/B) goes
through here, so a count is set in one place.

- host: each row scaled to run ~0.2-5 s on a desktop (opti7040); Mandelbrot and CD keep the reference sizes.
- board: the rows that fit a 256 KB arena and say something about sends and frames, one pass in ~30 s on a C6.
  Dropped: Storage and Json, whose live sets do not fit 256 KB (the C6 aborts out of memory); Mandelbrot and NBody,
  which are soft-float on these parts and verify only at sizes that take minutes (Mandelbrot 500: 180 s on the C6)
  or almost nothing (NBody 1: 4 ms).

--count brackets each row's timed block with `platform profileStart`/`profileStop`, for the counting build
(OEVM_COUNT, send/frame design §M): it prints a `count:` line before each row's `<name>: <ms>` line. A count run's
times are not timings; a non-counting build ignores the two prims, so only the count run uses the switch.
"""
import re
import sys
from pathlib import Path

PROFILES = {
    "host": {"Bounce": 300, "List": 100, "Permute": 200, "Storage": 200, "Richards": 5, "Queens": 200,
             "Sieve": 300, "Towers": 60, "NBody": 250000, "DeltaBlue": 2000, "Json": 20},
    "board": {"Storage": None, "Json": None, "Mandelbrot": None, "NBody": None},
}
CLOCK = "    t0:: 0 millis.\n    aBlock value.\n"
ROW = re.compile(r"^    \(run: '(\w+)'\s+bench: \w+ new\s+inner: (\d+)\)\s*ifFalse: \[ ok:: false \]\.\n", re.M)


def apply(src: str, profile: dict) -> str:
    seen = set()

    def row(m):
        name = m.group(1)
        seen.add(name)
        if name not in profile:
            return m.group(0)
        if profile[name] is None:
            return ""
        return m.group(0).replace(f"inner: {m.group(2)})", f"inner: {profile[name]})", 1)

    out = ROW.sub(row, src)
    missing = set(profile) - seen
    if missing:
        sys.exit(f"awfy_counts: runAll has no row for {sorted(missing)}; awfy.nsl moved, update the profile")
    return out


def count(src: str) -> str:
    if src.count(CLOCK) != 1:
        sys.exit("awfy_counts: millisecondsToRun: moved, update CLOCK")
    return src.replace(CLOCK, "    platform profileStart.\n" + CLOCK + "    platform profileStop.\n")


if __name__ == "__main__":
    args = sys.argv[1:]
    counting = "--count" in args
    args = [a for a in args if a != "--count"]
    if not args or args[0] not in PROFILES:
        sys.exit(f"usage: awfy_counts.py {{{'|'.join(PROFILES)}}} [--count] [source.nsl]")
    src = Path(args[1] if len(args) > 1 else Path(__file__).with_name("awfy.nsl")).read_text()
    out = apply(src, PROFILES[args[0]])
    sys.stdout.write(count(out) if counting else out)
