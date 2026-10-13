#!/usr/bin/env python3
"""esp32_stic.py BIN LOG — flash BIN at 0x1000 on the ESP32 at /dev/serial/by-id/usb-Silicon_Labs_CP2104_USB_to_UART_Bridge_Controller_01558C55-if00-port0, reset it with the reader already
open, and log the console until the node prints `done` (or 40 min). rete#37 STIC on the plain ESP32."""
import subprocess, sys, time, serial
binf, log = sys.argv[1], sys.argv[2]
ESPTOOL = sys.argv[3]
rc = subprocess.call([ESPTOOL, "--chip", "esp32", "--port", "/dev/serial/by-id/usb-Silicon_Labs_CP2104_USB_to_UART_Bridge_Controller_01558C55-if00-port0", "--baud", "921600", "--after", "no-reset",
                      "write-flash", "--flash-mode", "dio", "--flash-freq", "40m", "--flash-size", "4MB", "0x1000", binf],
                     stdout=open(log + ".flash", "w"), stderr=subprocess.STDOUT)
if rc: sys.exit(f"flash failed rc={rc}")
s = serial.Serial("/dev/serial/by-id/usb-Silicon_Labs_CP2104_USB_to_UART_Bridge_Controller_01558C55-if00-port0", 115200, timeout=0.5)
s.dtr = False; s.rts = True; time.sleep(0.15); s.rts = False       # EN pulse, IO0 high: run
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
