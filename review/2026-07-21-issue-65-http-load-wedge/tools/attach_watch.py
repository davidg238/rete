"""Attach to the console WITHOUT resetting the board.

reset_watch.py always pulses RTS (EN), which reboots the target -- fine for
catching a boot banner, fatal if you need to attach to a node mid-experiment.
Setting dtr/rts False BEFORE open() makes pyserial apply them as the port's
initial state, so EN and IO0 are never driven low and the target keeps running.
"""
import sys, time, serial
port, secs = sys.argv[1], float(sys.argv[2])
p = serial.Serial()
p.port = port; p.baudrate = 115200; p.timeout = 0.2
p.dtr = False; p.rts = False          # MUST be set before open(): no reset pulse
p.open()
print(f"--- attached (no reset), reading {secs}s ---", flush=True)
t0 = time.time()
while time.time() - t0 < secs:
    d = p.read(4096)
    if d:
        sys.stdout.write(d.decode("utf-8", "replace")); sys.stdout.flush()
p.close()
