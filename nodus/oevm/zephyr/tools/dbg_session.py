#!/usr/bin/env python3
"""rete#311 — drive the in-image debugger on a board over its console cable.

ONE handle on the port, read and write (two readers split the bytes; a collector must
be stopped first). Plain text lines, the `dbg:*` grammar of nodus/nsl/lib/supervisor.nsl,
the same lines tools/test_debugger_integration.py sends over stdin on the host.

Modes:
  session  the whole flow against DebugProbeC6: wait for `dbg:ready`, list methods,
           break on `countTo:`, continue, HOLD at the breakpoint for --hold seconds
           (longer than the probe's 8 s appLiveness, so a reboot here is the observer
           judging the hold as a wedge), inspect, step, over, out, clear, continue,
           then WATCH for --watch seconds counting ticks and failing on any reset or
           WEDGE line.
  run      wait for `dbg:ready`, send `dbg:continue`, stream for --watch seconds.
           For measuring a hooks image that is not being debugged.

Prints a timestamped transcript (`<` from the board, `>` to it) and a verdict table.
Exit 0 only when every step passed and nothing reset.

    python3 dbg_session.py --port /dev/serial/by-id/usb-Silicon_Labs_CP2102N_... \
        --reset --hold 20 --watch 60 session
"""
import argparse, re, sys, time

try:
    import serial
except ImportError:
    sys.exit("pyserial is needed (on the gateway: ~/esptool-venv/bin/python)")

RESET_RE = re.compile(r"rst:0x|ESP-ROM:|WEDGE|deadline=|Booting Zephyr")


class Board:
    def __init__(self, port, baud, log):
        self.ser = serial.Serial()
        self.ser.port, self.ser.baudrate, self.ser.timeout = port, baud, 0.2
        # Neither line asserted at open: the devkit's auto-reset pair only pulls EN
        # low for RTS asserted with DTR deasserted, and only drives IO9 for the
        # converse, so this opens quietly.
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.log = log
        self.buf = b""
        self.resets = []       # lines that say the board rebooted or was judged wedged

    def stamp(self):
        t = time.time()
        return time.strftime("%H:%M:%S", time.localtime(t)) + ".%03d" % int((t % 1) * 1000)

    def reset(self):
        """esptool's hard_reset: EN low for 100 ms with DTR deasserted."""
        self.ser.rts = True
        time.sleep(0.1)
        self.ser.rts = False
        self.log.write("%s * RTS pulse (EN)\n" % self.stamp())

    def send(self, line):
        self.ser.write((line + "\n").encode())
        self.ser.flush()
        self.log.write("%s > %s\n" % (self.stamp(), line))
        self.log.flush()

    def lines(self, deadline):
        """Yield lines until `deadline` (absolute time)."""
        while time.time() < deadline:
            chunk = self.ser.read(256)
            if chunk:
                self.buf += chunk
                while b"\n" in self.buf:
                    raw, self.buf = self.buf.split(b"\n", 1)
                    line = raw.decode("utf-8", "replace").rstrip("\r")
                    self.log.write("%s < %s\n" % (self.stamp(), line))
                    self.log.flush()
                    if RESET_RE.search(line):
                        self.resets.append((self.stamp(), line))
                    yield line

    def expect(self, pattern, timeout, collect=None):
        """First line matching `pattern` within `timeout` s, or None. Every line seen
        on the way is appended to `collect` when given."""
        rx = re.compile(pattern)
        for line in self.lines(time.time() + timeout):
            if collect is not None:
                collect.append(line)
            if rx.search(line):
                return line
        return None

    def drain(self, seconds):
        """Read for `seconds`; answer the lines seen."""
        return list(self.lines(time.time() + seconds))


def tick_lines(lines):
    return [l for l in lines if l.startswith("dbg-probe tick")]


def run_session(b, args):
    results = []           # (step, ok, detail)

    def step(name, ok, detail=""):
        results.append((name, bool(ok), detail))
        print("  %-28s %s  %s" % (name, "ok " if ok else "NOT OK", detail), flush=True)

    print("== session: %s" % args.port, flush=True)
    if args.reset:
        b.reset()
    ready = b.expect(r"^dbg:ready$", 20)
    step("dbg:ready after boot", ready is not None,
         "" if ready else "no dbg:ready in 20 s (already waiting? sending anyway)")
    b.resets = []   # the boot we asked for is not a finding; everything after it is

    # the registry: `<id> <selector> <tag>` lines, then `dbg:ok methods`
    seen = []
    b.send("dbg:methods")
    ok = b.expect(r"^dbg:ok methods$", 10, collect=seen)
    ids = {}
    for l in seen:
        m = re.match(r"^(\d+) #?(\S+) (-?\d+)$", l)
        if m:
            ids[m.group(2)] = int(m.group(1))
    target = ids.get(args.selector)
    step("dbg:methods lists %s" % args.selector, ok is not None and target is not None,
         "id=%s of %d methods" % (target, len(ids)))
    if target is None:
        return results

    b.send("dbg:break %d 0" % target)
    step("dbg:break", b.expect(r"^dbg:ok break$", 5) is not None)

    b.send("dbg:continue")
    paused = b.expect(r"^dbg:paused break ", 15)
    step("paused at the breakpoint", paused is not None, paused or "")
    if paused is None:
        return results

    # THE HOLD. Nothing should arrive; a reset or WEDGE line here is the finding.
    t0 = time.time()
    held = b.drain(args.hold)
    step("held %d s, no reset" % args.hold, not b.resets and not tick_lines(held),
         "%d lines during the hold" % len(held))

    b.send("dbg:inspect")
    stack = b.expect(r"^dbg:stack ", 5)
    step("dbg:inspect", stack is not None, (stack or "")[:70])

    for verb in ("dbg:step", "dbg:over", "dbg:out"):
        b.send(verb)
        p = b.expect(r"^dbg:paused step ", 10)
        step(verb, p is not None, p or "")

    b.send("dbg:clear %d 0" % target)
    step("dbg:clear", b.expect(r"^dbg:ok clear$", 5) is not None)

    b.send("dbg:continue")
    watched = b.drain(args.watch)
    ticks = tick_lines(watched)
    step("ticks resume, %d s watched" % args.watch, len(ticks) >= args.watch // 2 and not b.resets,
         "%d ticks, first %r" % (len(ticks), ticks[0] if ticks else None))
    step("no reset or WEDGE anywhere", not b.resets,
         "; ".join("%s %s" % r for r in b.resets)[:120])
    return results


def run_run(b, args):
    results = []
    if args.reset:
        b.reset()
    ready = b.expect(r"^dbg:ready$", 20)
    results.append(("dbg:ready", ready is not None, ""))
    b.resets = []
    b.send("dbg:continue")
    lines = b.drain(args.watch)
    results.append(("streamed %d s" % args.watch, True, "%d lines" % len(lines)))
    results.append(("no reset", not b.resets, "; ".join("%s %s" % r for r in b.resets)[:120]))
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["session", "run"])
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--reset", action="store_true", help="pulse RTS (EN) first, to catch dbg:ready")
    ap.add_argument("--hold", type=int, default=20, help="seconds held at the breakpoint")
    ap.add_argument("--watch", type=int, default=60, help="seconds watched after the continue")
    ap.add_argument("--selector", default="countTo:", help="the method to break in")
    ap.add_argument("--log", default="-", help="transcript file (default stdout)")
    args = ap.parse_args()

    log = sys.stdout if args.log == "-" else open(args.log, "a")
    b = Board(args.port, args.baud, log)
    results = run_session(b, args) if args.mode == "session" else run_run(b, args)

    print("== verdict")
    bad = 0
    for name, ok, detail in results:
        bad += not ok
        print("  %-5s %-28s %s" % ("ok" if ok else "FAIL", name, detail))
    print("== %s (%d of %d steps)" % ("PASS" if not bad else "FAIL", len(results) - bad, len(results)))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
