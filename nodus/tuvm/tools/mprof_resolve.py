#!/usr/bin/env python3
# Copyright (c) 2026 Ekorau LLC
"""#164: turn a node's per-method call-count dump into named rows.

    mprof_resolve.py <image.oimg.methods.json> [dump.log ...]

The VM reports a method as a blob-relative byte offset, because a CompiledMethod
carries no name and a raw address is meaningless off-node. The sidecar map that
`ns_compile_oevm.py --format=oimg` writes beside the image is the other half of
the join.

Reads the `mprof:` lines out of anything you hand it — a console collector log
is fine, the surrounding traffic is ignored — and prints the table sorted by
count.

REFUSES to guess. An offset absent from the map prints as UNKNOWN with the
offset intact, because a profile that silently drops the rows it cannot name is
a profile that reads as complete while hiding its largest caller. Likewise a
non-zero `overflow` is reported as INVALID at the top: a saturated table has
dropped activations, and no row in it can be trusted as a total.
"""
import json
import re
import sys

DUMP_RE = re.compile(r"mprof:\s+(@?\S+)\s+(\d+)\s*$", re.M)   # re.M: one match per LINE
HDR_RE = re.compile(r"mprof\[(\w+)\]:\s+methods=(\d+)\s+activations=(\d+)\s+"
                    r"overflow=(\d+)\s+cap=(\d+)")


def name_of(entry):
    if entry is None:
        return None
    qual = entry.get("qual") or []
    if len(qual) == 1:
        return qual[0]
    if len(qual) > 1:                     # bound in several vtables — say so
        return " | ".join(qual)
    return entry["id"]                    # a block, or an unbound method


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip())
        return 2
    with open(argv[1]) as f:
        mmap = json.load(f)

    text = "".join(open(p).read() for p in argv[2:]) if len(argv) > 2 \
        else sys.stdin.read()

    header = None
    for m in HDR_RE.finditer(text):
        header = m.groups()                # last window wins
    rows = [(k, int(n)) for k, n in DUMP_RE.findall(text)]
    if not rows:
        print("no 'mprof:' lines found — was the image built with "
              "-DOEVM_METHOD_PROFILE and did the dump fire?", file=sys.stderr)
        return 1

    if header:
        why, methods, acts, overflow, cap = header
        print(f"window={why} methods={methods} activations={acts} cap={cap}")
        if int(overflow):
            print(f"INVALID: overflow={overflow} — the table saturated and "
                  f"dropped activations. No count below is a total.")
        # The dump is emitted per slot, so these must agree; if they do not, the
        # log was truncated or two windows were spliced together.
        if len(rows) != int(methods):
            print(f"WARNING: {len(rows)} rows for methods={methods} — the log is "
                  f"truncated or holds more than one window.")
        if sum(n for _, n in rows) != int(acts):
            print(f"WARNING: rows sum to {sum(n for _, n in rows)}, header says "
                  f"{acts} — mixed windows.")

    total = sum(n for _, n in rows) or 1
    unknown = 0
    print(f"\n{'count':>12}  {'%':>6}  method")
    for key, n in sorted(rows, key=lambda r: -r[1]):
        if key.startswith("@"):
            name = f"UNKNOWN (not in the image blob: {key})"
            unknown += 1
        else:
            entry = mmap.get(key)
            if entry is None:
                name = f"UNKNOWN (offset {key} absent from the map — wrong build?)"
                unknown += 1
            else:
                name = name_of(entry)
        print(f"{n:>12}  {100.0 * n / total:>5.1f}%  {name}")
    if unknown:
        print(f"\n{unknown} row(s) unresolved — a map from a DIFFERENT build "
              f"resolves nothing and looks identical to a quiet program.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
