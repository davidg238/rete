# Copyright (c) 2026 Ekorau LLC
"""Lint for protocol/MANIFEST.cddl (#243 design §2.2). Errors, never warnings.

The lint plus closed-map validation at the zcbor gates is the completeness
gate: no path and no field ships before the manifest declares it, and no
manifest entry outlives its record.
"""
import re
import sys

from manifestlib import parse, ManifestError


def _overlaps(a, b):
    # Two WILDCARD patterns overlap iff same length and every position is
    # literal-equal or at least one side is a wildcard. Literal-vs-wildcard
    # is legal shadowing (design addendum: diag/spans/ctl under
    # diag/spans/<span> is today's catalogue), so only wildcard pairs are
    # checked here.
    if len(a) != len(b):
        return False
    return all(x.startswith("<") or y.startswith("<") or x == y
               for x, y in zip(a, b))


def lint(text):
    findings = []
    try:
        m = parse(text)
    except ManifestError as e:
        return [str(e)]
    wild = [e for e in m.entries if any(s.startswith("<") for s in e.path_pattern)]
    for i, a in enumerate(wild):
        for b in wild[i + 1:]:
            if _overlaps(a.path_pattern, b.path_pattern):
                findings.append(
                    f"wildcard overlap: {'/'.join(a.path_pattern)} vs {'/'.join(b.path_pattern)}")
    seen = set()
    for e in m.entries:
        p = "/".join(e.path_pattern)
        if p in seen:
            findings.append(f"duplicate path: {p}")
        seen.add(p)
    annotated = {e.rule for e in m.entries}
    referenced_syms = set()
    referred_rules = set()
    for name, body in m.rules.items():
        referenced_syms |= set(re.findall(r"\bsym-[A-Za-z0-9-]+", body))
        for ref in re.findall(r"\b[a-z][a-z0-9-]*\b", body):
            if ref in m.rules and ref != name:
                referred_rules.add(ref)
    for s in m.syms:
        if s not in referenced_syms:
            findings.append(f"unreferenced symbol rule: {s}")
    for s in referenced_syms:
        if s not in m.syms:
            findings.append(f"undefined symbol rule referenced: {s}")
    for e in m.entries:
        if e.rule not in m.rules:
            findings.append(f"annotation names a missing rule: {e.rule}")
    for name in m.rules:
        if name not in annotated and name not in referred_rules:
            findings.append(f"rule with no annotation and no referrer: {name}")
    return findings


if __name__ == "__main__":
    fs = lint(open(sys.argv[1]).read())
    for f in fs:
        print(f"manifest lint: {f}", file=sys.stderr)
    sys.exit(1 if fs else 0)
