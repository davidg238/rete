#!/usr/bin/env python3
"""zephyr/layoutdump/capture.py — bank a layoutdump run as zephyr/layout-<target>.json.

    python3 capture.py PORT OUT.json [--seconds N] [--ref layout-esp32c6.json]

Opens PORT (115200, DTR asserted — the dongle's CDC console gates on it, a UART
adapter ignores it), reads for N seconds (default 20 — longer than any BOOT_DELAY in
this directory), finds the ONE JSON object dump_layout.c prints, re-serialises it
exactly the way the checked-in layouts are (json.dump, indent=2, key order kept) and
writes OUT. With --ref it also says whether the result is byte-identical to another
target's file, because that is the question every capture so far has been asked
(esp32 = esp32c6 = nrf52840, rete#281) and the answer is a MEASUREMENT each time,
never an assumption — run.sh still refuses to bake without this target's own file.

Refuses on: no JSON at all (nothing printed — wrong port, wrong baud, board not reset,
reader opened after BOOT_DELAY), a JSON that does not parse (a TRUNCATED capture — the
dongle's console discards under burst, rete#281), or a JSON with no "structs" key
(some other program's output). An empty file is never written.

Reset the board AFTER this is running: dump_layout.c prints once from main() and
returns. On a UART-adapter board (Pico 2, Feather RP2040) copying the .uf2 resets it,
so start this, then drag the file. Written 2026-09-19 for rete#302.
"""
import argparse, json, sys, time, filecmp
import serial

ap = argparse.ArgumentParser()
ap.add_argument("port")
ap.add_argument("out")
ap.add_argument("--seconds", type=float, default=20.0)
ap.add_argument("--ref", help="another target's layout-*.json to compare against")
a = ap.parse_args()

s = serial.Serial(a.port, 115200, timeout=0.2, dsrdtr=False)
s.dtr = True
buf = bytearray()
t0 = time.time()
while time.time() - t0 < a.seconds:
    d = s.read(4096)
    if d:
        buf += d
        sys.stderr.write(d.decode("utf-8", "replace"))
s.close()

# Zephyr's console emits \r\n (UART and CDC alike); the marker is matched on the
# bare form so the search cannot depend on which console the bytes came through.
# Found 2026-09-21: the first RP2350 USB-CDC capture was COMPLETE and refused here.
text = buf.decode("utf-8", "replace").replace("\r\n", "\n")
start = text.find('{\n  "oop_size"')
if start < 0:
    sys.exit(f"\ncapture: no layout JSON in {len(buf)} bytes from {a.port} — "
             "wrong port/baud, or the board was not reset while this was reading")
depth = 0; end = -1
for i in range(start, len(text)):
    if text[i] == "{": depth += 1
    elif text[i] == "}":
        depth -= 1
        if depth == 0: end = i + 1; break
if end < 0:
    sys.exit("\ncapture: JSON never closed — a TRUNCATED capture (console dropped bytes); "
             "nothing written")
try:
    layout = json.loads(text[start:end])
except json.JSONDecodeError as e:
    sys.exit(f"\ncapture: JSON does not parse ({e}); nothing written")
if "structs" not in layout or "oop_size" not in layout:
    sys.exit("\ncapture: parsed an object with no structs/oop_size — not a layout dump")

with open(a.out, "w") as f:
    json.dump(layout, f, indent=2)
    f.write("\n")
n = len(layout["structs"])
print(f"\ncapture: {a.out} — oop_size {layout['oop_size']}, {n} structs")
if a.ref:
    same = filecmp.cmp(a.out, a.ref, shallow=False)
    print(f"capture: {'BYTE-IDENTICAL to' if same else 'DIFFERS from'} {a.ref}")
