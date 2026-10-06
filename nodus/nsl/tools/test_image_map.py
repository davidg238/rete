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


def compile_map(src, out):
    """Compile `src` to the .oimg `out`; answer (blob, map)."""
    r = subprocess.run([sys.executable, str(TOOLS / "ns_compile_oevm.py"),
                        str(src), str(out), "--format=oimg",
                        f"--layout={LAYOUT}"], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr or r.stdout
    return out.read_bytes(), json.loads(Path(str(out) + ".map.json").read_text())


@pytest.fixture(scope="module")
def fresh(tmp_path_factory):
    return compile_map(FIXTURE / "Fixture.nsl", tmp_path_factory.mktemp("img") / "Fixture.oimg")


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


def test_every_method_has_exactly_the_four_fields(fresh):
    _, doc = fresh
    for off, m in doc["methods"].items():
        assert off == str(int(off)), off
        assert set(m) == {"name", "file", "lines", "vars"}, (off, m)
        assert isinstance(m["vars"], list), m["name"]
        assert ">>" in m["name"] and not m["name"].startswith("Block_"), m["name"]
        assert 0 <= m["file"] < len(doc["files"])
        offsets = [o for o, _ in m["lines"]]
        assert offsets == sorted(offsets), m["name"]


def test_each_shape_the_map_names(fresh):
    mine = own(fresh[1])
    assert mine["Fixture>>main:args:"][1][0] == [1, 19]       # the entry's method; 0 is its MK_CONTEXT (captures k)
    assert mine["Fixture_Counter>>start:"] == (0, [[0, 12]])  # a nested class
    assert mine["Meta_Fixture_Counter>>from:"] == (0, [[0, 14]])  # class side
    assert mine["Fixture_block1>>value:"] == (0, [[1, 21]])   # a block, no prefix; 0 is its MK_CONTEXT (#343)
    assert mine["Fixture>>init_platform:"] == (0, [])         # synthesized: no lines


def test_a_kernel_method_points_at_the_kernel(fresh):
    _, doc = fresh
    m = next(m for m in doc["methods"].values() if m["name"] == "Kernel>>rankOf:")
    assert doc["files"][m["file"]] == "kernel.nsl"


def _method(doc, name):
    return next(m for m in doc["methods"].values() if m["name"] == name)


def test_vars_every_shape(fresh):
    """rete#323: each §3 shape of `vars` — a register for the whole method, an
    inlined block's register with its range, a captured local's Context slot."""
    names = {v["name"]: v for v in _method(fresh[1], "Fixture>>main:args:")["vars"]}
    assert names["platform2"] == {"name": "platform2", "reg": 1}
    assert names["args"] == {"name": "args", "reg": 2}
    assert set(names["k"]) == {"name", "ctx"}            # captured by the kept block
    i = names["i"]                                        # the inlined to:do: param
    assert set(i) == {"name", "reg", "from", "to"} and i["from"] < i["to"]
    assert "self" not in names


def test_vars_reused_register_has_disjoint_ranges(tmp_path):
    src = tmp_path / "Reuse.nsl"
    src.write_text(
        "class Reuse usingPlatform: platform = () (\n"
        "  public main: p args: a = (\n"
        "    true ifTrue: [ | x | x:: 1. x println ].\n"
        "    true ifTrue: [ | y | y:: 2. y println ].\n"
        "    ^ 0 )\n) : ()\n")
    _, doc = compile_map(src, tmp_path / "Reuse.oimg")
    vs = {v["name"]: v for v in _method(doc, "Reuse>>main:args:")["vars"]}
    assert vs["x"]["reg"] == vs["y"]["reg"]
    assert vs["x"]["to"] <= vs["y"]["from"]


def test_vars_nested_inlined_scope_lists_the_inner_name_later(tmp_path):
    """An inner inlined scope shadowing a name must be listed AFTER the outer one,
    so §3's "listed later wins" picks the inner register (rete#323 review)."""
    src = tmp_path / "Nest.nsl"
    src.write_text(
        "class Nest usingPlatform: platform = () (\n"
        "  public main: p args: a = (\n"
        "    1 to: 3 do: [ :i | 1 to: 2 do: [ :i | i println ] ].\n"
        "    ^ 0 )\n) : ()\n")
    _, doc = compile_map(src, tmp_path / "Nest.oimg")
    iv = [v for v in _method(doc, "Nest>>main:args:")["vars"] if v["name"] == "i"]
    assert len(iv) == 2, iv
    outer, inner = iv
    assert outer["from"] <= inner["from"] and inner["to"] <= outer["to"], iv
    assert outer["reg"] != inner["reg"], iv
