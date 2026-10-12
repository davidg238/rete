#!/usr/bin/env python3
"""weact_bootsel.py USB PORT LOG — put a WeAct running the #296 console harness into BOOTSEL (rete#37 board A/B).

Exits 0 at once when USB (e.g. 1-5) already shows a BOOTSEL volume. Otherwise it reads PORT and answers the
harness's two prompts with `bootsel`: the boot window ("spike296: arm?") and the park after a run ("spike296:
parked"). A board mid-run is waited for (up to 30 min). A board that never prompts (wedged, no reset) needs a hand
on BOOTSEL+RESET; this exits 2 then. Run under the uucp group."""
import glob, os, select, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../spikes/2026-10-10-296-image-in-sram"))
from spike296_console import open_port

usb, port, logpath = sys.argv[1:4]


def in_bootsel():
    for b in glob.glob("/sys/block/sd*"):
        if f"/{usb}/" in os.path.realpath(b) and os.popen(f"lsblk -rno LABEL /dev/{os.path.basename(b)}1 2>/dev/null").read().strip() == "RP2350":
            return True
    return False


if in_bootsel():
    sys.exit(0)
log = open(logpath, "a"); fd = None; buf = b""; t_end = time.time() + 1800
while time.time() < t_end:
    if in_bootsel():
        log.write(time.strftime("%H:%M:%S") + " in BOOTSEL\n"); sys.exit(0)
    if fd is None:
        try: fd = open_port(port, 5)
        except OSError: time.sleep(1); continue
    if not select.select([fd], [], [], 0.5)[0]:
        continue
    try: d = os.read(fd, 4096)
    except OSError: os.close(fd); fd = None; continue
    buf += d
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1); t = line.decode("utf-8", "replace").rstrip("\r")
        log.write(time.strftime("%H:%M:%S < ") + t + "\n"); log.flush()
        if "spike296: arm?" in t or "spike296: parked" in t:
            os.write(fd, b"bootsel\n"); log.write("> bootsel\n"); log.flush()
log.write("timeout: no prompt\n"); sys.exit(2)
