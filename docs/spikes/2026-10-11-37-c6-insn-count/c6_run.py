#!/usr/bin/env python3
"""c6_stic.py BIN OIMG LOG ESPTOOL — flash BIN at 0x0 and OIMG at program_a (0x220000) on c6spare
(/dev/ttyUSB1), reset it with the reader already open, and log the console until the node prints `done`
(or 40 min). rete#37 STIC on the ESP32-C6."""
import subprocess, sys, time, serial
binf, oimg, log, ESPTOOL = sys.argv[1:5]
PORT = "/dev/ttyUSB1"
rc = subprocess.call([ESPTOOL, "--chip", "esp32c6", "--port", PORT, "--baud", "921600", "--after", "no-reset",
                      "write-flash", "--flash-mode", "dio", "--flash-freq", "40m", "--flash-size", "8MB",
                      "0x0", binf, "0x220000", oimg],
                     stdout=open(log + ".flash", "w"), stderr=subprocess.STDOUT)
if rc: sys.exit(f"flash failed rc={rc}")
s = serial.Serial(PORT, 115200, timeout=0.5)
s.dtr = False; s.rts = True; time.sleep(0.15); s.rts = False       # EN pulse, IO9 high: run
out = open(log, "w"); buf = b""; t0 = time.time()
while time.time() - t0 < 2400:
    d = s.read(4096)
    if not d: continue
    buf += d
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        t = line.decode("utf-8", "replace").rstrip("\r")
        out.write(time.strftime("%H:%M:%S < ") + t + "\n"); out.flush()
        if t.strip() == "done":
            print("done"); sys.exit(0)
print("timeout"); sys.exit(2)
