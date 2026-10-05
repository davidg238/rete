import json
import subprocess
import sys
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parent
GRAMMAR = TOOLS.parent / "grammar" / "ns-parse.js"
sys.path.insert(0, str(TOOLS))

# nodus/tests since the #79 consolidation — the "sibling ns-tests repo" these
# skips guarded on was folded in, so the guard could never un-skip (rete#228).
NS_TESTS = TOOLS.parent.parent / "tests"
LAYOUT = TOOLS.parents[2] / "nodus" / "oevm" / "zephyr" / "layout-esp32c6.json"


def _parse(src_text, tmp_path):
    f = tmp_path / "frag.nsl"
    f.write_text(src_text)
    r = subprocess.run(["node", str(GRAMMAR), str(f)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return json.loads(r.stdout)


# A two-method class (packageUsing: manifest form — the proven fixture idiom).
# `^ 42` sits on line 2, `^ self answer` on line 3 (1-based source lines).
FIXTURE = """class Probe packageUsing: manifest = () (
public answer = ( ^ 42 )
public ask = ( ^ self answer )
) : ( )
"""


def test_compiler_collects_per_method_source_maps(tmp_path):
    import ns_compile_oevm as C
    ast = _parse(FIXTURE, tmp_path)
    compiler = C.OevmCompiler(ast)
    compiler.compile()

    sm = compiler.source_maps
    assert sm, "expected a non-empty source_maps dict"
    # Every entry is a list of (offset, line) with sane values.
    for name, pairs in sm.items():
        assert ">>" in name
        assert pairs, f"{name} should have at least one pair"
        for offset, line in pairs:
            assert isinstance(offset, int) and offset >= 0
            assert isinstance(line, int) and line >= 1
        offsets = [o for o, _ in pairs]
        assert offsets == sorted(offsets), f"{name} offsets must be non-decreasing"

    # The two source statements land on lines 2 and 3 (0-based file -> 1-based
    # lines: `^ 42` is line 2, `^ self answer` is line 3).
    all_lines = {line for pairs in sm.values() for _, line in pairs}
    assert {2, 3} <= all_lines


@pytest.mark.skipif(not NS_TESTS.exists(), reason="nodus/tests not present (broken checkout)")
def test_factory_body_initializers_are_mapped(tmp_path):
    """Regression test for Fix 1: compile_initializer must register source maps
    for initializers that have a factory body (initBody statements)."""
    src = NS_TESTS / "04_somns_basic" / "13_factory_body.nsl"
    out = tmp_path / "img.oimg"
    r = subprocess.run(
        [sys.executable, str(TOOLS / "ns_compile_oevm.py"), str(src), str(out),
         "--format=oimg", f"--layout={LAYOUT}"],
        capture_output=True, text=True)
    assert r.returncode == 0, r.stderr or r.stdout

    doc = json.loads((tmp_path / "img.oimg.map.json").read_text())

    # At least one init_* method of this file must carry lines, proving that
    # factory-body initializers are included in the source map.
    init_entries = {m["name"]: m["lines"] for m in doc["methods"].values()
                    if ">>init_" in m["name"] and m["file"] == 0 and m["lines"]}
    assert init_entries, "expected at least one init_* function in the source map"
    for name, pairs in init_entries.items():
        assert pairs, f"{name} has an empty pair list"
        for pair in pairs:
            assert len(pair) == 2
            assert isinstance(pair[0], int) and isinstance(pair[1], int)


def test_a_body_that_is_one_expression_gets_its_line(tmp_path):
    """rete#343: the last, non-return expression of a block body was compiled
    without record_line, so a one-expression block had no line at all — for an
    escaping block (its own method) and an inlined arm alike."""
    import ns_compile_oevm as C
    src = """class Probe packageUsing: manifest = () (
public answer = ( ^ 42 )
public pick: b = (
  | r k |
  r:: b
    ifTrue: [
      self answer ]
    ifFalse: [
      self answer + 1 ].
  k:: [ :x |
    x + r ].
  ^ k value: r )
) : ( )
"""
    compiler = C.OevmCompiler(_parse(src, tmp_path))
    compiler.compile()
    sm = compiler.source_maps
    pick = {line for _, line in sm["Probe>>pick:"]}
    assert {7, 9} <= pick, pick                       # the inlined arms
    blk = next(v for k, v in sm.items() if "_block" in k)
    assert [line for _, line in blk] == [11]          # the escaping block
