#!/usr/bin/env python3
# Copyright (c) 2026 Ekorau LLC
#
# #163 — join the VM's mprof dump (blob-relative offsets) against the image's
# .oimg.methods.json sidecar, and pair each window with the payload whose name
# was printed on stdout immediately before it.
#
# The VM can only report `ptr - oimg_blob_base`, because a CompiledMethod carries
# no name at runtime. An offset resolved against the WRONG build is not a wrong
# name, it is a plausible one — so the sidecar is read from beside the very .oimg
# that was run, never from a path the caller chose.
import json
import re
import sys


def main(stdout_path, stderr_path, sidecar_path):
    with open(sidecar_path) as f:
        sidecar = json.load(f)

    stdout_lines = open(stdout_path).read().splitlines()
    stderr_lines = open(stderr_path).read().splitlines()

    # The timing table is plain stdout — reprint it so one report holds both arms.
    print("=" * 78)
    for line in stdout_lines:
        if line.startswith("--- mprof"):
            continue
        print(line)

    # Payload label per window, in order. `count:of:` prints its header on stdout
    # BEFORE opening the window, so the Nth header names the Nth window.
    headers = [ln for ln in stdout_lines if ln.startswith("--- mprof ")]

    windows = []          # (header_index, summary_dict, [(offset, count)])
    cur = None
    for line in stderr_lines:
        m = re.match(r"mprof\[(\w+)\]: methods=(\d+) activations=(\d+) "
                     r"overflow=(\d+) cap=(\d+)", line)
        if m:
            cur = {"why": m.group(1), "methods": int(m.group(2)),
                   "activations": int(m.group(3)), "overflow": int(m.group(4)),
                   "cap": int(m.group(5)), "rows": []}
            windows.append(cur)
            continue
        m = re.match(r"mprof: (@?\S+) (\d+)", line)
        if m and cur is not None:
            cur["rows"].append((m.group(1), int(m.group(2))))

    if not windows:
        print("\nNO mprof WINDOWS ON stderr.")
        print("The VM was built WITHOUT -DOEVM_METHOD_PROFILE, so profileStart/"
              "profileStop were silent no-ops. This is the visible absence, not "
              "a zero result — rebuild with the flag.")
        return 1

    print("\n" + "=" * 78)
    print("ACTIVATION COUNTS PER ENCODE")
    for i, w in enumerate(windows):
        label = headers[i] if i < len(headers) else f"(window {i}, unlabelled)"
        bytes_m = re.search(r"bytes=(\d+)", label)
        nbytes = int(bytes_m.group(1)) if bytes_m else None
        name = label.replace("--- mprof ", "").split(" ")[0] if i < len(headers) else "?"

        per_byte = f"{w['activations'] / nbytes:.2f}" if nbytes else "-"
        # 11.83 us/activation is the C6 interpreter baseline measured by #164
        # (docs/results/2026-08-06-method-call-counter-spike.md). It is what
        # turns a host activation count into a DEVICE prediction.
        c6_ms = w["activations"] * 11.83 / 1000.0
        print(f"\n{name:10s} bytes={nbytes}  activations={w['activations']}  "
              f"act/byte={per_byte}  methods={w['methods']}  "
              f"overflow={w['overflow']}  -> C6 predicted {c6_ms:.1f} ms")
        if w["overflow"]:
            print("  !! OVERFLOW NON-ZERO — the table filled and activations were "
                  "DROPPED. Every count below is a floor, not a count.")
        for off, n in sorted(w["rows"], key=lambda r: -r[1])[:12]:
            entry = sidecar.get(off)
            if entry is None:
                nm = f"<not in sidecar: {off}>"
            else:
                nm = entry["qual"][0] if entry["qual"] else entry["id"]
            print(f"  {n:8d}  {nm}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit("usage: resolve.py <stdout> <stderr> <methods.json>")
    sys.exit(main(sys.argv[1], sys.argv[2], sys.argv[3]))
