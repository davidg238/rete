#!/usr/bin/env python3
# Reset the ESP32 and log the console (incl. the '#CD:' coredump) to stdout.
# Usage: python3 capture_coredump.py [seconds]   (default 12)  | tee crash.log
import serial, time, sys
port = "/dev/ttyUSB0"
dur = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0
s = serial.Serial(port, 115200, timeout=0.2)
# classic ESP32 auto-reset into RUN mode: IO0 high (DTR False), pulse EN via RTS
s.dtr = False; s.rts = True; time.sleep(0.15); s.rts = False
s.reset_input_buffer()
t0 = time.time()
while time.time() - t0 < dur:
    d = s.read(4096)
    if d:
        sys.stdout.buffer.write(d); sys.stdout.flush()
s.close()
