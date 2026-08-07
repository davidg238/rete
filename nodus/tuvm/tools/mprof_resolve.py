#!/usr/bin/env python3
# Copyright (c) 2026 Ekorau LLC
"""#164: turn a node's per-method call-count dump into named rows.

    mprof_resolve.py <image.oimg.methods.json> [--top N] [dump.log ...]

The VM reports a method as a blob-relative byte offset, because a CompiledMethod
carries no name and a raw address is meaningless off-node. The sidecar map that
`ns_compile_oevm.py --format=oimg` writes beside the image is the other half of
the join.

Reads the `mprof:` lines out of anything you hand it — a console collector log
is fine, the surrounding traffic is ignored — and prints the table sorted by
count.

REFUSES to guess. An offset absent from the map prints as UNKNOWN with the
offset intact, because a profile that silently drops the rows it cannot name is
a profile that reads as complete while hiding its largest caller. A non-zero
`overflow` is reported as INCOMPLETE, not INVALID: the slotted counts are exact,
but methods are missing and the percentages are lower bounds (the denominator
includes the dropped activations).
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


def resolve(mmap, text, top=0):
    """Join `mprof:` rows against the sidecar map and return
    [(name, count, percent), ...] sorted by count descending.

    The denominator is activations + overflow, NOT the header's activations.
    A saturated table drops activations entirely: they are counted in aggregate
    by `overflow` but attributed to no slot, so dividing by the slotted sum
    alone inflates every percentage. Adding overflow back makes each percentage
    a correct LOWER BOUND, which is a true statement rather than a flattering
    one. `top` truncates the returned rows and never the denominator.
    """
    header = None
    for m in HDR_RE.finditer(text):
        header = m.groups()                # last window wins
    rows = [(k, int(n)) for k, n in DUMP_RE.findall(text)]

    overflow = int(header[3]) if header else 0
    total = sum(n for _, n in rows) + overflow or 1

    out = []
    for key, n in sorted(rows, key=lambda r: -r[1]):
        if key.startswith("@"):
            name = f"UNKNOWN (not in the image blob: {key})"
        else:
            entry = mmap.get(key)
            name = name_of(entry) if entry is not None else \
                f"UNKNOWN (offset {key} absent from the map — wrong build?)"
        out.append((name, n, n / total * 100))
    return out[:top] if top else out


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip())
        return 2
    with open(argv[1]) as f:
        mmap = json.load(f)

    # Filter out --top and its value from the file argument list
    file_paths = []
    i = 2
    while i < len(argv):
        if argv[i] == "--top":
            i += 2  # skip the flag and its value
        else:
            file_paths.append(argv[i])
            i += 1

    text = "".join(open(p).read() for p in file_paths) if file_paths \
        else sys.stdin.read()

    header = None
    for m in HDR_RE.finditer(text):
        header = m.groups()
    rows = [(k, int(n)) for k, n in DUMP_RE.findall(text)]
    if not rows:
        print("no 'mprof:' lines found — was the image built with "
              "-DOEVM_METHOD_PROFILE and did the dump fire?", file=sys.stderr)
        return 1

    top = 0
    if "--top" in argv:
        top = int(argv[argv.index("--top") + 1])

    if header:
        why, methods, acts, overflow, cap = header
        print(f"window={why} methods={methods} activations={acts} cap={cap}")
        if int(overflow):
            # Not INVALID: the slotted counts are EXACT. A method already in the
            # table always increments correctly; only a NEW method arriving at a
            # full table is dropped. What overflow costs is completeness — some
            # methods are missing entirely — and it is already paid for in the
            # denominator above, which makes each percent a lower bound.
            print(f"INCOMPLETE: overflow={overflow} activations were dropped — "
                  f"the table saturated, so methods are MISSING from this table. "
                  f"Counts below are exact; percentages are lower bounds.")
        rows_len = len(rows)
        if rows_len != int(methods):
            print(f"WARNING: {rows_len} rows for methods={methods} — the log is "
                  f"truncated or holds more than one window.")
        rows_sum = sum(n for _, n in rows)
        if rows_sum != int(acts):
            print(f"WARNING: rows sum to {rows_sum}, header says {acts} — mixed "
                  f"windows.")

    resolved = resolve(mmap, text, top=top)
    unknown = sum(1 for name, _, _ in resolved if name.startswith("UNKNOWN"))
    print(f"\n{'count':>12}  {'%':>6}  method")
    for name, n, pct in resolved:
        print(f"{n:>12}  {pct:>5.1f}%  {name}")
    if top:
        print(f"\nshowing top {top} of {len(resolve(mmap, text))} rows — "
              f"percentages are of ALL activations, not of the shown rows.")
    if unknown:
        print(f"\n{unknown} row(s) unresolved — a map from a DIFFERENT build "
              f"resolves nothing and looks identical to a quiet program.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
