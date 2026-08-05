#!/usr/bin/env python3
"""Offline reader over the collector's logs. All parsing lives here (design D3).

Re-runnable over history and testable with no board, which is the whole point
of keeping the collector dumb.

Every pattern below is answerable by a real capture in fixtures/ — none of it
is a guess about what the device prints.
"""
import argparse
import datetime as dt
import re
import sys

from logstore import LogStore, DEFAULT_ROOT

STAMP_RE = re.compile(r"^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}[+-]\d{2}:\d{2}) (.*)$")
# Not present in the C6 fixtures, but the ESP32 captures in esp32-debug-artifacts/
# are full of it and a CONFIG_LOG colour change would bring it back here.
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
BUILD_ID_RE = re.compile(r"tuvm: build-id (\S+)")
# One physical boot prints BOTH of these — the ROM banner, then the kernel one
# some 30 lines later. Treating either as a run start splits every boot in two
# and puts the fault in a different run from the build id that names its binary.
# The ROM line IS the boot. The kernel banner only starts a run when there is no
# run in progress, which is how a collector attaching to an already-running board
# still gets a usable run boundary (§10's MagTag case).
ROM_BOOT_RE = re.compile(r"ESP-ROM:esp32c6")
KERNEL_BANNER_RE = re.compile(r"\*\*\* Booting Zephyr OS")
FATAL_RE = re.compile(r"ZEPHYR FATAL ERROR")
CD_BEGIN_RE = re.compile(r"#CD:BEGIN#")
CD_END_RE = re.compile(r"#CD:END#")
CD_RE = re.compile(r"#CD:([0-9a-fA-F]+)\s*$")


class NoRuns(Exception):
    """A non-empty log that yielded no runs. An error, not an empty success."""


def parse_line(line):
    m = STAMP_RE.match(line)
    if not m:
        raise ValueError("unstamped line (not written by this collector): %r" % line[:60])
    return dt.datetime.fromisoformat(m.group(1)), m.group(2)


def resolve_since(spec, now=None):
    now = now or dt.datetime.now(dt.timezone.utc).astimezone()
    m = re.fullmatch(r"-(\d+)([sm])", spec)
    if m:
        n = int(m.group(1))
        delta = dt.timedelta(seconds=n) if m.group(2) == "s" else dt.timedelta(minutes=n)
        return now - delta
    got = dt.datetime.fromisoformat(spec)
    if got.tzinfo is None:
        got = got.replace(tzinfo=now.tzinfo)
    return got


class Panic:
    def __init__(self, start, cd_lines):
        self.start = start
        self.cd_lines = cd_lines


class Run:
    def __init__(self, index, start, build_id, lines):
        self.index = index
        self.start = start
        self.build_id = build_id
        self.lines = lines

    def __repr__(self):
        return "<Run %d %s %s %d lines>" % (
            self.index, self.start.isoformat(timespec="seconds"),
            self.build_id or "NO BUILD ID", len(self.lines))


def split_runs(entries, strict=False):
    """Split at the boot banner. The build id is attached from the tuvm line
    that follows it — a run that faulted before printing one keeps build_id
    None, which is what makes D4 refuse rather than guess."""
    runs, cur = [], None
    for when, text in entries:
        clean = ANSI_RE.sub("", text)
        if ROM_BOOT_RE.search(clean) or (KERNEL_BANNER_RE.search(clean) and cur is None):
            cur = Run(len(runs) + 1, when, None, [])
            runs.append(cur)
        if cur is None:
            continue
        cur.lines.append((when, clean))
        if cur.build_id is None:
            m = BUILD_ID_RE.search(clean)
            if m:
                cur.build_id = m.group(1)
    if strict and entries and not runs:
        raise NoRuns("%d lines, zero runs — the banner pattern no longer "
                     "matches what the device prints" % len(entries))
    return runs


def find_panics(run):
    """A panic is a fatal-error banner and/or a #CD: block. Both are recorded:
    a fault that dies before the coredump backend runs still leaves a banner,
    and that is a panic worth listing even though it cannot be symbolized."""
    panics, cur, collecting = [], None, False
    for when, text in run.lines:
        if FATAL_RE.search(text):
            cur = Panic(when, [])
            panics.append(cur)
        if CD_BEGIN_RE.search(text):
            collecting = True
            if cur is None:
                cur = Panic(when, [])
                panics.append(cur)
            continue
        if CD_END_RE.search(text):
            collecting = False
            continue
        if collecting and cur is not None:
            m = CD_RE.search(text)
            if m:
                cur.cd_lines.append(m.group(1))
    return panics


def load(root, board, since=None):
    store = LogStore(root, board)
    out = []
    for day in store.days():
        if since and day < since.date():
            continue
        with open(store.path_for(day)) as fh:
            for line in fh:
                line = line.rstrip("\n")
                if not line:
                    continue
                try:
                    when, text = parse_line(line)
                except ValueError:
                    continue
                if since and when < since:
                    continue
                out.append((when, text))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description="Offline console log reader (#56)")
    ap.add_argument("--root", default=DEFAULT_ROOT)
    ap.add_argument("--board", default="c6")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p_tail = sub.add_parser("tail")
    p_tail.add_argument("-n", type=int, default=20)
    p_since = sub.add_parser("since")
    p_since.add_argument("spec")
    sub.add_parser("runs")
    sub.add_parser("panics")
    p_sym = sub.add_parser("symbolize")
    p_sym.add_argument("--run", type=int, default=None,
                       help="run index; default is the last run with a panic")
    p_sym.add_argument("--assume-build", default=None,
                       help="name the build id yourself when the run never printed one (D4a)")
    args = ap.parse_args(argv)

    if args.cmd == "tail":
        for when, text in load(args.root, args.board)[-args.n:]:
            print("%s %s" % (when.isoformat(timespec="milliseconds"), text))
        return 0
    if args.cmd == "since":
        for when, text in load(args.root, args.board, resolve_since(args.spec)):
            print("%s %s" % (when.isoformat(timespec="milliseconds"), text))
        return 0

    entries = load(args.root, args.board)
    try:
        runs = split_runs(entries, strict=True)
    except NoRuns as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 4

    if args.cmd == "runs":
        for run in runs:
            print(run)
        return 0

    if args.cmd == "panics":
        found = 0
        for run in runs:
            for panic in find_panics(run):
                found += 1
                print("run %d  %s  build-id %s  %d coredump lines" % (
                    run.index, panic.start.isoformat(timespec="seconds"),
                    run.build_id or "NONE — symbolize will refuse", len(panic.cd_lines)))
        if not found:
            print("no panics in %d runs" % len(runs))
        return 0

    if args.cmd == "symbolize":
        import symbolize as sym
        targets = [r for r in runs if (args.run is None or r.index == args.run)]
        for run in reversed(targets):
            panics = find_panics(run)
            if not panics:
                continue
            try:
                print(sym.symbolize_panic(run, panics[-1], assume_build=args.assume_build))
            except sym.Refused as exc:
                print("refused: %s" % exc, file=sys.stderr)
                return 5
            return 0
        print("no panic to symbolize", file=sys.stderr)
        return 1

    return 2


if __name__ == "__main__":
    sys.exit(main())
