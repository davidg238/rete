#!/usr/bin/env python3
"""spike296_console.py — drive one RP2350 running the rete#296 spike image over its USB-CDC
console, with the standard library only (opti7040 has no pyserial). Run under the uucp group:

    echo 'python3 spike296_console.py PORT --arm sram --then reboot --log x.log' | newgrp uucp

Opens PORT (a /dev/serial/by-id/ path survives re-enumeration), asserts DTR, logs every
line with a timestamp, answers the boot window ("spike296: arm?") with --arm until the echo
("spike296: arm=") arrives, and when the node parks ("spike296: parked") sends --then
(bootsel | reboot | none). Exits 0 on a clean park, 2 on timeout, 3 when the port vanishes.
"""
import argparse, fcntl, os, select, sys, termios, time

TIOCMBIS = 0x5416
TIOCM_DTR = 0x002

def open_port(path, wait_s):
    deadline = time.time() + wait_s
    while True:
        try:
            fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            break
        except OSError:
            if time.time() > deadline:
                raise
            time.sleep(0.25)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0                                  # iflag: raw
    attrs[1] = 0                                  # oflag
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL   # cflag
    attrs[3] = 0                                  # lflag: raw, no echo
    attrs[4] = attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    fcntl.ioctl(fd, TIOCMBIS, bytes([TIOCM_DTR, 0, 0, 0]))
    return fd

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--arm", choices=["sram", "flash"], default=None)
    ap.add_argument("--then", choices=["bootsel", "reboot", "none"], default="none")
    ap.add_argument("--log", required=True)
    ap.add_argument("--timeout", type=float, default=900.0)
    ap.add_argument("--open-wait", type=float, default=30.0)
    a = ap.parse_args()
    log = open(a.log, "a")
    def note(s):
        line = "%s %s" % (time.strftime("%H:%M:%S"), s)
        log.write(line + "\n"); log.flush(); print(line, flush=True)
    fd = open_port(a.port, a.open_wait)
    note("open %s" % a.port)
    buf = b""; armed = False; last_arm = 0.0; parked = False; done = False
    t_end = time.time() + a.timeout
    while time.time() < t_end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                chunk = os.read(fd, 4096)
            except OSError as e:
                note("port gone: %s" % e); log.close(); return 3
            if not chunk:
                note("port EOF"); log.close(); return 3
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").rstrip("\r")
                note("< " + text)
                if "spike296: arm=" in text:
                    armed = True
                if "spike296-node: done" in text:
                    done = True
                if "spike296: parked" in text:
                    parked = True
        if a.arm and not armed and time.time() - last_arm > 0.7:
            os.write(fd, (a.arm + "\n").encode()); last_arm = time.time()
            note("> " + a.arm)
        if parked:
            if a.then != "none":
                time.sleep(0.3)
                os.write(fd, (a.then + "\n").encode()); note("> " + a.then)
                time.sleep(0.5)
            log.close(); return 0
    note("timeout (done=%s)" % done); log.close(); return 2

if __name__ == "__main__":
    sys.exit(main())
