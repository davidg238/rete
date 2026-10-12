#!/usr/bin/env python3
"""Run awfy.nsl on Primordial Soup, the Newspeak reference VM, for a host comparison (rete#37).

nsl is a subset of Newspeak, so awfy.nsl compiles on Soup as written except for its one clock
read: nsl's `0 millis` primitive becomes Soup's `platform time Stopwatch`. This script makes that
substitution (the method `millisecondsToRun:` and one slot), compiles with Soup's CompilerApp and
runs the result. Soup reads a decimal literal as an exact Fraction and has no float literal
syntax, so each distinct one becomes a module slot computed once with `asFloat` (`floats`). Json's substring read is swapped for Soup's String>>copyFrom:to: (JSON_SWAPS).
Everything else reaches Soup unchanged, with one addition on Soup's side: Soup's
Newspeak has no `==` (its Object>>= is the identity primitive, 135), and AWFY's SOMns sources use
`==` for identity throughout. A copy of Soup's kernel gets `Object>>==` bound to that same
primitive, so `==` keeps its meaning where a class overrides `=`; and `Object>>notNil`, which
AWFY's sources also assume. Soup's own tree is not touched.

Soup lives at ~/workspace/primordialsoup (as nodus/tests/run_tests_soup.py expects), built with
`scons -Q out/ReleaseHost/primordialsoup out/snapshots/CompilerApp.vfuel`.

Usage: python3 nodus/tests/00_perf/awfy-soup.py [work-dir [source.nsl]]
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOUP = Path.home() / "workspace" / "primordialsoup"
EXE = SOUP / "out" / "ReleaseHost" / "primordialsoup"
COMPILER = SOUP / "out" / "snapshots" / "CompilerApp.vfuel"

CLOCK_NSL = re.compile(r"  millisecondsToRun: aBlock = \(\n.*?\n  \)\n", re.S)
CLOCK_SOUP = """  millisecondsToRun: aBlock = (
    | sw |
    sw:: Stopwatch new start.
    aBlock value.
    ^ sw elapsedMilliseconds
  )
"""
OBJECT_SOUP = "public class Object = super Object (\n) (\n"
IDENTITY = ("public == other <Object> ^<Boolean> = (\n\t(* :pragma: primitive: 135 *)\n\tpanic.\n)\n"
            "public notNil ^<Boolean> = ( ^ self isNil not )\n")
# Json's substrings: nsl's String has no copyFrom:to: (no String class; its methods are prims), so
# awfy.nsl reads them through one kernel ReadStream per parse. Soup's String has copyFrom:to:.
JSON_SWAPS = [
    ("    private captureStream = platform ReadStream on: string.\n",
     "    private captureStream = nil.\n"),
    ("      captureStream reset.\n      captureStream skip: start - 1.\n      ^ captureStream next: stop - start + 1\n",
     "      ^ input copyFrom: start to: stop\n"),
]
DECIMAL = re.compile(r"(?<![\w.)])(-?\d+\.\d+)(?![\w.])")  # a sign against the digits is part of it


def floats(src: str) -> tuple[str, list[str]]:
    """Soup reads a decimal literal as an exact Fraction (`0.1 + 0.2` is `(3/10)`) and has no float
    literal syntax; nsl, SOMns and AWFY mean a Float. Each distinct literal, outside strings and
    comments, becomes a module slot computed once (`float_2_0 = 2.0 asFloat`), so a loop reads a
    slot rather than converting a Fraction on every evaluation. Returns the source and the slots."""
    out, i, n = [], 0, len(src)
    slots: dict[str, str] = {}

    def slot(lit: str) -> str:
        name = "float_" + lit.replace("-", "m").replace(".", "_")
        slots[name] = lit
        return name

    while i < n:
        if src.startswith("(*", i):
            j = src.index("*)", i) + 2
        elif src[i] == "'":
            j = i + 1
            while True:
                j = src.index("'", j) + 1
                if not src.startswith("'", j):
                    break
                j += 1
        else:
            j = i
            while j < n and src[j] != "'" and not src.startswith("(*", j):
                j += 1
            out.append(DECIMAL.sub(lambda m: slot(m.group(1)), src[i:j]))
            i = j
            continue
        out.append(src[i:j])
        i = j
    return "".join(out), [f"private {k} = {v} asFloat." for k, v in sorted(slots.items())]


HEADER_NSL = "class Awfy usingPlatform: platform = (\n| "
HEADER_SOUP = "class Awfy usingPlatform: platform = (\n| private Stopwatch = platform time Stopwatch.\n  "


def main() -> int:
    work = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(tempfile.mkdtemp(prefix="awfy-soup-"))
    work.mkdir(parents=True, exist_ok=True)
    src = (Path(sys.argv[2]) if len(sys.argv) > 2 else HERE / "awfy.nsl").read_text()
    src = re.sub(r"^\s*\(\*.*?\*\)\s*", "", src, count=1, flags=re.S)  # Soup wants the class first
    src, n_clock = CLOCK_NSL.subn(CLOCK_SOUP, src)
    if n_clock != 1 or HEADER_NSL not in src:
        sys.exit("awfy-soup: awfy.nsl's clock method or module header moved; update this script")
    src, float_slots = floats(src)
    src = src.replace(HEADER_NSL, HEADER_SOUP + "".join(f"{d}\n  " for d in float_slots), 1)
    for old, new in JSON_SWAPS:
        if src.count(old) != 1:
            sys.exit("awfy-soup: Json's substring code moved; update this script")
        src = src.replace(old, new)
    ns, fuel = work / "AwfyBench.ns", work / "AwfyBench.vfuel"
    ns.write_text(src)
    libdir = work / "newspeak"
    libdir.mkdir(exist_ok=True)
    for p in (SOUP / "newspeak").glob("*.ns"):
        text = p.read_text()
        if p.name == "KernelForPrimordialSoup.ns":
            if text.count(OBJECT_SOUP) != 1:
                sys.exit("awfy-soup: Soup's Object declaration moved; update this script")
            text = text.replace(OBJECT_SOUP, OBJECT_SOUP + IDENTITY, 1)
        (libdir / p.name).write_text(text)
    lib = sorted(str(p) for p in libdir.glob("*.ns"))
    c = subprocess.run([str(EXE), str(COMPILER), *lib, str(ns), "Runtime", "AwfyBench", str(fuel)],
                       capture_output=True, text=True)
    if c.returncode != 0 or not fuel.exists():
        sys.stderr.write((c.stderr or c.stdout)[:4000])
        return 1
    return subprocess.run([str(EXE), str(fuel)]).returncode


if __name__ == "__main__":
    sys.exit(main())
