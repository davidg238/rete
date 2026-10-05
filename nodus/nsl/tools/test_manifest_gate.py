# Copyright (c) 2026 Ekorau LLC
"""rete#243: every record the node's own encoder produces validates against
protocol/MANIFEST.cddl via zcbor, and the manifest itself lints clean.

The two codecs (node Cbor, porta cval) were golden-tested only against each
other before this; now the node's side answers to the one declared contract,
and porta's side is covered transitively (cval is byte-equal-golden-tested
against the node's hex) plus directly by protocol/manifest's tests.
"""
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "protocol" / "tools"))
from manifestlib import parse  # noqa: E402
from manifest_lint import lint  # noqa: E402
import zcbor_env  # noqa: E402

CDDL = ROOT / "protocol" / "MANIFEST.cddl"
HARNESS = ROOT / "nodus" / "nsl" / "tools" / "run_tests_oevm.py"
FIXTURE = ROOT / "nodus" / "nsl" / "tools" / "fixtures" / "manifest_goldens.nsl"


def _golden_lines(tmp_path):
    # Two halves. (1) Run the fixture through the single-file harness (#252),
    # isolated exactly as test_single_file_mode.py isolates — a green run
    # proves the VM still encodes byte-for-byte what the expect block pins.
    # (2) Read the goldens from that expect block: on PASS the harness
    # swallows the fixture's stdout, so the pinned block IS the transcript.
    import os
    env = dict(os.environ)
    env.pop("OEVM_PATH", None)
    env["OEVM_BUILD_DIR"] = str(tmp_path / "bd")
    proc = subprocess.run(
        [sys.executable, str(HARNESS), str(FIXTURE)],
        capture_output=True, text=True, timeout=120, env=env)
    assert proc.returncode == 0, proc.stdout + proc.stderr  # expect-block golden holds
    out = []
    for line in FIXTURE.read_text().splitlines():
        if line == "*)":
            break
        if line.startswith("GOLDEN "):
            _, path, hexs = line.split()
            out.append((path, bytes.fromhex(hexs)))
    return out


def test_lint_clean():
    assert lint(CDDL.read_text()) == []


def test_every_golden_validates(tmp_path):
    m = parse(CDDL.read_text())
    lines = _golden_lines(tmp_path)
    assert lines, "fixture printed no GOLDEN lines"
    covered = set()
    for path, blob in lines:
        e = m.match(path)
        assert e is not None, f"fixture path {path} matches no manifest entry"
        try:
            zcbor_env.validate(CDDL, e.rule, blob)
        except subprocess.CalledProcessError as err:
            raise AssertionError(
                f"{path} (rule {e.rule}) failed zcbor: {err.stderr.decode()[-400:]}")
        covered.add(e.rule)
    rules_needing_cover = {e.rule for e in m.entries}
    assert rules_needing_cover <= covered, \
        f"rules with no golden: {rules_needing_cover - covered}"


def test_negative_control(tmp_path):
    # A Health golden validated as a Metrics instance must FAIL — a gate
    # that has never been seen red is the false-green shape.
    lines = dict(_golden_lines(tmp_path))
    with pytest.raises(subprocess.CalledProcessError):
        zcbor_env.validate(CDDL, "metrics", lines["obs/health"])
