#!/usr/bin/env python3
# ONE handle on the console header (PL2303 /dev/ttyUSB0). Two readers silently split the
# stream, so never run a second one. The header does NOT reset the board on open, which is
# why the running clock survives attaching to it.
import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
out = sys.argv[2] if len(sys.argv) > 2 else "console.log"
s = serial.Serial(port, 115200, timeout=1)
t0 = time.time()
with open(out, "ab", buffering=0) as f:
    line = b""
    while True:
        b = s.read(1)
        if not b:
            continue
        if b in (b"\n", b"\r"):
            if line:
                # wall clock, not capture-relative — the whole point is to compare a strike
                # against the minute it claims to be
                stamp = time.strftime("[%H:%M:%S] ").encode()
                f.write(stamp + line.replace(b"\x00", b"") + b"\n")
            line = b""
        else:
            line += b
