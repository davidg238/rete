#!/usr/bin/env python3
"""c6run.py LOG MARKER [ADDR FILE]... — rete#37 cross-VM summary on c6spare (/dev/ttyUSB1, CP2102N …d2e21074…).
Flash each FILE at ADDR (esptool, if any are given), reset the board with the reader already open, and log the
console until a line starting with MARKER (exit 0), a fault (exit 3: out of memory, a fatal error or the VM's
HALT, so a crash-looping board costs seconds, not the cap), or C6_TIMEOUT seconds (default 3600; exit 2).
Same reset as ../2026-10-11-37-stic-c6/c6_stic.py. ESPTOOL names esptool (default: esptool on PATH); C6_PORT the
console (default /dev/ttyUSB1, c6spare)."""
import os, subprocess, sys, time, serial
log, marker, pairs = sys.argv[1], sys.argv[2], sys.argv[3:]
PORT = os.environ.get("C6_PORT", "/dev/ttyUSB1")   # c6spare by default; c6b is /dev/ttyUSB0
if pairs:
    rc = subprocess.call([os.environ.get("ESPTOOL", "esptool"), "--chip", "esp32c6", "--port", PORT, "--baud", "921600",
                          "--after", "no-reset", "write-flash", "--flash-mode", "dio", "--flash-freq", "40m",
                          "--flash-size", "8MB", *pairs], stdout=open(log + ".flash", "w"), stderr=subprocess.STDOUT)
    if rc: sys.exit(f"flash failed rc={rc}")
s = serial.Serial(PORT, 115200, timeout=0.5)
s.dtr = False; s.rts = True; time.sleep(0.15); s.rts = False       # EN pulse, IO9 high: run
out = open(log, "w"); buf = b""; t0 = time.time()
FAULTS = ("Out of Memory", "FATAL", "HALT")
while time.time() - t0 < float(os.environ.get("C6_TIMEOUT", "3600")):
    d = s.read(4096)
    if not d: continue
    buf += d
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        t = line.decode("utf-8", "replace").rstrip("\r")
        out.write(time.strftime("%H:%M:%S < ") + t + "\n"); out.flush()
        if t.strip().startswith(marker):
            print("done"); sys.exit(0)
        if any(f in t for f in FAULTS):
            print("fault: " + t.strip()); sys.exit(3)
print("timeout"); sys.exit(2)
