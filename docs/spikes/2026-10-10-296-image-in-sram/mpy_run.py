#!/usr/bin/env python3
"""mpy_run.py PORT SCRIPT [--bootloader] [--log FILE] — run a .py file on a MicroPython board over
its USB-CDC REPL using the raw-REPL protocol, standard library only (no mpremote on opti7040).
Prints the script's output; with --bootloader sends machine.bootloader() afterwards so the board
drops back into BOOTSEL for the next UF2. Run under `newgrp uucp` like spike296_console.py.
"""
import argparse, fcntl, os, select, sys, termios, time

TIOCMBIS = 0x5416
TIOCM_DTR = 0x002

def open_port(path, wait_s=30):
    deadline = time.time() + wait_s
    while True:
        try:
            fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK); break
        except OSError:
            if time.time() > deadline: raise
            time.sleep(0.25)
    a = termios.tcgetattr(fd)
    a[0] = 0; a[1] = 0; a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL; a[3] = 0
    a[4] = a[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, a)
    fcntl.ioctl(fd, TIOCMBIS, bytes([TIOCM_DTR, 0, 0, 0]))
    return fd

def read_until(fd, marker, timeout):
    buf = b""; end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try: chunk = os.read(fd, 4096)
            except OSError: return buf, False
            if not chunk: return buf, False
            buf += chunk
            if marker in buf: return buf, True
    return buf, False

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port"); ap.add_argument("script")
    ap.add_argument("--bootloader", action="store_true")
    ap.add_argument("--log", default=None)
    ap.add_argument("--timeout", type=float, default=900.0)
    a = ap.parse_args()
    code = open(a.script, "rb").read()
    fd = open_port(a.port)
    os.write(fd, b"\r\x03\x03"); time.sleep(0.3)
    os.write(fd, b"\r\x01")
    _, ok = read_until(fd, b"raw REPL; CTRL-B to exit\r\n>", 5)
    if not ok:
        print("no raw REPL prompt", file=sys.stderr); return 2
    for i in range(0, len(code), 256):
        os.write(fd, code[i:i+256]); time.sleep(0.01)
    os.write(fd, b"\x04")
    _, ok = read_until(fd, b"OK", 5)
    if not ok:
        print("no OK after upload", file=sys.stderr); return 2
    out, ok = read_until(fd, b"\x04>", a.timeout)
    text = out.split(b"\x04>")[0].decode("utf-8", "replace")
    print(text, end="" if text.endswith("\n") else "\n")
    if a.log:
        with open(a.log, "a") as f:
            f.write("%s %s\n%s\n" % (time.strftime("%H:%M:%S"), a.script, text))
    if a.bootloader:
        os.write(fd, b"\x02"); time.sleep(0.2)   # friendly REPL
        os.write(fd, b"import machine; machine.bootloader()\r\n")
        time.sleep(0.5)
    return 0 if ok else 2

if __name__ == "__main__":
    sys.exit(main())
