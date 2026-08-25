# Copyright (c) 2026 Ekorau LLC
"""Lint contract tests (#243). The real MANIFEST.cddl is the primary fixture:
it must be clean, and each seeded defect must be a finding."""
from pathlib import Path

from manifest_lint import lint

GOOD = Path(__file__).resolve().parents[1] / "MANIFEST.cddl"


def test_real_manifest_is_clean():
    assert lint(GOOD.read_text()) == []


def test_wildcard_overlap_is_error():
    text = GOOD.read_text() + \
        "\n;# path=diag/spans/<x>  kind=cell  class=routine  dir=up\ndup-span = tstr\n"
    assert any("overlap" in f for f in lint(text))


def test_duplicate_path_is_error():
    text = GOOD.read_text() + \
        "\n;# path=obs/health  kind=cell  class=routine  dir=up\ndup-health = tstr\n"
    assert any("duplicate path" in f for f in lint(text))


def test_unreferenced_sym_is_error():
    text = GOOD.read_text() + '\nsym-orphan = #6.39("orphan")\n'
    assert any("orphan" in f for f in lint(text))


def test_undefined_sym_reference_is_error():
    text = GOOD.read_text() + \
        "\n;# path=x/y  kind=cell  class=routine  dir=up\nbad = #6.27([sym-Nope, { sym-up => uint }])\n"
    assert any("sym-Nope" in f for f in lint(text))


def test_unannotated_unreferenced_rule_is_error():
    text = GOOD.read_text() + "\nstray = tstr\n"
    assert any("stray" in f for f in lint(text))
