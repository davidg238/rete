#!/usr/bin/env python3
# Copyright (c) 2026 Ekorau LLC
"""#164: turn a node's per-method call-count dump into named rows.

    mprof_resolve.py <image.oimg.map.json> [--top N] [dump.log ...]

The VM reports a method as a blob-relative byte offset, because a CompiledMethod
carries no name and a raw address is meaningless off-node. The image map that
`ns_compile_oevm.py --format=oimg` writes beside the image (protocol/IMAGE-ARTIFACTS.md)
is the other half of the join.

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

# Allocation attribution (design docs/design/2026-08-12-allocation-attribution-design.md).
# A SEPARATE prefix, not extra fields on `mprof:` — DUMP_RE above is anchored at
# exactly two fields, so widening that line would make it match NOTHING and
# report "no mprof: lines found": a silent false negative in the only tool that
# reads this output. test_the_activation_regex_still_matches_when_alloc_rows_are_present
# is the guard.
ALLOC_RE = re.compile(r"mprofa:\s+(@?\S+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*$", re.M)
ALLOC_HDR_RE = re.compile(r"mprofa\[(\w+)\]:\s+noframe_arena=(\d+)/(\d+)\s+"
                          r"noframe_cheap=(\d+)/(\d+)\s+allocOverflow=(\d+)")


def name_of(entry):
    return None if entry is None else entry["name"]


def resolve(mmap, text, top=0):
    """Join `mprof:` rows against the image map's methods and return
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


def resolve_alloc(mmap, text, top=0):
    """Join `mprofa:` rows against the image map's methods and return
    [(name, arena_b, arena_n, cheap_b, cheap_n, pct), ...] sorted by total
    bytes descending.

    The denominator is the attributed bytes PLUS the no-frame bucket, so a run
    whose allocations mostly happen outside any frame cannot read as "these few
    methods account for everything". That bucket is boot and prim contexts; if
    it is large the table is not yet answering the question, and main() says so
    rather than leaving the reader to do the division.
    """
    rows = [(k, int(ab), int(an), int(cb), int(cn))
            for k, ab, an, cb, cn in ALLOC_RE.findall(text)]

    hdr = None
    for m in ALLOC_HDR_RE.finditer(text):
        hdr = m.groups()                       # last window wins
    nf_bytes = (int(hdr[1]) + int(hdr[3])) if hdr else 0

    total = sum(ab + cb for _, ab, _, cb, _ in rows) + nf_bytes or 1

    out = []
    for key, ab, an, cb, cn in sorted(rows, key=lambda r: -(r[1] + r[3])):
        if key.startswith("@"):
            name = f"UNKNOWN (not in the image blob: {key})"
        else:
            entry = mmap.get(key)
            name = name_of(entry) if entry is not None else \
                f"UNKNOWN (offset {key} absent from the map — wrong build?)"
        out.append((name, ab, an, cb, cn, (ab + cb) / total * 100))
    return out[:top] if top else out


def print_alloc(mmap, text, top=0):
    """The `--alloc` report. Kept beside resolve_alloc so the warnings that make
    the table honest cannot be lost by a caller that only wants the rows."""
    # ALL rows, never the truncated view: both numbers below are properties of
    # the WINDOW, not of what a caller asked to see. `--top 15` used to make
    # "distinct allocating methods" print 15 — the D8 sizing input reading back
    # the display flag — and shrank the no-frame denominator to the shown rows,
    # which inflates the §8 risk metric in the direction that hides it.
    all_rows = resolve_alloc(mmap, text)
    arows = all_rows[:top] if top else all_rows
    hdr = None
    for m in ALLOC_HDR_RE.finditer(text):
        hdr = m.groups()

    print("\n--- allocation attribution ---")
    if hdr is None:
        print("no 'mprofa[' summary line found — the window never reported, or "
              "the build predates allocation attribution.")
    else:
        why, nfab, nfan, nfcb, nfcn, aovf = hdr
        attributed = sum(ab + cb for _, ab, _, cb, _, _ in all_rows)
        nf = int(nfab) + int(nfcb)
        denom = attributed + nf or 1
        print(f"window={why} allocOverflow={aovf}")
        print(f"no-frame bytes: arena={nfab} ({nfan} allocs) "
              f"cheap={nfcb} ({nfcn} allocs) = {nf / denom * 100:.1f}% of all "
              f"attributed bytes")
        if nf > attributed:
            print("WARNING: the no-frame bucket EXCEEDS everything attributed to "
                  "a method. Most allocation is happening outside any frame, so "
                  "this table is not yet answering 'who allocates'.")
        if int(aovf):
            # Same reasoning as the activation table's overflow: the slotted
            # counts stay EXACT, what is lost is completeness.
            print(f"INCOMPLETE: allocOverflow={aovf} allocations were dropped — "
                  f"the table saturated, so METHODS ARE MISSING. Counts below "
                  f"are exact; percentages are lower bounds.")

    print(f"\n{'arenaB':>10} {'arenaN':>8} {'cheapB':>10} {'cheapN':>8} "
          f"{'%':>6}  method")
    for name, ab, an, cb, cn, pct in arows:
        print(f"{ab:>10} {an:>8} {cb:>10} {cn:>8} {pct:>5.1f}%  {name}")
    if top and len(all_rows) > top:
        print(f"\nshowing top {top} of {len(all_rows)} allocating methods — "
              f"percentages are of ALL attributed bytes, not of the shown rows.")
    print(f"\ndistinct allocating methods: {len(all_rows)}   "
          f"(stage-2 sizing input, design D8)")


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip())
        return 2
    with open(argv[1]) as f:
        mmap = json.load(f)["methods"]

    # Filter out flags (and --top's value) from the file argument list
    file_paths = []
    i = 2
    while i < len(argv):
        if argv[i] == "--top":
            i += 2  # skip the flag and its value
        elif argv[i] == "--alloc":
            i += 1
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

    if "--alloc" in argv:
        print_alloc(mmap, text, top=top)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
