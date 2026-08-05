#!/usr/bin/env python3
"""Persistent console collector — one process, one port, no parsing.

Design D3: a parser bug in the reader costs an answer; a parser bug in a
collector costs the evidence. So this program stamps bytes and writes them
down, and that is all it does.

Design D7: the heartbeat is written whether or not the device says anything,
because a log that stops looks identical to a board that went quiet. Every
downstream check asserts a *recent heartbeat present*, never an absence.
"""
import argparse
import datetime as dt
import errno
import fcntl
import os
import sys
import time

from lines import LineAssembler
from logstore import (LogStore, MARKER, DEFAULT_ROOT, DEFAULT_BUDGET,
                      acquire_lock, Busy)


# acquire_lock/Busy moved to logstore.py (2026-08-04): the one-reader rule is a
# property of the STORE, and udpsink.py needed it without needing a serial port.
# Re-exported here because callers and tests name them through this module.


def _default_opener(port_path):
    def opener():
        import serial
        return serial.Serial(port_path, 115200, timeout=0.2)
    return opener


class Collector:
    def __init__(self, port_path, store, heartbeat_s=60.0, now=None, open_port=None):
        self.port_path = port_path
        self.store = store
        self.heartbeat_s = heartbeat_s
        self.now = now or (lambda: dt.datetime.now(dt.timezone.utc).astimezone())
        self.open_port = open_port or _default_opener(port_path)
        self.asm = LineAssembler()
        self.port = None
        self.started = False
        self._last_beat = None
        self._lost = False

    # -- writing -------------------------------------------------------
    def _write(self, text):
        self.store.append(text, self.now())

    def _mark(self, text):
        self._write("%s %s" % (MARKER, text))

    # -- the cycle -----------------------------------------------------
    def step(self):
        if not self.started:
            self._mark("collector started pid=%d port=%s" % (os.getpid(), self.port_path))
            self._last_beat = self.now()
            self.started = True

        if self.port is None:
            try:
                self.port = self.open_port()
            except Exception as exc:
                if not self._lost:
                    self._mark("port lost: %s" % exc)
                    self._lost = True
                self._beat()
                return
            if self._lost:
                self._mark("port reacquired")
                self._lost = False

        try:
            data = self.port.read(4096)
        except Exception as exc:
            self._mark("port lost: %s" % exc)
            self._lost = True
            try:
                self.port.close()
            except Exception:
                pass
            self.port = None
            self._beat()
            return

        for line in self.asm.feed(data):
            self._write(line)
        self._beat()

    def _beat(self):
        # #147: the beat restates the PORT state, not just the process's own
        # pulse. `port lost` is written once, at the transition, so a collector
        # orphaned yesterday would look silent-but-fine in today's file; the
        # beat is what carries the state forward into every day and every tail.
        now = self.now()
        if (now - self._last_beat).total_seconds() >= self.heartbeat_s:
            self._mark("heartbeat port=%s" % ("down" if self._lost else "up"))
            self._last_beat = now

    def run(self, stop=None):
        while not (stop and stop()):
            self.step()
            if self.port is None:
                time.sleep(1.0)     # do not spin on a missing port


def main(argv=None):
    ap = argparse.ArgumentParser(description="Persistent console collector (#56)")
    ap.add_argument("port", help="/dev/serial/by-id/... path, or a PTY slave in tests")
    ap.add_argument("--board", default="c6")
    ap.add_argument("--root", default=DEFAULT_ROOT)
    ap.add_argument("--budget-bytes", type=int, default=DEFAULT_BUDGET)
    ap.add_argument("--heartbeat", type=float, default=60.0)
    args = ap.parse_args(argv)

    store = LogStore(args.root, args.board, args.budget_bytes)
    try:
        lock = acquire_lock(args.root, args.board)
    except Busy as exc:
        print("refusing to start: %s" % exc, file=sys.stderr)
        return 3

    collector = Collector(args.port, store, heartbeat_s=args.heartbeat)
    try:
        collector.run()
    except KeyboardInterrupt:
        collector._mark("collector stopped")
    finally:
        for path in store.prune():
            collector._mark("pruned %s" % path)
        lock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
