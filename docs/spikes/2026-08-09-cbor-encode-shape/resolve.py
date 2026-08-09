#!/usr/bin/env python3
# Copyright (c) 2026 Ekorau LLC
#
# #163 — pair each mprof window with the payload whose label was printed just
# before it, and add the two bench-specific columns: activations per BYTE, and
# the C6 time that count predicts.
#
# The offset -> name join is NOT reimplemented here. `nodus/tuvm/tools/
# mprof_resolve.py` (#164) already owns it, including the two things a
# hand-rolled version gets wrong: a method bound in several vtables keeps every
# name rather than the first one, and an offset outside the blob prints as
# UNKNOWN rather than as a plausible wrong name. This script splits the stream
# into windows and calls that.
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "nodus/tuvm/tools"))
import mprof_resolve as MR

# The C6 interpreter baseline, measured by #164 on a fixed-activation Hanoi
# workload (docs/results/2026-08-06-method-call-counter-spike.md). It is what
# turns a host activation count into a DEVICE prediction — counts are a property
# of the program, only per-activation time needs the device.
#
# IT IS AN AVERAGE, AND THE `host ns/act` COLUMN BESIDE IT IS THERE TO SAY WHEN
# THAT AVERAGE HAS STOPPED APPLYING. The model predicted the baseline codec's
# measured `pushEncode` to 4%, but a change that removes a NON-representative
# slice of activations breaks it: dropping ByteBuilder's ByteArray wrapper cut
# 66% of the frame's activations and only 33% of its host time, because the
# activations it removed were one-line accessors costing well under an average
# one. When `host ns/act` moves between two runs, the mix has shifted and the
# prediction is a bound, not an estimate — in that direction, a floor.
C6_US_PER_ACTIVATION = 11.83

TIMING_RE = re.compile(r"^(\S+) bytes=(\d+) ns/encode=(\d+)")


def windows(stderr_text):
    """Split the stream into one text chunk per profile window.

    mprof_resolve.resolve() reads a single window; handed several it would sum
    their rows and report the last header, which would silently merge ten
    payloads into one table.
    """
    chunks, cur = [], None
    for line in stderr_text.splitlines():
        if MR.HDR_RE.match(line):
            cur = [line]
            chunks.append(cur)
        elif cur is not None:
            cur.append(line)
    return ["\n".join(c) for c in chunks]


def main(stdout_path, stderr_path, sidecar_path):
    with open(sidecar_path) as f:
        mmap = json.load(f)

    stdout_lines = Path(stdout_path).read_text().splitlines()
    chunks = windows(Path(stderr_path).read_text())

    print("=" * 78)
    for line in stdout_lines:
        if not line.startswith("--- mprof"):
            print(line)

    # `count:of:` prints its header on stdout BEFORE opening the window, so the
    # Nth header names the Nth window.
    headers = [ln for ln in stdout_lines if ln.startswith("--- mprof ")]

    # Payload -> host ns/encode, from the timing arm of the same run, so the two
    # arms can be divided into ns-per-activation.
    ns = {}
    for line in stdout_lines:
        m = TIMING_RE.match(line)
        if m:
            ns[m.group(1)] = int(m.group(3))

    if not chunks:
        print("\nNO mprof WINDOWS ON stderr.")
        print("The VM was built WITHOUT -DOEVM_METHOD_PROFILE, so profileStart/"
              "profileStop were silent no-ops. That is the visible absence, not "
              "a zero result — rebuild with the flag.")
        return 1

    print("\n" + "=" * 78)
    print("ACTIVATION COUNTS PER ENCODE")
    for i, text in enumerate(chunks):
        label = headers[i] if i < len(headers) else f"(window {i}, unlabelled)"
        m = re.search(r"bytes=(\d+)", label)
        nbytes = int(m.group(1)) if m else None
        name = label.replace("--- mprof ", "").split(" ")[0] \
            if i < len(headers) else "?"

        hdr = None
        for h in MR.HDR_RE.finditer(text):
            hdr = h.groups()
        acts, overflow = int(hdr[2]), int(hdr[3])

        per_byte = f"{acts / nbytes:.2f}" if nbytes else "-"
        c6_ms = acts * C6_US_PER_ACTIVATION / 1000.0
        host_ns_act = f"{ns[name] / acts:.0f}" if name in ns and acts else "-"
        print(f"\n{name:10s} bytes={nbytes}  activations={acts}  "
              f"act/byte={per_byte}  methods={hdr[1]}  overflow={overflow}"
              f"  host ns/act={host_ns_act}"
              f"  -> C6 {c6_ms:.1f} ms (uniform-cost model)")
        if overflow:
            print("  !! OVERFLOW NON-ZERO — the table filled and activations "
                  "were DROPPED. Every count below is a floor, not a count.")
        for nm, n, pct in MR.resolve(mmap, text, top=12):
            print(f"  {n:8d}  {pct:5.1f}%  {nm}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit("usage: resolve.py <stdout> <stderr> <methods.json>")
    sys.exit(main(sys.argv[1], sys.argv[2], sys.argv[3]))
