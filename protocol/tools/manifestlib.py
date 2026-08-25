# Copyright (c) 2026 Ekorau LLC
"""Parser for protocol/MANIFEST.cddl's restricted subset + ;# annotations (#243).

Shared by manifest_lint.py and the test gates; porta/internal/manifest is the
Go twin. Stdlib only. The file it reads is 100% stock CDDL — this parser reads
the ;# annotation lines (the path binding) and enough of the rule structure to
answer "which rule, which kind, which class, which direction" for a path.

Matching (design §3 + addendum): segment-wise; <var> matches exactly one
segment; a literal entry SHADOWS a wildcard entry; two overlapping wildcard
entries are a lint error (manifest_lint.py), so match order never decides.
"""
import re
from dataclasses import dataclass


class ManifestError(Exception):
    pass


ANNOT_RE = re.compile(r"^;#\s+(.*)$")
SYM_RE = re.compile(r'^(sym-[A-Za-z0-9-]+)\s*=\s*#6\.39\("([^"]+)"\)\s*$')
RULE_RE = re.compile(r"^([a-z][a-z0-9-]*)\s*=\s*(.*)$")
KINDS = {"cell", "stream"}
CLASSES = {"routine", "must-deliver", "drop-first"}
DIRS = {"up", "down", "gw"}


@dataclass
class Entry:
    path_pattern: list  # segments; "<...>" is a one-segment wildcard
    kind: str
    cls: str
    dir: str
    rule: str


class Manifest:
    def __init__(self, entries, rules, syms):
        self.entries, self.rules, self.syms = entries, rules, syms
        self._literal = {}
        self._wild = []
        for e in entries:
            if any(s.startswith("<") for s in e.path_pattern):
                self._wild.append(e)
            else:
                self._literal["/".join(e.path_pattern)] = e

    def match(self, path):
        if path in self._literal:
            return self._literal[path]
        segs = path.split("/")
        for e in self._wild:
            if len(e.path_pattern) != len(segs):
                continue
            if all(p.startswith("<") or p == s
                   for p, s in zip(e.path_pattern, segs)):
                return e
        return None


def _parse_annot(line, lineno):
    fields = {}
    for tok in line.split():
        if "=" not in tok:
            raise ManifestError(f"line {lineno}: malformed annotation token {tok!r}")
        k, v = tok.split("=", 1)
        fields[k] = v
    for req in ("path", "kind", "class", "dir"):
        if req not in fields:
            raise ManifestError(f"line {lineno}: annotation missing {req}=")
    if fields["kind"] not in KINDS:
        raise ManifestError(f"line {lineno}: kind {fields['kind']!r} not in {sorted(KINDS)}")
    if fields["class"] not in CLASSES:
        raise ManifestError(f"line {lineno}: class {fields['class']!r} not in {sorted(CLASSES)}")
    if fields["dir"] not in DIRS:
        raise ManifestError(f"line {lineno}: dir {fields['dir']!r} not in {sorted(DIRS)}")
    return fields


def _bracket_delta(s):
    return (s.count("[") + s.count("{") + s.count("(")
            - s.count("]") - s.count("}") - s.count(")"))


def parse(text):
    entries, rules, syms = [], {}, {}
    pending = []  # annotations awaiting the rule beneath them
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i].rstrip()
        i += 1
        m = ANNOT_RE.match(line)
        if m:
            pending.append((_parse_annot(m.group(1), i), i))
            continue
        if not line or line.lstrip().startswith(";"):
            continue  # comments and blanks; pending annotations survive them
        m = SYM_RE.match(line)
        if m:
            if pending:
                raise ManifestError(
                    f"line {i}: annotation may not bind a sym-* rule")
            syms[m.group(1)] = m.group(2)
            continue
        m = RULE_RE.match(line)
        if not m:
            raise ManifestError(
                f"line {i}: not a rule, annotation, or comment: {line!r}")
        name, body = m.group(1), m.group(2)
        if name.startswith("sym-"):
            raise ManifestError(
                f"line {i}: sym-* rule not of the form sym-x = #6.39(\"x\"): {line!r}")
        depth = _bracket_delta(body)
        while depth > 0 and i < len(lines):
            nxt = lines[i]
            i += 1
            body += " " + nxt.strip()
            depth += _bracket_delta(nxt)
        if name in rules:
            raise ManifestError(f"line {i}: rule {name!r} defined twice")
        rules[name] = body
        for fields, _lineno in pending:
            entries.append(Entry(fields["path"].split("/"), fields["kind"],
                                 fields["class"], fields["dir"], name))
        pending = []
    if pending:
        raise ManifestError(
            f"line {pending[0][1]}: annotation has no rule beneath it")
    return Manifest(entries, rules, syms)
