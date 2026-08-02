import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from stmap import build_stmap


def test_build_stmap_shape():
    out = build_stmap("foo.nsl", {"Bar>>baz": [(0, 3), (2, 4)]})
    doc = json.loads(out)
    assert doc == {
        "source": "foo.nsl",
        "functions": {"Bar>>baz": [[0, 3], [2, 4]]},
    }


def test_build_stmap_empty():
    doc = json.loads(build_stmap("x.nsl", {}))
    assert doc == {"source": "x.nsl", "functions": {}}


def test_build_stmap_multiple_functions_preserve_order():
    sm = {"A>>m": [(0, 1)], "B>>n": [(0, 5), (1, 6)]}
    doc = json.loads(build_stmap("x.nsl", sm))
    assert list(doc["functions"].keys()) == ["A>>m", "B>>n"]
    assert doc["functions"]["B>>n"] == [[0, 5], [1, 6]]
