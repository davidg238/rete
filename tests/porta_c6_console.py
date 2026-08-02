#!/usr/bin/env python3
# Copyright (c) 2026 Ekorau LLC
"""Hold the ONE serial handle on the C6 console and tee it to a log.

A separate process for one reason: nothing else may open the port. Serial bytes
go to exactly one reader, so a monitor and a `cat` running together each see a
plausible-looking fraction of the output and neither looks wrong enough to
notice. This file is the only opener; everything else greps the log.

Decoding is errors="replace" on purpose. One NUL byte has turned a grep monitor
binary here before, and grep on a "binary file" answers nothing at all — which
reads exactly like a dead node.

The C6's console is uart0 on the CP210x bridge (zephyr/prj_esp32c6.conf:30),
which is NOT the interface esptool flashes over (USB-JTAG). So this handle can
be opened before a flash and held across it.

Usage:
  tests/porta_c6_console.py --port /dev/serial/by-id/usb-Silicon_Labs_... \\
      --log /tmp/c6_console.log --await 'PORTA NODE READY' --timeout 90
  tests/porta_c6_console.py --port ... --log ... --follow      # run until killed
"""
import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is not installed: python3 -m pip install --user pyserial")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--log", required=True)
    ap.add_argument("--await", dest="pattern", default=None,
                    help="exit 0 as soon as this substring appears")
    ap.add_argument("--timeout", type=float, default=90.0)
    ap.add_argument("--follow", action="store_true",
                    help="never exit on a pattern; read until killed")
    ap.add_argument("--reset", action="store_true",
                    help="pulse DTR/RTS to reset the board after opening")
    args = ap.parse_args()

    # Append, never truncate: the gate opens this reader once and greps the log
    # repeatedly, and a truncation between legs would silently erase leg 3's
    # evidence.
    with serial.Serial(args.port, args.baud, timeout=1) as ser, \
            open(args.log, "a", buffering=1) as log:
        if args.reset:
            ser.dtr = False
            ser.rts = True
            time.sleep(0.1)
            ser.rts = False

        deadline = time.time() + args.timeout
        while args.follow or time.time() < deadline:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            log.write(line + "\n")
            print(line, flush=True)
            if args.pattern and args.pattern in line:
                return 0
        return 0 if args.follow else 1


if __name__ == "__main__":
    sys.exit(main())
