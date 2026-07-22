import sys, time, serial
port = sys.argv[1]; secs = float(sys.argv[2]) if len(sys.argv) > 2 else 30
p = serial.Serial(port, 115200, timeout=0.2)
p.dtr = False; p.rts = True; time.sleep(0.2); p.rts = False
print(f"--- reset issued, reading {secs}s ---", flush=True)
t0 = time.time(); buf = b""
while time.time() - t0 < secs:
    d = p.read(4096)
    if d:
        buf += d
        sys.stdout.write(d.decode("utf-8", "replace")); sys.stdout.flush()
p.close()
print(f"\n--- {len(buf)} bytes read ---")
