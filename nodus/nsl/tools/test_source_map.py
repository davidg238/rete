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
def test_stmap_sidecar_written_path_a(tmp_path):
    src = NS_TESTS / "04_somns_basic" / "01_method_call.nsl"
    out = tmp_path / "img.c"
    r = subprocess.run(
        [sys.executable, str(TOOLS / "ns_compile_oevm.py"), str(src), str(out)],
        capture_output=True, text=True)
    assert r.returncode == 0, r.stderr or r.stdout

    stmap = tmp_path / "img.stmap"
    assert stmap.exists(), "expected a .stmap sidecar next to the image"
    doc = json.loads(stmap.read_text())
    assert doc["source"] == "01_method_call.nsl"
    assert isinstance(doc["functions"], dict) and doc["functions"]
    # Every function maps to a list of [offset, line] pairs.
    for name, pairs in doc["functions"].items():
        assert ">>" in name
        for pair in pairs:
            assert len(pair) == 2
            assert isinstance(pair[0], int) and isinstance(pair[1], int)
    # A specific known method key must be present.
    assert any(k.endswith(">>test") for k in doc["functions"])


@pytest.mark.skipif(not NS_TESTS.exists(), reason="nodus/tests not present (broken checkout)")
def test_factory_body_initializers_are_mapped(tmp_path):
    """Regression test for Fix 1: compile_initializer must register source maps
    for initializers that have a factory body (initBody statements)."""
    src = NS_TESTS / "04_somns_basic" / "13_factory_body.nsl"
    out = tmp_path / "img.c"
    r = subprocess.run(
        [sys.executable, str(TOOLS / "ns_compile_oevm.py"), str(src), str(out)],
        capture_output=True, text=True)
    assert r.returncode == 0, r.stderr or r.stdout

    stmap = tmp_path / "img.stmap"
    assert stmap.exists(), "expected a .stmap sidecar"
    doc = json.loads(stmap.read_text())

    # At least one init_* key must be present with a non-empty pair list,
    # proving that factory-body initializers are included in the source map.
    init_entries = {k: v for k, v in doc["functions"].items() if ">>init_" in k}
    assert init_entries, "expected at least one init_* function in the source map"
    for name, pairs in init_entries.items():
        assert pairs, f"{name} has an empty pair list"
        for pair in pairs:
            assert len(pair) == 2
            assert isinstance(pair[0], int) and isinstance(pair[1], int)
