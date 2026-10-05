"""The image map's writer against its contract (rete#337, protocol/IMAGE-ARTIFACTS.md).

The fixture in protocol/fixtures/image/ is what both sides check against: custos reads
the checked-in pair, and this test holds the compiler to it. Not byte for byte — the
image is mostly kernel, and every kernel edit would fail a byte compare — but the
fixture program's own methods by name, file and lines, and every §3 rule on a fresh
compile."""
import hashlib
import json
import subprocess
import sys
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parents[2]
FIXTURE = REPO / "protocol" / "fixtures" / "image"
LAYOUT = REPO / "nodus" / "oevm" / "zephyr" / "layout-esp32c6.json"


@pytest.fixture(scope="module")
def fresh(tmp_path_factory):
    out = tmp_path_factory.mktemp("img") / "Fixture.oimg"
    r = subprocess.run([sys.executable, str(TOOLS / "ns_compile_oevm.py"),
                        str(FIXTURE / "Fixture.nsl"), str(out), "--format=oimg",
                        f"--layout={LAYOUT}"], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr or r.stdout
    return out.read_bytes(), json.loads(Path(str(out) + ".map.json").read_text())


def own(doc):
    """The fixture program's methods (files[0]), by name: offsets move with the kernel."""
    return {m["name"]: (m["file"], m["lines"]) for m in doc["methods"].values()
            if m["file"] == 0}


def test_the_checked_in_pair_belongs_together():
    blob = (FIXTURE / "Fixture.oimg").read_bytes()
    doc = json.loads((FIXTURE / "Fixture.oimg.map.json").read_text())
    assert doc["image"] == {"sha256": hashlib.sha256(blob).hexdigest(),
                            "size": len(blob), "oop_size": 4}


def test_a_fresh_compile_writes_the_fixture_programs_methods_as_checked_in(fresh):
    checked_in = json.loads((FIXTURE / "Fixture.oimg.map.json").read_text())
    assert own(fresh[1]) == own(checked_in)


def test_the_map_names_its_image(fresh):
    blob, doc = fresh
    assert set(doc) == {"image", "entry", "files", "methods"}
    assert doc["image"] == {"sha256": hashlib.sha256(blob).hexdigest(),
                            "size": len(blob), "oop_size": 4}
    assert blob[:4] == b"OIMG"
    assert int.from_bytes(blob[12:16], "little") == len(blob)    # blob_size


def test_entry_and_files(fresh):
    _, doc = fresh
    assert doc["entry"] == "Fixture"
    assert doc["files"] == ["Fixture.nsl", "kernel.nsl", "supervisor.nsl"]


def test_every_method_has_exactly_the_three_fields(fresh):
    _, doc = fresh
    for off, m in doc["methods"].items():
        assert off == str(int(off)), off
        assert set(m) == {"name", "file", "lines"}, (off, m)
        assert ">>" in m["name"] and not m["name"].startswith("Block_"), m["name"]
        assert 0 <= m["file"] < len(doc["files"])
        offsets = [o for o, _ in m["lines"]]
        assert offsets == sorted(offsets), m["name"]


def test_each_shape_the_map_names(fresh):
    mine = own(fresh[1])
    assert mine["Fixture>>main:args:"][1][0] == [0, 18]       # the entry's method
    assert mine["Fixture_Counter>>start:"] == (0, [[0, 11]])  # a nested class
    assert mine["Meta_Fixture_Counter>>from:"] == (0, [[0, 13]])  # class side
    assert "Fixture_block1>>value:" in mine                   # a block, no prefix
    assert mine["Fixture>>init_platform:"] == (0, [])         # synthesized: no lines


def test_a_kernel_method_points_at_the_kernel(fresh):
    _, doc = fresh
    m = next(m for m in doc["methods"].values() if m["name"] == "Kernel>>rankOf:")
    assert doc["files"][m["file"]] == "kernel.nsl"
